import { compareUint64, fail, uint64, utf8Bytes } from "./codec.js";
import { Call, Outcome } from "./session.js";
import { ErrorCode } from "./wire-contract.js";

export type ResourceStatus = "ok" | "wait" | "invalid" | "denied" | "unsupported" | "io" | "limit" |
  "changed" | "cancelled" | "busy" | "unknown" | "closed" | "not-found" | "expired" | "protocol";
export interface NativeResourceDescriptor {
  readonly id: string; readonly generation: string; readonly sha256: string;
  readonly displayName: string; readonly mediaType: string; readonly sizeKnown: boolean; readonly bytes: string;
}
export interface NativePublication {
  readonly phase: "hashing" | "ready" | "sending" | "receipt" | "cancelling" | "succeeded" | "failed" | "stopped" | "uncertain";
  readonly error: ResourceStatus; readonly expired: boolean; readonly clockError: boolean;
  readonly hasTransfer: boolean; readonly confirmed: boolean;
  readonly transferState: "queued" | "sending" | "pending" | "succeeded" | "failed" | "cancelled" | "unknown";
  readonly descriptor: NativeResourceDescriptor; readonly readBytes: string; readonly accepted: string;
}
/** Host bootstrap only. Native owner retains all source/snapshot handles; the
 * plugin receives neither this port nor its lease/fd/native snapshot tokens. */
export interface NativeResourcePort {
  resourceWorkspaces(): readonly string[] | null;
  resourcePublish(root: string, path: string, mime: string, limit: string, timeoutMs: number): { readonly status: ResourceStatus; readonly handle?: string };
  resourcePublication(handle: string): NativePublication;
  resourceCancel(handle: string): ResourceStatus;
  resourceForget(handle: string): ResourceStatus;
  resourceTake(): { readonly status: ResourceStatus; readonly handle?: string; readonly descriptor?: NativeResourceDescriptor };
  resourceOpen(handle: string, limit: string, timeoutMs: number): ResourceStatus;
  resourceRead(handle: string, at: string, maxBytes: number): { readonly status: ResourceStatus; readonly bytes: Uint8Array };
  resourceRelease(handle: string): ResourceStatus;
}
export interface ResourceMetadata {
  readonly id: string; readonly generation: string; readonly sha256: string;
  readonly displayName: string; readonly mediaType: string;
  readonly size: { readonly kind: "known"; readonly bytes: string } | { readonly kind: "unknown" };
}
declare const localResource: unique symbol;
/** Opaque local owning reference. Copying/serializing metadata cannot create a
 * readable native reference; only this broker's take() can mint one. */
export interface ResourceRef extends ResourceMetadata { readonly [localResource]: true; }
export interface ResourceBudget { readonly byteLimit: string; readonly timeoutMs: number; }
export interface ScopedResourceInput { readonly root: string; readonly relativePath: string; readonly mediaType: string; }
export interface ReadChunk { readonly bytes: Uint8Array; readonly done: boolean; }
export interface BoundedReadStream { read(): Promise<Outcome<ReadChunk>>; }
type Publication = { poll(): boolean; cancel(): void; };
type Read = { settle(value: Outcome<ReadChunk>): void; };
type Local = { readonly handle: string; at: bigint; opening: boolean; done: boolean; pending?: Read; failure?: Outcome<ReadChunk>; };

