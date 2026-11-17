import {
  CodexAppServerClient,
  CodexOutcomeUnknownError,
  CodexProtocolError,
  type ApprovalDecision,
  type ClientInfo,
  type RequestID,
  type ServerNotification,
  type ServerRequest,
  type ThreadStartParams,
} from "./client.ts";

export type CodexConversationState = "new" | "starting-thread" | "ready" |
  "starting-turn" | "running" | "failed" | "closed";

export type CodexConversationEvent =
  | { readonly sequence: number; readonly kind: "notification"; readonly notification: ServerNotification }
  | { readonly sequence: number; readonly kind: "approval-request"; readonly request: ServerRequest }
  | { readonly sequence: number; readonly kind: "approval-resolved"; readonly threadID: string; readonly requestID: RequestID }
  | { readonly sequence: number; readonly kind: "outcome-unknown"; readonly requestID: RequestID }
  | { readonly sequence: number; readonly kind: "protocol-error"; readonly error: Error }
  | { readonly sequence: number; readonly kind: "closed"; readonly outcome: "idle" | "unknown" };
type CodexConversationEventInput = CodexConversationEvent extends infer Event
  ? Event extends { readonly sequence: number } ? Omit<Event, "sequence"> : never
  : never;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function requestKey(id: RequestID): string {
  return typeof id === "string" ? `s:${id}` : `n:${id}`;
}

/** One-thread, ordered conversation facade over the version-locked app-server client. */
export class CodexConversation {
  private stateValue: CodexConversationState = "new";
  private threadID: string | undefined;
  private activeTurnID: string | undefined;
  private completedTurnID: string | undefined;
  private threadStartPending = false;
  private turnStartPending = false;
  private sequence = 0;
  private readonly listeners = new Set<(event: CodexConversationEvent) => void>();
  private readonly approvals = new Map<string, ServerRequest>();
  private readonly unsubscribe: (() => void)[];
  private readonly client: CodexAppServerClient;
  private readonly stopProcess: (() => Promise<void>) | undefined;
  private closePromise: Promise<void> | undefined;

  constructor(client: CodexAppServerClient, stopProcess?: () => Promise<void>) {
    this.client = client;
    this.stopProcess = stopProcess;
    this.unsubscribe = [
      client.onNotification(event => this.receiveNotification(event)),
      client.onServerRequest(request => this.receiveApproval(request)),
      client.onProtocolError(error => this.fail(error)),
    ];
  }

  get state(): CodexConversationState { return this.stateValue; }
  get currentThreadID(): string | undefined { return this.threadID; }
  get currentTurnID(): string | undefined { return this.activeTurnID; }

