import type { PluginContext, ViewSpec } from "@ghosterm/plugin-sdk";

export function openDevelopmentViews(context: PluginContext) {
  const views = context.views;
  if (context.side !== "client" || !views) throw new Error("Client views required");
  const specs: readonly ViewSpec[] = [
    { kind: "table", viewId: "table-a", methods: { describe: "dev.exchange", read: "dev.page" } },
    { kind: "diff", viewId: "diff-b", methods: { describe: "dev.describe", read: "dev.page" } },
    { kind: "task", viewId: "task-c", methods: { describe: "dev.describe", read: "dev.page", cancel: "dev.cancel" } },
  ];
  const handles = specs.map(spec => views.open(spec));
  if (handles.some(handle => handle.state !== "ok")) throw new Error("View registration failed");
  // Plugin closure must not strand another Native registration or reuse an id.
  const temporary = views.open({ ...specs[0]!, viewId: "temporary" });
  temporary.close(); temporary.close();
  if (temporary.state !== "closed") throw new Error("View close failed");
  return handles;
}
