/** Exact UTF-8 size using native regexp passes, with no per-code-unit JS loop.
 * Replacements contain only ASCII, so later passes cannot count them again.
 * Unicode mode treats a valid surrogate pair as one astral code point. */
export function utf8ByteLength(value: string): number {
  if (/[\ud800-\udfff]/u.test(value)) throw new TypeError("invalid UTF-16 string");
  return value.replace(/[\u0080-\u07ff]/g, "xx")
    .replace(/[\u0800-\uffff]/gu, "xxx")
    .replace(/[\u{10000}-\u{10ffff}]/gu, "xxxx").length;
}
