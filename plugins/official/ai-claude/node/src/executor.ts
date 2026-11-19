import { spawn, execFileSync, type ChildProcess } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import { constants, openSync, closeSync, readSync, fstatSync, lstatSync, readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, isAbsolute, join } from "node:path";
import type { Readable, Writable } from "node:stream";
import { ClaudeApprovalBroker } from "./approvals.js";
import { ClaudeNodeSession } from "./session.js";
import { claudeOptions, CLAUDE_CLI_VERSION, CLAUDE_SDK_VERSION, type ClaudeNodeProfile } from "./profile.js";
import { record, snapshot, text, type Json } from "./json.js";
import { ClaudeStdioBridge, serveClaudeStdio } from "./stdio.js";

function absolute(value: unknown): string {
  const path = text(value, 4096);
  if (!isAbsolute(path) || path.includes("\0")) throw new Error("Explicit absolute Claude owner path required");
  return path;
}
function keys(value: Json, names: readonly string[]): asserts value is Record<string, Json> {
  if (!record(value) || Object.keys(value).sort().join("\0") !== [...names].sort().join("\0")) throw new Error("Invalid Claude owner profile");
}
function readProfile(path: string): Record<string, Json> {
  const fd = openSync(absolute(path), constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK);
  try {
    const info = fstatSync(fd);
    if (!info.isFile() || info.size > 65_536 || (process.getuid && info.uid !== process.getuid())) throw new Error("Invalid Claude profile file");
    const bytes = Buffer.alloc(65_537);
    let used = 0, count;
    while ((count = readSync(fd, bytes, used, bytes.length - used, null)) > 0) used += count;
    if (used > 65_536) throw new Error("Claude profile file exceeds its limit");
    const value = snapshot(JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(0, used))));
    keys(value, ["version", "cwd", "executable", "cliVersion", "sha256", "environmentNames", "tools"]);
    if (value.version !== 1 || value.cliVersion !== CLAUDE_CLI_VERSION) throw new Error("Unsupported Claude owner profile");
    return value;
  } finally { closeSync(fd); }
}

/** Consistency check of an explicitly selected file. It does not eliminate
 * same-user path races or prove publisher trust / a Core authorization grant. */
function verifyExecutable(path: string, expected: string): void {
  let fd: number;
  try { fd = openSync(path, constants.O_RDONLY | constants.O_NOFOLLOW | constants.O_NONBLOCK); }
  catch (error) {
    if (typeof error === "object" && error !== null && "code" in error && error.code === "ENOENT") {
      throw Object.assign(new Error("Selected Claude executable is unavailable"), { code: "CLAUDE_CLI_UNAVAILABLE" });
    }
    throw error;
  }
  try {
    const before = fstatSync(fd, { bigint: true });
    if (!before.isFile() || !(before.mode & 0o111n)) throw new Error("Claude executable is not a regular executable");
    const hash = createHash("sha256"), buffer = Buffer.alloc(32_768);
    for (let count; (count = readSync(fd, buffer, 0, buffer.length, null)) > 0;) hash.update(buffer.subarray(0, count));
    const after = fstatSync(fd, { bigint: true }), named = lstatSync(path, { bigint: true });
    for (const key of ["dev", "ino", "mode", "size", "mtimeNs", "ctimeNs"] as const) {
      if (before[key] !== after[key] || before[key] !== named[key]) throw new Error("Claude executable changed");
    }
    if (hash.digest("hex") !== expected) throw new Error("Claude executable digest mismatch");
  } finally { closeSync(fd); }
}

async function settled(promise: Promise<void>, milliseconds: number): Promise<boolean> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  try { return await Promise.race([promise.then(() => true), new Promise<boolean>(resolve => { timer = setTimeout(() => resolve(false), milliseconds); })]); }
  finally { clearTimeout(timer); }
}

/** Foreground Node executor. Only a trusted launcher selects this profile file;
 * the protocol has no command/cwd/environment/configuration mutation method. */
