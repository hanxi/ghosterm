import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "task primes" ||
      context.capabilities.query("ui.task") !== "allowed") throw new Error("Native task capability is required");
  return { dispose() {} };
}
