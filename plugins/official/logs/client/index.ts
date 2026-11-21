import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "logs" ||
      context.capabilities.query("session.rpc") !== "allowed") throw new Error("Log Client invocation and RPC grant required");
  return { dispose() {} };
}
