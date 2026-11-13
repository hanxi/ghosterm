/** Trusted Node host binding for the private child RPC candidate. Not exported
 * by the portable SDK or supplied to plugin modules. No lease is present here. */
import { NativePort, NativeResult, Status } from "../portable/session.js";
import { CapabilityAccess } from "../portable/index.js";
import { compareUint64, decode, encode, fail, snapshot, uint64, utf8Bytes } from "../portable/codec.js";
import { parseMessage } from "./message.js";

export interface SyncPipe {
  /** Exactly one bounded exchange. Failure closes the link; never replay. */
  exchange(json: string): string;
  abort(): void;
}
type RecordValue = Readonly<Record<string, unknown>>;
const statuses: readonly Status[] = ["ok", "wait", "invalid", "denied", "unsupported", "closed"];
const accesses: readonly CapabilityAccess[] = ["unknown", "allowed", "denied", "revoked", "unsupported"];
function record(value: unknown): RecordValue {
  if (!value || typeof value !== "object" || Array.isArray(value)) return fail("CHILD_REPLY");
  return value as RecordValue;
}
function keys(value: RecordValue, expected: readonly string[]): void {
  if (Object.keys(value).sort().join(",") !== [...expected].sort().join(",")) fail("CHILD_REPLY_FIELDS");
}
function name(value: string): void {
  if (!/^[a-z][A-Za-z0-9_.-]{0,127}$/.test(value)) fail("CHILD_METHOD");
}
function status(value: unknown): Status {
  if (!statuses.some(s => s === value)) return fail("CHILD_STATUS");
  return value as Status;
}
/** Synchronous exchanges run only on the Node plugin worker. Pipe IO lives on
 * a separate worker with finite waits. No cached grants, JS deadlines, replay,
 * OS sandbox or implicit resources/process service. Native Core owns outcomes. */
