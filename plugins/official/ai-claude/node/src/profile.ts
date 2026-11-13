import { isAbsolute } from "node:path";
import { Buffer } from "node:buffer";
import type { Options } from "@anthropic-ai/claude-agent-sdk";
import { ClaudeApprovalBroker } from "./approvals.js";
import { record, snapshot, text } from "./json.js";

export const CLAUDE_SDK_VERSION = "0.3.323";
export const CLAUDE_CLI_VERSION = "2.1.323";
const launchedScopes = new WeakSet<ClaudeApprovalBroker>();

export interface ClaudeNodeProfile {
  readonly cwd: string;
  readonly executable: string;
  /** Verified by the embedding process owner, never inferred from a filename. */
  readonly cliVersion: string;
  readonly environment: Readonly<Record<string, string>>;
  readonly spawn: NonNullable<Options["spawnClaudeCodeProcess"]>;
  readonly approvals: ClaudeApprovalBroker;
  readonly abortController: AbortController;
  readonly tools: readonly string[];
}

function path(value: unknown): string {
  const result = text(value, 4096);
  if (!isAbsolute(result) || result.includes("\0")) throw new TypeError("Claude requires an explicit absolute path");
  return result;
}

/** Explicit Node profile only. No process spawn, environment inheritance or SDK call here.
 * The embedding process owner still has to verify the executable and live grants. */
export function claudeOptions(profile: ClaudeNodeProfile): Options {
  if (profile.cliVersion !== CLAUDE_CLI_VERSION) throw new Error("Claude CLI differs from the locked SDK profile");
  if (typeof profile.spawn !== "function") throw new TypeError("Claude requires an explicit process owner");
  if (profile.approvals.closed || profile.abortController.signal.aborted) throw new Error("Claude profile is no longer live");
  const source = snapshot(profile.environment, 65_536);
  if (!record(source)) throw new TypeError("Claude environment must be a selected string map");
  const environment: Record<string, string> = Object.create(null) as Record<string, string>;
  for (const [name, value] of Object.entries(source)) {
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name) || typeof value !== "string" || value.includes("\0")) {
      throw new TypeError("Invalid selected Claude environment");
    }
    environment[name] = value;
  }
  // The Host chooses these locations. Tests supply only their owned project tmp directories.
  for (const name of ["HOME", "CLAUDE_CONFIG_DIR", "TMPDIR"]) path(environment[name]);
  if (!Array.isArray(profile.tools) || profile.tools.length > 64 ||
      new Set(profile.tools).size !== profile.tools.length ||
      profile.tools.some(value => typeof value !== "string" || !/^[A-Za-z][A-Za-z0-9_]{0,127}$/.test(value))) {
    throw new TypeError("Claude tools must be an explicit unique tool list");
  }
  if (Buffer.byteLength(JSON.stringify(environment)) > 65_536) throw new RangeError("Claude environment exceeds its byte budget");
  const cwd = path(profile.cwd), executable = path(profile.executable);
  const owner = profile.spawn, approvals = profile.approvals, abort = profile.abortController;
  let launched = false;
  return {
    cwd, pathToClaudeCodeExecutable: executable, env: environment, abortController: abort,
    spawnClaudeCodeProcess: request => {
      if (approvals.closed || abort.signal.aborted || request.signal.aborted) throw new Error("Claude profile is no longer live");
      if (launched || launchedScopes.has(approvals)) throw new Error("Claude profile cannot replay a process launch");
      if (request.command !== executable || request.cwd !== cwd) throw new Error("Claude SDK changed its explicit execution target");
      launched = true; // A throwing owner may already have taken effect. Never retry implicitly.
      launchedScopes.add(approvals);
      return owner(request);
    },
    permissionMode: "default", permissionPrompts: "host", allowDangerouslySkipPermissions: false,
    allowedTools: [], tools: [...profile.tools], settingSources: [], strictMcpConfig: true,
    mcpServers: {}, plugins: [], persistSession: false, enableFileCheckpointing: false,
    canUseTool: profile.approvals.canUseTool,
  };
}
