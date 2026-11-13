/** Bounded UTF-8 JSON-lines framing for a host-owned app-server stdout pipe. */

export const MAX_APP_SERVER_LINE_BYTES = 1_048_576;
export const APP_SERVER_DECODE_CHUNK_BYTES = 8192;

export class CodexLineDecoderError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CodexLineDecoderError";
  }
}

function decodeUTF8(bytes: Uint8Array): string {
  const units = new Uint16Array(bytes.length);
  let input = 0, output = 0;
  while (input < bytes.length) {
    const first = bytes[input++]!;
    let codePoint: number;
    if (first <= 0x7f) {
      codePoint = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      const second = bytes[input++];
      if (second === undefined || (second & 0xc0) !== 0x80) throw new TypeError("invalid UTF-8");
      codePoint = ((first & 0x1f) << 6) | (second & 0x3f);
    } else if (first >= 0xe0 && first <= 0xef) {
      const second = bytes[input++], third = bytes[input++];
      if (second === undefined || third === undefined || (second & 0xc0) !== 0x80 ||
          (third & 0xc0) !== 0x80 || (first === 0xe0 && second < 0xa0) ||
          (first === 0xed && second >= 0xa0)) throw new TypeError("invalid UTF-8");
      codePoint = ((first & 0x0f) << 12) | ((second & 0x3f) << 6) | (third & 0x3f);
    } else if (first >= 0xf0 && first <= 0xf4) {
      const second = bytes[input++], third = bytes[input++], fourth = bytes[input++];
      if (second === undefined || third === undefined || fourth === undefined ||
          (second & 0xc0) !== 0x80 || (third & 0xc0) !== 0x80 || (fourth & 0xc0) !== 0x80 ||
          (first === 0xf0 && second < 0x90) || (first === 0xf4 && second > 0x8f)) {
        throw new TypeError("invalid UTF-8");
      }
      codePoint = ((first & 0x07) << 18) | ((second & 0x3f) << 12) |
        ((third & 0x3f) << 6) | (fourth & 0x3f);
    } else {
      throw new TypeError("invalid UTF-8");
    }
    if (codePoint <= 0xffff) {
      units[output++] = codePoint;
    } else {
      codePoint -= 0x10000;
      units[output++] = 0xd800 | (codePoint >> 10);
      units[output++] = 0xdc00 | (codePoint & 0x3ff);
    }
  }
  const chunks: string[] = [];
  for (let offset = 0; offset < output; offset += 8192) {
    chunks.push(String.fromCharCode(...units.subarray(offset, Math.min(output, offset + 8192))));
  }
  return chunks.join("");
}

export class CodexLineDecoder {
  private readonly buffer = new Uint8Array(APP_SERVER_DECODE_CHUNK_BYTES);
  private length = 0;
  private lineBytes = 0;
  private readonly parts: string[] = [];
  private failed = false;
  private readonly onLine: (line: string) => void;

  constructor(onLine: (line: string) => void) {
    this.onLine = onLine;
  }

  push(bytes: Uint8Array): void {
    if (this.failed) throw new CodexLineDecoderError("Codex line decoder is failed");
    try {
      let at = 0;
      while (at < bytes.length) {
        const newline = bytes.indexOf(0x0a, at);
        const end = newline < 0 ? bytes.length : newline;
        const count = end - at;
        if (count > MAX_APP_SERVER_LINE_BYTES - this.lineBytes) {
          throw new CodexLineDecoderError("Codex app-server line exceeds 1 MiB");
        }
        this.lineBytes += count;
        while (at < end) {
          const taken = Math.min(end - at, this.buffer.length - this.length);
          this.buffer.set(bytes.subarray(at, at + taken), this.length);
          this.length += taken;
          at += taken;
          if (this.length === this.buffer.length) this.flush(false);
        }
        if (newline < 0) break;
        this.flush(true);
        let line = this.parts.join("");
        if (line.endsWith("\r")) line = line.slice(0, -1);
        this.parts.length = 0;
        this.lineBytes = 0;
        this.onLine(line);
        at = newline + 1;
      }
    } catch (error) {
      this.failed = true;
      if (error instanceof CodexLineDecoderError) throw error;
      if (error instanceof TypeError) throw new CodexLineDecoderError("Codex app-server emitted invalid UTF-8");
      throw error;
    }
  }

  /** Keep only an incomplete final UTF-8 sequence for the next bounded block. */
  private flush(final: boolean): void {
    let end = this.length;
    if (!final && end > 0) {
      let start = end - 1;
      while (start > 0 && (this.buffer[start]! & 0xc0) === 0x80 && end - start < 4) start--;
      const lead = this.buffer[start]!;
      const width = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 :
        lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
      if (end - start < width) end = start;
    }
    if (end > 0) this.parts.push(decodeUTF8(this.buffer.subarray(0, end)));
    const remaining = this.length - end;
    if (remaining > 0) this.buffer.copyWithin(0, end, this.length);
    this.length = remaining;
  }

  /** EOF is valid only between complete lines; partial JSON must remain unknown. */
  finish(): void {
    if (this.failed) throw new CodexLineDecoderError("Codex line decoder is failed");
    if (this.lineBytes !== 0) {
      this.failed = true;
      throw new CodexLineDecoderError("Codex app-server pipe ended inside a JSON line");
    }
  }
}
