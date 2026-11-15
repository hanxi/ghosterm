import { boolean, object, text } from "@ghosterm/plugin-sdk";
export const OpenView = { name: "views.client.open", input: object({ viewId: text(64) }), output: object({ opened: boolean() }) };
export const ReopenView = { ...OpenView, name: "views.reopen" };
