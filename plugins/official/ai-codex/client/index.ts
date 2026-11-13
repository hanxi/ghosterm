import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

/** Native Hosts own conversation controls and leave codex.event RPCs queued for Native receipt. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "ai codex") {
    throw new Error("Codex Client requires the ai codex Client command");
  }
  return { dispose() {} };
}
