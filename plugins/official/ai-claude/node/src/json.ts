import { Buffer } from "node:buffer";
import { isProxy } from "node:util/types";

export type Json = null | boolean | number | string | readonly Json[] | { readonly [key: string]: Json };

export function text(value: unknown, maximum: number, empty = false): string {
  if (typeof value !== "string" || (!empty && value.length === 0) || /[\ud800-\udfff]/u.test(value) ||
      Buffer.byteLength(value) > maximum) throw new TypeError("Invalid Claude text field");
  return value;
}

/** Snapshot JSON without invoking accessors, toJSON or prototype behavior. */
export function snapshot(value: unknown, maximum = 65_536): Json {
  let nodes = 0;
  const active = new Set<object>();
  function copy(input: unknown, depth: number): Json {
    if (++nodes > 4096 || depth > 32) throw new RangeError("Claude JSON structure limit");
    if (input === null || typeof input === "boolean") return input;
    if (typeof input === "string") {
      if (/[\ud800-\udfff]/u.test(input)) throw new TypeError("Invalid Claude UTF-16");
      return input;
    }
    if (typeof input === "number" && Number.isFinite(input) && (!Number.isInteger(input) || Number.isSafeInteger(input))) return input;
    if (typeof input !== "object" || input === null || active.has(input)) throw new TypeError("Claude input must be plain JSON");
    if (isProxy(input)) throw new TypeError("Claude JSON proxy is unsupported");
    const array = Array.isArray(input);
    const prototype = Object.getPrototypeOf(input);
    if (prototype !== (array ? Array.prototype : Object.prototype) && !(prototype === null && !array)) {
      throw new TypeError("Claude JSON prototype is unsupported");
    }
    active.add(input);
    const descriptors = Object.getOwnPropertyDescriptors(input);
    if (Reflect.ownKeys(descriptors).some(key => typeof key !== "string")) throw new TypeError("Claude JSON symbol key");
    const output: Json[] | Record<string, Json> = array ? [] : Object.create(null) as Record<string, Json>;
    const keys = Object.keys(descriptors).filter(key => !array || key !== "length").sort();
    if (array && keys.length !== input.length) throw new TypeError("Claude JSON array has holes or extra fields");
    for (const key of keys) {
      text(key, 4096, true);
      if (["__proto__", "prototype", "constructor"].includes(key) ||
          (array && (!/^(0|[1-9][0-9]*)$/.test(key) || Number(key) >= input.length))) {
        throw new TypeError("Invalid Claude JSON key");
      }
      const descriptor = descriptors[key]!;
      if (!descriptor.enumerable || !("value" in descriptor)) throw new TypeError("Claude JSON accessor is unsupported");
      (output as Record<string, Json>)[key] = copy(descriptor.value, depth + 1);
    }
    active.delete(input);
    return Object.freeze(output);
  }
  const result = copy(value, 0);
  if (Buffer.byteLength(JSON.stringify(result)) > maximum) throw new RangeError("Claude JSON byte limit");
  return result;
}

export function record(value: Json): value is { readonly [key: string]: Json } {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
