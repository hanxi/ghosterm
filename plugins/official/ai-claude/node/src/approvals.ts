import { Buffer } from "node:buffer";
import { randomUUID } from "node:crypto";
import type { CanUseTool, PermissionResult } from "@anthropic-ai/claude-agent-sdk";
import { record, snapshot, text, type Json } from "./json.js";

export interface ApprovalReference { readonly scope: string; readonly requestID: string; readonly token: string; }
export interface Question {
  readonly question: string;
  readonly header: string;
  readonly multiSelect: boolean;
  readonly options: readonly { readonly label: string; readonly description: string }[];
}
export interface ApprovalRequest {
  readonly reference: ApprovalReference;
  readonly kind: "tool" | "question";
  readonly toolName: string;
  readonly toolUseID: string;
  readonly agentID?: string;
  readonly input: Readonly<Record<string, Json>>;
  readonly defaultToNo: boolean;
  readonly suppressAlwaysAllowRule: boolean;
  readonly blockedPath?: string;
  readonly decisionReason?: string;
  readonly title?: string;
  readonly displayName?: string;
  readonly description?: string;
  readonly mcpServer?: { readonly name: string; readonly source: string };
  readonly questions?: readonly Question[];
}
export type ApprovalEvent =
  { readonly kind: "requested"; readonly request: ApprovalRequest } |
  { readonly kind: "resolved" | "expired"; readonly reference: ApprovalReference } |
  { readonly kind: "closed"; readonly scope: string; readonly reason: "stopped" | "failed" };

export class ApprovalExpiredError extends Error {
  constructor() { super("Claude approval is no longer live"); this.name = "AbortError"; }
}

type Entry = {
  readonly request: ApprovalRequest; readonly identity: string; readonly bytes: number;
  readonly promise: Promise<PermissionResult>;
  readonly resolve: (value: PermissionResult) => void; readonly reject: (error: Error) => void;
  readonly signals: Map<AbortSignal, () => void>;
  state: "pending" | "resolved" | "expired";
};
export interface ApprovalLimits { readonly pending?: number; readonly retained?: number; readonly bytes?: number; }

function questions(input: Readonly<Record<string, Json>>): readonly Question[] {
  const values = input.questions;
  if (!Array.isArray(values) || values.length < 1 || values.length > 4) throw new TypeError("Invalid Claude questions");
  const result = values.map(value => {
    if (!record(value) || typeof value.multiSelect !== "boolean" || !Array.isArray(value.options) ||
        value.options.length < 2 || value.options.length > 4) throw new TypeError("Invalid Claude question choices");
    const header = text(value.header, 128);
    if ([...header].length > 12) throw new TypeError("Claude question header exceeds 12 characters");
    const options = value.options.map(option => {
      if (!record(option)) throw new TypeError("Invalid Claude question option");
      return Object.freeze({ label: text(option.label, 256), description: text(option.description, 2048, true) });
    });
    if (new Set(options.map(option => option.label)).size !== options.length) throw new TypeError("Duplicate Claude choices");
    return Object.freeze({ question: text(value.question, 2048), header, multiSelect: value.multiSelect,
      options: Object.freeze(options) });
  });
  if (new Set(result.map(value => value.question)).size !== result.length) throw new TypeError("Ambiguous Claude question keys");
  return Object.freeze(result);
}

/** Node-side SDK prompt broker. It is not a Core capability or an OS sandbox. */
export class ClaudeApprovalBroker {
  readonly scope: string;
  private readonly entries = new Map<string, Entry>();
  private readonly events: ApprovalEvent[] = [];
  private readonly limits: Required<ApprovalLimits>;
  private serial = 0;
  private readonly generation = randomUUID();
  private bytes = 0;
  private stopped = false;
  private failed: Error | undefined;

  constructor(scope: string, limits: ApprovalLimits = {}) {
    this.scope = text(scope, 128);
    this.limits = { pending: limits.pending ?? 32, retained: limits.retained ?? 64, bytes: limits.bytes ?? 1_048_576 };
    for (const [key, ceiling] of [["pending", 32], ["retained", 64], ["bytes", 1_048_576]] as const) {
      const value = this.limits[key];
      if (!Number.isSafeInteger(value) || value < 1 || value > ceiling) throw new RangeError("Invalid Claude approval limits");
    }
    if (this.limits.retained < this.limits.pending) throw new RangeError("Retained approval budget is smaller than pending budget");
  }

