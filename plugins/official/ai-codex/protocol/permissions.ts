/** Stable 0.166.0 permission requests. No path resolution or policy changes.
 * A grant repeats exactly the displayed requested overlay for this turn only. */
import { utf8ByteLength } from "./utf8.ts";

type RecordValue = Readonly<Record<string, unknown>>;
export interface PermissionResponse {
  readonly permissions: RecordValue;
  readonly scope: "turn";
}
function record(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}
function require(value: unknown): asserts value {
  if (!value) throw new Error("invalid or unsupported Codex permission request");
}
function keys(value: unknown, required: readonly string[], optional: readonly string[] = []): asserts value is Record<string, unknown> {
  require(record(value) && required.every(name => Object.hasOwn(value, name)) &&
    Object.keys(value).every(name => required.includes(name) || optional.includes(name)));
}
function text(value: unknown, maximum = 4096): asserts value is string {
  require(typeof value === "string" && !value.includes("\0") && utf8ByteLength(value) <= maximum);
}
function path(value: unknown): string { text(value); require(value.length); return value; }
function paths(value: unknown): readonly string[] | null {
  if (value === null) return null;
  require(Array.isArray(value) && value.length <= 64);
  return value.map(path);
}
function sandboxPath(value: unknown): unknown {
  require(record(value));
  if (value.type === "path") { keys(value, ["type", "path"]); return { type: "path", path: path(value.path) }; }
  if (value.type === "glob_pattern") { keys(value, ["type", "pattern"]); return { type: "glob_pattern", pattern: path(value.pattern) }; }
  keys(value, ["type", "value"]); require(value.type === "special" && record(value.value));
  const special = value.value;
  if (special.kind === "project_roots") {
    keys(special, ["kind"], ["subpath"]);
    const subpath = special.subpath ?? null;
    require(subpath === null || typeof subpath === "string");
    if (subpath !== null) text(subpath);
    return { type: "special", value: { kind: "project_roots", subpath } };
  }
  keys(special, ["kind"]);
  require(["root", "minimal", "tmpdir", "slash_tmp"].includes(special.kind as string));
  return { type: "special", value: { kind: special.kind } };
}
function network(value: unknown): RecordValue {
  keys(value, [], ["enabled"]); const enabled = value.enabled ?? null;
  require(enabled === null || typeof enabled === "boolean");
  return { enabled };
}
function filesystem(value: unknown): RecordValue {
  keys(value, [], ["read", "write", "globScanMaxDepth", "entries"]);
  const result: Record<string, unknown> = { read: paths(value.read ?? null), write: paths(value.write ?? null) };
  if (Object.hasOwn(value, "globScanMaxDepth") && value.globScanMaxDepth !== null) {
    require(Number.isSafeInteger(value.globScanMaxDepth) && (value.globScanMaxDepth as number) >= 1);
    result.globScanMaxDepth = value.globScanMaxDepth;
  }
  if (Object.hasOwn(value, "entries") && value.entries !== null) {
    require(Array.isArray(value.entries) && value.entries.length <= 64);
    result.entries = value.entries.map(entry => {
      keys(entry, ["path", "access"]); require(["read", "write", "deny"].includes(entry.access as string));
      return { path: sandboxPath(entry.path), access: entry.access };
    });
  }
  return result;
}
function profile(value: unknown, requested: boolean): RecordValue {
  keys(value, [], ["network", "fileSystem"]);
  const result: Record<string, unknown> = {};
  if (Object.hasOwn(value, "network") && value.network !== null) result.network = network(value.network);
  else if (!requested) require(!Object.hasOwn(value, "network"));
  if (Object.hasOwn(value, "fileSystem") && value.fileSystem !== null) result.fileSystem = filesystem(value.fileSystem);
  else if (!requested) require(!Object.hasOwn(value, "fileSystem"));
  return result;
}
function freeze<T>(value: T): T {
  if (value !== null && typeof value === "object") {
    for (const child of Object.values(value)) freeze(child);
    Object.freeze(value);
  }
  return value;
}
/** Fresh immutable copy: listeners cannot mutate the stored approval scope. */
export function permissionRequest(value: unknown): RecordValue {
  keys(value, ["threadId", "turnId", "itemId", "startedAtMs", "cwd", "permissions"], ["environmentId", "reason"]);
  for (const key of ["threadId", "turnId", "itemId"]) { text(value[key], 128); require((value[key] as string).length); }
  const environmentId = value.environmentId ?? null;
  require(environmentId === null || typeof environmentId === "string");
  if (environmentId !== null) text(environmentId, 128);
  require(Number.isSafeInteger(value.startedAtMs) && (value.startedAtMs as number) >= 0);
  const reason = value.reason ?? null;
  text(value.cwd); require(reason === null || typeof reason === "string");
  if (reason !== null) text(reason);
  const overlay = profile(value.permissions, true);
  return freeze({ threadId: value.threadId, turnId: value.turnId, itemId: value.itemId,
    environmentId, startedAtMs: value.startedAtMs, cwd: value.cwd, reason,
    permissions: { network: overlay.network ?? null, fileSystem: overlay.fileSystem ?? null } });
}
export function permissionResponse(params: RecordValue, allow: boolean): PermissionResponse {
  return freeze({ permissions: allow ? profile(params.permissions, true) : {}, scope: "turn" });
}
/** Denial is an empty overlay. Session scope, arbitrary partial grants, extra
 * permissions and review-policy overrides are deliberately not emitted. */
export function validatePermissionResponse(params: RecordValue, value: unknown): PermissionResponse {
  keys(value, ["permissions", "scope"]); require(value.scope === "turn");
  const permissions = profile(value.permissions, false);
  require(Object.keys(permissions).length === 0 ||
    JSON.stringify(permissions) === JSON.stringify(profile(params.permissions, true)));
  return freeze({ permissions, scope: "turn" });
}