export async function runClaudeExecutor(path: string, input: Readable, output: Writable): Promise<number> {
  const config = readProfile(path);
  const executable = absolute(config.executable), cwd = absolute(config.cwd), sha256 = text(config.sha256, 64);
  if (!/^[a-f0-9]{64}$/.test(sha256)) throw new Error("Invalid Claude executable digest");
  const names = config.environmentNames;
  if (!Array.isArray(names) || names.length > 32 || new Set(names).size !== names.length) throw new Error("Invalid Claude environment selection");
  const environment: Record<string, string> = Object.create(null) as Record<string, string>;
  for (const name of names) {
    if (typeof name !== "string" || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(name) || process.env[name] === undefined) throw new Error("Selected Claude environment unavailable");
    environment[name] = process.env[name]!;
  }
  const broker = new ClaudeApprovalBroker(randomUUID());
  let child: ChildProcess | undefined, exited = false, childFailed = false, stderrBytes = 0;
  let exitPromise = Promise.resolve();
  const profile: ClaudeNodeProfile = { cwd, executable, cliVersion: CLAUDE_CLI_VERSION,
    environment, tools: config.tools as string[], approvals: broker, abortController: new AbortController(),
    spawn: request => {
      verifyExecutable(executable, sha256);
      child = spawn(request.command, request.args, { cwd: request.cwd, env: request.env, stdio: ["pipe", "pipe", "pipe"] });
      exitPromise = new Promise(resolve => {
        child!.once("exit", () => { exited = true; resolve(); });
        child!.once("error", () => { childFailed = true; resolve(); });
      });
      child.stderr!.on("data", (data: Buffer) => {
        stderrBytes += data.length;
        if (stderrBytes > 16_777_216) { childFailed = true; profile.abortController.abort(); child?.kill("SIGTERM"); }
      });
      return child as ReturnType<ClaudeNodeProfile["spawn"]>;
    } };
  // Validate before the version probe. No selected configuration is inferred
  // from the Node caller's HOME, cwd or installed system Claude executable.
  claudeOptions(profile);
  const require = createRequire(import.meta.url);
  const sdkPackage = join(dirname(require.resolve("@anthropic-ai/claude-agent-sdk")), "package.json");
  if (JSON.parse(readFileSync(sdkPackage, "utf8")).version !== CLAUDE_SDK_VERSION) throw new Error("Claude SDK version mismatch");
  verifyExecutable(executable, sha256);
  const version = execFileSync(executable, ["--version"], { cwd, env: environment, encoding: "utf8", timeout: 5000, maxBuffer: 4096 }).trim();
  if (version !== `${CLAUDE_CLI_VERSION} (Claude Code)`) {
    throw Object.assign(new Error("Claude CLI version mismatch"), { code: "CLAUDE_CLI_VERSION_MISMATCH" });
  }
  const session = new ClaudeNodeSession(profile), bridge = new ClaudeStdioBridge(session, broker);
  let cancelled = false, result = 5;
  const cancel = () => { cancelled = true; bridge.close(); input.destroy(new Error("Claude executor cancelled")); };
  const deadline = setTimeout(() => { bridge.close(); input.destroy(new Error("Claude executor deadline")); }, 600_000);
  process.once("SIGINT", cancel); process.once("SIGTERM", cancel);
  try { result = await serveClaudeStdio(input, output, bridge) === "stopped" ? 0 : 7; }
  catch { result = cancelled ? 130 : 5; }
  finally {
    clearTimeout(deadline);
    process.off("SIGINT", cancel); process.off("SIGTERM", cancel);
    bridge.close();
    if (child && !exited) {
      child.kill("SIGTERM");
      if (!await settled(exitPromise, 5000)) { child.kill("SIGKILL"); await settled(exitPromise, 2000); }
    }
    if (child && (!exited || childFailed)) result = 5;
  }
  return result;
}
