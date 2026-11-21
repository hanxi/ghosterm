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
