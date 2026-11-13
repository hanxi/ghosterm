export { boolean, commandArguments, option, positional, BoundaryError, compareUint64, decode, encode, integer, object, snapshot, text, uint64, utf8Bytes } from "./codec.js";
export type { Argument, Codec, Json } from "./codec.js";
export { SessionChannel } from "./session.js";
export type { Call, Method, NativePort, NativeResult, Outcome, RequestContext, Status } from "./session.js";
export type { ErrorCode } from "./wire-contract.js";
export type { ResourceBroker, ResourceRef, ResourceMetadata, ResourceBudget, ScopedResourceInput, BoundedReadStream, ReadChunk } from "./resources.js";
export type { WorkspaceHandle, ProcessRead, ProcessWrite, ProcessExit, ProcessChild, ProcessSpawnOptions, ProcessService } from "./process.js";

/** Current Session grant observation, not a promise that a host service exists.
 * Queries are live: revocation is never a cached allowed result. */
export type CapabilityAccess = "unknown" | "allowed" | "denied" | "revoked" | "unsupported";
export interface CapabilitySet { query(name: string): CapabilityAccess; }
export interface CancellationToken { readonly cancelled: boolean; }
/** Optional bounded worker hook. Called once per native Runtime pump after activation. */
export interface Disposable { step?(): void; dispose(): void; }
/** UI remains unavailable. Optional resources/processes exist only when the
 * native Host has configured the corresponding service and Session capability. */
export interface Invocation {
  readonly command: string;
  readonly workspaceHandle?: import("./process.js").WorkspaceHandle;
  /** Raw argv preserved exactly; never shell text. */
  readonly arguments: readonly string[];
  /** Host-decoded scalar options from the checked argsSchema CLI mapping. */
  readonly decodedArguments?: Readonly<Record<string, string | number | boolean>>;
}
export interface PluginContext {
  readonly invocation?: Invocation;
  readonly instanceId: string;
  readonly epoch: string;
  readonly side: "client" | "executor";
  readonly capabilities: CapabilitySet;
  readonly cancellation: CancellationToken;
  readonly session: import("./session.js").SessionChannel;
  readonly resources?: import("./resources.js").ResourceBroker;
  readonly process?: import("./process.js").ProcessService;
}
export interface PluginEntry { activate(context: PluginContext): Promise<Disposable> | Disposable; }
