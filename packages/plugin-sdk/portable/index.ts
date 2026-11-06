export { BoundaryError, compareUint64, decode, encode, integer, object, snapshot, text, uint64, utf8Bytes } from "./codec.js";
export type { Codec, Json } from "./codec.js";
export { SessionChannel } from "./session.js";
export type { Call, Method, NativePort, NativeResult, Outcome, RequestContext, Status } from "./session.js";
export type { ErrorCode } from "./wire-contract.js";
export type { ResourceBroker, ResourceRef, ResourceMetadata, ResourceBudget, ScopedResourceInput, BoundedReadStream, ReadChunk } from "./resources.js";

/** Current Session grant observation, not a promise that a host service exists.
 * Queries are live: revocation is never a cached allowed result. */
export type CapabilityAccess = "unknown" | "allowed" | "denied" | "revoked" | "unsupported";
export interface CapabilitySet { query(name: string): CapabilityAccess; }
export interface CancellationToken { readonly cancelled: boolean; }
export interface Disposable { dispose(): void; }
/** Runtime services implemented today. UI/process and command arguments follow;
 * optional resources exist only for a native resource-enabled Session. */
export interface PluginContext {
  readonly instanceId: string;
  readonly epoch: string;
  readonly side: "client" | "executor";
  readonly capabilities: CapabilitySet;
  readonly cancellation: CancellationToken;
  readonly session: import("./session.js").SessionChannel;
  readonly resources?: import("./resources.js").ResourceBroker;
}
export interface PluginEntry { activate(context: PluginContext): Promise<Disposable> | Disposable; }
