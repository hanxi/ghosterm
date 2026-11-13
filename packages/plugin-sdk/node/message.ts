/** Private child protocol lexical check. Preserve decoded duplicate-key checks
 * before JSON.parse can erase them. Whole-message limits include whitespace. */
import { fail, Json, snapshot, utf8Bytes } from "../portable/codec.js";

export function parseMessage(text: string): Json {
  if (!text.length || utf8Bytes(text) > 8192) return fail("CHILD_MESSAGE_LIMIT");
  let at = 0, nodes = 0;
  const space = (): void => { while (/[ \t\r\n]/.test(text[at] ?? "x")) at++; };
  const string = (): string => {
    if (text[at] !== '"') return fail("CHILD_JSON");
    const start = at++;
    for (;;) {
      const char = text[at++];
      if (char === undefined || (char.charCodeAt(0) < 32)) return fail("CHILD_JSON");
      if (char === '"') break;
      if (char === "\\") {
        const escape = text[at++];
        if (escape === "u") {
          if (!/^[0-9a-fA-F]{4}$/.test(text.slice(at, at + 4))) return fail("CHILD_JSON");
          at += 4;
        } else if (escape === undefined || !'"\\/bfnrt'.includes(escape)) return fail("CHILD_JSON");
      }
    }
    const value: unknown = JSON.parse(text.slice(start, at));
    if (typeof value !== "string") return fail("CHILD_JSON");
    utf8Bytes(value); return value;
  };
  const value = (depth: number): void => {
    if (++nodes > 4096 || depth > 32) return fail("CHILD_JSON_LIMIT");
    space();
    const char = text[at];
    if (char === '"') { string(); return; }
    if (char === "{" || char === "[") {
      const object = char === "{", close = object ? "}" : "]";
      const seen = new Set<string>();
      at++; space();
      if (text[at] === close) { at++; return; }
      for (;;) {
        if (object) {
          const key = string();
          if (seen.has(key)) return fail("CHILD_DUPLICATE");
          seen.add(key); space();
          if (text[at++] !== ":") return fail("CHILD_JSON");
        }
        value(depth + 1); space();
        if (text[at] === close) { at++; return; }
        if (text[at++] !== ",") return fail("CHILD_JSON");
        space();
      }
    }
    for (const literal of ["true", "false", "null"]) {
      if (text.startsWith(literal, at)) { at += literal.length; return; }
    }
    const number = /^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/.exec(text.slice(at));
    if (!number) return fail("CHILD_JSON");
    at += number[0].length;
  };
  value(0); space();
  if (at !== text.length) return fail("CHILD_JSON");
  return snapshot(JSON.parse(text) as unknown);
}
