import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";
/** Native Host will own chat presentation. It can observe the raw RPC bridge;
 * no Native conversation UI is advertised by this candidate package yet. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "ai claude") throw new Error("Claude Client command required");
  return { dispose() {} };
}
