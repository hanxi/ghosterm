import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "diff" ||
      context.capabilities.query("ui.diff") !== "allowed") throw new Error("Native diff capability is required");
  return { dispose() {} };
}