export class NodeNativePort implements NativePort {
  readonly #pipe: SyncPipe;
  readonly #handles = new Set<string>();
  readonly #tickets = new Set<string>();
  #sequence = 0n;
  #reference = "0";
  #broken = false;
  #revoked = false;
  #calling = false;
  constructor(pipe: SyncPipe) { this.#pipe = pipe; }
  #break(): void { this.#broken = true; this.#pipe.abort(); }
  #call(op: string, fields: RecordValue, extra: readonly string[] = []): RecordValue {
    if (this.#broken || this.#sequence === 18446744073709551615n) return fail("CHILD_LINK_CLOSED");
    if (this.#calling) { this.#break(); return fail("CHILD_REENTRY"); }
    this.#calling = true;
    try {
      const id = String(++this.#sequence);
      const request = JSON.stringify(snapshot({ v: 1, id, op, ...fields }));
      if (utf8Bytes(request) > 8192) fail("CHILD_MESSAGE_LIMIT");
      const text = this.#pipe.exchange(request);
      if (this.#broken) fail("CHILD_LINK_CLOSED");
      if (utf8Bytes(text) > 8192) fail("CHILD_MESSAGE_LIMIT");
      const reply = record(parseMessage(text));
      if (reply.v !== 1 || reply.id !== id) fail("CHILD_REPLY_IDENTITY");
      const s = status(reply.status);
      const interrupted = op === "respond" && reply.interrupted !== undefined;
      if (interrupted && reply.interrupted !== true) fail("CHILD_REPLY_FIELDS");
      keys(reply, ["v", "id", "status", ...(s === "ok" ? extra : []), ...(interrupted ? ["interrupted"] : [])]);
      return reply;
    } catch (error) { this.#break(); throw error; }
    finally { this.#calling = false; }
  }
  #newReference(value: unknown, table: Set<string>): string {
    const ref = this.#value(() => uint64(value, true));
    if (compareUint64(ref, this.#reference) <= 0 || table.size >= 64) {
      this.#break(); return fail("CHILD_REFERENCE_REUSE");
    }
    this.#reference = ref; table.add(ref); return ref;
  }
  #value<T>(parse: () => T): T {
    try { return parse(); } catch (error) { this.#break(); throw error; }
  }
  request(method: string, json: string, timeoutMs: number): { readonly status: Status; readonly handle?: string } {
    name(method);
    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 3600000) fail("TIMEOUT");
    const input = decode(json);
    if (this.#revoked) return { status: "closed" };
    if (this.#handles.size >= 64) return { status: "wait" };
    const reply = this.#call("request", { method, input, timeoutMs }, ["handle"]);
    const s = status(reply.status);
    return s === "ok" ? { status: s, handle: this.#newReference(reply.handle, this.#handles) } : { status: s };
  }
  result(handle: string): NativeResult {
    uint64(handle, true);
    if (!this.#handles.has(handle)) return fail("CHILD_HANDLE");
    const r = this.#call("result", { ref: handle }, ["state", "confirmed", "cancelRequested", "deadlineElapsed", "outcome"]);
    if (r.status !== "ok" || typeof r.confirmed !== "boolean" || typeof r.cancelRequested !== "boolean" ||
        typeof r.deadlineElapsed !== "boolean" || typeof r.state !== "string" ||
        !["queued", "pending", "succeeded", "failed", "cancelled", "unknown"].includes(r.state)) {
      this.#break(); return fail("CHILD_RESULT");
    }
    const state = r.state as NativeResult["state"];
    const pending = state === "queued" || state === "pending";
    if ((pending && (r.confirmed || r.outcome !== null)) || (!pending && r.outcome === null) ||
        (state === "succeeded" && !r.confirmed)) {
      this.#break(); return fail("CHILD_RESULT");
    }
    return { state, confirmed: r.confirmed, outcome: pending ? "" : this.#value(() => encode(r.outcome)) };
  }
  cancel(handle: string): Status {
    uint64(handle, true);
    if (!this.#handles.has(handle)) return "invalid";
    return status(this.#call("cancel", { ref: handle }).status);
  }
  forget(handle: string): Status {
    uint64(handle, true);
    if (!this.#handles.has(handle)) return "invalid";
    const s = status(this.#call("forget", { ref: handle }).status);
    if (s === "ok") this.#handles.delete(handle);
    return s;
  }
  take(methods: readonly string[]): { readonly ticket: string; readonly method: string; readonly json: string } | null {
    if (methods.length > 16 || new Set(methods).size !== methods.length) fail("CHILD_METHODS");
    for (const method of methods) name(method);
    if (this.#revoked || !methods.length || this.#tickets.size >= 64) return null;
    const reply = this.#call("take", { methods }, ["ticket", "method", "input"]);
    if (reply.status === "wait" || reply.status === "closed" || reply.status === "denied") return null;
    if (reply.status !== "ok" || typeof reply.method !== "string" || !methods.includes(reply.method)) {
      this.#break(); return fail("CHILD_TAKE");
    }
    return { ticket: this.#newReference(reply.ticket, this.#tickets), method: reply.method, json: this.#value(() => encode(reply.input)) };
  }
  check(ticket: string): Status {
    uint64(ticket, true);
    if (!this.#tickets.has(ticket)) return "invalid";
    return status(this.#call("check", { ref: ticket }).status);
  }
  respond(ticket: string, json: string): Status {
    uint64(ticket, true);
    if (!this.#tickets.has(ticket)) return "invalid";
    const outcome = decode(json);
    // Native may replace a stale answer with unknown; this ACK then retires the
    // local ticket without claiming a successful business effect.
    const reply = this.#call("respond", { ref: ticket, outcome });
    const s = status(reply.status);
    if (s === "ok") this.#tickets.delete(ticket);
    return s;
  }
  access(capability: string): CapabilityAccess {
    name(capability);
    const reply = this.#call("access", { capability }, ["access"]);
    if (reply.status !== "ok" || !accesses.some(a => a === reply.access)) {
      this.#break(); return fail("CHILD_ACCESS");
    }
    return reply.access as CapabilityAccess;
  }
  revoke(): void {
    if (this.#revoked) return;
    this.#revoked = true;
    if (this.#broken) return;
    if (this.#call("revoke", {}).status !== "ok") { this.#break(); fail("CHILD_REVOKE"); }
  }
  get broken(): boolean { return this.#broken; }
  get retained(): number { return this.#handles.size + this.#tickets.size; }
}
