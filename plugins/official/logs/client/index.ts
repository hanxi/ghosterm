import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "logs" ||
      context.capabilities.query("session.rpc") !== "allowed" ||
      context.capabilities.query("ui.table") !== "allowed") throw new Error("Log Client requires the native table and RPC grants");
  return { dispose() {} };
}
