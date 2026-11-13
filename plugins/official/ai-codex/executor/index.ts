import { boolean, encode, object, text, type Call, type Disposable, type PluginContext,
  type RequestContext } from "@ghosterm/plugin-sdk";
import type { ApprovalDecision, RequestID } from "../protocol/client.ts";
import { CodexProtocolError } from "../protocol/client.ts";
import type { CodexConversationEvent } from "../protocol/conversation.ts";
import { spawnCodexAppServer, type CodexAppServerProcess } from "../protocol/process.ts";

const empty = object({});
const threadResult = object({ threadID: text(128) });
const messageInput = object({ text: text(2048) });
const turnResult = object({ turnID: text(128) });
const interruptResult = object({ accepted: boolean() });
const approvalInput = object({ requestID: text(140), decisionJSON: text(2048) });
const eventText = text(3072);
const eventAck = object({ accepted: boolean() });
const EVENT_QUEUE_LIMIT = 16;

function requestKey(id: RequestID): string {
  return typeof id === "string" ? `s:${id}` : `n:${id}`;
}

function eventPayload(event: CodexConversationEvent): string {
  switch (event.kind) {
    case "notification":
      return encode({ sequence: event.sequence, kind: event.kind,
        notification: { method: event.notification.method, params: event.notification.params ?? null } });
    case "approval-request":
      return encode({ sequence: event.sequence, kind: event.kind,
        request: { id: event.request.id, method: event.request.method, params: event.request.params } });
    case "approval-resolved":
      return encode({ sequence: event.sequence, kind: event.kind, threadID: event.threadID,
        requestID: event.requestID });
    case "outcome-unknown":
      return encode({ sequence: event.sequence, kind: event.kind, requestID: event.requestID });
    case "protocol-error":
      return encode({ sequence: event.sequence, kind: event.kind,
        message: event.error.message.slice(0, 512) });
    case "closed":
      return encode({ sequence: event.sequence, kind: event.kind, outcome: event.outcome });
  }
}