  onEvent(listener: (event: CodexConversationEvent) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  async startThread(params: ThreadStartParams): Promise<string> {
    if (this.stateValue !== "new" || !this.client.active) {
      throw new CodexProtocolError("Codex conversation cannot start a thread in its current state");
    }
    this.stateValue = "starting-thread";
    this.threadStartPending = true;
    try {
      const result = await this.client.threadStart(params);
      this.threadStartPending = false;
      const thread = isRecord(result) && isRecord(result.thread) ? result.thread : undefined;
      if (!thread || typeof thread.id !== "string" || !thread.id) {
        throw new CodexProtocolError("Codex thread/start returned no thread id");
      }
      if (params.sandbox === "read-only" && (!isRecord(result) || !isRecord(result.sandbox) || result.sandbox.type !== "readOnly")) {
        throw new CodexProtocolError("Codex did not confirm the requested read-only sandbox");
      }
      if (params.approvalsReviewer === "user" && (!isRecord(result) || result.approvalsReviewer !== "user")) {
        throw new CodexProtocolError("Codex did not confirm the requested user approval reviewer");
      }
      if (params.approvalPolicy === "on-request" && (!isRecord(result) || result.approvalPolicy !== "on-request")) {
        throw new CodexProtocolError("Codex did not confirm the requested approval policy");
      }
      if (this.isClosed()) throw new CodexOutcomeUnknownError("thread/start");
      this.threadID = thread.id;
      this.stateValue = "ready";
      if (isRecord(result) && typeof result.approvalPolicy === "string" && typeof result.approvalsReviewer === "string" && isRecord(result.sandbox)) {
        this.emit({ kind: "notification", notification: { method: "ghosterm/threadConfigured", params: {
          threadId: thread.id, approvalPolicy: result.approvalPolicy, approvalsReviewer: result.approvalsReviewer,
          sandboxType: result.sandbox.type,
        } } });
      }
      return thread.id;
    } catch (error) {
      this.threadStartPending = false;
      if (!this.isClosed()) {
        if (error instanceof CodexOutcomeUnknownError) {
          this.stateValue = "failed";
          this.emit({ kind: "outcome-unknown", requestID: error.requestID });
        } else {
          this.fail(error instanceof Error ? error : new Error(String(error)));
        }
      }
      throw error;
    }
  }

  async sendMessage(text: string, clientUserMessageId?: string): Promise<string> {
    if (this.stateValue !== "ready" || !this.threadID || typeof text !== "string" || text.length === 0) {
      throw new CodexProtocolError("Codex conversation is not ready for a message");
    }
    const threadID = this.threadID;
    this.stateValue = "starting-turn";
    this.turnStartPending = true;
    this.activeTurnID = undefined;
    this.completedTurnID = undefined;
    try {
      const turnStart = this.client.turnStart({
        threadId: threadID,
        input: [{ type: "text", text, text_elements: [] }],
        ...(clientUserMessageId === undefined ? {} : { clientUserMessageId }),
      });
      const result = await turnStart;
      this.turnStartPending = false;
      const turn = isRecord(result) && isRecord(result.turn) ? result.turn : undefined;
      if (!turn || typeof turn.id !== "string" || !turn.id) {
        throw new CodexProtocolError("Codex turn/start returned no turn id");
      }
      if (this.isClosed()) throw new CodexOutcomeUnknownError("turn/start");
      if (this.activeTurnID && this.activeTurnID !== turn.id) {
        throw new CodexProtocolError("Codex turn/start response does not match turn/started");
      }
      if (this.completedTurnID !== turn.id) {
        this.activeTurnID = turn.id;
        this.stateValue = "running";
      }
      return turn.id;
    } catch (error) {
      this.turnStartPending = false;
      if (this.isClosed()) throw error;
      if (error instanceof CodexOutcomeUnknownError) {
        this.stateValue = "failed";
        this.emit({ kind: "outcome-unknown", requestID: error.requestID });
      } else if (!this.isClosed()) {
        this.stateValue = "ready";
        this.activeTurnID = undefined;
        this.fail(error instanceof Error ? error : new Error(String(error)));
      }
      throw error;
    }
  }

  async interrupt(): Promise<void> {
    if (this.stateValue !== "running" || !this.threadID || !this.activeTurnID) {
      throw new CodexProtocolError("Codex conversation has no interruptible turn");
    }
    await this.client.turnInterrupt({ threadId: this.threadID, turnId: this.activeTurnID });
  }

  respondApproval(requestID: RequestID, decision: ApprovalDecision): void {
    const key = requestKey(requestID);
    const request = this.approvals.get(key);
    if (this.stateValue !== "running" || !this.activeTurnID || !request ||
        request.params.turnId !== this.activeTurnID) {
      throw new CodexProtocolError("approval is not pending for this conversation");
    }
    this.client.respond(requestID, decision);
    this.approvals.delete(key);
  }

  close(): Promise<void> {
    if (this.closePromise) return this.closePromise;
    if (this.stateValue === "closed") return Promise.resolve();
    const outcome = this.threadStartPending || this.turnStartPending || this.activeTurnID ||
      this.stateValue === "starting-thread" || this.stateValue === "starting-turn" ||
      this.stateValue === "running" ? "unknown" : "idle";
    this.stateValue = "closed";
    this.approvals.clear();
    this.unsubscribe.forEach(unsubscribe => unsubscribe());
    this.client.close();
    this.closePromise = Promise.resolve().then(() => this.stopProcess?.());
    this.emit({ kind: "closed", outcome });
    return this.closePromise;
  }

  private receiveApproval(request: ServerRequest): void {
    const params = request.params;
    if (!this.threadID || !isRecord(params) || params.threadId !== this.threadID ||
        typeof params.turnId !== "string" || !params.turnId ||
        typeof params.itemId !== "string" || !params.itemId || typeof params.startedAtMs !== "number") {
      this.fail(new CodexProtocolError("Codex approval request has an invalid thread, turn, or item"));
      return;
    }
    if (this.stateValue === "starting-turn" && !this.activeTurnID) {
      this.activeTurnID = params.turnId;
      this.stateValue = "running";
    } else if (this.stateValue !== "running" || this.activeTurnID !== params.turnId) {
      this.fail(new CodexProtocolError("Codex approval request belongs to an inactive turn"));
      return;
    }
    const key = requestKey(request.id);
    this.approvals.set(key, request);
    this.emit({ kind: "approval-request", request });
  }

  private receiveNotification(notification: ServerNotification): void {
    if (notification.method === "serverRequest/resolved" && isRecord(notification.params)) {
      const { threadId, requestId } = notification.params;
      if (typeof threadId !== "string" || threadId !== this.threadID ||
          !(typeof requestId === "string" || (typeof requestId === "number" && Number.isSafeInteger(requestId)))) {
        this.fail(new CodexProtocolError("Codex approval resolution has an invalid thread or request id"));
        return;
      }
      if (this.approvals.delete(requestKey(requestId))) {
        this.emit({ kind: "approval-resolved", threadID: threadId, requestID: requestId });
      }
      return;
    }
    if ((notification.method === "turn/started" || notification.method === "turn/completed") &&
        isRecord(notification.params)) {
      const { threadId, turn } = notification.params;
      if (threadId !== this.threadID || !isRecord(turn) || typeof turn.id !== "string" || !turn.id) {
        this.fail(new CodexProtocolError("Codex turn notification has an invalid thread or turn id"));
        return;
      }
      if (notification.method === "turn/started") {
        if (this.stateValue !== "starting-turn" && this.stateValue !== "running") {
          this.fail(new CodexProtocolError("Codex started a turn outside a user message"));
          return;
        }
        if (this.activeTurnID && this.activeTurnID !== turn.id) {
          this.fail(new CodexProtocolError("Codex started overlapping turns"));
          return;
        }
        this.activeTurnID = turn.id;
        this.stateValue = "running";
      } else {
        if ((this.activeTurnID && this.activeTurnID !== turn.id) ||
            (this.stateValue !== "starting-turn" && this.stateValue !== "running")) {
          this.fail(new CodexProtocolError("Codex completed an unrelated turn"));
          return;
        }
        this.completedTurnID = turn.id;
        this.activeTurnID = undefined;
        this.approvals.clear();
        this.stateValue = "ready";
      }
    }
    if (isRecord(notification.params) && typeof notification.params.threadId === "string" &&
        this.threadID && notification.params.threadId !== this.threadID) {
      this.fail(new CodexProtocolError("Codex notification belongs to an unknown thread"));
      return;
    }
    this.emit({ kind: "notification", notification });
  }

  private fail(error: Error): void {
    if (this.stateValue !== "closed") this.stateValue = "failed";
    this.approvals.clear();
    this.emit({ kind: "protocol-error", error });
  }

  private isClosed(): boolean { return this.stateValue === "closed"; }

  private emit(event: CodexConversationEventInput): void {
    const sequenced = { ...event, sequence: ++this.sequence } as CodexConversationEvent;
    for (const listener of this.listeners) listener(sequenced);
  }
}
