import { boolean, encode, fail, integer, object, snapshot, text, uint64, utf8Bytes } from "./codec.js";
import type { Codec, Json } from "./codec.js";
import type { Call, Method, Outcome, RequestContext, SessionChannel } from "./session.js";

const MAX_FIELDS = 8;
const MAX_OPTIONS = 8;
const MAX_TEXT = 256;
const FORM_BYTES = 2048;

function exact<T extends string>(value: T): Codec<T> {
  return Object.freeze({ schema: snapshot({ const: value }), parse(input: unknown) {
    if (input !== value) return fail("FORM_KIND");
    return value;
  } });
}

function list<T>(item: Codec<T>, maximum: number): Codec<readonly T[]> {
  return Object.freeze({ schema: snapshot({ type: "array", minItems: 1, maxItems: maximum, items: item.schema }),
    parse(input: unknown) {
      const values = snapshot(input);
      if (!Array.isArray(values) || values.length < 1 || values.length > maximum) return fail("FORM_LIST");
      return Object.freeze(values.map(value => item.parse(value)));
    } });
}

const keyPattern = /^[A-Za-z][A-Za-z0-9_-]{0,31}$/;
const key: Codec<string> = Object.freeze({
  schema: snapshot({ type: "string", pattern: keyPattern.source }),
  parse(input: unknown) {
    if (typeof input !== "string" || !keyPattern.test(input) ||
        input === "constructor" || input === "prototype") return fail("FORM_KEY");
    return input;
  },
});
const requestId: Codec<string> = Object.freeze({
  schema: snapshot({ type: "string", pattern: "^[1-9][0-9]{0,19}$" }),
  parse(input: unknown) { return uint64(input, true); },
});
const label: Codec<string> = Object.freeze({
  schema: snapshot({ type: "string", minLength: 1, maxLength: 96 }),
  parse(input: unknown) {
    const value = text(96).parse(input);
    if (!value.trim() || /[\u0000-\u001f\u007f]/u.test(value)) return fail("FORM_LABEL");
    return value;
  },
});
const option = object({ id: key, label });
const textField = object({ kind: exact("text"), id: key, label, required: boolean(), maxLength: integer(1, MAX_TEXT) });
const toggleField = object({ kind: exact("toggle"), id: key, label });
const choiceField = object({ kind: exact("choice"), id: key, label, options: list(option, MAX_OPTIONS) });
export type FormField = ReturnType<typeof textField.parse> | ReturnType<typeof toggleField.parse> | ReturnType<typeof choiceField.parse>;
const field: Codec<FormField> = Object.freeze({
  schema: snapshot({ oneOf: [textField.schema, toggleField.schema, choiceField.schema] }),
  parse(input: unknown): FormField {
    const value = record(input);
    switch (value.kind) {
    case "text": return textField.parse(value);
    case "toggle": return toggleField.parse(value);
    case "choice": {
      const result = choiceField.parse(value);
      if (new Set(result.options.map(option => option.id)).size !== result.options.length) return fail("FORM_OPTIONS");
      return result;
    }
    default: return fail("FORM_KIND");
    }
  },
});
function record(input: unknown): Readonly<Record<string, Json>> {
  const value = snapshot(input);
  if (!value || typeof value !== "object" || Array.isArray(value)) return fail("FORM_OBJECT");
  return value as Readonly<Record<string, Json>>;
}

const form = object({ requestId, title: label, fields: list(field, MAX_FIELDS) });
export type FormRequest = ReturnType<typeof form.parse>;
export type FormReply = { readonly requestId: string; readonly kind: "dismissed" } |
  { readonly requestId: string; readonly kind: "submitted"; readonly values: Readonly<Record<string, string | boolean>> };

export const FormRequestCodec: Codec<FormRequest> = Object.freeze({
  schema: form.schema,
  parse(input: unknown) {
    const value = form.parse(input);
    if (new Set(value.fields.map(field => field.id)).size !== value.fields.length) return fail("FORM_FIELDS");
    if (utf8Bytes(encode(value)) > FORM_BYTES) return fail("FORM_BYTES");
    return value;
  },
});

