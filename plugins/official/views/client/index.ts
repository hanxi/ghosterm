import type { PluginContext, ViewHandle, ViewSpec } from "@ghosterm/plugin-sdk";
import { OpenView } from "../shared.js";

export function activate(context: PluginContext) {
  const views = context.views;
  if (context.side !== "client" || !views || ["session.rpc", "ui.views", "ui.table", "ui.diff", "ui.task"].some(c => context.capabilities.query(c) !== "allowed")) throw new Error("UNSUPPORTED");
  const specs: readonly ViewSpec[] = [
    { kind: "table", viewId: "alpha", methods: { describe: "alpha.describe", read: "alpha.page" } },
    { kind: "table", viewId: "beta", methods: { describe: "beta.describe", read: "beta.page" } },
    { kind: "diff", viewId: "changes", methods: { describe: "changes.describe", read: "changes.page" } },
    { kind: "task", viewId: "progress", methods: { describe: "work.describe", read: "work.read", cancel: "work.cancel" } }
  ];
  const handles = new Map<string, ViewHandle>();
  let live = true;
  for (const spec of specs) handles.set(spec.viewId, views.open(spec));
  context.session.register(OpenView, (input, request) => {
    request.check();
    if (!live || context.cancellation.cancelled) throw new Error("Closed");
    const spec = specs.find(s => s.viewId === input.viewId);
    if (!spec) throw new Error("Unknown view");
    if (handles.get(spec.viewId)?.state === "ok") return { opened: false };
    handles.set(spec.viewId, views.open(spec));
    return { opened: true };
  });
  return { dispose() { live = false; handles.clear(); } };
}
