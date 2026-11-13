import { Buffer } from "node:buffer";
import { randomUUID } from "node:crypto";
import { startup, type Query, type SDKUserMessage, type SpawnedProcess, type WarmQuery } from "@anthropic-ai/claude-agent-sdk";
import { ApprovalExpiredError } from "./approvals.js";
import { claudeOptions, type ClaudeNodeProfile } from "./profile.js";
import { record, snapshot, text, type Json } from "./json.js";

export type ClaudeSessionPhase = "new" | "starting" | "ready" | "running" | "interrupting" | "closed" | "failed" | "lost";
export type ClaudeSessionEvent =
  { readonly sequence: number; readonly kind: "sdk-message"; readonly message: Json } |
  { readonly sequence: number; readonly kind: "interrupt-receipt"; readonly messageID: string; readonly receipt: Json } |
  { readonly sequence: number; readonly kind: "closed"; readonly reason: "stopped" | "failed" | "lost";
    readonly pendingMessageID: string | null };
export interface ClaudeSessionLimits {
  readonly events?: number;
  readonly queuedBytes?: number;
  readonly outputBytes?: number;
  readonly lifetimeMs?: number;
}
type EventInput = ClaudeSessionEvent extends infer E ? E extends { sequence: number } ? Omit<E, "sequence"> : never : never;

/** One SDK streaming-input conversation. Core grants and OS child reaping belong
 * to the embedding process owner; this class never searches for an executable. */
export class ClaudeNodeSession {
  private phaseValue: ClaudeSessionPhase = "new";
  private failureValue: Error | undefined;
  private readonly limits: Required<ClaudeSessionLimits>;
  private readonly profile: ClaudeNodeProfile;
  private warm: WarmQuery | undefined;
  private query: Query | undefined;
  private child: SpawnedProcess | undefined;
  private inputEnded = false;
  private drainTimer: ReturnType<typeof setTimeout> | undefined;
  private readonly ended = () => {
    this.endInput();
    // The SDK can keep its message iterator pending after transport EOF in
    // streaming-input mode. Allow buffered result frames to drain, then stop
    // observing rather than keeping an unresolved turn alive indefinitely.
    if (!this.terminal && !this.drainTimer) this.drainTimer = setTimeout(() =>
      this.finish("lost", new Error("Claude SDK output ended")), 500);
  };
  private readonly childError = (error: Error) => this.finish("lost", error);
  private active: string | undefined;
  private queued: SDKUserMessage | undefined;
  private waiter: ((value: IteratorResult<SDKUserMessage>) => void) | undefined;
  private interruption: Promise<void> | undefined;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private readonly events: { event: ClaudeSessionEvent; bytes: number }[] = [];
  private bytes = 0;
  private output = 0;
  private sequence = 0;
  private readonly abort = () => this.close();

  constructor(profile: ClaudeNodeProfile, limits: ClaudeSessionLimits = {}) {
    this.profile = { ...profile };
    this.limits = { events: limits.events ?? 256, queuedBytes: limits.queuedBytes ?? 2_097_152,
      outputBytes: limits.outputBytes ?? 67_108_864, lifetimeMs: limits.lifetimeMs ?? 600_000 };
    for (const [key, minimum, maximum] of [["events", 2, 256], ["queuedBytes", 1024, 2_097_152],
      ["outputBytes", 1024, 268_435_456], ["lifetimeMs", 1, 3_600_000]] as const) {
      const value = this.limits[key];
      if (!Number.isSafeInteger(value) || value < minimum || value > maximum) throw new RangeError("Invalid Claude session limit");
    }
  }

  get phase(): ClaudeSessionPhase { return this.phaseValue; }
  get failure(): Error | undefined { return this.failureValue; }
  get pendingMessageID(): string | undefined { return this.active; }
  get queuedBytes(): number { return this.bytes; }
  takeEvent(): ClaudeSessionEvent | undefined {
    const value = this.events.shift();
    if (value) this.bytes -= value.bytes;
    return value?.event;
  }
  private get terminal(): boolean { return ["closed", "failed", "lost"].includes(this.phaseValue); }

  async start(): Promise<void> {
    if (this.phaseValue !== "new") throw new Error("Claude session can only start once");
    this.phaseValue = "starting";
    this.profile.abortController.signal.addEventListener("abort", this.abort, { once: true });
    this.timer = setTimeout(() => this.finish("failed", new Error("Claude session lifetime exhausted")), this.limits.lifetimeMs);
    this.timer.unref();
    try {
      const options = claudeOptions({ ...this.profile, spawn: request => {
        const child = this.profile.spawn(request); this.child = child;
        child.stdout.once("end", this.ended);
        child.once("exit", this.ended); child.once("error", this.childError);
        return child;
      } });
      options.includePartialMessages = true;
      options.canUseTool = (...args) => {
        if (this.phaseValue !== "running" || this.active === undefined) return Promise.reject(new ApprovalExpiredError());
        const result = this.profile.approvals.canUseTool(...args);
        if (this.profile.approvals.closed) this.finish("failed", this.profile.approvals.failure ?? new ApprovalExpiredError());
        return result;
      };
      const warm = await startup({ options, initializeTimeoutMs: 15_000 });
      if (this.terminal) { warm.close(); throw new ApprovalExpiredError(); }
      this.warm = warm;
      const input: AsyncIterable<SDKUserMessage> = { [Symbol.asyncIterator]: () => ({ next: () => this.nextInput() }) };
      this.query = warm.query(input);
      this.phaseValue = "ready";
      void this.read(this.query).catch(error => this.finish(this.inputEnded ? "lost" : "failed", this.asError(error)));
    } catch (error) {
      this.finish("failed", this.asError(error));
      throw error;
    }
  }

