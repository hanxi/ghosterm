import { Codec, encode, fail, integer, object, snapshot, text, uint64, utf8Bytes } from "./codec.js";
import type { Method } from "./session.js";

/** Candidate standard table values, independent of a renderer or OS. Native
 * Hosts must advertise ui.table before a Client contribution can be activated.
 * Structural schemas are followed by identity, range and aggregate-byte checks. */
export const TABLE_PAGE_ROWS_MAX = 16;
export const TABLE_COLUMNS_MAX = 8;
export const TABLE_BODY_BYTES_MAX = 2048;

function list<T>(item: Codec<T>, maximum: number, minimum = 0): Codec<readonly T[]> {
  return Object.freeze({ schema: snapshot({ type: "array", items: item.schema, minItems: minimum, maxItems: maximum }),
    parse(value: unknown): readonly T[] {
      const copy = snapshot(value);
      if (!Array.isArray(copy) || copy.length < minimum || copy.length > maximum) fail("TABLE_ARRAY");
      return Object.freeze(copy.map(v => item.parse(v)));
    } });
}
function boundedText(maximum: number): Codec<string> {
  const codec = text(maximum);
  return Object.freeze({ schema: codec.schema, parse(value: unknown) {
    const result = codec.parse(value);
    if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(result)) fail("TABLE_TEXT");
    return result;
  } });
}
const identity: Codec<string> = Object.freeze({ schema: snapshot({ type: "string", pattern: "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$" }),
  parse(value: unknown) {
    if (typeof value !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(value)) fail("TABLE_ID");
    return value;
  } });
const revision: Codec<string> = Object.freeze({ schema: snapshot({ type: "string", pattern: "^[1-9][0-9]{0,19}$" }),
  parse(value: unknown) { return uint64(value, true); } });
const total = integer(0, 1000000), offset = integer(0, 1000000), limit = integer(1, TABLE_PAGE_ROWS_MAX);
const column = object({ id: identity, title: boundedText(64) });
const row = object({ id: identity, cells: list(boundedText(256), TABLE_COLUMNS_MAX, 1) });
const descriptor = object({ viewId: identity, revision, title: boundedText(128),
  columns: list(column, TABLE_COLUMNS_MAX, 1), total, pageSize: limit });
const request = object({ viewId: identity, revision, offset, limit });
const page = object({ viewId: identity, revision, offset, total, rows: list(row, TABLE_PAGE_ROWS_MAX) });
export type TableDescriptor = ReturnType<typeof descriptor.parse>;
export type TablePageRequest = ReturnType<typeof request.parse>;
export type TablePage = ReturnType<typeof page.parse>;

function body<T>(codec: Codec<T>, validate: (value: T) => void): Codec<T> {
  return Object.freeze({ schema: codec.schema, parse(value: unknown): T {
    const result = codec.parse(value);
    if (utf8Bytes(encode(result)) > TABLE_BODY_BYTES_MAX) fail("TABLE_BYTES");
    validate(result); return result;
  } });
}
export const TableDescriptorCodec: Codec<TableDescriptor> = body(descriptor, value => {
  if (!value.title || value.columns.some(c => !c.title) || new Set(value.columns.map(c => c.id)).size !== value.columns.length) fail("TABLE_COLUMNS");
});
export const TablePageRequestCodec: Codec<TablePageRequest> = body(request, () => {});
export const TablePageCodec: Codec<TablePage> = body(page, value => {
  if (value.offset > value.total || value.rows.length > value.total - value.offset ||
      (value.rows.length === 0 && value.offset < value.total) || new Set(value.rows.map(r => r.id)).size !== value.rows.length) fail("TABLE_RANGE");
});
export const TableDescribeMethod: Method<Readonly<Record<string, never>>, TableDescriptor> = Object.freeze({
  name: "view.table.describe", input: object({}), output: TableDescriptorCodec });
export const TablePageMethod: Method<TablePageRequest, TablePage> = Object.freeze({
  name: "view.table.page", input: TablePageRequestCodec, output: TablePageCodec });

/** A response must still belong to the descriptor and exact selected page.
 * A cancelled/replaced UI request cannot be applied merely because it is valid
 * JSON. Callers retain their own current request generation as well. */
export function validateTablePage(description: TableDescriptor, requested: TablePageRequest, received: TablePage): TablePage {
  const d = TableDescriptorCodec.parse(description), r = TablePageRequestCodec.parse(requested), p = TablePageCodec.parse(received);
  if (r.viewId !== d.viewId || p.viewId !== d.viewId || r.revision !== d.revision || p.revision !== d.revision) fail("TABLE_STALE");
  if (p.offset !== r.offset || p.total !== d.total || r.limit > d.pageSize || p.rows.length > r.limit ||
      p.rows.length !== Math.min(r.limit, d.total - r.offset) || p.rows.some(item => item.cells.length !== d.columns.length)) fail("TABLE_PAGE");
  return p;
}
