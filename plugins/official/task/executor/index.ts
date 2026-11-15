import { commandArguments, integer, option, TaskCancelMethod, TaskDescribeMethod, TaskReadMethod } from "@ghosterm/plugin-sdk";
import type { Disposable, PluginContext, TaskCancelAck, TaskDescriptor } from "@ghosterm/plugin-sdk";
import { PrimeTask } from "./primes.js";

export const Arguments = commandArguments({ limit: option(integer(0, 1000000), "limit", 1000000) });
export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "task primes" ||
      context.capabilities.query("session.rpc") !== "allowed") throw new Error("Task Executor invocation and RPC grant required");
  let live = true;
  const check = () => {
    if (!live || context.cancellation.cancelled || context.capabilities.query("session.rpc") !== "allowed") throw new Error("Task instance is no longer active");
  };
  const args = Arguments.parse(context.invocation.arguments);
  // The Session output codecs validate these values at the actual wire boundary;
  // activation must not build the same schemas or snapshots a second time.
  const description: TaskDescriptor = Object.freeze({ viewId: context.instanceId, revision: context.epoch, title: "Count primes",
    unit: "candidates classified", cancellable: true });
  const job = new PrimeTask(args.limit);
  const same = (input: { viewId: string; revision: string }) => {
    if (input.viewId !== description.viewId || input.revision !== description.revision) throw new Error("Stale task request");
  };
  let cancellation: TaskCancelAck | undefined;
  context.session.register(TaskDescribeMethod, (_input, request) => { request.check(); check(); return description; });
  context.session.register(TaskReadMethod, (input, request) => {
    request.check(); check(); same(input);
    return { viewId: description.viewId, revision: description.revision, ...job.snapshot() };
  });
  context.session.register(TaskCancelMethod, (input, request) => {
    request.check(); check(); same(input);
    if (cancellation) {
      if (input.requestId !== cancellation.requestId) throw new Error("A cancellation request already owns this task");
      return cancellation;
    }
    request.check(); check();
    const kind = job.cancel();
    cancellation = Object.freeze({ viewId: description.viewId, revision: description.revision, requestId: input.requestId, kind });
    return cancellation;
  });
  return { step() { if (!live) return; check(); job.step(); }, dispose() { live = false; job.dispose(); } };
}
