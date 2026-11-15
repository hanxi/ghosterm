import { commandArguments, DiffDescribeMethod, DiffDescriptorCodec, DiffPageMethod, option, text } from "@ghosterm/plugin-sdk";
import type { DiffDescriptor, DiffRow, Disposable, PluginContext } from "@ghosterm/plugin-sdk";
import { compareSteps, lines } from "./compare.js";

export const Arguments = commandArguments({ before: option(text(3072), "before", '""'), after: option(text(3072), "after", '""'),
  beforeLabel: option(text(64), "before-label", "Before"), afterLabel: option(text(64), "after-label", "After") });

export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "diff" ||
      context.capabilities.query("session.rpc") !== "allowed") throw new Error("Diff Executor invocation and RPC grant required");
  let live = true;
  const check = (): void => {
    if (!live || context.cancellation.cancelled || context.capabilities.query("session.rpc") !== "allowed") throw new Error("Diff instance is no longer active");
  };
  const args = Arguments.parse(context.invocation.arguments);
  type Comparison = { description: DiffDescriptor; rows: readonly DiffRow[]; oldOffsets: number[]; newOffsets: number[] };
  let result: Comparison | undefined, release!: () => void;
  const ready = new Promise<void>(resolve => { release = resolve; });
  function* prepare(): Generator<void, Comparison> {
    check(); const before = lines(args.before); yield;
    check(); const after = lines(args.after); yield;
    const rows = yield* compareSteps(before, after, check);
    const oldOffsets = [0], newOffsets = [0]; let added = 0, removed = 0;
    for (let i = 0; i < rows.length; i++) {
      const row = rows[i]!;
      if (row.kind === "insert") added++;
      if (row.kind === "delete") removed++;
      oldOffsets.push(oldOffsets[oldOffsets.length - 1]! + (row.kind === "insert" ? 0 : 1));
      newOffsets.push(newOffsets[newOffsets.length - 1]! + (row.kind === "delete" ? 0 : 1));
      if (i % 32 === 31) { check(); yield; }
    }
    check(); yield;
    const description = DiffDescriptorCodec.parse({ viewId: context.instanceId, revision: context.epoch,
      title: "Text comparison", beforeLabel: args.beforeLabel, afterLabel: args.afterLabel, beforeLines: before.length, afterLines: after.length,
      added, removed, total: rows.length, pageSize: 3 });
    return { description, rows, oldOffsets, newOffsets };
  }
  const worker = prepare();
  const settled = async () => { await ready; check(); if (!result) throw new Error("Diff preparation is not complete"); return result; };
  context.session.register(DiffDescribeMethod, async (_input, request) => {
    const value = await settled(); request.check(); check(); return value.description;
  });
  context.session.register(DiffPageMethod, async (input, request) => {
    const { description, rows, oldOffsets, newOffsets } = await settled(); request.check(); check();
    if (input.viewId !== description.viewId || input.revision !== description.revision || input.offset > rows.length ||
        input.limit > description.pageSize) throw new Error("Stale or invalid diff page");
    return { viewId: description.viewId, revision: description.revision, offset: input.offset, total: rows.length,
      beforeOffset: oldOffsets[input.offset]!, afterOffset: newOffsets[input.offset]!, rows: rows.slice(input.offset, input.offset + input.limit) };
  });
  return { step() {
    if (!live || result) return;
    const next = worker.next(); if (next.done) { result = next.value; release(); }
  }, dispose() { live = false; release(); } };
}
