/** Portable values only. No DOM, Node, filesystem, timers or native handles. */
export class BoundaryError extends Error {
  constructor(readonly code: string) { super(code); this.name = "BoundaryError"; }
}
export type Json = null | boolean | number | string | readonly Json[] | { readonly [key: string]: Json };
export interface Codec<T> { readonly schema: Json; parse(value: unknown): T; }
export function fail(code: string): never { throw new BoundaryError(code); }
export function utf8Bytes(value: string): number {
  let bytes = 0;
  for (let i = 0; i < value.length; i++) {
    const c = value.charCodeAt(i);
    if (c >= 0xd800 && c <= 0xdbff) {
      const low = value.charCodeAt(++i);
      if (!(low >= 0xdc00 && low <= 0xdfff)) fail("UTF16");
      bytes += 4;
    } else if (c >= 0xdc00 && c <= 0xdfff) fail("UTF16");
    else bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
  }
  return bytes;
}
/** Canonical unsigned decimal; never round a native uint64 through Number. */
export function uint64(value: unknown, positive = false): string {
  if (typeof value !== "string" || !/^(0|[1-9][0-9]{0,19})$/.test(value) ||
      (value.length === 20 && value > "18446744073709551615") || (positive && value === "0")) fail("UINT64");
  return value;
}
export function compareUint64(a: string, b: string): -1 | 0 | 1 {
  uint64(a); uint64(b); return a === b ? 0 : a.length < b.length || (a.length === b.length && a < b) ? -1 : 1;
}
/** Copy plain JSON without invoking accessors/toJSON. Snapshot before codecs or
 * Promise suspension. The native parser still validates raw wire/duplicates. */
export function snapshot(value: unknown): Json {
  let nodes = 0;
  const active = new Set<object>();
  function copy(v: unknown, depth: number): Json {
    if (++nodes > 4096 || depth > 32) return fail("JSON_LIMIT");
    if (v === null || typeof v === "boolean") return v;
    if (typeof v === "string") { utf8Bytes(v); return v; }
    if (typeof v === "number") {
      if (!Number.isFinite(v) || (Number.isInteger(v) && !Number.isSafeInteger(v))) fail("JSON_NUMBER");
      return v;
    }
    if (typeof v !== "object" || active.has(v)) return fail("JSON_VALUE");
    const array = Array.isArray(v), proto: unknown = Object.getPrototypeOf(v);
    if (!array && proto !== Object.prototype && proto !== null) fail("JSON_OBJECT");
    if (Object.getOwnPropertySymbols(v).length) fail("JSON_SYMBOL");
    active.add(v);
    const descriptors = Object.getOwnPropertyDescriptors(v);
    const out: { [key: string]: Json } = Object.create(null) as { [key: string]: Json };
    const items: Json[] = [];
    let count = 0;
    for (const key of Object.keys(descriptors)) {
      if (array && key === "length") continue;
      utf8Bytes(key);
      if (["__proto__", "constructor", "prototype"].includes(key)) fail("JSON_KEY");
      const d = descriptors[key];
      if (!d || !d.enumerable || !("value" in d)) fail("JSON_ACCESSOR");
      const child: unknown = d.value;
      if (array) {
        if (key !== String(count++)) fail("JSON_ARRAY");
        items.push(copy(child, depth + 1));
      } else out[key] = copy(child, depth + 1);
    }
    if (array && count !== v.length) fail("JSON_ARRAY");
    active.delete(v);
    return Object.freeze(array ? items : out);
  }
  return copy(value, 0);
}
export function encode(value: unknown): string {
  const text = JSON.stringify(snapshot(value));
  if (utf8Bytes(text) > 3072) fail("BODY_LIMIT");
  return text;
}
export function decode(text: string): Json {
  if (utf8Bytes(text) > 3072) fail("BODY_LIMIT");
  const value: unknown = JSON.parse(text); return snapshot(value);
}
export function text(maxLength = 1024): Codec<string> {
  if (!Number.isSafeInteger(maxLength) || maxLength < 0 || maxLength > 3072) fail("SCHEMA_LIMIT");
  return Object.freeze({ schema: Object.freeze({ type: "string", maxLength }), parse(v: unknown) {
    if (typeof v !== "string" || [...v].length > maxLength) return fail("SCHEMA");
    utf8Bytes(v); return v;
  } });
}
export function integer(minimum: number, maximum: number): Codec<number> {
  if (!Number.isSafeInteger(minimum) || !Number.isSafeInteger(maximum) || minimum > maximum) fail("SCHEMA_LIMIT");
  return Object.freeze({ schema: Object.freeze({ type: "integer", minimum, maximum }), parse(v: unknown) {
    if (typeof v !== "number" || !Number.isSafeInteger(v) || v < minimum || v > maximum) return fail("SCHEMA");
    return v;
  } });
}
type Parsed<F extends Readonly<Record<string, Codec<unknown>>>> = { readonly [K in keyof F]: F[K] extends Codec<infer T> ? T : never };
export function object<F extends Readonly<Record<string, Codec<unknown>>>>(fields: F): Codec<Parsed<F>> {
  const frozen = Object.freeze({ ...fields }), keys = Object.keys(frozen);
  if (keys.length > 64 || keys.some(k => ["__proto__", "constructor", "prototype"].includes(k))) fail("SCHEMA_LIMIT");
  const properties: { [key: string]: Json } = Object.create(null) as { [key: string]: Json };
  for (const k of keys) { const codec = frozen[k]; if (!codec) fail("SCHEMA"); properties[k] = codec.schema; }
  const schema = snapshot({ type: "object", properties, required: keys, additionalProperties: false });
  return Object.freeze({ schema, parse(v: unknown): Parsed<F> {
    if (!v || typeof v !== "object" || Array.isArray(v)) return fail("SCHEMA");
    const values = snapshot(v) as Readonly<Record<string, unknown>>, actual = Object.keys(values);
    if (actual.length !== keys.length || actual.some(k => !Object.prototype.hasOwnProperty.call(frozen, k))) fail("SCHEMA");
    const out: { [key: string]: unknown } = Object.create(null) as { [key: string]: unknown };
    for (const k of keys) { const codec = frozen[k]; if (!codec) fail("SCHEMA"); out[k] = codec.parse(values[k]); }
    return Object.freeze(out) as Parsed<F>;
  } });
}
