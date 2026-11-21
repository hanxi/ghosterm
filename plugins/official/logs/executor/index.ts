import { commandArguments, integer, option } from "@ghosterm/plugin-sdk";
import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";
import { LogStore } from "./log-store.js";
import { LogPageMethod } from "../shared.js";

const Arguments = commandArguments({ count: option(integer(1, 10000), "count", 5000) });

export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "logs" ||
      context.capabilities.query("session.rpc") !== "allowed") throw new Error("Log Executor invocation and RPC grant required");
  const { count } = Arguments.parse(context.invocation.arguments);
  const store = new LogStore(4096, {
    count,
    message(sequence) {
      const level = sequence % 97 === 0 ? "WARN" : "INFO";
      return `${level} worker ${sequence % 8} completed sample record ${sequence}`;
    },
  });
  let live = true;
  context.session.register(LogPageMethod, (input, request) => {
    request.check();
    if (!live || context.cancellation.cancelled || context.capabilities.query("session.rpc") !== "allowed") {
      throw new Error("Log instance is no longer active");
    }
    // A zero upper bound opens a search snapshot, later requests retain its end.
    const page = store.page(input.after, input.limit, input.query, input.through || count);
    request.check();
    return page;
  });
  return { dispose() { live = false; } };
}