  get closed(): boolean { return this.stopped; }
  get failure(): Error | undefined { return this.failed; }
  get retainedBytes(): number { return this.bytes; }
  get retainedCount(): number { return this.entries.size; }
  get pending(): readonly ApprovalRequest[] {
    return Object.freeze([...this.entries.values()].filter(value => value.state === "pending").map(value => value.request));
  }
  takeEvent(): ApprovalEvent | undefined { return this.events.shift(); }

  readonly canUseTool: CanUseTool = (toolName, input, context) => {
    try { return this.request(toolName, input, context); }
    catch (error) { return Promise.reject(error); }
  };

  private request(toolName: string, input: Record<string, unknown>, context: Parameters<CanUseTool>[2]): Promise<PermissionResult> {
    if (this.stopped || context.signal.aborted) return Promise.reject(new ApprovalExpiredError());
    const value = snapshot(input);
    if (!record(value)) throw new TypeError("Claude tool input must be an object");
    const metadata = {
      ...(context.agentID === undefined ? {} : { agentID: text(context.agentID, 128) }),
      ...(context.blockedPath === undefined ? {} : { blockedPath: text(context.blockedPath, 4096) }),
      ...(context.decisionReason === undefined ? {} : { decisionReason: text(context.decisionReason, 2048, true) }),
      ...(context.title === undefined ? {} : { title: text(context.title, 2048, true) }),
      ...(context.displayName === undefined ? {} : { displayName: text(context.displayName, 2048, true) }),
      ...(context.description === undefined ? {} : { description: text(context.description, 2048, true) }),
      ...(context.mcpServer === undefined ? {} : { mcpServer: Object.freeze({
        name: text(context.mcpServer.name, 128), source: text(context.mcpServer.source, 128) }) }),
    };
    const raw = snapshot({ toolName: text(toolName, 128), input: value,
      requestID: text(context.requestId, 128), toolUseID: text(context.toolUseID, 128),
      ...metadata,
      defaultToNo: context.defaultToNo === undefined ? false : context.defaultToNo,
      suppressAlwaysAllowRule: context.suppressAlwaysAllowRule === undefined ? false : context.suppressAlwaysAllowRule }, 81_920);
    if (!record(raw) || typeof raw.defaultToNo !== "boolean" || typeof raw.suppressAlwaysAllowRule !== "boolean") {
      throw new TypeError("Invalid Claude approval hints");
    }
    const identity = JSON.stringify(raw);
    const previous = this.entries.get(context.requestId);
    if (previous) {
      if (previous.identity !== identity) {
        const failure = new Error("Claude request ID was reused for different input or identity");
        this.close(failure); throw failure;
      }
      if (previous.state === "pending") this.watch(previous, context.signal);
      return previous.promise;
    }
    const bytes = Buffer.byteLength(identity);
    if (this.pending.length >= this.limits.pending || bytes > this.limits.bytes) throw new RangeError("Claude pending approval budget exhausted");
    while (this.entries.size >= this.limits.retained || this.bytes > this.limits.bytes - bytes) {
      // An unread request event still owns its full payload. Keep charging that
      // entry until the consumer takes the event, even after an API decision.
      const removable = [...this.entries].find(([, entry]) => entry.state !== "pending" &&
        !this.events.some(event => event.kind === "requested" && event.request === entry.request));
      if (!removable) throw new RangeError("Claude retained approval budget exhausted");
      this.entries.delete(removable[0]); this.bytes -= removable[1].bytes;
    }
    if (this.serial >= Number.MAX_SAFE_INTEGER) throw new RangeError("Claude approval token exhausted");
    const reference = Object.freeze({ scope: this.scope, requestID: context.requestId, token: `approval-${this.generation}-${++this.serial}` });
    const request: ApprovalRequest = Object.freeze({ reference, kind: toolName === "AskUserQuestion" ? "question" : "tool",
      toolName, toolUseID: context.toolUseID, input: value,
      defaultToNo: raw.defaultToNo, suppressAlwaysAllowRule: raw.suppressAlwaysAllowRule,
      ...metadata,
      ...(toolName === "AskUserQuestion" ? { questions: questions(value) } : {}) });
    let resolve!: Entry["resolve"], reject!: Entry["reject"];
    const promise = new Promise<PermissionResult>((yes, no) => { resolve = yes; reject = no; });
    // Keep a terminal cached rejection observed even after all SDK waiters cancel.
    void promise.catch(() => {});
    const entry: Entry = { request, identity, bytes, promise, resolve, reject, signals: new Map(), state: "pending" };
    this.entries.set(context.requestId, entry); this.bytes += bytes;
    this.watch(entry, context.signal);
    this.publish({ kind: "requested", request });
    return promise;
  }

