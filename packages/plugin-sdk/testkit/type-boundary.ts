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
