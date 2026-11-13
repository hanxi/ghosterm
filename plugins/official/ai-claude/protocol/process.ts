import type { PluginContext, ProcessChild, ProcessExit } from "@ghosterm/plugin-sdk";
import { ClaudeLineDecoder } from "./line-decoder.ts";
import { byteLength, canonical, encode, parse, record } from "./json.ts";
import { CLAUDE_CLI_VERSION, CLAUDE_NODE_MIN_MAJOR, CLAUDE_SDK_VERSION } from "./version.ts";

export const METHODS = ["start", "send", "interrupt", "approve", "answer", "poll", "stop"] as const;
export class ClaudeTransportError extends Error {
  constructor(readonly code: string) { super(`Claude Node transport: ${code}`); }
}
type Pending = { id: string; sequence: number; method: string; line: string; offset: number;
  bytes: Uint8Array | undefined; byteOffset: number; resolve: (line: string) => void; reject: (error: Error) => void };

/** One outstanding child operation. No replay and no ambient executable/argv. */
export class ClaudeNodeProcess {
  private decoder: ClaudeLineDecoder | undefined;
  private sequence = 0;
  private pending: Pending | undefined;
  private line: string | undefined;
  private dispatch: (() => void) | undefined;
  private stdoutEOF = false;
  private stderrEOF = false;
  private stopAccepted = false;
  private closed = false;
  private error: ClaudeTransportError | undefined;
  private observed: ProcessExit | undefined;
  private releasePromise: Promise<void> | undefined;
  private reapedValue = false;
  private readonly exitPromise: Promise<ProcessExit>;
  private exitResolve!: (exit: ProcessExit) => void;
  private exitReject!: (error: Error) => void;

  constructor(private readonly child: ProcessChild) {
    this.decoder = new ClaudeLineDecoder(line => {
      if (!this.pending || this.line !== undefined || this.dispatch) throw new ClaudeTransportError("UNSOLICITED_RESPONSE");
      this.line = line;
    });
    this.exitPromise = new Promise((resolve, reject) => { this.exitResolve = resolve; this.exitReject = reject; });
    void this.exitPromise.catch(() => {});
  }
  get busy(): boolean { return this.pending !== undefined; }
  get failure(): ClaudeTransportError | undefined { return this.error; }
  get reaped(): boolean { return this.reapedValue; }
  get exit(): ProcessExit | undefined { return this.observed; }
  get phase(): string { return this.error ? "failed" : this.reapedValue ? "closed" : this.closed ? "closing" : "live"; }

  call(method: string, parameters: string): Promise<string> {
    if (this.closed || this.error || this.reapedValue || this.pending || !METHODS.some(value => value === method)) {
      return Promise.reject(new ClaudeTransportError("NOT_READY"));
    }
    const params = parse(parameters, true);
    if (!record(params) || byteLength(parameters) > 64_000 || !Number.isSafeInteger(this.sequence + 1)) throw new ClaudeTransportError("PARAMETERS");
    const sequence = ++this.sequence, id = `command-${sequence}`;
    const line = canonical({ id, method, params, sequence, version: 1 }) + "\n";
    if (byteLength(line) > 65_536) throw new ClaudeTransportError("INPUT_LIMIT");
    const promise = new Promise<string>((resolve, reject) => {
      this.pending = { id, sequence, method, line, offset: 0, bytes: undefined, byteOffset: 0, resolve, reject };
    });
    void promise.catch(() => {}); return promise;
  }

