import { BoundaryError, Codec, compareUint64, decode, encode, integer, object, snapshot, text, uint64, utf8Bytes } from "../portable/index.js";
import { Method, NativePort, NativeResult, Outcome, SessionChannel, Status } from "../portable/index.js";
declare function native(op: string, side: number, ...args: (string | number)[]): unknown;
const root = globalThis as typeof globalThis & { tick?: () => void };
function check(ok: unknown, label: string): asserts ok { if (!ok) throw new Error("ASSERT " + label); }
function rejects(fn: () => unknown, code: string): void {
  let got = ""; try { fn(); } catch (e) { if (e instanceof BoundaryError) got = e.code; }
  check(got === code, "reject " + code);
}
function port(side: number): NativePort {
  return {
    request: (method, json, ms) => native("request", side, method, json, ms) as ReturnType<NativePort["request"]>,
    result: h => native("result", side, h) as NativeResult,
    cancel: h => native("cancel", side, h) as Status,
    forget: h => native("forget", side, h) as Status,
    take: () => native("take", side) as ReturnType<NativePort["take"]>,
    check: h => native("check", side, h) as Status,
    respond: (h, json) => native("respond", side, h, json) as Status,
    revoke: () => { native("revoke", side); },
  };
}
const message = object({ text: text(40), count: integer(0, 100) });
type Message = ReturnType<typeof message.parse>;
const echo: Method<Message, Message> = { name: "fixture.echo", input: message, output: message };
const ask: Method<Message, Message> = { ...echo, name: "fixture.ask" };
const input = { text: "中文😀", count: 17 };
function success(v: Outcome<Message>, expected = input.text): void { check(v.kind === "success" && v.value.text === expected && v.value.count === 17, "business result"); }
async function cycles(n: number): Promise<void> {
  while (n-- > 0) await new Promise<void>(resolve => { const old = root.tick; root.tick = () => { if (old) root.tick = old; else delete root.tick; old?.(); resolve(); }; });
}
function codecs(): void {
  for (const name of ["process", "require", "window", "document", "fetch", "setTimeout", "std", "os"]) check(!(name in globalThis), "no ambient " + name);
  check(utf8Bytes("a中😀") === 8 && compareUint64("9007199254740993", "9007199254740992") === 1, "utf8 and exact counters");
  check(uint64("18446744073709551615") === "18446744073709551615", "uint64 max");
  for (const v of ["01", "-1", "18446744073709551616", 9007199254740992, "1e2"]) rejects(() => uint64(v), "UINT64");
  rejects(() => utf8Bytes("\ud800"), "UTF16"); rejects(() => utf8Bytes("\udfff"), "UTF16");
  for (const v of [NaN, Infinity, 9007199254740992]) rejects(() => encode(v), "JSON_NUMBER");
  const cycle: { self?: unknown } = {}; cycle.self = cycle; rejects(() => snapshot(cycle), "JSON_VALUE");
  let invoked = 0; const accessor = { get value() { invoked++; return 1; } };
  rejects(() => snapshot(accessor), "JSON_ACCESSOR"); check(invoked === 0, "getter not invoked");
  rejects(() => snapshot({ toJSON() { invoked++; return {}; } }), "JSON_VALUE"); check(invoked === 0, "toJSON not invoked");
  rejects(() => snapshot([, 1]), "JSON_ARRAY"); rejects(() => snapshot(new Date()), "JSON_OBJECT");
  rejects(() => snapshot({ [Symbol()]: 1 }), "JSON_SYMBOL"); rejects(() => decode('{"__proto__":1}'), "JSON_KEY");
  rejects(() => encode("中".repeat(1024)), "BODY_LIMIT");
  check(utf8Bytes(encode("中".repeat(1023))) === 3071, "body byte bound");
  const source = { a: { text: "old" } }, copy = snapshot(source); source.a.text = "new";
  check(JSON.stringify(copy) === '{"a":{"text":"old"}}' && Object.isFrozen(copy), "snapshot immutable");
  rejects(() => message.parse({ ...input, extra: true }), "SCHEMA"); rejects(() => message.parse({ text: "x", count: 101 }), "SCHEMA");
  check(text(1).parse("😀") === "😀", "unicode scalar schema length");
  rejects(() => text(1).parse("😀😀"), "SCHEMA");
}
export async function run(mode: string): Promise<void> {
  if (mode === "codec") { codecs(); return; }
  const h = new SessionChannel(port(0)), e = new SessionChannel(port(1));
  root.tick = () => { h.step(); e.step(); };
  if (mode === "roundtrip") {
    e.register(echo, value => { native("effect", 1); return value; });
    h.register(ask, value => { native("effect", 0); return value; });
    const mutable = { ...input }, call = h.request(echo, mutable, 1000); mutable.text = "changed";
    success(await call.outcome); success(await e.request(ask, input, 1000).outcome);
    check(native("effects", 0) === 2 && h.retained === 0 && e.retained === 0, "two directions released");
  } else if (mode === "input") {
    const before = native("requests", 0);
    rejects(() => h.request(echo, { text: "x", count: 101 }, 1000), "SCHEMA");
    rejects(() => h.request(echo, { text: "x", count: 1 }, 0), "TIMEOUT");
    check(native("requests", 0) === before, "invalid not admitted");
  } else if (mode === "incoming-schema") {
    e.register(echo, value => { native("effect", 1); return value; });
    const loose: Codec<Message> = { schema: {}, parse: value => value as Message };
    const v = await h.request({ ...echo, input: loose }, { text: "x", count: 101 }, 1000).outcome;
    check(v.kind === "failed" && v.error.code === "INVALID_INPUT" && v.error.effect === "none", "incoming full schema");
    check(native("effects", 0) === 0, "invalid schema no effect");
  } else if (mode === "output-schema") {
    e.register(echo, () => ({ text: "x", count: 101 }));
    const v = await h.request(echo, input, 1000).outcome;
    check(v.kind === "unknown", "invalid output not success");
  } else if (mode === "result-schema") {
    const call = h.request(echo, input, 1000); native("reply", 0, '{"kind":"success","value":{"wrong":1}}');
    const v = await call.outcome; check(v.kind === "unknown" && v.reason === "INVALID_OUTCOME", "result full schema");
  } else if (mode === "queued-cancel" || mode === "queued-timeout") {
    const call = h.request(echo, input, 20);
    if (mode === "queued-cancel") call.cancel(); else native("advance", 0, 20);
    const v = await call.outcome;
    check(mode === "queued-cancel" ? v.kind === "cancelled" && v.effect === "none" : v.kind === "failed" && v.error.code === "EXPIRED" && v.error.effect === "none", "queued terminal fact");
    check(h.retained === 0 && native("effects", 0) === 0, "queued released");
  } else if (["sent-cancel", "sent-timeout", "dispose", "stale"].includes(mode)) {
    let resume: () => void = () => {}, started = false;
    e.register(echo, async (value, context) => {
      started = true; await new Promise<void>(r => { resume = r; }); context.check(); native("effect", 1); return value;
    });
    const call = h.request(echo, input, 20); while (!started) await cycles(1);
    if (mode === "sent-cancel") call.cancel();
    else if (mode === "sent-timeout") native("advance", 0, 20);
    else if (mode === "stale") native("stale", 1);
    else e.dispose();
    await cycles(4); resume();
    if (mode === "stale") { await cycles(4); e.dispose(); }
    const v = await call.outcome; await cycles(4);
    check(v.kind === "unknown" && native("effects", 0) === 0, "late effect refused");
    if (mode === "dispose" || mode === "stale") check(e.retained === 0, "disposed handlers released");
    if (mode === "dispose") rejects(() => e.request(ask, input, 1000), "CLOSED");
  } else if (mode === "admission") {
    const denied = await h.request({ ...echo, name: "fixture.denied" }, input, 1000).outcome;
    const missing = await h.request({ ...echo, name: "fixture.missing" }, input, 1000).outcome;
    check(denied.kind === "failed" && denied.error.code === "DENIED" && denied.error.effect === "none", "native denied");
    check(missing.kind === "failed" && missing.error.code === "UNSUPPORTED", "native unsupported");
  } else if (mode === "capacity") {
    const calls = Array.from({ length: 64 }, () => h.request(echo, input, 1000));
    rejects(() => h.request(echo, input, 1000), "BUSY");
    for (const c of calls) c.cancel();
    for (const c of calls) check((await c.outcome).kind === "cancelled", "bounded cancellation");
    check(h.retained === 0, "all observations forgotten");
  } else if (mode === "outcomes") {
    for (const raw of [
      '{"kind":"failed","error":{"code":"IO","message":"failure","effect":"possible"}}',
      '{"kind":"cancelled","effect":"possible"}', '{"kind":"unknown","reason":"lost"}',
    ]) {
      const call = h.request(echo, input, 1000); native("reply", 0, raw);
      const result = await call.outcome; check(JSON.stringify(result) === raw, "outcome preserved");
    }
  } else if (mode === "confirmation") {
    const fake: NativePort = { ...port(0), request: () => ({ status: "ok", handle: "1" }),
      result: () => ({ state: "succeeded", confirmed: false, outcome: '{"kind":"success","value":{"text":"中文😀","count":17}}' }), forget: () => "ok" };
    const s = new SessionChannel(fake), call = s.request(echo, input, 1000); s.step();
    const v = await call.outcome; check(v.kind === "unknown", "no fake publication receipt");
  } else throw new Error("unknown case");
  delete root.tick;
}
