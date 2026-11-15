import { boolean, Codec, compareUint64, encode, fail, object, snapshot, text, uint64, utf8Bytes } from "./codec.js";
import type { Method } from "./session.js";

/** Standard task observations. A cancel acknowledgement is an accepted request,
 * never proof that work stopped; only a later confirmed snapshot supplies that.
 * Native owners also keep transport uncertainty and current-instance identity. */
export const TASK_BODY_BYTES_MAX = 2048;
export type TaskTotal = Readonly<{ kind: "known"; units: string }> | Readonly<{ kind: "unknown" }>;
export type TaskResult = Readonly<{ kind: "none" }> | Readonly<{ kind: "success"; summary: string }> |
  Readonly<{ kind: "failed"; code: string; message: string }> | Readonly<{ kind: "cancelled"; effect: "none" | "possible" }> |
  Readonly<{ kind: "unknown"; reason: string }>;
export type TaskPhase = "queued" | "running" | "cancelling" | "succeeded" | "failed" | "cancelled" | "unknown";
export interface TaskDescriptor { readonly viewId: string; readonly revision: string; readonly title: string; readonly unit: string; readonly cancellable: boolean; }
export interface TaskSnapshot { readonly viewId: string; readonly revision: string; readonly sequence: string; readonly phase: TaskPhase;
  readonly completed: string; readonly total: TaskTotal; readonly message: string; readonly result: TaskResult; }
export interface TaskReadRequest { readonly viewId: string; readonly revision: string; }
export interface TaskCancelRequest extends TaskReadRequest { readonly requestId: string; }
export interface TaskCancelAck extends TaskCancelRequest { readonly kind: "requested" | "alreadyFinished"; }

