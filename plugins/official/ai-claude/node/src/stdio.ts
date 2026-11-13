import { Buffer } from "node:buffer";
import { TextDecoder } from "node:util";
import type { Readable, Writable } from "node:stream";
import type { ClaudeApprovalBroker, ApprovalReference } from "./approvals.js";
import { record, snapshot, text, type Json } from "./json.js";
import type { ClaudeNodeSession } from "./session.js";
import { CLAUDE_CLI_VERSION, CLAUDE_SDK_VERSION } from "./profile.js";

export class ClaudeStdioProtocolError extends Error {}
const FRAME_MAX = 65_536;
const CACHE_MAX = 2_097_152;
type Request = { id: string; sequence: number; method: string; params: Record<string, Json> };
type Cached = { identity: string; reply: Promise<string>; bytes: number };

function fields(value: Json, keys: readonly string[]): asserts value is Record<string, Json> {
  if (!record(value) || Object.keys(value).sort().join("\0") !== [...keys].sort().join("\0")) {
    throw new ClaudeStdioProtocolError("Unexpected Claude stdio fields");
  }
}
function reference(value: Json | undefined): ApprovalReference {
  fields(value ?? null, ["scope", "requestID", "token"]);
  const item = value as Record<string, Json>;
  return { scope: text(item.scope, 128), requestID: text(item.requestID, 128), token: text(item.token, 160) };
}

/** Private Node child protocol, not wire v1 and not authority to spawn a process.
 * Constructor receives an already selected profile/session from its owner. */
export class ClaudeStdioBridge {
  private sequence = 0;
  private busy = false;
  private stoppedValue = false;
  private explicitStopValue = false;
  private readonly cache = new Map<number, Cached>();
  private bytes = 0;
  private approvalNext = true;
  constructor(private readonly session: ClaudeNodeSession, private readonly approvals: ClaudeApprovalBroker) {}
  get stopped(): boolean { return this.stoppedValue; }
  get explicitStop(): boolean { return this.explicitStopValue; }

  handle(line: string): Promise<string> {
    try { return this.accept(line); }
    catch (error) { this.close(); return Promise.reject(error); }
  }
  private accept(line: string): Promise<string> {
    if (Buffer.byteLength(line) > FRAME_MAX) throw new ClaudeStdioProtocolError("Claude stdio frame limit");
    const value = snapshot(JSON.parse(line), FRAME_MAX);
    // Canonical JSON makes duplicate keys, alternate numeric spellings and
    // ambiguous replays unrepresentable without another permissive parser.
    if (JSON.stringify(value) !== line) throw new ClaudeStdioProtocolError("Claude stdio requires canonical JSON");
    fields(value, ["version", "id", "sequence", "method", "params"]);
    if (value.version !== 1 || !Number.isSafeInteger(value.sequence) || (value.sequence as number) < 1) {
      throw new ClaudeStdioProtocolError("Invalid Claude stdio version or sequence");
    }
    const id = text(value.id, 128), method = text(value.method, 32);
    const params = value.params;
    if (!record(params!)) throw new ClaudeStdioProtocolError("Claude stdio params must be an object");
    const sequence = value.sequence as number;
    const previous = this.cache.get(sequence);
    if (previous) {
      if (previous.identity !== line) throw new ClaudeStdioProtocolError("Changed Claude stdio replay");
      return previous.reply;
    }
    if (this.stoppedValue || sequence !== this.sequence + 1 || this.busy) {
      throw new ClaudeStdioProtocolError("Claude stdio sequence is not live");
    }
    this.validate(method, params!);
    this.sequence = sequence; this.busy = true;
    const reply = this.perform({ id, sequence, method, params: params! }).then(result => {
      const response = JSON.stringify({ version: 1, id, sequence, ok: true, result });
      if (Buffer.byteLength(response) > 600_000) throw new ClaudeStdioProtocolError("Claude stdio response limit");
      return response;
    }, () => JSON.stringify({ version: 1, id, sequence, ok: false, error: "ACTION_REJECTED", phase: this.session.phase }))
      .then(response => {
        const entry = this.cache.get(sequence)!;
        entry.bytes += Buffer.byteLength(response); this.bytes += Buffer.byteLength(response);
        this.busy = false;
        while (this.cache.size > 32 || this.bytes > CACHE_MAX) {
          const first = this.cache.entries().next().value!;
          this.cache.delete(first[0]); this.bytes -= first[1].bytes;
        }
        return response;
      }).catch(error => { this.close(); throw error; });
    void reply.catch(() => {});
    const bytes = Buffer.byteLength(line);
    this.cache.set(sequence, { identity: line, reply, bytes }); this.bytes += bytes;
    return reply;
  }

