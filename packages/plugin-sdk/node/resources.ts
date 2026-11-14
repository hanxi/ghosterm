/** Host-only Node adapter. Native Resources own effects, scopes and receipts. */
import { NativeResourcePort, NativeResourceDescriptor, NativePublication, ResourceStatus } from "../portable/resources.js";
import { fail, uint64, compareUint64, utf8Bytes } from "../portable/codec.js";
import { NodeNativePort } from "./port.js";
type Value = Record<string, unknown>;
const statuses = ["ok", "wait", "invalid", "denied", "unsupported", "io", "limit", "changed", "cancelled", "busy", "unknown", "closed", "not-found", "expired", "protocol"];
function object(v: unknown): Value {
  if (!v || typeof v !== "object" || Array.isArray(v)) return fail("CHILD_RESOURCE_VALUE");
  return v as Value;
}
function exact(v: Value, keys: readonly string[]): void {
  if (Object.keys(v).sort().join(",") !== [...keys].sort().join(",")) fail("CHILD_RESOURCE_FIELDS");
}
function text(v: unknown, max: number): string {
  if (typeof v !== "string" || v.includes("\0") || utf8Bytes(v) > max) return fail("CHILD_RESOURCE_TEXT");
  return v;
}
function boolean(v: unknown): boolean { if (typeof v !== "boolean") return fail("CHILD_RESOURCE_BOOL"); return v; }
function descriptor(value: unknown): NativeResourceDescriptor {
  const d = object(value); exact(d, ["id", "generation", "sha256", "displayName", "mediaType", "sizeKnown", "bytes"]);
  if (typeof d.id !== "string" || !/^([0-9a-f]{32})?$/.test(d.id) || typeof d.sha256 !== "string" || !/^([0-9a-f]{64})?$/.test(d.sha256)) return fail("CHILD_RESOURCE_ID");
  return { id: d.id, sha256: d.sha256, generation: uint64(d.generation), displayName: text(d.displayName, 1024), mediaType: text(d.mediaType, 128), sizeKnown: boolean(d.sizeKnown), bytes: uint64(d.bytes) };
}
function path64(path: string): string {
  text(path, 4096);
  const bytes: number[] = [];
  for (const c of path) {
    const n = c.codePointAt(0)!;
    if (n < 128) bytes.push(n);
    else if (n < 2048) bytes.push(192 | n >> 6, 128 | n & 63);
    else if (n < 65536) bytes.push(224 | n >> 12, 128 | n >> 6 & 63, 128 | n & 63);
    else bytes.push(240 | n >> 18, 128 | n >> 12 & 63, 128 | n >> 6 & 63, 128 | n & 63);
  }
  const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  let result = "";
  for (let i = 0; i < bytes.length; i += 3) {
    const n = bytes[i]! << 16 | (bytes[i + 1] ?? 0) << 8 | (bytes[i + 2] ?? 0);
    result += alphabet[n >>> 18]! + alphabet[n >>> 12 & 63]! + (i + 1 < bytes.length ? alphabet[n >>> 6 & 63]! : "=") + (i + 2 < bytes.length ? alphabet[n & 63]! : "=");
  }
  return result;
}
function budget(limit: string, timeoutMs: number): void {
  uint64(limit);
  if (compareUint64(limit, "16777216") > 0 || !Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 3600000) fail("CHILD_RESOURCE_BUDGET");
}
export class NodeResourcePort implements NativeResourcePort {
  readonly #port: NodeNativePort;
  readonly #publications = new Set<string>();
  readonly #snapshots = new Set<string>();
  #reference = "0";
  constructor(port: NodeNativePort) { this.#port = port; }
  #call<T>(action: string, fields: Value, parse: (value: Value, status: ResourceStatus) => T): T {
    return this.#port.resourceCall(action, fields, value => {
      const v = object(value);
      if (typeof v.status !== "string" || !statuses.includes(v.status)) return fail("CHILD_RESOURCE_STATUS");
      return parse(v, v.status as ResourceStatus);
    });
  }
  #new(value: unknown, table: Set<string>): string {
    const ref = uint64(value, true);
    if (compareUint64(ref, this.#reference) <= 0 || table.size >= (table === this.#publications ? 16 : 64)) return fail("CHILD_RESOURCE_REUSE");
    this.#reference = ref; table.add(ref); return ref;
  }
  resourceWorkspaces(): readonly string[] | null {
    return this.#call("workspaces", {}, (v,s) => {
      exact(v, s === "ok" ? ["status", "roots"] : ["status"]);
      if (s === "unsupported") return null;
      if (s !== "ok" || !Array.isArray(v.roots) || v.roots.length > 8 || v.roots.some(r => typeof r !== "string" || !/^[a-z][a-z0-9_.-]{0,63}$/.test(r)) || new Set(v.roots).size !== v.roots.length) return fail("CHILD_RESOURCE_ROOTS");
      return Object.freeze(v.roots as string[]);
    });
  }
  resourcePublish(root: string, path: string, mime: string, limit: string, timeoutMs: number): { status: ResourceStatus; handle?: string } {
    budget(limit, timeoutMs); text(root, 64); text(mime, 128);
    return this.#call("publish", { root, path64: path64(path), mime, limit, timeoutMs }, (v,s) => {
      exact(v, s === "ok" ? ["status", "handle"] : ["status"]);
      return s === "ok" ? { status: s, handle: this.#new(v.handle, this.#publications) } : { status: s };
    });
  }
  resourcePublication(handle: string): NativePublication {
    if (!this.#publications.has(uint64(handle, true))) return fail("CHILD_RESOURCE_HANDLE");
    return this.#call("publication", { ref: handle }, (v,s) => {
      exact(v, s === "ok" ? ["status", "publication"] : ["status"]);
      if (s !== "ok") return fail("CHILD_RESOURCE_PUBLICATION");
      const p = object(v.publication);
      exact(p, ["phase", "error", "expired", "clockError", "hasTransfer", "confirmed", "transferState", "readBytes", "accepted", "descriptor"]);
      if (typeof p.phase !== "string" || !["hashing", "ready", "sending", "receipt", "cancelling", "succeeded", "failed", "stopped", "uncertain"].includes(p.phase) || typeof p.error !== "string" || !statuses.includes(p.error) || typeof p.transferState !== "string" || !["queued", "sending", "pending", "succeeded", "failed", "cancelled", "unknown"].includes(p.transferState)) return fail("CHILD_RESOURCE_PUBLICATION");
      return { phase: p.phase as NativePublication["phase"], error: p.error as ResourceStatus, transferState: p.transferState as NativePublication["transferState"], expired: boolean(p.expired), clockError: boolean(p.clockError), hasTransfer: boolean(p.hasTransfer), confirmed: boolean(p.confirmed), readBytes: uint64(p.readBytes), accepted: uint64(p.accepted), descriptor: descriptor(p.descriptor) };
    });
  }
  #finish(action: string, handle: string, table: Set<string>, retire: boolean): ResourceStatus {
    if (!table.has(uint64(handle, true))) return "invalid";
    return this.#call(action, { ref: handle }, (v,s) => { exact(v,["status"]); if (s === "ok" && retire) table.delete(handle); return s; });
  }
  resourceCancel(handle: string): ResourceStatus { return this.#finish("cancel", handle, this.#publications, false); }
  resourceForget(handle: string): ResourceStatus { return this.#finish("forget", handle, this.#publications, true); }
  resourceRelease(handle: string): ResourceStatus { return this.#finish("release", handle, this.#snapshots, true); }
  resourceTake(): { status: ResourceStatus; handle?: string; descriptor?: NativeResourceDescriptor } {
    return this.#call("take", {}, (v,s) => {
      exact(v, s === "ok" ? ["status", "handle", "descriptor"] : ["status"]);
      if (s !== "ok") return { status: s };
      const d = descriptor(v.descriptor); uint64(d.generation, true);
      if (!d.id || !d.sha256 || !d.sizeKnown || compareUint64(d.bytes,"16777216") > 0) return fail("CHILD_RESOURCE_DESCRIPTOR");
      return { status: s, handle: this.#new(v.handle, this.#snapshots), descriptor: d };
    });
  }
  resourceOpen(handle: string, limit: string, timeoutMs: number): ResourceStatus {
    budget(limit, timeoutMs);
    if (!this.#snapshots.has(uint64(handle, true))) return "invalid";
    return this.#call("open", { ref: handle, limit, timeoutMs }, (v,s) => { exact(v,["status"]); return s; });
  }
  resourceRead(handle: string, at: string, maxBytes: number): { status: ResourceStatus; bytes: Uint8Array } {
    uint64(at);
    if (!Number.isInteger(maxBytes) || maxBytes < 1 || maxBytes > 16384) return fail("CHILD_RESOURCE_READ");
    if (!this.#snapshots.has(uint64(handle, true))) return { status: "invalid", bytes: new Uint8Array() };
    return this.#call("read", { ref: handle, at, maxBytes }, (v,s) => {
      exact(v,["status", "hex"]);
      if (typeof v.hex !== "string" || !/^([0-9a-f]{2})*$/.test(v.hex) || v.hex.length > Math.min(3072,maxBytes)*2 || (s !== "ok" && v.hex.length)) return fail("CHILD_RESOURCE_BYTES");
      const bytes = new Uint8Array(v.hex.length / 2);
      for (let i = 0; i < bytes.length; i++) bytes[i] = parseInt(v.hex.slice(i*2,i*2+2),16);
      return { status: s, bytes };
    });
  }
  get retained(): number { return this.#publications.size + this.#snapshots.size; }
}
