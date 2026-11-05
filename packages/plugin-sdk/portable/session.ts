import { Codec, decode, encode, fail, snapshot, uint64 } from "./codec.js";
import { ERROR_CODES, ErrorCode, MESSAGE_MAX, REASON_MAX } from "./wire-contract.js";

export type Status = "ok" | "wait" | "invalid" | "denied" | "unsupported" | "closed";
export type Outcome<T> = { readonly kind: "success"; readonly value: T } |
  { readonly kind: "failed"; readonly error: { readonly code: ErrorCode; readonly message: string; readonly effect: "none" | "possible" } } |
  { readonly kind: "cancelled"; readonly effect: "none" | "possible" } |
  { readonly kind: "unknown"; readonly reason: string };
export interface NativeResult {
  readonly state: "queued" | "pending" | "succeeded" | "failed" | "cancelled" | "unknown";
  readonly confirmed: boolean; readonly outcome: string;
}
/** Trusted runtime adapter, bound to ONE native owner/lease. Never supplied by
 * a plugin or exported into plugin modules. Native always rechecks grants. */
export interface NativePort {
  request(method: string, json: string, timeoutMs: number): { readonly status: Status; readonly handle?: string };
  result(handle: string): NativeResult;
  cancel(handle: string): Status;
  forget(handle: string): Status;
  take(): { readonly ticket: string; readonly method: string; readonly json: string } | null;
  check(ticket: string): Status;
  respond(ticket: string, json: string): Status;
  revoke(): void;
}
export interface Method<I, O> { readonly name: string; readonly input: Codec<I>; readonly output: Codec<O>; }
export interface RequestContext { check(): void; readonly cancelled: boolean; }
export interface Call<T> { readonly outcome: Promise<Outcome<T>>; cancel(): void; }
type Pending = { poll(): boolean; cancel(): void; };
type Incoming = { cancelled: boolean; reply?: string; };
type Handler = (input: unknown, context: RequestContext) => Promise<string>;
function methodName(name: string): void { if (!/^[a-z][A-Za-z0-9_.-]{0,127}$/.test(name)) fail("METHOD"); }
function failure(code: ErrorCode): Outcome<never> { return { kind: "failed", error: { code, message: code, effect: "none" } }; }
function outcome<T>(json: string, codec: Codec<T>): Outcome<T> {
  const v = decode(json);
  if (!v || typeof v !== "object" || Array.isArray(v)) return fail("OUTCOME");
  const o = v as Readonly<Record<string, unknown>>, keys = Object.keys(o).sort().join(",");
  if (o.kind === "success" && keys === "kind,value") return { kind: "success", value: codec.parse(o.value) };
  if (o.kind === "unknown" && keys === "kind,reason" && typeof o.reason === "string" && [...o.reason].length >= 1 && [...o.reason].length <= REASON_MAX) return { kind: "unknown", reason: o.reason };
  if (o.kind === "cancelled" && keys === "effect,kind" && (o.effect === "none" || o.effect === "possible")) return { kind: "cancelled", effect: o.effect };
  if (o.kind === "failed" && keys === "error,kind" && o.error && typeof o.error === "object" && !Array.isArray(o.error)) {
    const e = o.error as Readonly<Record<string, unknown>>;
    if (Object.keys(e).sort().join(",") === "code,effect,message" && typeof e.code === "string" &&
        ERROR_CODES.some(code => code === e.code) && typeof e.message === "string" && [...e.message].length >= 1 &&
        [...e.message].length <= MESSAGE_MAX && (e.effect === "none" || e.effect === "possible")) {
      return { kind: "failed", error: { code: e.code as ErrorCode, message: e.message, effect: e.effect } };
    }
  }
  return fail("OUTCOME");
}
/** The native worker drives C Session IO, then step(); JS never polls sockets or
 * invents a timeout/receipt. Up to 64 outstanding calls and 64 taken handlers.
 * NativePort owns lease identity, deadlines, IDs, direction and capabilities. */
