import { Codec, fail, snapshot } from "./codec.js";

export type ViewKind = "table" | "diff" | "task";
export type ViewStatus = "ok" | "invalid" | "busy" | "unsupported" | "unknown" | "denied" | "revoked" | "closed";
export type ViewSpec = Readonly<{ kind: "table" | "diff"; viewId: string; methods: Readonly<{ describe: string; read: string }> }> |
  Readonly<{ kind: "task"; viewId: string; methods: Readonly<{ describe: string; read: string; cancel: string }> }>;
/** Trusted local worker adapter, never a remote RPC or authority from a plugin.
 * Native rechecks role, selected command, method direction and live grants. */
export interface NativeViewPort {
  viewOpen(kind: string, viewId: string, describe: string, read: string, cancel: string | undefined): Readonly<{ status: ViewStatus; handle?: string }>;
  viewState(handle: string): ViewStatus;
  viewClose(handle: string): ViewStatus;
}
export interface ViewHandle {
  readonly handle: string;
  readonly viewId: string;
  readonly kind: ViewKind;
  readonly state: ViewStatus;
  close(): void;
}
export const VIEWS_MAX = 8;
const idPattern = "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$";
const methodPattern = "^[a-z][A-Za-z0-9_.-]{0,127}$";
const methodSchema = { type: "string", pattern: methodPattern };
function shape(task: boolean) {
  return { type: "object", additionalProperties: false, required: ["kind", "viewId", "methods"], properties: {
    kind: { type: "string", enum: task ? ["task"] : ["table", "diff"] }, viewId: { type: "string", pattern: idPattern },
    methods: { type: "object", additionalProperties: false, required: task ? ["describe", "read", "cancel"] : ["describe", "read"],
      properties: task ? { describe: methodSchema, read: methodSchema, cancel: methodSchema } : { describe: methodSchema, read: methodSchema } } } };
}
/** Structural schema plus distinct method names, matching Native registration.
 * Registration is not a rendering or business-completion receipt. */
export const ViewSpecCodec: Codec<ViewSpec> = Object.freeze({ schema: snapshot({ oneOf: [shape(false), shape(true)] }), parse(input: unknown): ViewSpec {
  const value = snapshot(input);
  if (!value || Array.isArray(value) || typeof value !== "object" || Object.keys(value).sort().join(",") !== "kind,methods,viewId") fail("VIEW_SPEC");
  const { kind, viewId, methods } = value as Readonly<Record<string, unknown>>;
  if (typeof kind !== "string" || !["table", "diff", "task"].includes(kind) || typeof viewId !== "string" || !new RegExp(idPattern).test(viewId) ||
      !methods || Array.isArray(methods) || typeof methods !== "object") fail("VIEW_SPEC");
  const m = methods as Readonly<Record<string, unknown>>, task = kind === "task";
  if (Object.keys(m).sort().join(",") !== (task ? "cancel,describe,read" : "describe,read") ||
      Object.values(m).some(v => typeof v !== "string" || !new RegExp(methodPattern).test(v)) || new Set(Object.values(m)).size !== Object.keys(m).length) fail("VIEW_METHODS");
  return value as ViewSpec;
} });
function status(value: ViewStatus): ViewStatus {
  if (!["ok", "invalid", "busy", "unsupported", "unknown", "denied", "revoked", "closed"].includes(value)) fail("VIEW_NATIVE_STATUS");
  return value;
}
/** Local registration facade. No timers, DOM, process or Session/wire messages.
 * Native controls availability; closing never cancels Executor business work.
 * The creating worker disposes this registry before revoking its Session. */
export class ViewRegistry {
  readonly #port: NativeViewPort;
  readonly #handles = new Map<string, ViewHandle>();
  #live = true;
  constructor(port: NativeViewPort) { this.#port = port; }
  open(input: ViewSpec): ViewHandle {
    if (!this.#live) fail("VIEW_CLOSED");
    const spec = ViewSpecCodec.parse(input);
    for (const [id, handle] of this.#handles) if (handle.state === "closed") this.#handles.delete(id);
    if (this.#handles.has(spec.viewId)) fail("VIEW_DUPLICATE");
    if (this.#handles.size >= VIEWS_MAX) fail("VIEW_LIMIT");
    const admitted = this.#port.viewOpen(spec.kind, spec.viewId, spec.methods.describe, spec.methods.read, spec.kind === "task" ? spec.methods.cancel : undefined);
    if (status(admitted.status) !== "ok") fail("VIEW_" + admitted.status.toUpperCase());
    const id = admitted.handle;
    if (typeof id !== "string" || !/^[0-9a-f]{32}\.[1-9][0-9]{0,19}$/.test(id) ||
        id.split(".")[1]!.length === 20 && id.split(".")[1]! > "18446744073709551615" || [...this.#handles.values()].some(h => h.handle === id)) fail("VIEW_NATIVE_HANDLE");
    const port = this.#port, owner = this;
    let live = true;
    const handle: ViewHandle = Object.freeze({ handle: id, viewId: spec.viewId, kind: spec.kind,
      get state(): ViewStatus { return !live || !owner.#live ? "closed" : status(port.viewState(id)); },
      close() {
        if (!live || !owner.#live) return;
        const result = status(port.viewClose(id));
        if (result !== "ok" && result !== "closed") fail("VIEW_" + result.toUpperCase());
        live = false;
        if (owner.#handles.get(spec.viewId) === handle) owner.#handles.delete(spec.viewId);
      } });
    this.#handles.set(spec.viewId, handle);
    return handle;
  }
  dispose(): void {
    if (!this.#live) return;
    this.#live = false;
    // One failing close must not strand other Native registrations.
    for (const handle of this.#handles.values()) {
      try { this.#port.viewClose(handle.handle); } catch { /* Native retains its own bounded owner. */ }
    }
    this.#handles.clear();
  }
  get retained(): number { return this.#handles.size; }
}
