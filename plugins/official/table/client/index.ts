import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

/** A Native standard table contribution, not a WebView or a terminal renderer.
 * Current Hosts without that service must reject this package explicitly. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "client" || context.invocation?.command !== "table" ||
      context.capabilities.query("ui.table") !== "allowed") throw new Error("Native table capability is required");
  return { dispose() {} };
}
