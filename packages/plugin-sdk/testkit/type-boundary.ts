import { integer, object, text } from "../portable/codec.js";
import { SessionChannel } from "../portable/session.js";
declare const session: SessionChannel;
const method = { name: "typed.echo", input: object({ value: text() }), output: object({ count: integer(0, 100) }) };
const call = session.request(method, { value: "ok" }, 1000);
void call.outcome.then(result => { if (result.kind === "success") { const count: number = result.value.count; void count; } });
// @ts-expect-error schema input is checked statically as well as at runtime
session.request(method, { value: 1 }, 1000);
// @ts-expect-error portable code has no DOM
document.title = "bad";
// @ts-expect-error portable code has no Node globals
require("node:fs");
// @ts-expect-error portable code has no ambient timers
setTimeout(() => {}, 1);

import type { PluginContext, CapabilityAccess, PluginEntry } from "../portable/index.js";
declare const context: PluginContext;
const access: CapabilityAccess = context.capabilities.query("session.rpc"); void access;
const entry: PluginEntry = { activate(ctx) { const side: "client" | "executor" = ctx.side; void side; return { dispose() {} }; } }; void entry;
// @ts-expect-error observation is not boolean consent
const approved: boolean = context.capabilities.query("session.rpc"); void approved;
// @ts-expect-error activation metadata is immutable
context.side = "client";
// @ts-expect-error no unimplemented ambient process service
context.process.spawn("tool");

// @ts-expect-error raw arguments are immutable
context.invocation?.arguments.push("late");
// @ts-expect-error invocation arguments are strings, not decoded typed options
const argumentCount: number | undefined = context.invocation?.arguments[0]; void argumentCount;
