import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";
/** The macOS Host owns its Native conversation adapter over the raw RPC bridge.
 * This package does not declare a portable Native view contribution. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "ai claude") throw new Error("Claude Client command required");
  return { dispose() {} };
}
