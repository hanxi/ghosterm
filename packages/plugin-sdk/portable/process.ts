/** Opaque native workspace authority. It never contains a path or file handle. */
export interface WorkspaceHandle {
  readonly id: string;
}

export type ProcessRead = { readonly state: "data"; readonly bytes: Uint8Array } |
  { readonly state: "wait" } | { readonly state: "eof" } | { readonly state: "closed" } |
  { readonly state: "error"; readonly error: Error };
export type ProcessWrite = { readonly state: "data"; readonly count: number } |
  { readonly state: "wait" } | { readonly state: "closed" } |
  { readonly state: "error"; readonly error: Error };
export type ProcessExit =
  { readonly state: "running" } |
  { readonly state: "exited"; readonly code: number | null; readonly signal: number | null;
    readonly terminationReason?: "deadline" | "output-limit" };

/** Nonblocking stdio plus explicit child lifecycle, supplied by the Host. */
export interface ProcessChild {
  readonly verifiedVersion: string;
  readStdout(maxBytes: number): ProcessRead;
  readStderr(maxBytes: number): ProcessRead;
  writeStdin(bytes: Uint8Array): ProcessWrite;
  closeStdin(): void;
  signal(signal: "terminate" | "kill"): void;
  poll(): ProcessExit;
  /** Resolves only after the child has exited and its OS process has been reaped. */
  release(): Promise<void>;
}

export interface ProcessSpawnOptions {
  readonly program: string;
  readonly argv: readonly string[];
  readonly cwd: WorkspaceHandle;
  readonly envPolicy: "selected-user-environment";
  readonly cancellation: import("./index.js").CancellationToken;
}

/** Available only when the Host has configured the pipes service and a live grant. */
export interface ProcessService {
  spawn(options: ProcessSpawnOptions): Promise<ProcessChild>;
}
