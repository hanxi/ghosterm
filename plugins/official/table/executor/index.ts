import { commandArguments, integer, option, TableDescribeMethod, TableDescriptorCodec, TablePageMethod } from "@ghosterm/plugin-sdk";
import type { Disposable, PluginContext, TablePageRequest } from "@ghosterm/plugin-sdk";

export const Arguments = commandArguments({ count: option(integer(0, 1000000), "count", 1000) });

/** Real computed sample data, generated a page at a time. No input file, child
 * process, account, filesystem, network API or whole-dataset allocation. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "table" ||
      context.capabilities.query("session.rpc") !== "allowed") throw new Error("Table Executor invocation and RPC grant required");
  const args = Arguments.parse(context.invocation.arguments);
  const description = TableDescriptorCodec.parse({ viewId: context.instanceId, revision: context.epoch,
    title: "Number powers", columns: [{ id: "n", title: "Number" }, { id: "square", title: "Square" }, { id: "cube", title: "Cube" }],
    total: args.count, pageSize: 16 });
  let live = true;
  const check = (): void => {
    if (!live || context.cancellation.cancelled || context.capabilities.query("session.rpc") !== "allowed") throw new Error("Table instance is no longer active");
  };
  context.session.register(TableDescribeMethod, (_input, request) => { request.check(); check(); return description; });
  context.session.register(TablePageMethod, (input: TablePageRequest, request) => {
    request.check(); check();
    if (input.viewId !== description.viewId || input.revision !== description.revision || input.offset > description.total || input.limit > description.pageSize) throw new Error("Stale or invalid table page");
    const count = Math.min(input.limit, description.total - input.offset);
    const rows = Array.from({ length: count }, (_, index) => {
      const n = BigInt(input.offset + index + 1);
      return { id: n.toString(), cells: [n.toString(), (n * n).toString(), (n * n * n).toString()] };
    });
    request.check(); check();
    // SessionChannel applies TablePageMethod.output before sending. Avoid
    // repeatedly copying/encoding the same page inside the handler as well.
    return { viewId: description.viewId, revision: description.revision,
      offset: input.offset, total: description.total, rows };
  });
  return { dispose() { live = false; } };
}