export class SessionChannel {
  #live = true;
  #stepping = false;
  readonly #pending = new Map<string, Pending>();
  readonly #incoming = new Map<string, Incoming>();
  readonly #handlers = new Map<string, Handler>();
  readonly #port: NativePort;
  constructor(port: NativePort) { this.#port = port; }
  request<I, O>(method: Method<I, O>, input: I, timeoutMs: number): Call<O> {
    methodName(method.name);
    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 3600000) fail("TIMEOUT");
    if (!this.#live) fail("CLOSED");
    if (this.#pending.size >= 64) fail("BUSY");
    const json = encode(method.input.parse(snapshot(input)));
    const output = method.output;
    const admission = this.#port.request(method.name, json, timeoutMs);
    if (admission.status !== "ok") {
      const codes = { wait: "BUSY", invalid: "INVALID_INPUT", denied: "DENIED", unsupported: "UNSUPPORTED", closed: "CANCELLED" } as const;
      const value = failure(codes[admission.status]);
      return { outcome: Promise.resolve(value), cancel() {} };
    }
    const handle = uint64(admission.handle, true);
    if (this.#pending.has(handle)) fail("HANDLE_REUSE");
    let settle: (value: Outcome<O>) => void = () => { fail("PROMISE"); };
    const promise = new Promise<Outcome<O>>(resolve => { settle = resolve; });
    let requested = false;
    const entry: Pending = {
      cancel: () => { if (!requested) { requested = true; this.#port.cancel(handle); } },
      poll: () => {
        const result = this.#port.result(handle);
        if (result.state === "queued" || result.state === "pending") return false;
        let value: Outcome<O>;
        try {
          value = outcome(result.outcome, output);
          if ((value.kind === "success" && (result.state !== "succeeded" || !result.confirmed)) ||
              (value.kind !== "success" && value.kind !== result.state)) fail("OUTCOME_STATE");
        }
        catch { value = { kind: "unknown", reason: "INVALID_OUTCOME" }; }
        if (this.#port.forget(handle) !== "ok") return false;
        settle(value); return true;
      },
    };
    this.#pending.set(handle, entry);
    return { outcome: promise, cancel: () => { if (this.#pending.has(handle)) entry.cancel(); } };
  }
  register<I, O>(method: Method<I, O>, handler: (input: I, context: RequestContext) => O | Promise<O>): void {
    methodName(method.name);
    if (!this.#live || this.#handlers.size >= 16 || this.#handlers.has(method.name)) fail("REGISTRY");
    const inputCodec = method.input, outputCodec = method.output;
    this.#handlers.set(method.name, async (input, context) => {
      let parsed: I;
      try { parsed = inputCodec.parse(input); } catch { return encode(failure("INVALID_INPUT")); }
      context.check();
      try {
        const value = await handler(parsed, context);
        context.check();
        return encode({ kind: "success", value: outputCodec.parse(snapshot(value)) });
      } catch { return encode({ kind: "unknown", reason: "HANDLER_INTERRUPTED" }); }
    });
  }
  /** Bounded native-to-JS admission. Promise jobs are drained separately by the
   * runtime with its own CPU/job budget; never invoke this on a UI/TTY thread. */
  step(): void {
    if (this.#stepping) fail("REENTRY");
    this.#stepping = true;
    try {
      for (const [handle, item] of this.#pending) if (item.poll()) this.#pending.delete(handle);
      for (const [ticket, item] of this.#incoming) {
        if (!this.#live || this.#port.check(ticket) !== "ok") item.cancelled = true;
        if (item.reply !== undefined && this.#port.respond(ticket, item.reply) === "ok") this.#incoming.delete(ticket);
      }
      if (!this.#live) return;
      for (let n = 0; n < 8 && this.#incoming.size < 64; n++) {
        const request = this.#port.take(); if (!request) break;
        const ticket = uint64(request.ticket, true);
        if (this.#incoming.has(ticket)) fail("TICKET_REUSE");
        const item: Incoming = { cancelled: false }; this.#incoming.set(ticket, item);
        const context: RequestContext = Object.freeze({
          get cancelled() { return item.cancelled; },
          check: () => { if (!this.#live || item.cancelled || this.#port.check(ticket) !== "ok") { item.cancelled = true; fail("CANCELLED"); } },
        });
        const handler = this.#handlers.get(request.method);
        const run = async (): Promise<string> => {
          context.check();
          if (!handler) return encode(failure("UNSUPPORTED"));
          return handler(decode(request.json), context);
        };
        void run().then(reply => {
          // Cancellation is a fact, not proof that an async handler had no effect.
          item.reply = !this.#live || item.cancelled ? encode({ kind: "unknown", reason: "CANCELLED" }) : reply;
        }, () => { item.reply = encode({ kind: "unknown", reason: "HANDLER_INTERRUPTED" }); });
      }
    } finally { this.#stepping = false; }
  }
  /** Revocation is immediate; never await a plugin's cooperative dispose.
   * Keep driving step/native cleanup to release terminal observations. */
  dispose(): void {
    if (!this.#live) return; this.#live = false; this.#port.revoke();
    for (const item of this.#incoming.values()) item.cancelled = true;
    this.#incoming.clear();
    for (const item of this.#pending.values()) item.cancel();
    this.#handlers.clear();
  }
  get retained(): number { return this.#pending.size + this.#incoming.size; }
}