  /** Returns an SDK uuid for correlation, not a delivery/execution receipt. */
  send(prompt: string): string {
    if (this.phaseValue !== "ready" || this.inputEnded || this.profile.approvals.closed) throw new Error("Claude session is not ready for input");
    const content = text(prompt, 65_536);
    const uuid = randomUUID();
    this.profile.approvals.beginTurn(uuid);
    const message: SDKUserMessage = { type: "user", uuid, message: { role: "user", content }, parent_tool_use_id: null };
    this.active = uuid; this.phaseValue = "running";
    const waiter = this.waiter;
    if (waiter) { this.waiter = undefined; waiter({ value: message, done: false }); }
    else this.queued = message;
    return uuid;
  }

  private nextInput(): Promise<IteratorResult<SDKUserMessage>> {
    if (this.terminal || this.inputEnded) return Promise.resolve({ value: undefined, done: true });
    if (this.queued) {
      const message = this.queued; this.queued = undefined;
      return Promise.resolve({ value: message, done: false });
    }
    if (this.waiter) return Promise.reject(new Error("Concurrent Claude input pulls"));
    return new Promise(resolve => { this.waiter = resolve; });
  }

  interrupt(): Promise<void> {
    if (this.phaseValue === "interrupting" && this.interruption) return this.interruption;
    if (this.phaseValue !== "running" || !this.query || !this.active) return Promise.reject(new Error("No live Claude turn to interrupt"));
    const messageID = this.active;
    this.phaseValue = "interrupting";
    this.profile.approvals.expirePending();
    if (this.profile.approvals.closed) {
      const error = new Error("Claude approval broker failed during interrupt");
      this.finish("failed", error); return Promise.reject(error);
    }
    this.interruption = this.query.interrupt().then(receipt => {
      // A receipt acknowledges control handling. Only a correlated backend result
      // ends the active turn; queued survivors in the receipt remain explicit.
      if (!this.terminal) this.publish({ kind: "interrupt-receipt", messageID,
        receipt: receipt === undefined ? null : snapshot(receipt) });
    }).catch(error => { this.finish("lost", this.asError(error)); throw error; });
    void this.interruption.catch(() => {});
    return this.interruption;
  }

  private async read(query: Query): Promise<void> {
    for await (const message of query) {
      if (this.terminal) return;
      // Record our validation/budget failure before the SDK iterator's return()
      // closes stdout; cleanup EOF must not relabel that failure as transport loss.
      try { this.receive(message); }
      catch (error) { this.finish("failed", this.asError(error)); return; }
      if (this.terminal) return;
    }
    if (!this.terminal) this.finish("lost", new Error("Claude SDK stream ended without an explicit close"));
  }

  private receive(message: unknown): void {
      const value = snapshot(message, 524_288);
      if (!record(value) || typeof value.type !== "string") throw new Error("Invalid Claude SDK message");
      const bytes = Buffer.byteLength(JSON.stringify(value));
      if (this.output > this.limits.outputBytes - bytes) throw new RangeError("Claude session output budget exhausted");
      this.output += bytes;
      if (value.type === "result") {
        if (typeof value.is_error !== "boolean" || typeof value.subtype !== "string" ||
            (value.subtype === "success") === value.is_error) throw new Error("Invalid Claude result status");
        // Session-scoped failures can lack an echoed uuid. Preserve their SDK
        // diagnostic but never attribute a missing/foreign receipt to this send.
        if (!this.active || value.user_message_uuid !== this.active) {
          this.publish({ kind: "sdk-message", message: value });
          this.finish("lost", new Error("Uncorrelated Claude result; turn outcome unknown")); return;
        }
        this.profile.approvals.expirePending();
        if (this.profile.approvals.closed) throw new Error("Claude approval broker failed at turn boundary");
        this.publish({ kind: "sdk-message", message: value });
        this.active = undefined; this.interruption = undefined; this.phaseValue = "ready";
      } else this.publish({ kind: "sdk-message", message: value });
  }

  private publish(input: EventInput): void {
    const event = Object.freeze({ ...input, sequence: ++this.sequence }) as ClaudeSessionEvent;
    const bytes = Buffer.byteLength(JSON.stringify(event));
    // Reserve one bounded terminal observation even when the consumer stops.
    if (this.events.length >= this.limits.events - 1 || this.bytes + bytes > this.limits.queuedBytes - 512) {
      throw new RangeError("Claude session event budget exhausted");
    }
    this.events.push({ event, bytes }); this.bytes += bytes;
  }

  close(): void { this.finish("stopped"); }
  private asError(value: unknown): Error { return value instanceof Error ? value : new Error("Claude SDK failed"); }
  private endInput(): void {
    this.inputEnded = true; this.queued = undefined;
    const waiter = this.waiter; this.waiter = undefined;
    waiter?.({ value: undefined, done: true });
  }
  private finish(reason: "stopped" | "failed" | "lost", failure?: Error): void {
    if (this.terminal) return;
    const pendingMessageID = this.active ?? null;
    this.phaseValue = reason === "stopped" ? "closed" : reason;
    this.failureValue = failure;
    clearTimeout(this.timer);
    clearTimeout(this.drainTimer);
    this.profile.abortController.signal.removeEventListener("abort", this.abort);
    this.child?.stdout.off("end", this.ended);
    this.child?.off("exit", this.ended); this.child?.off("error", this.childError);
    this.profile.abortController.abort();
    this.endInput();
    this.profile.approvals.close(failure);
    this.query?.close(); this.warm?.close();
    const event: ClaudeSessionEvent = Object.freeze({ sequence: ++this.sequence, kind: "closed", reason, pendingMessageID });
    const bytes = Buffer.byteLength(JSON.stringify(event));
    this.events.push({ event, bytes }); this.bytes += bytes;
  }
}