function failure<T>(code: ErrorCode, effect: "none" | "possible" = "none"): Outcome<T> {
  return { kind: "failed", error: { code, message: code, effect } };
}
function statusFailure<T>(status: ResourceStatus, effect: "none" | "possible" = "none"): Outcome<T> {
  if (status === "unknown") return { kind: "unknown", reason: "RESOURCE_RESULT_UNKNOWN" };
  if (status === "cancelled" || status === "closed") return { kind: "cancelled", effect };
  const codes: Partial<Record<ResourceStatus, ErrorCode>> = { invalid: "INVALID_INPUT", denied: "DENIED", unsupported: "UNSUPPORTED", io: "IO", limit: "RESOURCE_LIMIT", changed: "INTEGRITY", busy: "BUSY", "not-found": "IO", expired: "EXPIRED", protocol: "PROTOCOL" };
  return failure(codes[status] ?? "PROTOCOL", effect);
}
function budget(value: ResourceBudget): void {
  uint64(value.byteLimit);
  if (compareUint64(value.byteLimit, "16777216") > 0 || !Number.isInteger(value.timeoutMs) || value.timeoutMs < 1 || value.timeoutMs > 3600000) fail("RESOURCE_BUDGET");
}
function metadata(v: NativeResourceDescriptor): ResourceMetadata {
  if (!/^[a-f0-9]{32}$/.test(v.id) || !/^[a-f0-9]{64}$/.test(v.sha256)) fail("RESOURCE_METADATA");
  uint64(v.generation, true); uint64(v.bytes);
  if (utf8Bytes(v.displayName) > 1024 || utf8Bytes(v.mediaType) > 128 || compareUint64(v.bytes, "16777216") > 0) fail("RESOURCE_METADATA");
  return Object.freeze({ id: v.id, generation: v.generation, sha256: v.sha256, displayName: v.displayName, mediaType: v.mediaType,
    size: v.sizeKnown ? Object.freeze({ kind: "known" as const, bytes: v.bytes }) : Object.freeze({ kind: "unknown" as const }) });
}
/** Native Session/Source/Store own permission, deadlines, credit and receipts.
 * step() only observes those facts, at most 8 read completions per pump. */
