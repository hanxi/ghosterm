import type { PluginContext } from "../../../../packages/plugin-sdk/portable/index.js";
import { CodexAppServerStdio } from "./stdio.ts";
import type { ClientInfo } from "./client.ts";

/** A Codex server launched through the Host-authorized Executor process service. */
export interface CodexAppServerProcess {
  readonly stdio: CodexAppServerStdio;
  readonly ready: Promise<unknown>;
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
  const stdio = new CodexAppServerStdio(child);
  const ready = stdio.client.initialize(clientInfo);
  let stopped = false;
  return {
    stdio,
    ready,
    step() { stdio.step(); },
    async stop() {
      if (!stopped) {
        stopped = true;
        stdio.close();
        child.signal("terminate");
      }
      await child.release();
    },
  };
}