  step(): void {
    if (this.closed || this.reapedValue) return;
    try {
      this.write();
      if (this.dispatch) { const dispatch = this.dispatch; this.dispatch = undefined; dispatch(); }
      else if (this.line !== undefined) {
        const line = this.line; this.line = undefined;
        const value = parse(line, true), pending = this.pending;
        if (!pending || !record(value) || value.version !== 1 || value.id !== pending.id || value.sequence !== pending.sequence ||
            typeof value.ok !== "boolean" || Object.keys(value).sort().join() !==
              (value.ok ? "id,ok,result,sequence,version" : "error,id,ok,phase,sequence,version")) throw new ClaudeTransportError("RESPONSE_IDENTITY");
        if (value.ok && pending.method === "start" && (!record(value.result) || value.result.sdkVersion !== CLAUDE_SDK_VERSION ||
            value.result.cliVersion !== CLAUDE_CLI_VERSION)) throw new ClaudeTransportError("BACKEND_VERSION");
        if (!value.ok && value.error !== "ACTION_REJECTED") throw new ClaudeTransportError("RESPONSE_STATUS");
        this.dispatch = () => {
          if (this.pending !== pending || this.closed) throw new ClaudeTransportError("EXPIRED_RESPONSE");
          if (pending.method === "stop" && value.ok) {
            if (!record(value.result) || !["closed", "failed", "lost"].includes(String(value.result.phase))) throw new ClaudeTransportError("STOP_STATUS");
            this.stopAccepted = true;
          }
          this.pending = undefined; pending.resolve(line);
        };
      } else if (!this.stdoutEOF) {
        const read = this.child.readStdout(8192);
        if (read.state === "data") {
          if (!read.bytes.length || read.bytes.length > 8192) throw new ClaudeTransportError("READ_COUNT");
          this.decoder!.push(read.bytes);
        } else if (read.state === "eof" || read.state === "closed") { this.decoder!.finish(); this.stdoutEOF = true; }
        else if (read.state !== "wait") throw new ClaudeTransportError("STDOUT");
      }
      if (!this.stderrEOF) {
        const read = this.child.readStderr(8192);
        if (read.state === "data" && (!read.bytes.length || read.bytes.length > 8192)) throw new ClaudeTransportError("READ_COUNT");
        if (read.state === "eof" || read.state === "closed") this.stderrEOF = true;
        else if (read.state !== "data" && read.state !== "wait") throw new ClaudeTransportError("STDERR");
      }
      this.observed = this.child.poll();
      if (this.stdoutEOF && this.line === undefined && !this.dispatch) {
        if (!this.stopAccepted || this.pending) throw new ClaudeTransportError("OUTCOME_UNKNOWN");
        if (this.stderrEOF && this.observed.state === "exited") void this.release();
      }
    } catch (error) {
      this.error = error instanceof ClaudeTransportError ? error : new ClaudeTransportError("PROTOCOL");
      void this.stop().catch(() => {});
    }
  }

  private write(): void {
    const value = this.pending;
    if (!value || this.stdoutEOF || value.offset === value.line.length && !value.bytes) return;
    if (!value.bytes) {
      let end = Math.min(value.offset + 1024, value.line.length);
      if (end < value.line.length && /[\ud800-\udbff]/.test(value.line[end - 1]!)) end--;
      value.bytes = encode(value.line.slice(value.offset, end)); value.offset = end; value.byteOffset = 0;
    }
    const bytes = value.bytes.subarray(value.byteOffset), write = this.child.writeStdin(bytes);
    if (write.state === "wait") return;
    if (write.state !== "data" || !Number.isInteger(write.count) || write.count < 1 || write.count > bytes.length) throw new ClaudeTransportError("WRITE_COUNT");
    value.byteOffset += write.count;
    if (value.byteOffset === value.bytes.length) value.bytes = undefined;
  }
  waitForExit(): Promise<ProcessExit> { return this.exitPromise; }
  private release(): Promise<void> {
    if (!this.releasePromise) {
      this.closed = true;
      this.decoder = undefined;
      this.releasePromise = this.child.release().then(() => {
        this.reapedValue = true;
        this.exitResolve(this.observed?.state === "exited" ? this.observed : { state: "exited", code: null, signal: null });
      }, () => { const error = new ClaudeTransportError("REAP"); this.error = error; this.exitReject(error); throw error; });
      void this.releasePromise.catch(() => {});
    }
    return this.releasePromise;
  }
  stop(): Promise<void> {
    if (!this.closed) {
      this.closed = true;
      this.decoder = undefined;
      const pending = this.pending; this.pending = undefined; this.line = undefined; this.dispatch = undefined;
      pending?.reject(this.error ?? new ClaudeTransportError("OUTCOME_UNKNOWN"));
      try { this.child.closeStdin(); this.observed = this.child.poll(); if (this.observed.state === "running") this.child.signal("terminate"); }
      catch { this.error ??= new ClaudeTransportError("STOP"); }
    }
    return this.release();
  }
}

export async function spawnClaudeNode(context: PluginContext): Promise<ClaudeNodeProcess> {
  if (context.side !== "executor" || context.capabilities.query("process.spawn") !== "allowed" ||
      context.capabilities.query("process.pipes") !== "allowed" || !context.process || !context.invocation?.workspaceHandle) throw new ClaudeTransportError("UNSUPPORTED");
  const child = await context.process.spawn({ program: "claude-node", argv: ["claude-node"], cwd: context.invocation.workspaceHandle,
    envPolicy: "selected-user-environment", cancellation: context.cancellation });
  const version = /^v([0-9]+)\.([0-9]+)\.([0-9]+)$/.exec(child.verifiedVersion);
  if (!version || version.slice(1).some(value => !Number.isSafeInteger(Number(value))) || Number(version[1]) < CLAUDE_NODE_MIN_MAJOR) {
    child.signal("terminate"); await child.release(); throw new ClaudeTransportError("NODE_VERSION");
  }
  return new ClaudeNodeProcess(child);
}