/** Official Codex Executor entry. It starts only after the Host admits a thread request. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "ai codex") {
    throw new CodexProtocolError("Codex requires the ai codex Executor command");
  }

  let child: CodexAppServerProcess | undefined;
  let unsubscribe: (() => void) | undefined;
  let conversation: ReturnType<CodexAppServerProcess["createConversation"]> | undefined;
  let closing = false;
  let closePromise: Promise<void> | undefined;
  let failure: Error | undefined;
  let flight: Call<{ readonly accepted: boolean }> | undefined;
  let startRequest: RequestContext | undefined;
  let activeTurnRequest: RequestContext | undefined;
  let interruptedTurnRequest: RequestContext | undefined;
  let interruptPromise: Promise<void> | undefined;
  const events: string[] = [];
  const approvals = new Map<string, RequestID>();
  const eventMethod = { name: "codex.event", input: eventText, output: eventAck } as const;

  const close = (): Promise<void> => {
    if (closePromise) return closePromise;
    closing = true;
    unsubscribe?.();
    unsubscribe = undefined;
    events.length = 0;
    approvals.clear();
    flight?.cancel();
    const active = conversation;
    conversation = undefined;
    closePromise = (async () => {
      if (active) await active.close();
      else if (child) await child.stop();
      child = undefined;
    })();
    return closePromise;
  };

  const interruptCancelledTurn = (): void => {
    const request = activeTurnRequest;
    if (!request?.cancelled || !conversation?.currentTurnID || interruptedTurnRequest === request) return;
    interruptedTurnRequest = request;
    interruptPromise = conversation.interrupt().catch(error => {
      failure = error instanceof Error ? error : new Error(String(error));
    });
  };

  context.session.register({ name: "codex.thread.start", input: empty, output: threadResult }, async (_input, request) => {
    request.check();
    if (child || conversation || startRequest || closing) throw new CodexProtocolError("Codex thread already started");
    startRequest = request;
    try {
      const process = await spawnCodexAppServer(context, {
        name: "ghosterm", title: "Ghosterm Codex", version: "0.1.0",
      });
      child = process;
      // Cancellation may stop the child before the handler reaches `await ready`.
      void process.ready.catch(() => {});
      if (closing || request.cancelled) {
        await process.stop();
        child = undefined;
        request.check();
      }
      await process.ready;
      request.check();
      conversation = process.createConversation();
      unsubscribe = conversation.onEvent(event => {
        try {
          if (event.kind === "approval-request") {
            approvals.set(requestKey(event.request.id), event.request.id);
            if (approvals.size > 32) throw new CodexProtocolError("too many pending Codex approvals");
          } else if (event.kind === "approval-resolved") {
            approvals.delete(requestKey(event.requestID));
          }
          if (events.length >= EVENT_QUEUE_LIMIT) throw new CodexProtocolError("Codex event queue is full");
          events.push(eventPayload(event));
        } catch (error) {
          failure = error instanceof Error ? error : new Error(String(error));
        }
      });
      const id = await conversation.startThread({});
      request.check();
      return { threadID: id };
    } catch (error) {
      try { await close(); } catch { /* retain the activation request failure */ }
      throw error;
    } finally {
      if (startRequest === request) startRequest = undefined;
    }
  });

  context.session.register({ name: "codex.turn.send", input: messageInput, output: turnResult }, async (input, request) => {
    request.check();
    if (!conversation || closing) throw new CodexProtocolError("Codex thread is not ready");
    if (activeTurnRequest) throw new CodexProtocolError("a Codex turn request is already active");
    activeTurnRequest = request;
    try {
      const turnID = await conversation.sendMessage(input.text);
      if (request.cancelled) {
        interruptCancelledTurn();
        if (interruptPromise) await interruptPromise;
      }
      request.check();
      return { turnID };
    } finally {
      if (activeTurnRequest === request) activeTurnRequest = undefined;
      if (interruptedTurnRequest === request) interruptedTurnRequest = undefined;
      interruptPromise = undefined;
    }
  });

  context.session.register({ name: "codex.turn.interrupt", input: empty, output: interruptResult }, async (_input, request) => {
    request.check();
    if (!conversation || closing) throw new CodexProtocolError("Codex thread is not ready");
    await conversation.interrupt();
    request.check();
    return { accepted: true };
  });

  context.session.register({ name: "codex.approval.respond", input: approvalInput, output: interruptResult }, (input, request) => {
    request.check();
    const id = approvals.get(input.requestID);
    if (!conversation || id === undefined) throw new CodexProtocolError("Codex approval is unknown or expired");
    const decision = JSON.parse(input.decisionJSON) as ApprovalDecision;
    conversation.respondApproval(id, decision);
    approvals.delete(input.requestID);
    return { accepted: true };
  });

  const disposable: Disposable = {
    step(): void {
      if (closing) return;
      if (failure) { void close().catch(() => {}); return; }
      if (startRequest?.cancelled) { void close().catch(() => {}); return; }
      try {
        child?.step();
        if (child?.stdio.failure) throw child.stdio.failure;
        if (child?.exit?.state === "exited") throw new Error("Codex app-server exited");
        interruptCancelledTurn();
      } catch (error) {
        failure = error instanceof Error ? error : new Error(String(error));
        void close().catch(() => {});
        return;
      }
      if (flight || events.length === 0) return;
      const payload = events.shift();
      if (payload === undefined) return;
      try {
        const call = context.session.request(eventMethod, payload, 30000);
        flight = call;
        void call.outcome.then(outcome => {
          if (flight !== call) return;
          flight = undefined;
          if (outcome.kind !== "success" || !outcome.value.accepted) {
            failure = new Error("Codex event delivery did not receive an acknowledged outcome");
          }
        }, error => {
          if (flight === call) {
            flight = undefined;
            failure = error instanceof Error ? error : new Error(String(error));
          }
        });
      } catch (error) {
        failure = error instanceof Error ? error : new Error(String(error));
      }
    },
    dispose(): void { void close().catch(() => {}); },
  };
  return disposable;
}