  private watch(entry: Entry, signal: AbortSignal): void {
    if (entry.signals.has(signal)) return;
    if (entry.signals.size >= 8) { const error = new RangeError("Too many duplicate Claude approval waiters"); this.close(error); throw error; }
    const abort = () => {
      if (entry.state !== "pending") return;
      entry.state = "expired"; this.detach(entry); entry.reject(new ApprovalExpiredError());
      this.publish({ kind: "expired", reference: entry.request.reference });
    };
    entry.signals.set(signal, abort);
    signal.addEventListener("abort", abort, { once: true });
    if (signal.aborted) abort();
  }

  private detach(entry: Entry): void {
    for (const [signal, listener] of entry.signals) signal.removeEventListener("abort", listener);
    entry.signals.clear();
  }

  private current(reference: ApprovalReference): Entry | undefined {
    const entry = this.entries.get(reference.requestID);
    return !this.stopped && reference.scope === this.scope && entry?.state === "pending" &&
      entry.request.reference.token === reference.token ? entry : undefined;
  }

  decide(reference: ApprovalReference, allow: boolean): boolean {
    const entry = this.current(reference);
    if (!entry || (allow && entry.request.kind === "question")) return false;
    return this.settle(entry, allow ? { behavior: "allow", updatedInput: entry.request.input,
      toolUseID: entry.request.toolUseID } : { behavior: "deny", message: "User declined in Ghosterm.", toolUseID: entry.request.toolUseID });
  }

  answer(reference: ApprovalReference, answers: Readonly<Record<string, string>>): boolean {
    const entry = this.current(reference);
    if (!entry || entry.request.kind !== "question") return false;
    const value = snapshot(answers, 16_384);
    const expected = entry.request.questions!;
    if (!record(value) || Object.keys(value).length !== expected.length ||
        expected.some(question => typeof value[question.question] !== "string" || !value[question.question])) {
      throw new TypeError("Every Claude question needs an explicit answer");
    }
    for (const question of expected) text(value[question.question], 2048);
    const updatedInput = snapshot({ ...entry.request.input, answers: value });
    if (!record(updatedInput)) throw new TypeError("Invalid Claude answer input");
    return this.settle(entry, { behavior: "allow", updatedInput, toolUseID: entry.request.toolUseID });
  }

  private settle(entry: Entry, result: PermissionResult): boolean {
    entry.state = "resolved"; this.detach(entry);
    // Publish before resolving: queue failure must not let a tool run unseen.
    if (!this.publish({ kind: "resolved", reference: entry.request.reference })) {
      entry.reject(this.failed ?? new ApprovalExpiredError()); return false;
    }
    entry.resolve(Object.freeze(result)); return true;
  }

  private publish(event: ApprovalEvent): boolean {
    if (this.events.length >= 128) { this.close(new RangeError("Claude approval event queue exhausted")); return false; }
    this.events.push(Object.freeze(event)); return true;
  }

  /** Retire prompts at a turn boundary without closing the conversation. */
  expirePending(): void {
    for (const entry of this.entries.values()) {
      if (entry.state !== "pending") continue;
      entry.state = "expired"; this.detach(entry); entry.reject(new ApprovalExpiredError());
      if (!this.publish({ kind: "expired", reference: entry.request.reference })) return;
    }
  }

  close(failure?: Error): void {
    if (this.stopped) return;
    this.stopped = true; this.failed = failure;
    for (const entry of this.entries.values()) {
      this.detach(entry);
      if (entry.state === "pending") { entry.state = "expired"; entry.reject(failure ?? new ApprovalExpiredError()); }
    }
    this.entries.clear(); this.bytes = 0; this.events.length = 0;
    this.events.push(Object.freeze({ kind: "closed", scope: this.scope, reason: failure ? "failed" : "stopped" }));
  }
}
