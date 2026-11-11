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

export function boolean(): Codec<boolean> {
  return Object.freeze({ schema: Object.freeze({ type: "boolean" }), parse(v: unknown) {
    if (typeof v !== "boolean") return fail("SCHEMA"); return v;
  } });
}
type Scalar = string | number | boolean;
export interface Argument<T extends Scalar> {
  readonly codec: Codec<T>;
  readonly position?: number;
  readonly option?: string;
  readonly fallback?: T;
}
export function positional<T extends Scalar>(codec: Codec<T>, position: number): Argument<T> {
  if (!Number.isSafeInteger(position) || position < 0 || position >= 32) fail("ARGUMENT_SCHEMA");
  return Object.freeze({ codec, position });
}
export function option<T extends Scalar>(codec: Codec<T>, name: string, fallback: T): Argument<T> {
  if (!/^[a-z][a-z0-9-]{0,63}$/.test(name)) fail("ARGUMENT_SCHEMA");
  return Object.freeze({ codec, option: name, fallback: codec.parse(snapshot(fallback)) });
}
type ArgumentValues<F extends Readonly<Record<string, Argument<Scalar>>>> = {
  readonly [K in keyof F]: F[K] extends Argument<infer T> ? T : never;
};
/** Candidate argv mapping, generated alongside the value schema. No shell,
 * environment, filesystem, aliases, short options or implicit coercion. */
export function commandArguments<F extends Readonly<Record<string, Argument<Scalar>>>>(fields: F): Codec<ArgumentValues<F>> {
  const keys = Object.keys(fields), positions: string[] = [], options = new Map<string, string>();
  const codecs: Record<string, Codec<Scalar>> = Object.create(null) as Record<string, Codec<Scalar>>;
  const kinds = new Map<string, string>(), defaults: Record<string, Scalar> = Object.create(null) as Record<string, Scalar>;
  const mapping: Record<string, Json> = Object.create(null) as Record<string, Json>;
  if (keys.length > 32) fail("ARGUMENT_SCHEMA");
  for (const key of keys) {
    if (!/^[a-zA-Z][a-zA-Z0-9_]{0,63}$/.test(key) || ["constructor", "prototype"].includes(key)) fail("ARGUMENT_SCHEMA");
    const field = fields[key]; if (!field) fail("ARGUMENT_SCHEMA");
    const shape = snapshot(field.codec.schema);
    if (!shape || typeof shape !== "object" || Array.isArray(shape)) fail("ARGUMENT_SCHEMA");
    const type = (shape as Readonly<Record<string, Json>>).type;
    if (type !== "string" && type !== "integer" && type !== "boolean") fail("ARGUMENT_SCHEMA");
    codecs[key] = field.codec; kinds.set(key, type);
    if (field.position !== undefined) {
      if (field.option !== undefined || field.fallback !== undefined || !Number.isSafeInteger(field.position) || field.position < 0 || field.position >= 32 || positions[field.position] !== undefined) fail("ARGUMENT_SCHEMA");
      positions[field.position] = key; mapping[key] = { position: field.position };
    } else {
      if (!field.option || !/^[a-z][a-z0-9-]{0,63}$/.test(field.option) || options.has(field.option) || field.fallback === undefined) fail("ARGUMENT_SCHEMA");
      defaults[key] = field.codec.parse(snapshot(field.fallback)); options.set(field.option, key);
      mapping[key] = { option: field.option, default: defaults[key] as Scalar };
    }
  }
  for (let i = 0; i < positions.length; i++) if (!positions[i]) fail("ARGUMENT_SCHEMA");
  const values = object(codecs);
  const schema = snapshot({ ...(values.schema as Readonly<Record<string, Json>>), "x-ghosterm-cli": { version: 1, fields: mapping } });
  return Object.freeze({ schema, parse(input: unknown): ArgumentValues<F> {
    const argv = snapshot(input);
    if (!Array.isArray(argv) || argv.length > 32) fail("ARGUMENT_LIMIT");
    let bytes = 0;
    for (const arg of argv) {
      if (typeof arg !== "string" || arg.includes("\0")) fail("ARGUMENT_VALUE");
      const n = utf8Bytes(arg); bytes += n + 1;
      if (n > 4096 || bytes > 16384) fail("ARGUMENT_LIMIT");
    }
    const result: Record<string, Scalar> = { ...defaults }, seen = new Set<string>();
    let position = 0, literal = false;
    for (let i = 0; i < argv.length; i++) {
      const arg = argv[i] as string;
      if (!literal && arg === "--") { literal = true; continue; }
      let key: string | undefined, value: string;
      if (!literal && arg.startsWith("--")) {
        const equals = arg.indexOf("="), name = arg.slice(2, equals < 0 ? undefined : equals);
        key = options.get(name); if (!key) fail("ARGUMENT_UNKNOWN");
        if (equals >= 0) value = arg.slice(equals + 1);
        else if (kinds.get(key) === "boolean") value = "true";
        else {
          const next = argv[++i];
          if (typeof next !== "string" || next.startsWith("--")) fail("ARGUMENT_MISSING");
          value = next;
        }
      } else {
        key = positions[position++]; if (!key) fail("ARGUMENT_EXTRA"); value = arg;
      }
      const resolvedKey: string = key ?? fail("ARGUMENT_SCHEMA");
      if (seen.has(resolvedKey)) fail("ARGUMENT_DUPLICATE"); seen.add(resolvedKey);
      let decoded: Scalar = value;
      if (kinds.get(resolvedKey) === "integer") {
        if (!/^-?(0|[1-9][0-9]*)$/.test(value) || value === "-0" || !Number.isSafeInteger(Number(value))) fail("ARGUMENT_VALUE");
        decoded = Number(value);
      } else if (kinds.get(resolvedKey) === "boolean") {
        if (value !== "true" && value !== "false") fail("ARGUMENT_VALUE"); decoded = value === "true";
      }
      const codec = codecs[resolvedKey]; if (!codec) fail("ARGUMENT_SCHEMA"); result[resolvedKey] = codec.parse(decoded);
    }
    if (position !== positions.length) fail("ARGUMENT_MISSING");
    return values.parse(result) as ArgumentValues<F>;
  } });
}
