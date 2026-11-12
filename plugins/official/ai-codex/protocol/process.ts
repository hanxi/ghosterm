import type { PluginContext } from "../../../../packages/plugin-sdk/portable/index.js";
import { CodexAppServerStdio } from "./stdio.ts";
import { CodexProtocolError, type ClientInfo } from "./client.ts";
import { CODEX_CLI_VERSION } from "./version.ts";
import type { ProcessExit } from "../../../../packages/plugin-sdk/portable/process.js";
import { CodexConversation } from "./conversation.ts";

/** A Codex server launched through the Host-authorized Executor process service. */
export interface CodexAppServerProcess {
  readonly stdio: CodexAppServerStdio;
  readonly ready: Promise<unknown>;
  readonly exit: ProcessExit | undefined;
  readonly reaped: boolean;
  /** Conversation facade whose close() also stops and reaps this child. */
  createConversation(): CodexConversation;
  /** One bounded nonblocking IO slice; the Host must call this from its worker. */
  step(): void;
  /** Close protocol state, request termination, and wait for OS reaping. */
  stop(): Promise<void>;
}

/** Spawn only the pinned Host mapping named `codex`; never use a shell or PATH. */
export async function spawnCodexAppServer(
  context: PluginContext,
  clientInfo: ClientInfo,
): Promise<CodexAppServerProcess> {
  if (context.side !== "executor" ||
      context.capabilities.query("process.spawn") !== "allowed" ||
      context.capabilities.query("process.pipes") !== "allowed") {
    throw new Error("Codex app-server requires an authorized Executor process grant");
  }
  const process = context.process;
  const cwd = context.invocation?.workspaceHandle;
  if (!process || !cwd) {
    throw new Error("Codex app-server requires Host process service and an authorized workspace");
  }

  const child = await process.spawn({
    program: "codex",
    argv: ["app-server"],
    cwd,
    envPolicy: "selected-user-environment",
    cancellation: context.cancellation,
  });
  if (child.verifiedVersion !== CODEX_CLI_VERSION) {
    try {
      child.signal("terminate");
    } finally {
      await child.release();
    }
    throw new CodexProtocolError(
      `Codex version mismatch: expected ${CODEX_CLI_VERSION}, got ${child.verifiedVersion}`,
    );
  }
  const stdio = new CodexAppServerStdio(child);
  let stopped = false;
  let exit: ProcessExit | undefined;
  let reaped = false;
  let releasePromise: Promise<void> | undefined;
  const release = (): Promise<void> => {
    if (!releasePromise) {
      stdio.close();
      releasePromise = child.release().then(() => {
        reaped = true;
        exit = child.poll();
      });
      // step() may observe natural exit without a caller awaiting stop(). Keep
      // the rejection observed here; stop() returns the same rejecting promise.
      void releasePromise.catch(() => {});
    }
    return releasePromise;
  };
  const stop = async (): Promise<void> => {
    if (!stopped && !releasePromise) {
      stopped = true;
      child.signal("terminate");
    }
    await release();
  };
  const ready = stdio.client.initialize(clientInfo).catch(async error => {
    // Initialization failure means the process cannot serve this protocol
    // session. Reap it here even when the caller only awaits `ready`.
    await stop();
    throw error;
  });
  return {
    stdio,
    ready,
    get exit() { return exit; },
    get reaped() { return reaped; },
    createConversation() { return new CodexConversation(stdio.client, stop); },
    step() {
      if (releasePromise) return;
      stdio.step();
      exit = child.poll();
      if (exit.state === "exited" && stdio.drained) void release();
    },
    stop,
  };
}
