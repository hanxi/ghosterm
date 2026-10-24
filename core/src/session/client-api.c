#define _POSIX_C_SOURCE 200809L
#include "base/secure-random.h"
#include "ghosterm.h"
#include "ipc/local-session-provider.h"
#include "ipc/session-client.h"
#include "ipc/ssh-session-provider.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct gt_session {
  gt_session_sink_t sink;
  gt_session_auth_t authentication;
  gt_session_client_t *client;
  gt_local_session_provider_t *local;
  gt_ssh_session_provider_t *ssh;
  bool busy, cancelled;
};
static bool bytes_nonzero(const uint8_t *bytes, size_t n) {
  uint8_t value = 0;
  for (size_t i = 0; i < n; i++)
    value |= bytes[i];
  return value != 0;
}
static bool system_clock(void *context, uint64_t *out) {
  (void)context;
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) || t.tv_sec < 0 ||
      (uint64_t)t.tv_sec > (UINT64_MAX - 999) / 1000)
    return false;
  *out = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
  return true;
}
static gt_client_sink_rc_t snapshot(void *context, uint64_t generation,
                                    uint64_t sequence, uint32_t format,
                                    const uint8_t *bytes, size_t n,
                                    bool readonly, gt_status_t *error) {
  gt_session_t *s = context;
  return (gt_client_sink_rc_t)s->sink.snapshot(
      s->sink.context, generation, sequence, format, bytes, n, readonly, error);
}
static gt_client_sink_rc_t event(void *context, uint64_t generation,
                                 uint64_t sequence,
                                 const gt_state_event_t *source,
                                 gt_status_t *error) {
  gt_session_t *s = context;
  gt_session_event_t e = {.kind = (gt_session_event_kind_t)source->kind};
  if (source->kind == GT_STATE_OUTPUT) {
    e.bytes = source->payload;
    e.length = source->length;
  } else if (source->kind == GT_STATE_RESIZE &&
             source->length == sizeof(gt_state_size_t)) {
    gt_state_size_t size;
    memcpy(&size, source->payload, sizeof(size));
    e.size = (gt_session_size_t){size.cols, size.rows, size.px_width,
                                 size.px_height};
  } else if (source->kind == GT_STATE_COLOR_SCHEME &&
             source->length == sizeof(gt_state_color_scheme_t)) {
    gt_state_color_scheme_t c;
    memcpy(&c, source->payload, sizeof(c));
    if (c.dark > 1) {
      *error = GT_ERR_PROTOCOL;
      return GT_CLIENT_SINK_FAILED;
    }
    e.dark = c.dark != 0;
  } else if(source->kind==GT_STATE_STYLE && source->length==sizeof(e.style)) {
    memcpy(&e.style,source->payload,sizeof(e.style));
    if(e.style.dark>1){*error=GT_ERR_PROTOCOL;return GT_CLIENT_SINK_FAILED;}
  } else {
    *error = GT_ERR_UNSUPPORTED;
    return GT_CLIENT_SINK_FAILED;
  }
  return (gt_client_sink_rc_t)s->sink.event(s->sink.context, generation,
                                            sequence, &e, error);
}
static gt_status_t applied(void *context, gt_client_applied_t *out) {
  gt_session_t *s = context;
  gt_session_applied_t a = {0};
  gt_status_t e = s->sink.applied(s->sink.context, &a);
  *out = (gt_client_applied_t){a.known, a.generation, a.sequence};
  return e;
}
static void invalidated(void *context, uint64_t generation, gt_status_t error) {
  gt_session_t *s = context;
  s->sink.invalidated(s->sink.context, generation, error);
}
static gt_session_command_t command(gt_worker_command_t kind) {
  return kind == GT_WORKER_INPUT    ? GT_SESSION_COMMAND_INPUT
         : kind == GT_WORKER_RESIZE ? GT_SESSION_COMMAND_RESIZE
                                    : kind==GT_WORKER_STYLE?GT_SESSION_COMMAND_STYLE:GT_SESSION_COMMAND_COLOR;
}
static void result(void *context, const gt_user_reply_t *source) {
  gt_session_t *s = context;
  gt_session_receipt_t r = {.kind = command(source->kind),
                            .stamp = {source->stamp.generation,
                                      source->stamp.command_id,
                                      source->stamp.applied_sequence},
                            .status = source->status,
                            .delivery = (gt_session_delivery_t)source->delivery,
                            .rejected = source->rejected,
                            .rejection = (uint32_t)source->rejection};
  s->sink.receipt(s->sink.context, &r);
}
static void requested(void *context, uint64_t request, uint32_t attempt) {
  gt_session_t *s = context;
  s->authentication.requested(s->authentication.context, request, attempt);
}
static void cancelled(void *context, uint64_t request) {
  gt_session_t *s = context;
  s->authentication.cancelled(s->authentication.context, request);
}
static void hostkey(void *context, uint64_t request,
                    const gt_ssh_connector_info_t *info) {
  gt_session_t *s = context;
  s->authentication.hostkey(s->authentication.context, request, info->key_type,
                            info->fingerprint);
}
static gt_status_t creation(const gt_session_create_t *in,
                            gt_create_spec_t *out) {
  memset(out, 0, sizeof(*out));
  if (!in)
    return GT_ERR_INVALID_ARG;
  memcpy(out->nonce, in->nonce, 16);
  const gt_pty_opts_t *p = &in->process;
  out->command = p->cmd_kind;
  out->cols = p->cols;
  out->rows = p->rows;
  out->px_width = p->px_width;
  out->px_height = p->px_height;
  out->environment = GT_CREATE_HOST_ENV;
  out->executable = p->executable;
  out->cwd = p->cwd;
  out->term = p->term ? p->term : "xterm-256color";
  if (p->argv)
    while (p->argv[out->argc]) {
      if (out->argc == GT_CREATE_MAX_ARGS)
        return GT_ERR_INVALID_ARG;
      out->argv[out->argc] = p->argv[out->argc];
      out->argc++;
    }
  if (p->env)
    while (p->env[out->envc]) {
      if (out->envc == GT_CREATE_MAX_ENV)
        return GT_ERR_INVALID_ARG;
      out->env[out->envc] = p->env[out->envc];
      out->envc++;
    }
  uint8_t bytes[GT_CREATE_MAX_BYTES];
  size_t n;
  gt_status_t e = gt__create_spec_encode(out, bytes, sizeof(bytes), &n);
  gt__secure_erase(bytes, sizeof(bytes));
  return e;
}
gt_status_t gt_session_open(const gt_session_options_t *o, gt_session_t **out) {
  if (out)
    *out = NULL;
  if (!o || !out)
    return GT_ERR_INVALID_ARG;
  bool pending = bytes_nonzero(o->pending_nonce, 16);
  if ((o->kind != GT_SESSION_LOCAL && o->kind != GT_SESSION_SSH) ||
      ((o->create ? 1 : 0) + (o->session ? 1 : 0) + (pending ? 1 : 0) +
           (o->catalog_only ? 1 : 0) !=
       1) ||
      ((o->session || pending) && !bytes_nonzero(o->instance, 16)) ||
      (!o->catalog_only &&
       (!o->snapshot_format || !o->sink.snapshot || !o->sink.event ||
        !o->sink.applied || !o->sink.invalidated || !o->sink.receipt)))
    return GT_ERR_INVALID_ARG;
  if (o->kind == GT_SESSION_SSH &&
      (!o->authentication.requested || !o->authentication.cancelled ||
       !o->authentication.hostkey))
    return GT_ERR_INVALID_ARG;
  gt_create_spec_t spec;
  gt_status_t error = GT_OK;
  if (o->create && (error = creation(o->create, &spec)) != GT_OK)
    return error;
  gt_session_t *s = calloc(1, sizeof(*s));
  if (!s)
    return GT_ERR_IO;
  s->sink = o->sink;
  s->authentication = o->authentication;
  bool (*clock)(void *, uint64_t *) = o->clock ? o->clock : system_clock;
  gt_session_link_t link;
  if (o->kind == GT_SESSION_LOCAL) {
    gt_local_session_provider_config_t c = {o->proxy, o->daemon, o->runtime,
                                            o->clock_context, clock};
    error = gt__local_session_provider_open(&c, NULL, &s->local);
    if (error == GT_OK && !gt__local_session_provider_link(s->local, &link))
      error = GT_ERR_INTERNAL;
  } else {
    gt_ssh_session_provider_config_t c = {
        .proxy = o->proxy,
        .daemon = o->daemon,
        .runtime = o->runtime,
        .authentication_timeout_ms =
            o->authentication_timeout_ms ? o->authentication_timeout_ms : 60000,
        .clock_context = o->clock_context,
        .clock = clock,
        .context = s,
        .requested = requested,
        .cancelled = cancelled,
        .hostkey = hostkey};
    error = gt__ssh_session_provider_open(&c, NULL, &s->ssh);
    if (error == GT_OK && !gt__ssh_session_provider_link(s->ssh, &link))
      error = GT_ERR_INTERNAL;
  }
  if (error == GT_OK) {
    gt_session_client_config_t c = {
        .link = link,
        .required_features = o->required_features,
        .session = o->session,
        .create = o->create ? &spec : NULL,
        .catalog_only = o->catalog_only,
        .recovery_timeout_ms =
            o->recovery_timeout_ms ? o->recovery_timeout_ms : 60000,
        .backoff_ms = 100,
        .bootstrap_timeout_ms = 5000,
        .management_heartbeat_ms = 1000,
        .management_timeout_ms = 5000,
        .clock_context = o->clock_context,
        .clock = clock,
        .attachment = {
            .format = o->snapshot_format,
            .snapshot_limit =
                o->snapshot_limit ? o->snapshot_limit : 16 * 1024 * 1024,
            .commands = 8,
            .restore_timeout_ms = 30000,
            .apply_timeout_ms = 10000,
            .heartbeat_ms = 1000,
            .peer_timeout_ms = 5000,
            .command_timeout_ms = 5000,
            .error_timeout_ms = 5000,
            .sink = {s, snapshot, event, applied, invalidated, result}}};
    memcpy(c.compatibility, o->compatibility, 32);
    memcpy(c.instance, o->instance, 16);
    memcpy(c.pending_nonce, o->pending_nonce, 16);
    error = gt__session_client_open(&c, NULL, &s->client);
  }
  gt__secure_erase(&spec, sizeof(spec));
  if (error != GT_OK) {
    gt__ssh_session_provider_close(s->ssh);
    (void)gt__local_session_provider_destroy(s->local);
    gt__secure_erase(s, sizeof(*s));
    free(s);
    return error;
  }
  *out = s;
  return GT_OK;
}
gt_session_state_t gt_session_step(gt_session_t *s) {
  if (!s || s->busy)
    return GT_SESSION_STATE_FAILED;
  s->busy = true;
  gt_session_phase_t p = gt__session_client_step(s->client);
  s->busy = false;
  return (gt_session_state_t)p;
}
bool gt_session_info(const gt_session_t *s, gt_session_info_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!s || !out || s->busy)
    return false;
  gt_session_client_info_t i;
  if (!gt__session_client_info(s->client, &i))
    return false;
  *out = (gt_session_info_t){
      .state = (gt_session_state_t)i.phase,
      .error = i.error,
      .reason = (uint32_t)i.reason,
      .attempts = i.attempts,
      .busy_replies = i.busy_replies,
      .session = i.session,
      .generation = i.generation,
      .readonly =
          i.phase != GT_SESSION_LIVE || i.attachment.readonly || i.input_locked,
      .has_admitted_create = i.has_admitted_create,
      .stop = {(gt_session_stop_state_t)i.stop.state, i.stop.session,
               i.stop.request, (gt_session_control_status_t)i.stop.status,
               i.stop.error}};
  memcpy(out->instance, i.instance, 16);
  if (i.has_attachment) {
    out->has_process = i.attachment.has_process;
    out->process = i.attachment.process;
    out->has_sequence = i.attachment.has_sequence;
    out->sequence = i.attachment.sequence;
    out->has_terminal = i.attachment.has_terminal;
    out->state_error = i.attachment.state_error;
    if (out->has_terminal && i.attachment.terminal.has_result) {
      gt_worker_result_t *r = &i.attachment.terminal.result;
      out->has_exit_code = r->has_exit_code;
      out->exit_code = r->has_exit_code ? r->exit_code : 0;
      out->terminal_kind = (uint32_t)r->kind;
      out->terminal_status = r->status;
      out->termination_status = r->termination_status;
    }
  }
  return true;
}
gt_status_t gt_session_catalog(gt_session_t *s, gt_session_row_t *rows,
                               size_t capacity, size_t *count) {
  if (count)
    *count = 0;
  if (!s || s->busy || !count || capacity > 64 || (capacity && !rows))
    return GT_ERR_INVALID_ARG;
  if (capacity)
    memset(rows, 0, capacity * sizeof(*rows));
  gt_managed_info_t source[GT_CONTROL_MAX_ROWS];
  size_t n = 0;
  s->busy = true;
  gt_status_t error =
      gt__session_client_catalog(s->client, source, GT_CONTROL_MAX_ROWS, &n);
  s->busy = false;
  if (error != GT_OK)
    return error;
  *count = n;
  if (capacity < n)
    return GT_ERR_INVALID_ARG;
  for (size_t i = 0; i < n; i++) {
    gt_managed_info_t *r = &source[i];
    gt_worker_result_t *v = &r->result;
    rows[i] = (gt_session_row_t){.session = r->id,
                                 .phase = (uint32_t)r->phase,
                                 .terminating = r->termination_requested,
                                 .has_result = r->has_result};
    if (r->has_result) {
      rows[i].has_sequence = v->has_sequence;
      rows[i].sequence = v->has_sequence ? v->sequence : 0;
      rows[i].has_exit_code = v->has_exit_code;
      rows[i].exit_code = v->has_exit_code ? v->exit_code : 0;
      rows[i].terminal_kind = (uint32_t)v->kind;
      rows[i].status = v->status;
      rows[i].termination_status = v->termination_status;
    }
  }
  return GT_OK;
}
gt_session_submit_t gt_session_catalog_refresh(gt_session_t *s) {
  if (!s || s->busy)
    return GT_SESSION_SUBMIT_INVALID;
  s->busy = true;
  gt_client_submit_t rc = gt__session_client_catalog_refresh(s->client);
  s->busy = false;
  return (gt_session_submit_t)rc;
}
gt_session_submit_t gt_session_terminate(gt_session_t *s, uint64_t target) {
  if (!s || s->busy)
    return GT_SESSION_SUBMIT_INVALID;
  s->busy = true;
  gt_client_submit_t rc = gt__session_client_terminate(s->client, target);
  s->busy = false;
  return (gt_session_submit_t)rc;
}
static gt_session_submit_t submit(gt_session_t *s, uint64_t generation,
                                  uint64_t sequence, gt_user_request_t *request,
                                  gt_session_stamp_t *out) {
  if (out)
    memset(out, 0, sizeof(*out));
  if (!s || s->busy || !out)
    return GT_SESSION_SUBMIT_INVALID;
  s->busy = true;
  gt_worker_stamp_t stamp;
  gt_client_submit_t rc = gt__session_client_submit_at(
      s->client, generation, sequence, request, &stamp);
  s->busy = false;
  if (rc == GT_CLIENT_QUEUED)
    *out = (gt_session_stamp_t){stamp.generation, stamp.command_id,
                                stamp.applied_sequence};
  return (gt_session_submit_t)rc;
}
gt_session_submit_t gt_session_input(gt_session_t *s, uint64_t generation,
                                     uint64_t sequence, const void *bytes,
                                     size_t n, gt_session_stamp_t *out) {
  gt_user_request_t r = {.kind = GT_WORKER_INPUT, .input = bytes, .length = n};
  return submit(s, generation, sequence, &r, out);
}
gt_session_submit_t gt_session_resize(gt_session_t *s, uint64_t generation,
                                      uint64_t sequence, gt_session_size_t size,
                                      gt_session_stamp_t *out) {
  gt_user_request_t r = {
      .kind = GT_WORKER_RESIZE,
      .size = {size.cols, size.rows, size.px_width, size.px_height}};
  return submit(s, generation, sequence, &r, out);
}
gt_session_submit_t gt_session_color(gt_session_t *s, uint64_t generation,
                                     uint64_t sequence, bool dark,
                                     gt_session_stamp_t *out) {
  gt_user_request_t r = {.kind = GT_WORKER_COLOR_SCHEME,
                         .color = {dark ? 1 : 0}};
  return submit(s, generation, sequence, &r, out);
}
gt_status_t gt_session_auth_supply(gt_session_t *s, uint64_t request,
                                   gt_ssh_t **ssh, uint8_t family,
                                   const uint8_t address[16], uint32_t scope) {
  if (!s || s->busy || !address)
    return GT_ERR_INVALID_ARG;
  if (!s->ssh)
    return GT_ERR_UNSUPPORTED;
  gt_ssh_connector_config_t c = {.family = family, .scope = scope};
  memcpy(c.address, address, 16);
  s->busy = true;
  gt_status_t e = gt__ssh_session_provider_supply(s->ssh, request, ssh, &c);
  s->busy = false;
  return e;
}
gt_status_t gt_session_auth_adopt(gt_session_t *s,uint64_t request,gt_ssh_t **ssh) {
  if(!s || s->busy || !ssh || !*ssh)return GT_ERR_INVALID_ARG;
  if(!s->ssh)return GT_ERR_UNSUPPORTED;
  s->busy=true;gt_status_t error=gt__ssh_session_provider_adopt(s->ssh,request,ssh);s->busy=false;
  return error;
}
gt_status_t gt_session_auth_reject(gt_session_t *s, uint64_t request,
                                   gt_status_t error) {
  if (!s || s->busy)
    return GT_ERR_INVALID_ARG;
  if (!s->ssh)
    return GT_ERR_UNSUPPORTED;
  s->busy = true;
  gt_status_t e = gt__ssh_session_provider_reject(s->ssh, request, error);
  s->busy = false;
  return e;
}
gt_status_t gt_session_auth_decide(gt_session_t *s, uint64_t request,
                                   gt_hostkey_decision_t decision) {
  if (!s || s->busy)
    return GT_ERR_INVALID_ARG;
  if (!s->ssh)
    return GT_ERR_UNSUPPORTED;
  s->busy = true;
  gt_status_t e = gt__ssh_session_provider_decide(s->ssh, request, decision);
  s->busy = false;
  return e;
}
void gt_session_cancel(gt_session_t *s) {
  if (!s || s->busy || s->cancelled)
    return;
  s->busy = true;
  s->cancelled = true;
  gt__session_client_cancel(s->client);
  s->busy = false;
}
bool gt_session_close(gt_session_t *s) {
  if (!s)
    return true;
  if (s->busy)
    return false;
  gt_session_cancel(s);
  s->busy = true;
  if (s->local && !gt__local_session_provider_quiesce(s->local)) {
    s->busy = false;
    return false;
  }
  gt__session_client_close(s->client);
  gt__ssh_session_provider_close(s->ssh);
  (void)gt__local_session_provider_destroy(s->local);
  gt__secure_erase(s, sizeof(*s));
  free(s);
  return true;
}

gt_session_submit_t gt_session_style(gt_session_t *s,uint64_t generation,uint64_t sequence,
                                     const gt_session_style_t *style,gt_session_stamp_t *stamp) {
  if(!style || style->dark>1){if(stamp)memset(stamp,0,sizeof(*stamp));return GT_SESSION_SUBMIT_INVALID;}
  gt_user_request_t request={.kind=GT_WORKER_STYLE,.style=*style};
  return submit(s,generation,sequence,&request,stamp);
}