export class ResourceBroker {
  readonly #port: NativeResourcePort;
  readonly workspaces: readonly string[];
  readonly #publications = new Map<string, Publication>();
  readonly #refs = new Map<ResourceRef, Local>();
  #live = true;
  #stepping = false;
  constructor(port: NativeResourcePort) {
    const roots = port.resourceWorkspaces(); if (!roots) fail("UNSUPPORTED");
    if (roots.length > 8 || roots.some(r => !/^[a-z][a-z0-9_.-]{0,63}$/.test(r)) || new Set(roots).size !== roots.length) fail("RESOURCE_ROOTS");
    this.workspaces = Object.freeze([...roots]); this.#port = port;
  }
  publish(input: ScopedResourceInput, limits: ResourceBudget): Call<ResourceMetadata> {
    budget(limits);
    if (!this.#live) fail("CLOSED");
    if (this.#publications.size >= 16) fail("RESOURCE_LIMIT");
    if (!this.workspaces.includes(input.root)) return { outcome: Promise.resolve(failure("DENIED")), cancel() {} };
    if (!input.relativePath || input.relativePath.includes("\0") || utf8Bytes(input.relativePath) > 4096 || !input.mediaType || input.mediaType.includes("\0") || utf8Bytes(input.mediaType) > 128) fail("RESOURCE_INPUT");
    const admitted = this.#port.resourcePublish(input.root, input.relativePath, input.mediaType, limits.byteLimit, limits.timeoutMs);
    if (admitted.status !== "ok") return { outcome: Promise.resolve(statusFailure(admitted.status)), cancel() {} };
    const handle = uint64(admitted.handle, true); if (this.#publications.has(handle)) fail("HANDLE_REUSE");
    let settle: (value: Outcome<ResourceMetadata>) => void = () => { fail("PROMISE"); };
    const promise = new Promise<Outcome<ResourceMetadata>>(resolve => { settle = resolve; });
    let cancelled = false;
    const item: Publication = {
      cancel: () => { if (!cancelled) { cancelled = true; this.#port.resourceCancel(handle); } },
      poll: () => {
        const result = this.#port.resourcePublication(handle);
        if (["hashing", "ready", "sending", "receipt", "cancelling"].includes(result.phase)) return false;
        let value: Outcome<ResourceMetadata>;
        if (result.phase === "uncertain" || result.transferState === "unknown" || result.error === "unknown") value = { kind: "unknown", reason: "RESOURCE_RESULT_UNKNOWN" };
        else if (result.phase === "succeeded") {
          try {
            if (!result.confirmed || result.transferState !== "succeeded") fail("RESOURCE_RECEIPT");
            value = { kind: "success", value: metadata(result.descriptor) };
          } catch { value = { kind: "unknown", reason: "INVALID_RESOURCE_RECEIPT" }; }
        } else if (result.clockError) value = failure("IO", result.hasTransfer ? "possible" : "none");
        else if (result.expired) value = failure("EXPIRED", result.hasTransfer ? "possible" : "none");
        else if (result.phase === "stopped" || result.error === "cancelled") value = { kind: "cancelled", effect: result.hasTransfer ? "possible" : "none" };
        else value = statusFailure(result.error, result.hasTransfer ? "possible" : "none");
        if (this.#port.resourceForget(handle) !== "ok") return false;
        settle(value); return true;
      },
    };
    this.#publications.set(handle, item);
    return { outcome: promise, cancel: () => { if (this.#publications.has(handle)) item.cancel(); } };
  }
  /** Returns a newly owned local snapshot or an explicit native status. Peer
   * publication metadata alone is never accepted as a readable ResourceRef. */
  take(): { readonly status: ResourceStatus; readonly ref?: ResourceRef } {
    if (!this.#live) return { status: "closed" };
    if (this.#refs.size >= 64) return { status: "limit" };
    const admitted = this.#port.resourceTake(); if (admitted.status !== "ok") return { status: admitted.status };
    const handle = uint64(admitted.handle, true);
    try {
      if (!admitted.descriptor || [...this.#refs.values()].some(r => r.handle === handle)) fail("RESOURCE_REF");
      const ref = metadata(admitted.descriptor) as ResourceRef;
      this.#refs.set(ref, { handle, at: 0n, opening: false, done: false }); return { status: "ok", ref };
    } catch (error) { this.#port.resourceRelease(handle); throw error; }
  }
  /** One sequential stream per native ref. All reads are budgeted in native;
   * promises resume only when this broker is pumped on the original worker. */
  open(ref: ResourceRef, limits: ResourceBudget): { readonly status: ResourceStatus; readonly stream?: BoundedReadStream } {
    budget(limits); const local = this.#refs.get(ref); if (!local) return { status: "invalid" };
    if (!this.#live) return { status: "closed" }; if (local.opening) return { status: "busy" };
    const status = this.#port.resourceOpen(local.handle, limits.byteLimit, limits.timeoutMs); if (status !== "ok") return { status };
    local.opening = true;
    return { status, stream: Object.freeze({ read: () => {
      if (!this.#live || !this.#refs.has(ref)) return Promise.resolve<Outcome<ReadChunk>>({ kind: "cancelled", effect: "none" });
      if (local.pending) return Promise.resolve(failure<ReadChunk>("RESOURCE_LIMIT"));
      if (local.failure) return Promise.resolve(local.failure);
      if (local.done) return Promise.resolve<Outcome<ReadChunk>>({ kind: "success", value: { bytes: new Uint8Array(), done: true } });
      return new Promise<Outcome<ReadChunk>>(settle => { local.pending = { settle }; });
    } }) };
  }
  release(ref: ResourceRef): ResourceStatus {
    const local = this.#refs.get(ref); if (!local) return "invalid";
    const status = this.#port.resourceRelease(local.handle);
    if (status === "ok") { local.pending?.settle({ kind: "cancelled", effect: "none" }); delete local.pending; this.#refs.delete(ref); }
    return status;
  }
  step(): void {
    if (this.#stepping) fail("REENTRY"); this.#stepping = true;
    try {
      for (const [handle, item] of this.#publications) if (item.poll()) this.#publications.delete(handle);
      if (!this.#live) return;
      let used = 0;
      for (const [ref, local] of this.#refs) {
        const read = local.pending; if (!read || used >= 8) continue;
        const result = this.#port.resourceRead(local.handle, local.at.toString(), 16384);
        if (result.status === "wait") continue;
        used++; delete local.pending;
        if (result.status !== "ok") { local.failure = statusFailure(result.status); read.settle(local.failure); continue; }
        const size = ref.size;
        if (!(result.bytes instanceof Uint8Array) || result.bytes.length > 16384 || size.kind !== "known" || local.at + BigInt(result.bytes.length) > BigInt(size.bytes) || (!result.bytes.length && local.at !== BigInt(size.bytes))) {
          local.failure = failure("PROTOCOL"); read.settle(local.failure); continue;
        }
        const bytes = result.bytes.slice(); local.at += BigInt(bytes.length); local.done = bytes.length === 0;
        read.settle({ kind: "success", value: Object.freeze({ bytes, done: local.done }) });
      }
    } finally { this.#stepping = false; }
  }
  dispose(): void {
    if (!this.#live) return; this.#live = false;
    for (const item of this.#publications.values()) item.cancel();
    for (const [ref, local] of this.#refs) { local.pending?.settle({ kind: "cancelled", effect: "none" }); delete local.pending; this.release(ref); }
  }
  get retained(): number { return this.#publications.size + this.#refs.size; }
}
