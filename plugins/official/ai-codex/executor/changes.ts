import { BoundaryError, encode, utf8Bytes } from "@ghosterm/plugin-sdk";
import { CodexProtocolError } from "../protocol/client.ts";

export const CODEX_CHANGE_MAX_BYTES = 524288;
export const CODEX_CHANGES_MAX_BYTES = 1048576;
export const CODEX_CHANGES_MAX_ITEMS = 16;

export interface CodexChangeChunk {
  readonly status: "ok" | "expired" | "invalid";
  readonly snapshotID: string;
  /** Offsets and lengths count UTF-16 code units, as in JavaScript String.length. */
  readonly offset: number;
  readonly nextOffset: number;
  readonly totalUnits: number;
  readonly totalBytes: number;
  readonly content: string;
  readonly done: boolean;
}

type Snapshot = { id: string; content: string; bytes: number };
function record(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}
function splitSurrogate(value: string, offset: number): boolean {
  const previous = value.charCodeAt(offset - 1), current = value.charCodeAt(offset);
  return previous >= 0xd800 && previous <= 0xdbff && current >= 0xdc00 && current <= 0xdfff;
}

/** Immutable in-memory backend patches. No workspace files are opened or modified. */
export class CodexChangeStore {
  private nextID = 0;
  private readonly snapshots = new Map<string, Snapshot>();
  private bytes = 0;

  get retainedBytes(): number { return this.bytes; }
  get retainedItems(): number { return this.snapshots.size; }

  isCurrent(snapshotID: string, threadID: unknown, turnID: unknown, itemID: unknown): boolean {
    if (![threadID, turnID, itemID].every(value => typeof value === "string" && value.length > 0)) return false;
    const snapshot = this.snapshots.get(JSON.stringify([threadID, turnID, itemID]));
    return snapshot !== undefined && snapshot.id === snapshotID;
  }

  capture(method: string, params: unknown): unknown {
    if (!record(params)) return params;
    const nested = (method === "item/started" || method === "item/completed") &&
      record(params.item) && params.item.type === "fileChange" ? params.item : undefined;
    if (method !== "item/fileChange/patchUpdated" && !nested) return params;
    const itemID = nested ? nested.id : params.itemId;
    const changes = nested ? nested.changes : params.changes;
    if (typeof itemID !== "string" || !itemID || utf8Bytes(itemID) > 128 ||
        typeof params.threadId !== "string" || !params.threadId || utf8Bytes(params.threadId) > 128 ||
        typeof params.turnId !== "string" || !params.turnId || utf8Bytes(params.turnId) > 128 ||
        !Array.isArray(changes) || !changes.every(change => record(change) &&
          typeof change.path === "string" && change.path.length > 0 && typeof change.diff === "string" &&
          record(change.kind) && ["add", "delete", "update"].includes(String(change.kind.type)))) {
      throw new CodexProtocolError("Codex file change cannot be captured as a complete snapshot");
    }
    const key = JSON.stringify([params.threadId, params.turnId, itemID]);
    const content = JSON.stringify({ schemaVersion: 1, threadId: params.threadId,
      turnId: params.turnId, itemId: itemID, changes });
    const bytes = utf8Bytes(content);
    const previous = this.snapshots.get(key);
    let marker: Record<string, unknown>;
    if (previous?.content === content) {
      marker = { snapshotID: previous.id, snapshotUnits: content.length, snapshotBytes: bytes };
    } else if (bytes > CODEX_CHANGE_MAX_BYTES) {
      this.remove(key);
      marker = { snapshotUnavailable: "limit" };
    } else {
      this.remove(key);
      if (this.nextID >= Number.MAX_SAFE_INTEGER) throw new CodexProtocolError("Codex snapshot IDs exhausted");
      while (this.snapshots.size >= CODEX_CHANGES_MAX_ITEMS || this.bytes > CODEX_CHANGES_MAX_BYTES - bytes) {
        const oldest = this.snapshots.keys().next().value;
        if (oldest === undefined) throw new CodexProtocolError("Codex snapshot budget is inconsistent");
        this.remove(oldest);
      }
      const id = `change-${++this.nextID}`;
      this.snapshots.set(key, { id, content, bytes });
      this.bytes += bytes;
      marker = { snapshotID: id, snapshotUnits: content.length, snapshotBytes: bytes };
    }
    return nested ? { ...params, item: { ...nested, _ghosterm: marker } } : { ...params, _ghosterm: marker };
  }

  read(snapshotID: string, offset: number): CodexChangeChunk {
    const snapshot = [...this.snapshots.values()].find(value => value.id === snapshotID);
    const unavailable = (status: "expired" | "invalid"): CodexChangeChunk => ({ status, snapshotID,
      offset: 0, nextOffset: 0, totalUnits: 0, totalBytes: 0, content: "", done: false });
    if (!snapshot) return unavailable("expired");
    const { content, bytes } = snapshot;
    if (!Number.isSafeInteger(offset) || offset < 0 || offset > content.length || splitSurrogate(content, offset)) {
      return unavailable("invalid");
    }
    let end = Math.min(content.length, offset + 1536);
    while (true) {
      if (splitSurrogate(content, end)) end--;
      const value: CodexChangeChunk = { status: "ok", snapshotID, offset, nextOffset: end,
        totalUnits: content.length, totalBytes: bytes, content: content.slice(offset, end), done: end === content.length };
      try {
        // Include the actual outcome wrapper in the frozen RPC body budget.
        encode({ kind: "success", value });
        return value;
      } catch (error) {
        if (!(error instanceof BoundaryError) || error.code !== "BODY_LIMIT" || end <= offset) throw error;
        end = offset + Math.floor((end - offset) / 2);
        if (end <= offset) throw new CodexProtocolError("Codex change chunk cannot fit the RPC budget");
      }
    }
  }

  clear(): void {
    this.snapshots.clear();
    this.bytes = 0;
    // IDs are never reused while this store is alive, including after clear().
  }

  private remove(key: string): void {
    const previous = this.snapshots.get(key);
    if (previous) { this.bytes -= previous.bytes; this.snapshots.delete(key); }
  }
}
