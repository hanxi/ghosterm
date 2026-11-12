/** Stable Codex app-server JSON-RPC client. Process ownership and pipe IO belong to the host. */

export type RequestID = string | number;
export type AppServerMethod = "thread/start" | "turn/start" | "turn/interrupt";

export interface ClientInfo {
  readonly name: string;
  readonly title: string;
  readonly version: string;
}

export interface ThreadStartParams {
  readonly model?: string | null;
  readonly modelProvider?: string | null;
  readonly cwd?: string | null;
  readonly approvalPolicy?: string | null;
  readonly sandbox?: string | null;
}

export interface TurnStartParams {
  readonly threadId: string;
  readonly input: readonly unknown[];
}

export interface TurnInterruptParams {
  readonly threadId: string;
  readonly turnId: string;
}

export interface ServerRequest {
  readonly id: RequestID;
  readonly method: "item/commandExecution/requestApproval" | "item/fileChange/requestApproval";
  readonly params: Readonly<Record<string, unknown>>;
}

export type BasicApprovalDecision = "accept" | "acceptForSession" | "decline" | "cancel";
export type CommandExecutionApprovalDecision = BasicApprovalDecision |
  { readonly acceptWithExecpolicyAmendment: readonly string[] } |
  { readonly applyNetworkPolicyAmendment: { readonly host: string; readonly action: "allow" | "deny" } };
export type FileChangeApprovalDecision = BasicApprovalDecision;
export type ApprovalDecision = CommandExecutionApprovalDecision | FileChangeApprovalDecision;

export interface ServerNotification {
  readonly method: string;
  readonly params?: unknown;
}

export class CodexProtocolError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CodexProtocolError";
  }
}

/** A closed pipe cannot prove whether a request took effect. Never retry it implicitly. */
export class CodexOutcomeUnknownError extends Error {
  readonly requestID: RequestID;

  constructor(requestID: RequestID) {
    super(`Codex app-server outcome is unknown for request ${String(requestID)}`);
    this.name = "CodexOutcomeUnknownError";
    this.requestID = requestID;
  }
}

type Pending = { resolve(value: unknown): void; reject(error: Error): void };
type Phase = "new" | "initializing" | "active" | "closed" | "failed";

const SERVER_REQUESTS = new Set([
  "item/commandExecution/requestApproval",
  "item/fileChange/requestApproval",
]);
const CLIENT_METHODS = new Set<AppServerMethod>(["thread/start", "turn/start", "turn/interrupt"]);
const MAX_LINE_BYTES = 1_048_576;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function idKey(value: unknown): string | null {
  if (typeof value === "string") return `s:${value}`;
  if (typeof value === "number" && Number.isSafeInteger(value)) return `n:${value}`;
  return null;
}

/**
 * The adapter has no child-process dependency. A host-owned process/pipe service
 * supplies writeLine and forwards each complete stdout line to receiveLine.
 */
export class CodexAppServerClient {
  private phase: Phase = "new";
  private nextID = 1;
  private readonly pending = new Map<string, Pending>();
  private readonly serverRequests = new Map<string, ServerRequest>();
  private readonly notifications = new Set<(event: ServerNotification) => void>();
  private readonly requests = new Set<(request: ServerRequest) => void>();
  private readonly protocolErrors = new Set<(error: CodexProtocolError) => void>();
  private readonly writeLine: (line: string) => void;

  constructor(writeLine: (line: string) => void) {
    this.writeLine = writeLine;
  }

  get active(): boolean { return this.phase === "active"; }

  onNotification(listener: (event: ServerNotification) => void): () => void {
    this.notifications.add(listener);
    return () => this.notifications.delete(listener);
  }

  onServerRequest(listener: (request: ServerRequest) => void): () => void {
    this.requests.add(listener);
    return () => this.requests.delete(listener);
  }

  onProtocolError(listener: (error: CodexProtocolError) => void): () => void {
    this.protocolErrors.add(listener);
    return () => this.protocolErrors.delete(listener);
  }

  async initialize(clientInfo: ClientInfo): Promise<unknown> {
    if (this.phase !== "new") throw new CodexProtocolError("app-server initialize is single-use");
    if (!clientInfo.name || !clientInfo.title || !clientInfo.version) {
      throw new CodexProtocolError("invalid Codex clientInfo");
    }
    this.phase = "initializing";
    try {
      const result = await this.sendRequest("initialize", { clientInfo });
      if (!isRecord(result)) throw new CodexProtocolError("invalid initialize result");
      this.sendNotification("initialized", {});
      this.phase = "active";
      return result;
    } catch (error) {
      this.markFailedUnlessClosed();
      throw error;
    }
  }

  threadStart(params: ThreadStartParams): Promise<unknown> {
    return this.activeRequest("thread/start", params);
  }

  turnStart(params: TurnStartParams): Promise<unknown> {
    return this.activeRequest("turn/start", params);
  }

  turnInterrupt(params: TurnInterruptParams): Promise<unknown> {
    return this.activeRequest("turn/interrupt", params);
  }

