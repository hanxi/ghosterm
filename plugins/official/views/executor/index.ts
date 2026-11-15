import { object, TableDescriptorCodec, TablePageCodec, TablePageRequestCodec, validateTablePage,
  DiffDescriptorCodec, DiffPageRequestCodec, DiffPageCodec,
  TaskDescriptorCodec, TaskSnapshotCodec, TaskReadRequestCodec, TaskCancelRequestCodec, TaskCancelAckCodec } from "@ghosterm/plugin-sdk";
import type { PluginContext, TaskSnapshot, TaskCancelAck } from "@ghosterm/plugin-sdk";
import { OpenView, ReopenView } from "../shared.js";

export function activate(context: PluginContext) {
  if (context.side !== "executor" || context.capabilities.query("session.rpc") !== "allowed") throw new Error("Executor RPC required");
  let live = true;
  const check = () => { if (!live || context.cancellation.cancelled || context.capabilities.query("session.rpc") !== "allowed") throw new Error("Closed"); };
  for (const [viewId, total, factor] of [["alpha", 64, 1], ["beta", 5, 10]] as const) {
    const d = TableDescriptorCodec.parse({ viewId, revision: "1", title: viewId === "alpha" ? "Alpha numbers" : "Beta numbers",
      columns: [{ id: "number", title: "Number" }, { id: "value", title: "Value" }], total, pageSize: 16 });
    context.session.register({ name: viewId + ".describe", input: object({}), output: TableDescriptorCodec }, (_input, request) => { request.check(); check(); return d; });
    context.session.register({ name: viewId + ".page", input: TablePageRequestCodec, output: TablePageCodec }, (input, request) => {
      request.check(); check();
      const rows = Array.from({ length: Math.min(input.limit, Math.max(0, total - input.offset)) }, (_, i) => {
        const number = input.offset + i + 1; return { id: String(number), cells: [String(number), String(number * factor)] };
      });
      return validateTablePage(d, input, { viewId, revision: "1", offset: input.offset, total, rows });
    });
  }
  const diff = DiffDescriptorCodec.parse({ viewId: "changes", revision: "1", title: "A read-only change", beforeLabel: "Before", afterLabel: "After",
    beforeLines: 1, afterLines: 1, added: 1, removed: 1, total: 2, pageSize: 2 });
  context.session.register({ name: "changes.describe", input: object({}), output: DiffDescriptorCodec }, (_input, request) => { request.check(); check(); return diff; });
  context.session.register({ name: "changes.page", input: DiffPageRequestCodec, output: DiffPageCodec }, (input, request) => {
    request.check(); check();
    if (input.viewId !== diff.viewId || input.revision !== diff.revision || input.offset !== 0 || input.limit !== diff.pageSize) throw new Error("Stale or invalid diff page");
    // The SDK validates this bounded output; the Native reader checks descriptor
    // continuity. Avoid a second deep codec traversal inside the handler stack.
    return { viewId: "changes", revision: "1", offset: 0, total: 2, beforeOffset: 0, afterOffset: 0,
      rows: [{ kind: "delete", beforeLine: 1, afterLine: 0, text: "before", ending: "none" },
        { kind: "insert", beforeLine: 0, afterLine: 1, text: "after", ending: "none" }] } as const;
  });
  const task = TaskDescriptorCodec.parse({ viewId: "progress", revision: "1", title: "Internal counter", unit: "steps", cancellable: true });
  let completed = 0, sequence = 1, phase: TaskSnapshot["phase"] = "running", ack: TaskCancelAck | undefined;
  context.session.register({ name: "work.describe", input: object({}), output: TaskDescriptorCodec }, (_input, request) => { request.check(); check(); return task; });
  const scope = (input: { viewId: string; revision: string }) => { if (input.viewId !== task.viewId || input.revision !== task.revision) throw new Error("Stale task"); };
  context.session.register({ name: "work.read", input: TaskReadRequestCodec, output: TaskSnapshotCodec }, (input, request) => {
    request.check(); check(); scope(input);
    return TaskSnapshotCodec.parse({ viewId: task.viewId, revision: task.revision, sequence: String(sequence), phase, completed: String(completed), total: { kind: "known", units: "1000000" },
      message: "Counting without external effects", result: phase === "cancelled" ? { kind: "cancelled", effect: "none" } : phase === "succeeded" ? { kind: "success", summary: "Counter complete" } : { kind: "none" } });
  });
  context.session.register({ name: "work.cancel", input: TaskCancelRequestCodec, output: TaskCancelAckCodec }, (input, request) => {
    request.check(); check(); scope(input);
    if (ack) { if (ack.requestId !== input.requestId) throw new Error("Different cancel identity"); return ack; }
    ack = { ...input, kind: phase === "succeeded" ? "alreadyFinished" : "requested" };
    if (phase === "running") { phase = "cancelling"; sequence++; }
    return ack;
  });
  context.session.register(ReopenView, async (input, request) => {
    request.check(); check();
    const outcome = await context.session.request(OpenView, input, 5000).outcome;
    request.check(); check();
    if (outcome.kind !== "success") throw new Error("Client registration not confirmed");
    return outcome.value;
  });
  return { step() {
    check();
    if (phase === "cancelling") { phase = "cancelled"; sequence++; }
    else if (phase === "running") { completed++; sequence++; if (completed === 1000000) phase = "succeeded"; }
  }, dispose() { live = false; } };
}
