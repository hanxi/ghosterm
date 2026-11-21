/** Instance-local log retention. A cursor is the last sequence inspected, so
 * filtered pages can advance even when none of the inspected lines matched. */
export interface LogEntry {
  readonly sequence: number;
  readonly message: string;
}

export interface LogPage {
  readonly entries: readonly LogEntry[];
  readonly cursor: number;
  readonly oldest: number;
  readonly latest: number;
  readonly truncated: boolean;
}

const MAX_RETAINED_LINES = 4096;
const MAX_LINE_UNITS = 512;
const MAX_QUERY_UNITS = 128;
const MAX_SCAN = 128;
const MAX_PAGE = 16;

export class LogStore {
  private readonly entries: (LogEntry | undefined)[];
  private latest = 0;

  constructor(capacity: number, private readonly sample?: {
    readonly count: number;
    readonly message: (sequence: number) => string;
  }) {
    if (!Number.isInteger(capacity) || capacity < 1 || capacity > MAX_RETAINED_LINES) {
      throw new RangeError("Invalid log retention");
    }
    this.entries = new Array<LogEntry | undefined>(capacity);
    this.latest = sample?.count ?? 0;
  }

  append(message: string): number {
    if (message.length > MAX_LINE_UNITS || /[\u0000-\u001f\u007f]/u.test(message)) {
      throw new RangeError("Invalid log line");
    }
    if (this.latest === Number.MAX_SAFE_INTEGER) throw new RangeError("Log sequence exhausted");
    const sequence = ++this.latest;
    this.entries[(sequence - 1) % this.entries.length] = Object.freeze({ sequence, message });
    return sequence;
  }

  page(after: number, limit: number, query = "", through = this.latest): LogPage {
    if (!Number.isSafeInteger(after) || !Number.isSafeInteger(through) ||
        after < 0 || through < after || through > this.latest ||
        !Number.isInteger(limit) || limit < 1 || limit > MAX_PAGE ||
        query.length > MAX_QUERY_UNITS) {
      throw new RangeError("Invalid log page");
    }
    const oldest = Math.max(1, this.latest - this.entries.length + 1);
    const truncated = after < through && after + 1 < oldest;
    let cursor = Math.min(through, Math.max(after, oldest - 1));
    const entries: LogEntry[] = [];
    const end = cursor + Math.min(MAX_SCAN, through - cursor);
    while (cursor < end && entries.length < limit) {
      const index = cursor % this.entries.length;
      const sequence = ++cursor;
      // Synthetic history needs no eager allocation. A live append replaces
      // the same ring slot once its older sequence has left retention.
      const entry = this.entries[index] ?? { sequence, message: this.sample!.message(sequence) };
      if (entry.message.includes(query)) entries.push(entry);
    }
    return { entries, cursor, oldest, latest: through, truncated };
  }
}
