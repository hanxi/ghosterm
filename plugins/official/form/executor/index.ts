import { boolean, object, requestForm, text } from "@ghosterm/plugin-sdk";
import type { Disposable, PluginContext } from "@ghosterm/plugin-sdk";

export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "form" ||
      context.capabilities.query("session.rpc") !== "allowed" ||
      context.capabilities.query("ui.form") !== "allowed") throw new Error("Form Session grant is required");

  let status = "pending";
  const call = requestForm(context.session, {
    requestId: "1", title: "Choose sample settings", fields: [
      { kind: "text", id: "name", label: "Display name", required: true, maxLength: 64 },
      { kind: "toggle", id: "summary", label: "Show a summary" },
      { kind: "choice", id: "layout", label: "Layout", options: [
        { id: "compact", label: "Compact" }, { id: "comfortable", label: "Comfortable" },
      ] },
    ],
  }, 120_000);
  void call.outcome.then(outcome => {
    status = outcome.kind === "success" ? outcome.value.kind : outcome.kind;
  });
  context.session.register({ name: "form.status", input: object({}), output: object({ status: text(16) }) },
    (_input, request) => { request.check(); return { status }; });
  context.session.register({ name: "form.cancel", input: object({}), output: object({ requested: boolean() }) },
    (_input, request) => {
      request.check();
      const requested = status === "pending";
      if (requested) call.cancel();
      return { requested };
    });
  return { dispose() { call.cancel(); } };
}