  private validate(method: string, params: Record<string, Json>): void {
    switch (method) {
      case "start": case "interrupt": case "stop": fields(params, []); break;
      case "poll":
        fields(params, params.maxEvents === undefined ? [] : ["maxEvents"]);
        if (params.maxEvents !== undefined && (!Number.isSafeInteger(params.maxEvents) ||
            typeof params.maxEvents !== "number" || params.maxEvents < 1 || params.maxEvents > 32)) {
          throw new ClaudeStdioProtocolError("Invalid Claude poll batch limit");
        }
        break;
      case "send": fields(params, ["text"]); text(params.text, 65_536); break;
      case "approve":
        fields(params, ["reference", "allow"]); reference(params.reference);
        if (typeof params.allow !== "boolean") throw new ClaudeStdioProtocolError("Explicit Claude decision required");
        break;
      case "answer":
        fields(params, ["reference", "answers"]); reference(params.reference);
        if (!record(params.answers!)) throw new ClaudeStdioProtocolError("Claude answers must be a string map");
        for (const answer of Object.values(params.answers!)) text(answer, 2048);
        break;
      default: throw new ClaudeStdioProtocolError("Unsupported Claude stdio method");
    }
  }

  private async perform(request: Request): Promise<unknown> {
    const { method, params } = request;
    switch (method) {
      case "start":
        if (this.session.phase !== "new") throw new Error("Claude session already started");
        void this.session.start().catch(() => {});
        return { requested: true, phase: this.session.phase, sdkVersion: CLAUDE_SDK_VERSION, cliVersion: CLAUDE_CLI_VERSION };
      case "send": return { messageID: this.session.send(params.text as string) };
      case "interrupt":
        if (!["running", "interrupting"].includes(this.session.phase)) throw new Error("No Claude input to interrupt");
        void this.session.interrupt().catch(() => {});
        return { requested: true, phase: this.session.phase };
      case "approve": return { accepted: this.approvals.decide(reference(params.reference), params.allow as boolean) };
      case "answer": return { accepted: this.approvals.answer(reference(params.reference), params.answers as Record<string, string>) };
      case "poll": {
        const take = () => {
          const first = this.approvalNext ? "approval" : "session", second = this.approvalNext ? "session" : "approval";
          this.approvalNext = !this.approvalNext;
          for (const source of [first, second]) {
            const event = source === "approval" ? this.approvals.takeEvent() : this.session.takeEvent();
            if (event) return { source, event };
          }
          return undefined;
        };
        if (params.maxEvents === undefined) return take() ?? { source: "none", phase: this.session.phase };
        const events = []; let bytes = 0;
        // The final event can be a complete 512 KiB SDK message. Stop before
        // taking another; 4 KiB prefix plus one event stays under 600,000 bytes
        // and the Portable decoder's node bound. No consumed event is discarded.
        while (events.length < Number(params.maxEvents) && bytes < 4096) {
          const item = take(); if (!item) break;
          events.push(item); bytes += Buffer.byteLength(JSON.stringify(item));
        }
        return { source: "batch", events, phase: this.session.phase };
      }
      case "stop": this.explicitStopValue = true; this.close(); return { phase: this.session.phase, pendingMessageID: this.session.pendingMessageID ?? null };
      default: throw new ClaudeStdioProtocolError("Unsupported Claude stdio method");
    }
  }
  close(): void { this.stoppedValue = true; this.session.close(); }
}

/** Bounded binary framing. Writes await stream callbacks; no unsolicited output
 * or unbounded output pump. Transport EOF is distinct from an explicit stop. */
export async function serveClaudeStdio(input: Readable, output: Writable, bridge: ClaudeStdioBridge): Promise<"stopped" | "eof"> {
  const pending = Buffer.alloc(FRAME_MAX);
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let used = 0;
  const onError = (error: Error) => { bridge.close(); input.destroy(error); };
  const onEnd = () => bridge.close();
  output.on("error", onError);
  input.once("end", onEnd);
  try {
    for await (const chunk of input) {
      if (!Buffer.isBuffer(chunk)) throw new ClaudeStdioProtocolError("Claude stdio requires binary input");
      let offset = 0;
      while (offset < chunk.length) {
        const newline = chunk.indexOf(10, offset), end = newline < 0 ? chunk.length : newline;
        const size = end - offset;
        if (used + size > FRAME_MAX) throw new ClaudeStdioProtocolError("Claude stdio frame limit");
        chunk.copy(pending, used, offset, end); used += size; offset = end + 1;
        if (newline < 0) break;
        const response = await bridge.handle(decoder.decode(pending.subarray(0, used))); used = 0;
        await new Promise<void>((resolve, reject) => output.write(response + "\n", error => error ? reject(error) : resolve()));
        if (bridge.stopped) return bridge.explicitStop ? "stopped" : "eof";
      }
    }
    if (used) throw new ClaudeStdioProtocolError("Incomplete Claude stdio frame at EOF");
    return "eof";
  } finally {
    bridge.close(); output.off("error", onError); input.off("end", onEnd);
  }
}
