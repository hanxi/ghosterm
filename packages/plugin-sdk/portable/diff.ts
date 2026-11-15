import { Codec, encode, fail, integer, object, snapshot, text, uint64, utf8Bytes } from "./codec.js";
import type { Method } from "./session.js";

/** Candidate read-only line comparison. Ending and exact text are separate;
 * no normalization, patch application, filesystem identity or approval implied. */
export const DIFF_PAGE_ROWS_MAX = 3;
export const DIFF_LINES_MAX = 128;
export const DIFF_TEXT_MAX = 128;
export const DIFF_BODY_BYTES_MAX = 2048;

/** Build only the requested shape. Importing the aggregate SDK must not spend
 * every Runtime's activation slice constructing unused view schemas. */
function lazy<T>(build: () => Codec<T>): Codec<T> {
  let value: Codec<T> | undefined;
  const codec = () => value ??= build();
  return Object.freeze({ get schema() { return codec().schema; }, parse(input: unknown) { return codec().parse(input); } });
}

function choice<T extends string>(values: readonly T[]): Codec<T> {
  return Object.freeze({ schema: snapshot({ type: "string", enum: values }), parse(value: unknown) {
    if (typeof value !== "string" || !values.includes(value as T)) fail("DIFF_ENUM");
    return value as T;
  } });
}
function boundedText(maximum: number, line = false): Codec<string> {
  const codec = text(maximum);
  return Object.freeze({ schema: codec.schema, parse(value: unknown) {
    const result = codec.parse(value);
    if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(result) || (line && /[\r\n]/.test(result))) fail("DIFF_TEXT");
    return result;
  } });
}
const identity: Codec<string> = Object.freeze({ schema: snapshot({ type: "string", pattern: "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$" }),
  parse(value: unknown) {
    if (typeof value !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(value)) fail("DIFF_ID");
    return value;
  } });
const revision: Codec<string> = Object.freeze({ schema: snapshot({ type: "string", pattern: "^[1-9][0-9]{0,19}$" }),
  parse(value: unknown) { return uint64(value, true); } });
const lineCount = integer(0, DIFF_LINES_MAX), total = integer(0, 2 * DIFF_LINES_MAX);
const limit = integer(1, DIFF_PAGE_ROWS_MAX);
const descriptor = () => object({ viewId: identity, revision, title: boundedText(128), beforeLabel: boundedText(64), afterLabel: boundedText(64),
  beforeLines: lineCount, afterLines: lineCount, added: lineCount, removed: lineCount, total, pageSize: limit });
const request = () => object({ viewId: identity, revision, offset: total, limit });
const row = lazy(() => object({ kind: choice(["equal", "delete", "insert"] as const), beforeLine: lineCount, afterLine: lineCount,
  text: boundedText(DIFF_TEXT_MAX, true), ending: choice(["lf", "crlf", "cr", "none"] as const) }));
const rows: Codec<readonly ReturnType<typeof row.parse>[]> = lazy(() => Object.freeze({
  schema: snapshot({ type: "array", items: row.schema, maxItems: DIFF_PAGE_ROWS_MAX }), parse(value: unknown) {
    const copy = snapshot(value);
    if (!Array.isArray(copy) || copy.length > DIFF_PAGE_ROWS_MAX) fail("DIFF_ROWS");
    return Object.freeze(copy.map(item => row.parse(item)));
  } }));
const page = () => object({ viewId: identity, revision, offset: total, total, beforeOffset: lineCount, afterOffset: lineCount, rows });
export type DiffDescriptor = ReturnType<ReturnType<typeof descriptor>["parse"]>;
export type DiffPageRequest = ReturnType<ReturnType<typeof request>["parse"]>;
export type DiffRow = ReturnType<typeof row.parse>;
export type DiffPage = ReturnType<ReturnType<typeof page>["parse"]>;

