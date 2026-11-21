import { boolean, integer, object, snapshot, text } from "@ghosterm/plugin-sdk";
import type { Codec, Method } from "@ghosterm/plugin-sdk";
import type { LogPage } from "./executor/log-store.js";

const sequence = integer(0, Number.MAX_SAFE_INTEGER);
const entry = object({ sequence: integer(1, Number.MAX_SAFE_INTEGER), message: text(128) });
const entries: Codec<LogPage["entries"]> = Object.freeze({
  schema: snapshot({ type: "array", items: entry.schema, maxItems: 4 }),
  parse(value: unknown) {
    if (!Array.isArray(value) || value.length > 4) throw new RangeError("Invalid log entries");
    return Object.freeze(value.map(item => entry.parse(item)));
  },
});
const page = object({ entries, cursor: sequence, oldest: integer(1, Number.MAX_SAFE_INTEGER),
  latest: sequence, truncated: boolean() });
export const LogRead = object({ after: sequence, through: sequence, limit: integer(1, 4), query: text(64) });
export const LogPageMethod: Method<ReturnType<typeof LogRead.parse>, LogPage> = Object.freeze({
  name: "logs.page",
  input: LogRead,
  output: page,
});

const searchInput = object({ viewId: text(64), revision: text(20),
  after: sequence, through: sequence, query: text(64) });
const cell = text(256);
const row = object({ id: text(64), cells: {
  schema: snapshot({ type: "array", maxItems: 8, items: cell.schema }),
  parse(value: unknown) {
    if (!Array.isArray(value) || value.length < 1 || value.length > 8) throw new RangeError("Invalid search cells");
    return Object.freeze(value.map(item => cell.parse(item)));
  },
} });
const searchOutput = object({ viewId: text(64), revision: text(20), cursor: sequence,
  latest: sequence, truncated: boolean(), rows: {
    schema: snapshot({ type: "array", maxItems: 4, items: row.schema }),
    parse(value: unknown) {
      if (!Array.isArray(value) || value.length > 4) throw new RangeError("Invalid search rows");
      return Object.freeze(value.map(item => row.parse(item)));
    },
  } });
export const TableSearchMethod: Method<ReturnType<typeof searchInput.parse>, ReturnType<typeof searchOutput.parse>> =
  Object.freeze({ name: "view.table.search", input: searchInput, output: searchOutput });
