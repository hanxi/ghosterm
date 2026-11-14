/** Trusted Host adapter; all OS children, grants and budgets belong to Core. */
import { ProcessService, ProcessChild, ProcessSpawnOptions, ProcessRead, ProcessWrite, ProcessExit } from "../portable/process.js";
import { fail, uint64, compareUint64, utf8Bytes } from "../portable/codec.js";
import { NodeNativePort } from "./port.js";
type Value = Readonly<Record<string, unknown>>;
type Entry = { ref: string; cancellation: ProcessSpawnOptions["cancellation"]; cancelled: boolean;
  releasing?: Promise<void>; resolve?: () => void; reject?: (error: unknown) => void };
function record(v: unknown): Value {
  if (!v || typeof v !== "object" || Array.isArray(v)) return fail("CHILD_PROCESS_VALUE");
  return v as Value;
}
function keys(v: Value, names: readonly string[]): void {
  if (Object.keys(v).sort().join(",") !== [...names].sort().join(",")) fail("CHILD_PROCESS_FIELDS");
}
function integer(v: unknown, low: number, high: number): number {
  if (typeof v !== "number" || !Number.isSafeInteger(v) || v < low || v > high) return fail("CHILD_PROCESS_INTEGER");
  return v;
}
function text(v: unknown, max: number): string {
  if (typeof v !== "string" || v.includes("\0") || utf8Bytes(v) > max) return fail("CHILD_PROCESS_TEXT");
  return v;
}
export class NodeProcessService implements ProcessService {
  readonly #port: NodeNativePort;
  readonly #children = new Map<string, Entry>();
  #reference = "0";
  #disposed = false;
  constructor(port: NodeNativePort) { this.#port = port; }
  #call(action: string, fields: Value, expected: readonly string[] = []): Value {
    return this.#port.processCall(action, fields, value => {
      const v = record(value);
      if (!["ok", "wait", "invalid", "denied", "unsupported", "error", "closed"].includes(v.status as string)) fail("CHILD_PROCESS_STATUS");
      keys(v, ["status", ...(v.status === "ok" ? expected : [])]);
      return v;
    });
  }
  available(): boolean {
    return this.#port.processCall("available", {}, value => {
      const v = record(value); keys(v, ["status", "available"]);
      if (v.status !== "ok" || typeof v.available !== "boolean") return fail("CHILD_PROCESS_AVAILABLE");
      return v.available;
    });
  }
  #live(e: Entry): void {
    if (this.#disposed || !this.#children.has(e.ref)) fail("PROCESS_CLOSED");
    if (!e.cancelled && e.cancellation.cancelled) {
      e.cancelled = true;
      this.#ok("signal", { ref: e.ref, signal: "terminate" });
    }
  }
  #ok(action: string, fields: Value): void {
    const v = this.#call(action, fields);
    if (v.status !== "ok") fail("PROCESS_" + String(v.status));
  }
  async spawn(o: ProcessSpawnOptions): Promise<ProcessChild> {
    if (this.#disposed || o.cancellation.cancelled) return fail("PROCESS_CANCELLED");
    if (this.#children.size >= 8) return fail("PROCESS_LIMIT");
    if (!/^[A-Za-z0-9_.-]{1,128}$/.test(o.program) || !/^[A-Za-z0-9_.-]{1,64}$/.test(o.cwd.id) ||
        o.envPolicy !== "selected-user-environment" || !Array.isArray(o.argv) || !o.argv.length || o.argv.length > 64) return fail("PROCESS_OPTIONS");
    let bytes = 0;
    for (const a of o.argv) { text(a, 4096); bytes += utf8Bytes(a) + 1; }
    if (!o.argv[0] || bytes > 16384) return fail("PROCESS_ARGUMENTS");
    let version = "", ref = "";
    const v = this.#port.processCall("spawn", { program: o.program, root: o.cwd.id, argv: [...o.argv], envPolicy: o.envPolicy }, value => {
      const r = record(value);
      if (!["ok", "wait", "invalid", "denied", "unsupported", "error", "closed"].includes(r.status as string)) return fail("CHILD_PROCESS_STATUS");
      keys(r, r.status === "ok" ? ["status", "ref", "version"] : ["status"]);
      if (r.status === "ok") {
        ref = uint64(r.ref, true); version = text(r.version, 128);
        if (!version || compareUint64(ref, this.#reference) <= 0 || this.#children.has(ref)) fail("CHILD_PROCESS_REFERENCE");
      }
      return r;
    });
    if (v.status !== "ok") return fail("PROCESS_" + String(v.status));
    const e: Entry = { ref, cancellation: o.cancellation, cancelled: false };
    this.#reference = ref; this.#children.set(ref, e);
    return Object.freeze({ verifiedVersion: version,
      readStdout: (max: number) => this.#read(e, false, max), readStderr: (max: number) => this.#read(e, true, max),
      writeStdin: (bytes: Uint8Array) => this.#write(e, bytes),
      closeStdin: () => { this.#live(e); this.#ok("closeStdin", { ref }); },
      signal: (signal: "terminate" | "kill") => {
        this.#live(e); if (signal !== "terminate" && signal !== "kill") fail("PROCESS_SIGNAL");
        this.#ok("signal", { ref, signal });
      },
      poll: () => { this.#live(e); return this.#poll(e); }, release: () => this.#release(e),
    });
  }
  #io(action: string, fields: Value, maximum: number, read: boolean): Value {
    return this.#port.processCall(action, fields, value => {
      const v = record(value); keys(v, ["status", "state", "count", "error", ...(read ? ["hex"] : [])]);
      if (v.status !== "ok" || !["data", "wait", "eof", "closed", "error"].includes(v.state as string) ||
          (!read && v.state === "eof")) fail("CHILD_PROCESS_IO");
      const count = integer(v.count, 0, Math.min(maximum, 3072)), error = integer(v.error, 0, 9);
      if ((v.state !== "data" && count !== 0) || (v.state === "data" && !count) ||
          (v.state !== "error" && v.state !== "closed" && error !== 0)) fail("CHILD_PROCESS_IO");
      if (read && (typeof v.hex !== "string" || !/^(?:[0-9a-f]{2})*$/.test(v.hex) || v.hex.length !== count * 2)) fail("CHILD_PROCESS_BYTES");
      return v;
    });
  }
  #read(e: Entry, stderr: boolean, maxBytes: number): ProcessRead {
    this.#live(e); integer(maxBytes, 1, 16384);
    const r = this.#io("read", { ref: e.ref, stderr, maxBytes }, maxBytes, true);
    if (r.state === "data") {
      const hex = r.hex as string, bytes = new Uint8Array(hex.length / 2);
      for (let i = 0; i < bytes.length; i++) bytes[i] = Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
      return { state: "data", bytes };
    }
    return r.state === "error" ? { state: "error", error: Error("PROCESS_IO_" + String(r.error)) } : { state: r.state as "wait" | "eof" | "closed" };
  }
  #write(e: Entry, bytes: Uint8Array): ProcessWrite {
    this.#live(e);
    if (e.cancelled) return { state: "closed" };
    if (!(bytes instanceof Uint8Array) || !bytes.length || bytes.length > 16384) return fail("PROCESS_WRITE");
    const part = bytes.subarray(0, 3072);
    let hex = ""; for (const byte of part) hex += byte.toString(16).padStart(2, "0");
    const r = this.#io("write", { ref: e.ref, hex }, part.length, false);
    return r.state === "data" ? { state: "data", count: r.count as number } : r.state === "error" ?
      { state: "error", error: Error("PROCESS_IO_" + String(r.error)) } : { state: r.state as "wait" | "closed" };
  }
  #poll(e: Entry): ProcessExit {
    return this.#port.processCall("poll", { ref: e.ref }, value => {
      const v = record(value); keys(v, ["status", "exited", "code", "signal", "reason"]);
      if (v.status !== "ok" || typeof v.exited !== "boolean" || !["none", "deadline", "output-limit"].includes(v.reason as string)) return fail("CHILD_PROCESS_EXIT");
      const code = integer(v.code, 0, 255), signal = integer(v.signal, 0, 255);
      if ((!v.exited && (code !== 0 || signal !== 0)) || (v.exited && signal > 0 && code !== 0)) fail("CHILD_PROCESS_EXIT");
      return v.exited ? { state: "exited", code: signal ? null : code, signal: signal || null,
        ...(v.reason === "none" ? {} : { terminationReason: v.reason as "deadline" | "output-limit" }) } : { state: "running" };
    });
  }
  #release(e: Entry): Promise<void> {
    if (e.releasing) return e.releasing;
    this.#live(e);
    e.releasing = new Promise<void>((resolve, reject) => { e.resolve = resolve; e.reject = reject; });
    this.#retire(e); return e.releasing;
  }
  #retire(e: Entry): void {
    try {
      const r = this.#call("release", { ref: e.ref });
      if (r.status === "wait") return;
      if (r.status !== "ok") fail("PROCESS_RELEASE");
      this.#children.delete(e.ref); e.resolve?.();
    } catch (error) { e.reject?.(error); throw error; }
  }
  step(): void {
    for (const e of this.#children.values()) { this.#live(e); if (e.releasing) this.#retire(e); }
  }
  dispose(): void {
    if (this.#disposed) return;
    this.#disposed = true;
    for (const e of this.#children.values()) e.reject?.(Error("PROCESS_CLOSED"));
    this.#children.clear();
  }
}