function lazy<T>(build: () => Codec<T>): Codec<T> {
  let value: Codec<T> | undefined;
  const codec = () => value ??= build();
  return Object.freeze({ get schema() { return codec().schema; }, parse(input: unknown) { return codec().parse(input); } });
}
function choice<T extends string>(values: readonly T[]): Codec<T> {
  return Object.freeze({ schema: snapshot({ type: "string", enum: values }), parse(value: unknown) {
    if (typeof value !== "string" || !values.includes(value as T)) fail("TASK_ENUM");
    return value as T;
  } });
}
function variant<T>(variants: Readonly<Record<string, Codec<T>>>): Codec<T> {
  return Object.freeze({ schema: snapshot({ oneOf: Object.values(variants).map(v => v.schema) }), parse(value: unknown) {
    const copy = snapshot(value);
    if (!copy || Array.isArray(copy) || typeof copy !== "object") fail("TASK_VARIANT");
    const kind = (copy as Readonly<Record<string, unknown>>).kind;
    if (typeof kind !== "string" || !Object.prototype.hasOwnProperty.call(variants, kind)) fail("TASK_VARIANT");
    return variants[kind]!.parse(copy);
  } });
}
function boundedText(maximum: number, nonempty = false): Codec<string> {
  const codec = text(maximum);
  return Object.freeze({ schema: codec.schema, parse(value: unknown) {
    const result = codec.parse(value);
    if ((nonempty && !result) || /[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(result)) fail("TASK_TEXT");
    return result;
  } });
}
const identity: Codec<string> = Object.freeze({ schema: snapshot({ type: "string", pattern: "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$" }), parse(value: unknown) {
  if (typeof value !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(value)) fail("TASK_ID"); return value;
} });
const revision: Codec<string> = Object.freeze({ schema: snapshot({ type: "string", pattern: "^[1-9][0-9]{0,19}$" }), parse(value: unknown) { return uint64(value, true); } });
const counter: Codec<string> = Object.freeze({ schema: snapshot({ type: "string", pattern: "^(0|[1-9][0-9]{0,19})$" }), parse(value: unknown) { return uint64(value); } });
const total: Codec<TaskTotal> = lazy(() => variant<TaskTotal>({ known: object({ kind: choice(["known"] as const), units: counter }), unknown: object({ kind: choice(["unknown"] as const) }) }));
const result: Codec<TaskResult> = lazy(() => variant<TaskResult>({
  none: object({ kind: choice(["none"] as const) }),
  success: object({ kind: choice(["success"] as const), summary: boundedText(256, true) }),
  failed: object({ kind: choice(["failed"] as const), code: boundedText(64, true), message: boundedText(256, true) }),
  cancelled: object({ kind: choice(["cancelled"] as const), effect: choice(["none", "possible"] as const) }),
  unknown: object({ kind: choice(["unknown"] as const), reason: boundedText(128, true) }),
}));
function body<T>(codec: Codec<T>, validate: (value: T) => void): Codec<T> {
  return Object.freeze({ schema: codec.schema, parse(value: unknown): T {
    const result = codec.parse(value);
    if (utf8Bytes(encode(result)) > TASK_BODY_BYTES_MAX) fail("TASK_BYTES");
    validate(result); return result;
  } });
}
export const TaskDescriptorCodec: Codec<TaskDescriptor> = lazy(() => body(object({ viewId: identity, revision,
  title: boundedText(128, true), unit: boundedText(64, true), cancellable: boolean() }), () => {}));
export const TaskReadRequestCodec: Codec<TaskReadRequest> = lazy(() => body(object({ viewId: identity, revision }), () => {}));
export const TaskCancelRequestCodec: Codec<TaskCancelRequest> = lazy(() => body(object({ viewId: identity, revision, requestId: identity }), () => {}));
export const TaskCancelAckCodec: Codec<TaskCancelAck> = lazy(() => body(object({ viewId: identity, revision, requestId: identity,
  kind: choice(["requested", "alreadyFinished"] as const) }), () => {}));
export const TaskSnapshotCodec: Codec<TaskSnapshot> = lazy(() => body(object({ viewId: identity, revision, sequence: revision,
  phase: choice(["queued", "running", "cancelling", "succeeded", "failed", "cancelled", "unknown"] as const),
  completed: counter, total, message: boundedText(128), result }), value => {
  if (value.total.kind === "known" && compareUint64(value.completed, value.total.units) > 0) fail("TASK_PROGRESS");
  if (value.phase === "queued" && value.completed !== "0") fail("TASK_QUEUED");
  const expected = value.phase === "succeeded" ? "success" : ["failed", "cancelled", "unknown"].includes(value.phase) ? value.phase : "none";
  if (value.result.kind !== expected) fail("TASK_RESULT");
  if (value.phase === "succeeded" && value.total.kind === "known" && value.completed !== value.total.units) fail("TASK_INCOMPLETE");
}));
const empty = object({});
export const TaskDescribeMethod: Method<Readonly<Record<string, never>>, TaskDescriptor> = Object.freeze({ name: "view.task.describe", input: empty, output: TaskDescriptorCodec });
export const TaskReadMethod: Method<TaskReadRequest, TaskSnapshot> = Object.freeze({ name: "view.task.read", input: TaskReadRequestCodec, output: TaskSnapshotCodec });
export const TaskCancelMethod: Method<TaskCancelRequest, TaskCancelAck> = Object.freeze({ name: "view.task.cancel", input: TaskCancelRequestCodec, output: TaskCancelAckCodec });
function same(description: TaskDescriptor, value: TaskReadRequest): void {
  if (value.viewId !== description.viewId || value.revision !== description.revision) fail("TASK_STALE");
}
export function taskIsTerminal(phase: TaskPhase): boolean { return ["succeeded", "failed", "cancelled"].includes(phase); }
export function validateTaskSnapshot(description: TaskDescriptor, requested: TaskReadRequest, received: TaskSnapshot, previous?: TaskSnapshot): TaskSnapshot {
  const d = TaskDescriptorCodec.parse(description), r = TaskReadRequestCodec.parse(requested), p = TaskSnapshotCodec.parse(received);
  same(d, r); same(d, p);
  if (!d.cancellable && (p.phase === "cancelling" || p.phase === "cancelled")) fail("TASK_CANCEL_UNSUPPORTED");
  if (previous !== undefined) {
    const prior = TaskSnapshotCodec.parse(previous); same(d, prior);
    const order = compareUint64(p.sequence, prior.sequence);
    if (order < 0 || (order === 0 && encode(p) !== encode(prior))) fail("TASK_SEQUENCE");
    if (compareUint64(p.completed, prior.completed) < 0) fail("TASK_REGRESSION");
    if (prior.total.kind === "known" && (p.total.kind !== "known" || p.total.units !== prior.total.units)) fail("TASK_TOTAL");
    if (taskIsTerminal(prior.phase) && encode(p) !== encode(prior)) fail("TASK_TERMINAL");
    if (prior.phase === "running" && p.phase === "queued") fail("TASK_PHASE_REGRESSION");
    if (prior.phase === "cancelling" && (p.phase === "queued" || p.phase === "running")) fail("TASK_CANCEL_REGRESSION");
  }
  return p;
}
export function validateTaskCancelAck(description: TaskDescriptor, requested: TaskCancelRequest, received: TaskCancelAck): TaskCancelAck {
  const d = TaskDescriptorCodec.parse(description), r = TaskCancelRequestCodec.parse(requested), a = TaskCancelAckCodec.parse(received);
  same(d, r); same(d, a);
  if (!d.cancellable || a.requestId !== r.requestId) fail("TASK_CANCEL_ACK");
  return a;
}
