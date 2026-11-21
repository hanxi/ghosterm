import { commandArguments, integer, option, TableDescribeMethod, TablePageMethod } from "@ghosterm/plugin-sdk";
import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";
import { LogStore } from "./log-store.js";
import { LogPageMethod } from "../shared.js";

const Arguments = commandArguments({ count: option(integer(1, 10000), "count", 5000) });
const Retention = 4096;
const PageSize = 4;

export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "logs" ||
      context.capabilities.query("session.rpc") !== "allowed") throw new Error("Log Executor invocation and RPC grant required");
  const { count } = Arguments.parse(context.invocation.arguments);
  const store = new LogStore(Retention, {
    count,
    message(sequence) {
      const level = sequence % 97 === 0 ? "WARN" : "INFO";
      return `${level} worker ${sequence % 8} completed sample record ${sequence}`;
    },
  });
  const total = Math.min(count, Retention);
  const first = count - total + 1;
  const viewId = context.instanceId;
  const revision = context.epoch;
  let live = true;
  function check(): void {
    if (!live || context.cancellation.cancelled || context.capabilities.query("session.rpc") !== "allowed") {
      throw new Error("Log instance is no longer active");
    }
  }
  context.session.register(TableDescribeMethod, (_input, request) => {
    request.check();
    check();
    return { viewId, revision, title: "Sample logs", total, pageSize: PageSize,
      columns: [{ id: "sequence", title: "Sequence" }, { id: "message", title: "Message" }] };
  });
  context.session.register(TablePageMethod, (input, request) => {
    request.check();
    check();
    const { offset, limit } = input;
    if (input.viewId !== viewId || input.revision !== revision || offset > total || limit > PageSize) {
      throw new Error("Stale or invalid log table page");
    }
    const page = store.page(first - 1 + offset, limit);
    request.check();
    return { viewId, revision, offset, total, rows: page.entries.map(({ sequence, message }) => {
      const id = String(sequence);
      return { id, cells: [id, message] };
    }) };
  });
  context.session.register(LogPageMethod, (input, request) => {
    request.check();
    check();
    // A zero upper bound opens a search snapshot, later requests retain its end.
    const page = store.page(input.after, input.limit, input.query, input.through || count);
    request.check();
    return page;
  });
  return { dispose() { live = false; } };
}
