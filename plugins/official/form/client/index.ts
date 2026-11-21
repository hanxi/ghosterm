import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "form" ||
      context.capabilities.query("ui.form") !== "allowed") throw new Error("Native form capability is required");
  return { dispose() {} };
}