const values: Codec<Readonly<Record<string, string | boolean>>> = Object.freeze({
  schema: snapshot({ type: "object", maxProperties: MAX_FIELDS,
    additionalProperties: { oneOf: [text(MAX_TEXT).schema, boolean().schema] } }),
  parse(input: unknown) {
    const value = record(input);
    const ids = Object.keys(value);
    if (ids.length > MAX_FIELDS) return fail("FORM_FIELDS");
    const result: Record<string, string | boolean> = Object.create(null);
    for (const id of ids) result[key.parse(id)] = typeof value[id] === "boolean" ? boolean().parse(value[id]) : text(MAX_TEXT).parse(value[id]);
    return Object.freeze(result);
  },
});
const dismissed = object({ requestId, kind: exact("dismissed") });
const submitted = object({ requestId, kind: exact("submitted"), values });
export const FormReplyCodec: Codec<FormReply> = Object.freeze({
  schema: snapshot({ oneOf: [dismissed.schema, submitted.schema] }),
  parse(input: unknown): FormReply {
    const value = record(input);
    if (value.kind === "dismissed") return dismissed.parse(value);
    if (value.kind !== "submitted") return fail("FORM_KIND");
    const reply = submitted.parse(value);
    if (utf8Bytes(encode(reply)) > FORM_BYTES) return fail("FORM_BYTES");
    return reply;
  },
});
export const FormRequestMethod: Method<FormRequest, FormReply> = Object.freeze({
  name: "view.form.request", input: FormRequestCodec, output: FormReplyCodec,
});

export function validateFormReply(request: FormRequest, answer: unknown): FormReply {
  const form = FormRequestCodec.parse(request);
  const reply = FormReplyCodec.parse(answer);
  if (reply.requestId !== form.requestId) return fail("FORM_STALE");
  if (reply.kind === "dismissed") return reply;
  if (Object.keys(reply.values).length !== form.fields.length) return fail("FORM_VALUES");
  for (const field of form.fields) {
    const value = reply.values[field.id];
    switch (field.kind) {
    case "text": {
      const answer = text(field.maxLength).parse(value);
      if ((field.required && !answer.trim()) || /[\u0000-\u001f\u007f]/u.test(answer)) return fail("FORM_TEXT");
      break;
    }
    case "toggle": boolean().parse(value); break;
    case "choice":
      if (!field.options.some(option => option.id === value)) return fail("FORM_CHOICE");
      break;
    }
  }
  return reply;
}

/** The Session binds the reply to its native lease and ticket. The form ID and
 * captured schema also bind the business payload, including after cancellation. */
export function requestForm(session: SessionChannel, input: FormRequest, timeoutMs: number): Call<FormReply> {
  const form = FormRequestCodec.parse(input);
  const call = session.request(FormRequestMethod, form, timeoutMs);
  let cancelled = false;
  return {
    cancel() { cancelled = true; call.cancel(); },
    outcome: call.outcome.then((outcome): Outcome<FormReply> => {
      if (outcome.kind !== "success") return outcome;
      if (cancelled) return { kind: "cancelled", effect: "none" };
      try { return { kind: "success", value: validateFormReply(form, outcome.value) }; }
      catch { return { kind: "unknown", reason: "INVALID_FORM_REPLY" }; }
    }),
  };
}

/** The presenter receives the Session cancellation context so it can withdraw
 * its own prompt. The post-await check prevents it from responding after expiry. */
export function registerFormPresenter(session: SessionChannel,
  present: (form: FormRequest, context: RequestContext) => Promise<FormReply>): void {
  session.register(FormRequestMethod, async (form, context) => {
    context.check();
    const answer = await present(form, context);
    context.check();
    return validateFormReply(form, answer);
  });
}