  /** Host feeds one complete UTF-8 line. Codex 0.166.0 omits `jsonrpc` on replies. */
  receiveLine(line: string): void {
    if (this.phase === "closed" || this.phase === "failed") return;
    if (new TextEncoder().encode(line).byteLength > MAX_LINE_BYTES) {
      this.protocolFailure("app-server line exceeds 1 MiB");
      return;
    }
    let message: unknown;
    try { message = JSON.parse(line); }
    catch { this.protocolFailure("app-server emitted invalid JSON"); return; }
    if (!isRecord(message) || (message.jsonrpc !== undefined && message.jsonrpc !== "2.0")) {
      this.protocolFailure("app-server emitted an invalid JSON-RPC envelope");
      return;
    }

    if (Object.prototype.hasOwnProperty.call(message, "id")) {
      const key = idKey(message.id);
      if (key === null) { this.protocolFailure("invalid JSON-RPC id"); return; }
      if (typeof message.method === "string") {
        this.receiveServerRequest(key, message);
      } else {
        this.receiveResponse(key, message);
      }
      return;
    }
    if (typeof message.method !== "string") {
      this.protocolFailure("JSON-RPC notification has no method");
      return;
    }
    if (message.method === "serverRequest/resolved" && isRecord(message.params)) {
      const resolvedKey = idKey(message.params.requestId);
      if (resolvedKey !== null) this.serverRequests.delete(resolvedKey);
    }
    const event: ServerNotification = { method: message.method, ...(message.params === undefined ? {} : { params: message.params }) };
    for (const listener of this.notifications) listener(event);
  }

  /** Resolves a single server request using its original JSON-RPC id. */
  respond(requestID: RequestID, decision: ApprovalDecision): void {
    const key = idKey(requestID);
    const request = key === null ? undefined : this.serverRequests.get(key);
    if (key === null || !request) {
      throw new CodexProtocolError("unknown, expired, or already answered app-server request");
    }
    const validBasic = decision === "accept" || decision === "acceptForSession" ||
      decision === "decline" || decision === "cancel";
    let valid = validBasic;
    if (request.method === "item/commandExecution/requestApproval" && isRecord(decision)) {
      const keys = Object.keys(decision);
      const payload = Object.values(decision)[0];
      if (keys.length === 1 && keys[0] === "acceptWithExecpolicyAmendment") {
        valid = Array.isArray(payload) && payload.every(value => typeof value === "string");
      } else if (keys.length === 1 && keys[0] === "applyNetworkPolicyAmendment" && isRecord(payload)) {
        valid = Object.keys(payload).length === 2 && typeof payload.host === "string" &&
          (payload.action === "allow" || payload.action === "deny");
      } else {
        valid = false;
      }
    }
    if (!valid) throw new CodexProtocolError("approval decision does not match the pending request schema");
    this.serverRequests.delete(key);
    this.write({ jsonrpc: "2.0", id: requestID, result: { decision } });
  }

  /** Call only after the host has stopped/closed both pipe ends. */
  close(): void {
    if (this.phase === "closed") return;
    this.phase = "closed";
    for (const [key, pending] of this.pending) {
      const requestID = key.slice(2);
      pending.reject(new CodexOutcomeUnknownError(key.startsWith("n:") ? Number(requestID) : requestID));
    }
    this.pending.clear();
    this.serverRequests.clear();
  }

  private activeRequest(method: AppServerMethod, params: unknown): Promise<unknown> {
    if (this.phase !== "active" || !CLIENT_METHODS.has(method)) {
      return Promise.reject(new CodexProtocolError("Codex app-server is not active"));
    }
    return this.sendRequest(method, params);
  }

  private sendRequest(method: string, params: unknown): Promise<unknown> {
    const id = this.nextID++;
    const key = idKey(id)!;
    return new Promise((resolve, reject) => {
      this.pending.set(key, { resolve, reject });
      try { this.write({ jsonrpc: "2.0", id, method, params }); }
      catch (error) {
        this.pending.delete(key);
        reject(error instanceof Error ? error : new Error(String(error)));
      }
    });
  }

  private sendNotification(method: string, params: unknown): void {
    this.write({ jsonrpc: "2.0", method, params });
  }

  private write(message: Record<string, unknown>): void {
    if (this.phase === "closed" || this.phase === "failed") throw new CodexProtocolError("app-server is closed");
    const line = JSON.stringify(message);
    if (new TextEncoder().encode(line).byteLength > MAX_LINE_BYTES) {
      throw new CodexProtocolError("outgoing app-server line exceeds 1 MiB");
    }
    this.writeLine(line);
  }

  private receiveResponse(key: string, message: Record<string, unknown>): void {
    const pending = this.pending.get(key);
    if (!pending) { this.protocolFailure("response has an unknown JSON-RPC id"); return; }
    this.pending.delete(key);
    if (isRecord(message.error)) {
      pending.reject(new CodexProtocolError(`app-server request failed: ${String(message.error.message ?? "unknown error")}`));
    } else if (Object.prototype.hasOwnProperty.call(message, "result")) {
      pending.resolve(message.result);
    } else {
      pending.reject(new CodexProtocolError("JSON-RPC response has neither result nor error"));
    }
  }

  private receiveServerRequest(key: string, message: Record<string, unknown>): void {
    if (this.phase !== "active" || !SERVER_REQUESTS.has(message.method as string) || !isRecord(message.params)) {
      this.protocolFailure("unsupported or malformed app-server request");
      return;
    }
    if (this.serverRequests.has(key)) { this.protocolFailure("duplicate app-server request id"); return; }
    const request = { id: message.id as RequestID,
      method: message.method as ServerRequest["method"], params: message.params };
    this.serverRequests.set(key, request);
    for (const listener of this.requests) listener(request);
  }

  private protocolFailure(message: string): void {
    this.phase = "failed";
    for (const [key, pending] of this.pending) {
      const suffix = key.slice(2);
      pending.reject(new CodexOutcomeUnknownError(key.startsWith("n:") ? Number(suffix) : suffix));
    }
    this.pending.clear();
    this.serverRequests.clear();
    const error = new CodexProtocolError(message);
    for (const listener of this.protocolErrors) listener(error);
  }

  private markFailedUnlessClosed(): void {
    if (this.phase !== "closed") this.phase = "failed";
  }
}