function body<T>(codec: Codec<T>, validate: (value: T) => void): Codec<T> {
  return Object.freeze({ schema: codec.schema, parse(value: unknown): T {
    const result = codec.parse(value);
    if (utf8Bytes(encode(result)) > DIFF_BODY_BYTES_MAX) fail("DIFF_BYTES");
    validate(result); return result;
  } });
}
export const DiffDescriptorCodec: Codec<DiffDescriptor> = lazy(() => body(descriptor(), value => {
  if (!value.title || !value.beforeLabel || !value.afterLabel || value.total - value.added !== value.beforeLines ||
      value.total - value.removed !== value.afterLines || value.added > value.afterLines || value.removed > value.beforeLines) fail("DIFF_COUNTS");
}));
export const DiffPageRequestCodec: Codec<DiffPageRequest> = lazy(() => body(request(), () => {}));
export const DiffPageCodec: Codec<DiffPage> = lazy(() => body(page(), value => {
  if (value.offset > value.total || value.rows.length > value.total - value.offset ||
      (!value.rows.length && value.offset < value.total) ||
      value.beforeOffset + value.afterOffset < value.offset || value.beforeOffset + value.afterOffset > 2 * value.offset) fail("DIFF_RANGE");
  let before = value.beforeOffset, after = value.afterOffset;
  for (const item of value.rows) {
    if (item.beforeLine !== (item.kind === "insert" ? 0 : ++before) ||
        item.afterLine !== (item.kind === "delete" ? 0 : ++after)) fail("DIFF_COORDINATES");
  }
}));
export const DiffDescribeMethod: Method<Readonly<Record<string, never>>, DiffDescriptor> = Object.freeze({
  name: "view.diff.describe", input: object({}), output: DiffDescriptorCodec });
export const DiffPageMethod: Method<DiffPageRequest, DiffPage> = Object.freeze({
  name: "view.diff.page", input: DiffPageRequestCodec, output: DiffPageCodec });

/** Validate the exact selected page. A sequential consumer also supplies its
 * previous page, so a valid-looking new page cannot change the line cursors. */
export function validateDiffPage(description: DiffDescriptor, requested: DiffPageRequest, received: DiffPage, previous?: DiffPage): DiffPage {
  const d = DiffDescriptorCodec.parse(description), r = DiffPageRequestCodec.parse(requested), p = DiffPageCodec.parse(received);
  if (r.viewId !== d.viewId || p.viewId !== d.viewId || r.revision !== d.revision || p.revision !== d.revision) fail("DIFF_STALE");
  if (p.offset !== r.offset || p.total !== d.total || r.limit > d.pageSize ||
      p.rows.length !== Math.min(r.limit, d.total - r.offset) || p.beforeOffset > d.beforeLines || p.afterOffset > d.afterLines) fail("DIFF_PAGE");
  let before = p.beforeOffset, after = p.afterOffset;
  for (const item of p.rows) {
    if (item.kind !== "insert") before++;
    if (item.kind !== "delete") after++;
    if (before > d.beforeLines || after > d.afterLines || (item.ending === "none" &&
        ((item.kind !== "insert" && before !== d.beforeLines) || (item.kind !== "delete" && after !== d.afterLines)))) fail("DIFF_LINES");
  }
  if (p.offset + p.rows.length === d.total && (before !== d.beforeLines || after !== d.afterLines)) fail("DIFF_INCOMPLETE");
  if (previous !== undefined) {
    const prior = DiffPageCodec.parse(previous);
    if (prior.viewId !== d.viewId || prior.revision !== d.revision || prior.total !== d.total ||
        prior.offset + prior.rows.length !== p.offset ||
        prior.beforeOffset + prior.rows.filter(item => item.kind !== "insert").length !== p.beforeOffset ||
        prior.afterOffset + prior.rows.filter(item => item.kind !== "delete").length !== p.afterOffset) fail("DIFF_CONTINUITY");
  }
  return p;
}
