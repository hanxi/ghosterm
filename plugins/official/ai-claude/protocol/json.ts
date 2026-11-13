/** JSON-only child protocol. No Node/DOM and no invocation of caller accessors. */
export type Json = null | boolean | number | string | readonly Json[] | { readonly [key: string]: Json };
export function record(value: unknown): value is { readonly [key: string]: Json } {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
export function byteLength(value: string): number {
  if (/[\ud800-\udfff]/u.test(value)) throw new TypeError("Malformed Claude UTF-16");
  return value.replace(/[\u0080-\u07ff]/g, "xx").replace(/[\u0800-\uffff]/gu, "xxx")
    .replace(/[\u{10000}-\u{10ffff}]/gu, "xxxx").length;
}
export function parse(text: string, canonical = false): Json {
  const value: unknown = JSON.parse(text);
  let nodes = 0;
  function check(item: unknown, depth: number): asserts item is Json {
    if (++nodes > 10_000 || depth > 40) throw new RangeError("Claude JSON structure limit");
    if (item === null || typeof item === "boolean") return;
    if (typeof item === "string") { byteLength(item); return; }
    if (typeof item === "number" && Number.isFinite(item) && (!Number.isInteger(item) || Number.isSafeInteger(item))) return;
    if (typeof item !== "object" || item === null) throw new TypeError("Invalid Claude JSON");
    for (const key of Object.keys(item)) {
      byteLength(key);
      if (["__proto__", "constructor", "prototype"].includes(key)) throw new TypeError("Reserved Claude JSON key");
      check((item as Record<string, unknown>)[key], depth + 1);
    }
    Object.freeze(item);
  }
  check(value, 0);
  if (canonical && JSON.stringify(value) !== text) throw new TypeError("Noncanonical Claude JSON");
  return value;
}
/** Node's inbound protocol sorts every object. Values must come from parse(). */
export function canonical(value: Json): string {
  function sort(item: Json): Json {
    if (Array.isArray(item)) return item.map(sort);
    if (!record(item)) return item;
    const result: Record<string, Json> = Object.create(null) as Record<string, Json>;
    for (const key of Object.keys(item).sort()) result[key] = sort(item[key]!);
    return result;
  }
  return JSON.stringify(sort(value));
}
/** Small outbound write slices; callers bound this input to 1024 UTF-16 units. */
export function encode(value: string): Uint8Array {
  const bytes: number[] = [];
  for (const char of value) {
    const cp = char.codePointAt(0)!;
    if (cp >= 0xd800 && cp <= 0xdfff) throw new TypeError("Malformed Claude UTF-16");
    if (cp < 128) bytes.push(cp);
    else if (cp < 2048) bytes.push(0xc0 | cp >> 6, 0x80 | cp & 63);
    else if (cp < 65536) bytes.push(0xe0 | cp >> 12, 0x80 | cp >> 6 & 63, 0x80 | cp & 63);
    else bytes.push(0xf0 | cp >> 18, 0x80 | cp >> 12 & 63, 0x80 | cp >> 6 & 63, 0x80 | cp & 63);
  }
  return Uint8Array.from(bytes);
}
