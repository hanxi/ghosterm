import { CodexAppServerClient, CodexProtocolError } from "./client.ts";
import { CodexLineDecoder } from "./line-decoder.ts";

/** Nonblocking process-pipe surface supplied by a trusted native Host. */
export type PipeRead = { readonly state: "data"; readonly bytes: Uint8Array } |
  { readonly state: "wait" } | { readonly state: "eof" } | { readonly state: "closed" } |
  { readonly state: "error"; readonly error: Error };
export type PipeWrite = { readonly state: "data"; readonly count: number } |
  { readonly state: "wait" } | { readonly state: "closed" } |
  { readonly state: "error"; readonly error: Error };
export interface CodexProcessPipes {
  readStdout(maxBytes: number): PipeRead;
  readStderr(maxBytes: number): PipeRead;
  writeStdin(bytes: Uint8Array): PipeWrite;
  closeStdin(): void;
}

export const CODEX_PIPE_CHUNK_MAX = 16_384;
export const CODEX_PENDING_WRITE_MAX = 1_048_576;
export const CODEX_PENDING_LINES_MAX = 32;

function encodeUTF8(text: string): Uint8Array {
  const bytes: number[] = [];
  for (let i = 0; i < text.length; i++) {
    let cp = text.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff) {
      const low = text.charCodeAt(++i);
      if (!(low >= 0xdc00 && low <= 0xdfff)) throw new CodexProtocolError("invalid UTF-16 output");
      cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
      throw new CodexProtocolError("invalid UTF-16 output");
    }
    if (cp <= 0x7f) bytes.push(cp);
    else if (cp <= 0x7ff) bytes.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp <= 0xffff) bytes.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else bytes.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
  }
  return Uint8Array.from(bytes);
}

/**
 * Couples the portable JSON-RPC client to a host-owned nonblocking stdio process.
 * The caller drives step() on its worker and remains responsible for process
 * authorization, deadlines, termination escalation, and eventual reaping.
 */
export class CodexAppServerStdio {
  readonly client: CodexAppServerClient;
  #pipes: CodexProcessPipes;
  #decoder: CodexLineDecoder;
  #writes: Uint8Array[] = [];
  #writeOffset = 0;
  #queuedBytes = 0;
  #lines: string[] = [];
  #closed = false;
  #stdoutEOF = false;
  #stderrEOF = false;
  #failure: Error | undefined;

  constructor(pipes: CodexProcessPipes) {
    this.#pipes = pipes;
    this.client = new CodexAppServerClient(line => this.#queueLine(line));
    this.#decoder = new CodexLineDecoder(line => {
      if (this.#lines.length >= CODEX_PENDING_LINES_MAX) throw new CodexProtocolError("Codex decoded line queue is full");
      this.#lines.push(line);
    });
    this.client.onProtocolError(error => this.#fail(error));
  }

  get failure(): Error | undefined { return this.#failure; }
  get closed(): boolean { return this.#closed; }
  /** True after both output pipes have reached EOF and buffered bytes were read. */
  get drained(): boolean { return this.#stdoutEOF && this.#stderrEOF && this.#lines.length === 0; }

  /** One bounded, zero-wait IO slice. Stderr is drained but never retained/logged. */
  step(): void {
    if (this.#closed || this.#failure || this.drained) return;
    try {
      if (!this.#stdoutEOF) this.#flushWrites(CODEX_PIPE_CHUNK_MAX);
      // Decode and business handling use separate worker slices. A large line
      // must not spend its whole UTF-8 decode plus patch capture budget at once.
      const line = this.#lines.shift();
      if (line !== undefined) this.client.receiveLine(line);
      else if (!this.#stdoutEOF) this.#drainStdout(CODEX_PIPE_CHUNK_MAX);
      if (this.#stdoutEOF && this.#lines.length === 0) this.client.close();
      if (!this.#stderrEOF) this.#drainStderr(CODEX_PIPE_CHUNK_MAX);
    } catch (error) {
      this.#fail(error instanceof Error ? error : new Error(String(error)));
    }
  }

  /** Call after the process owner has stopped and reaped the child. */
  close(): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#pipes.closeStdin();
    this.client.close();
    this.#writes = [];
    this.#lines = [];
    this.#queuedBytes = 0;
  }

  #queueLine(line: string): void {
    if (this.#closed || this.#failure) throw this.#failure ?? new CodexProtocolError("pipe is closed");
    const bytes = encodeUTF8(`${line}\n`);
    if (bytes.length > CODEX_PENDING_WRITE_MAX - this.#queuedBytes) {
      const error = new CodexProtocolError("Codex pending stdio write queue exceeds 1 MiB");
      this.#fail(error);
      throw error;
    }
    this.#writes.push(bytes);
    this.#queuedBytes += bytes.length;
  }

  #flushWrites(budget: number): void {
    while (budget > 0 && this.#writes.length) {
      const head = this.#writes[0]!;
      const remaining = head.subarray(this.#writeOffset, Math.min(head.length, this.#writeOffset + budget));
      const result = this.#pipes.writeStdin(remaining);
      if (result.state === "wait") return;
      if (result.state === "closed") throw new CodexProtocolError("Codex stdin closed during write");
      if (result.state === "error") throw result.error;
      if (!Number.isInteger(result.count) || result.count <= 0 || result.count > remaining.length) {
        throw new CodexProtocolError("invalid host stdio write count");
      }
      this.#writeOffset += result.count;
      this.#queuedBytes -= result.count;
      budget -= result.count;
      if (this.#writeOffset === head.length) {
        this.#writes.shift();
        this.#writeOffset = 0;
      }
    }
  }

  #drainStdout(budget: number): void {
    while (budget > 0 && !this.#stdoutEOF) {
      const result = this.#pipes.readStdout(Math.min(CODEX_PIPE_CHUNK_MAX, budget));
      if (result.state === "wait") return;
      if (result.state === "closed" || result.state === "eof") {
        this.#decoder.finish();
        this.#stdoutEOF = true;
        return;
      }
      if (result.state === "error") throw result.error;
      if (!(result.bytes instanceof Uint8Array) || result.bytes.length === 0 || result.bytes.length > budget) {
        throw new CodexProtocolError("invalid host stdout read size");
      }
      budget -= result.bytes.length;
      this.#decoder.push(result.bytes);
      if (this.#lines.length) return;
    }
  }

  #drainStderr(budget: number): void {
    while (budget > 0 && !this.#stderrEOF) {
      const result = this.#pipes.readStderr(Math.min(CODEX_PIPE_CHUNK_MAX, budget));
      if (result.state === "wait") return;
      if (result.state === "closed" || result.state === "eof") {
        this.#stderrEOF = true;
        return;
      }
      if (result.state === "error") throw result.error;
      if (!(result.bytes instanceof Uint8Array) || result.bytes.length === 0 || result.bytes.length > budget) {
        throw new CodexProtocolError("invalid host stderr read size");
      }
      budget -= result.bytes.length;
    }
  }

  #fail(error: Error): void {
    if (!this.#failure) this.#failure = error;
    this.client.close();
    this.#writes = [];
    this.#lines = [];
    this.#queuedBytes = 0;
    this.#writeOffset = 0;
  }
}
