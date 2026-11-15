import { BoundaryError, DIFF_LINES_MAX, DIFF_TEXT_MAX, text, utf8Bytes } from "@ghosterm/plugin-sdk";
import type { DiffRow } from "@ghosterm/plugin-sdk";

type Line = Pick<DiffRow, "text" | "ending">;
function fail(code: string): never { throw new BoundaryError(code); }
/** Preserve every input code unit and line terminator, including no final EOL. */
export function lines(encoded: string): readonly Line[] {
  if (utf8Bytes(encoded) > 3072) fail("DIFF_INPUT_BYTES");
  const source = text(3072).parse(JSON.parse(encoded) as unknown);
  const result: Line[] = []; let at = 0;
  const separators = /\r\n|\n|\r/g;
  const append = (value: string, ending: Line["ending"]): void => {
    if ([...value].length > DIFF_TEXT_MAX || /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(value)) fail("DIFF_INPUT_LINE");
    result.push(Object.freeze({ text: value, ending }));
    if (result.length > DIFF_LINES_MAX) fail("DIFF_INPUT_LINES");
  };
  for (let match = separators.exec(source); match; match = separators.exec(source)) {
    append(source.slice(at, match.index), match[0] === "\r\n" ? "crlf" : match[0] === "\r" ? "cr" : "lf");
    at = match.index + match[0].length;
  }
  if (at < source.length) append(source.slice(at), "none");
  return Object.freeze(result);
}

/** Bounded LCS; deterministic deletion-first ties, no ambient I/O or dependency. */
export function* compareSteps(before: readonly Line[], after: readonly Line[], check: () => void): Generator<void, readonly DiffRow[]> {
  if (before.length > DIFF_LINES_MAX || after.length > DIFF_LINES_MAX) fail("DIFF_INPUT_LINES");
  check();
  // Intern exact lines once. The bounded quadratic loop compares integer IDs,
  // rather than repeatedly reading strings and calling a JS equality function.
  const identities = new Map<string, number>();
  const identify = (line: Line): number => {
    const key = line.ending + "\0" + line.text;
    let id = identities.get(key);
    if (id === undefined) { id = identities.size + 1; identities.set(key, id); }
    return id;
  };
  const oldIDs: number[] = [], newIDs: number[] = [];
  for (const [source, target] of [[before, oldIDs], [after, newIDs]] as const) {
    for (let i = 0; i < source.length; i++) {
      target.push(identify(source[i]!));
      if (i % 32 === 31) { check(); yield; }
    }
  }
  const equal = (i: number, j: number) => oldIDs[i] === newIDs[j];
  const rows: DiffRow[] = [];
  if (before.length === after.length && oldIDs.every((id, i) => id === newIDs[i])) {
    for (let i = 0; i < before.length; i++) {
      rows.push(Object.freeze({ ...before[i]!, kind: "equal", beforeLine: i + 1, afterLine: i + 1 }));
      if (i % 32 === 31) { check(); yield; }
    }
    check(); return Object.freeze(rows);
  }
  const present = new Set(oldIDs);
  if (!newIDs.some(id => present.has(id))) {
    for (let i = 0; i < before.length; i++) {
      rows.push(Object.freeze({ ...before[i]!, kind: "delete", beforeLine: i + 1, afterLine: 0 }));
      if (i % 32 === 31) { check(); yield; }
    }
    for (let j = 0; j < after.length; j++) {
      rows.push(Object.freeze({ ...after[j]!, kind: "insert", beforeLine: 0, afterLine: j + 1 }));
      if (j % 32 === 31) { check(); yield; }
    }
    check(); return Object.freeze(rows);
  }
  const width = after.length + 1, matrix = new Uint16Array((before.length + 1) * width);
  for (let i = before.length - 1; i >= 0; i--) {
    if (i % 32 === 0) { check(); yield; }
    for (let j = after.length - 1; j >= 0; j--) matrix[i * width + j] = oldIDs[i] === newIDs[j] ?
      1 + matrix[(i + 1) * width + j + 1]! : Math.max(matrix[(i + 1) * width + j]!, matrix[i * width + j + 1]!);
  }
  let i = 0, j = 0;
  while (i < before.length || j < after.length) {
    if (i < before.length && j < after.length && equal(i, j)) {
      rows.push(Object.freeze({ ...before[i]!, kind: "equal", beforeLine: ++i, afterLine: ++j }));
    } else if (i < before.length && (j === after.length || matrix[(i + 1) * width + j]! >= matrix[i * width + j + 1]!)) {
      rows.push(Object.freeze({ ...before[i]!, kind: "delete", beforeLine: ++i, afterLine: 0 }));
    } else rows.push(Object.freeze({ ...after[j]!, kind: "insert", beforeLine: 0, afterLine: ++j }));
    if (rows.length % 32 === 0) { check(); yield; }
  }
  check(); return Object.freeze(rows);
}

/** Synchronous developer helper; Native Executor advances compareSteps once per pump. */
export function compare(before: readonly Line[], after: readonly Line[], check: () => void): readonly DiffRow[] {
  const work = compareSteps(before, after, check);
  for (;;) { const result = work.next(); if (result.done) return result.value; }
}
