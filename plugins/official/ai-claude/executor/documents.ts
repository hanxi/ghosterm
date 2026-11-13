import { byteLength, parse, record } from "../protocol/json.ts";

export const PARAMS_MAX = 64_000;
export const RESPONSE_MAX = 600_000;
export const RESPONSE_BUDGET = 2_097_152;
const PAGE_UNITS = 256;
type Document = { readonly inputID: string; text?: string; bytes: number };

/** Per-Runtime memory only. IDs are observations, not permissions or paths. */
export class ClaudeDocuments {
  private serial = 0;
  private input: { id: string; total: number; text: string; sealed: boolean } | undefined;
  private readonly responses = new Map<string, Document>();
  private reserved = 0;
  private latest = { responseID: "", inputID: "" };
  open(total: number) {
    if (this.input) return { status: "busy", inputID: "", nextOffset: 0 };
    if (!Number.isSafeInteger(total) || total < 2 || total > PARAMS_MAX) return { status: "invalid", inputID: "", nextOffset: 0 };
    const id = `input-${++this.serial}`;
    this.input = { id, total, text: "", sealed: false };
    return { status: "ok", inputID: id, nextOffset: 0 };
  }
  append(id: string, offset: number, content: string) {
    const input = this.input;
    if (!input || input.id !== id || input.sealed || offset !== input.text.length || !content ||
        content.length > 512 || input.text.length + content.length > input.total) return { status: "invalid", nextOffset: offset };
    byteLength(content);
    input.text += content;
    return { status: "ok", nextOffset: input.text.length };
  }
  seal(id: string) {
    const input = this.input;
    if (!input || input.id !== id || input.text.length !== input.total) return { status: "invalid" };
    try {
      if (byteLength(input.text) > PARAMS_MAX || !record(parse(input.text, true))) return { status: "invalid" };
    } catch { return { status: "invalid" }; }
    input.sealed = true; return { status: "ok" };
  }
  discard(id: string) { if (!this.input || this.input.id !== id) return { accepted: false }; this.input = undefined; return { accepted: true }; }
  take(id: string): string | undefined {
    if (!this.input?.sealed || this.input.id !== id) return undefined;
    const text = this.input.text; this.input = undefined; return text;
  }
  reserve(inputID: string): string | undefined {
    if (this.responses.size >= 4 || this.reserved > RESPONSE_BUDGET - RESPONSE_MAX) return undefined;
    const id = `response-${++this.serial}`;
    this.responses.set(id, { inputID, bytes: RESPONSE_MAX }); this.reserved += RESPONSE_MAX;
    return id;
  }
  capture(id: string, text: string) {
    const value = this.responses.get(id), bytes = byteLength(text);
    if (!value || value.text !== undefined || bytes > RESPONSE_MAX) throw new Error("Claude response reservation invalid");
    this.reserved += bytes - value.bytes; value.bytes = bytes; value.text = text;
    this.latest = { responseID: id, inputID: value.inputID };
    return { status: "ok", responseID: id, totalUnits: text.length, totalBytes: bytes };
  }
  read(id: string, offset: number) {
    const item = this.responses.get(id), text = item?.text;
    const result = { status: "missing", responseID: id, offset, nextOffset: offset, totalUnits: text?.length ?? 0,
      totalBytes: text === undefined ? 0 : item!.bytes, content: "", done: false };
    if (text === undefined) return result;
    if (!Number.isSafeInteger(offset) || offset < 0 || offset > text.length ||
        (offset > 0 && offset < text.length && /[\udc00-\udfff]/.test(text[offset]!))) return { ...result, status: "invalid" };
    let end = Math.min(offset + PAGE_UNITS, text.length);
    if (end < text.length && /[\ud800-\udbff]/.test(text[end - 1]!)) end--;
    return { ...result, status: "ok", content: text.slice(offset, end), nextOffset: end, done: end === text.length };
  }
  forget(id: string) {
    const value = this.responses.get(id);
    if (!value || value.text === undefined) return { accepted: false };
    this.reserved -= value.bytes; this.responses.delete(id);
    if (this.latest.responseID === id) this.latest = { responseID: "", inputID: "" };
    return { accepted: true };
  }
  abandon(id: string): void {
    const value = this.responses.get(id);
    if (value?.text === undefined && value) { this.responses.delete(id); this.reserved -= value.bytes; }
  }
  get latestResponse() { return this.latest; }
  get reservedBytes() { return this.reserved; }
  clear(): void { this.input = undefined; this.responses.clear(); this.reserved = 0; this.latest = { responseID: "", inputID: "" }; }
}
