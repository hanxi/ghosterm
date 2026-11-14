/* Same strict private mailbox as Session and Resource RPC. */
static bool process_jobs_status(gt_process_rpc_t *p, gt_status_t s) {
    const char *name = s == GT_OK                  ? "ok"
                       : s == GT_PROCESS_JOBS_WAIT ? "wait"
                       : s == GT_ERR_AUTH          ? "denied"
                       : s == GT_ERR_UNSUPPORTED   ? "unsupported"
                       : s == GT_ERR_INVALID_ARG   ? "invalid"
                                                   : "error";
    return process_status(p, GT_EXT_RPC_OK) &&
           process_append(p, ",\"value\":{\"status\":\"%s\"", name);
}

static bool process_jobs_dispatch(gt_process_rpc_t *p) {
    static const char *const basic[] = {"v", "id", "op", "action"};
    static const char *const spawn[] = {
        "v", "id", "op", "action", "program", "root", "argv", "envPolicy"};
    static const char *const refkeys[] = {"v", "id", "op", "action", "ref"};
    static const char *const readkeys[] = {"v", "id", "op", "action", "ref", "maxBytes", "stderr"};
    static const char *const writekeys[] = {"v", "id", "op", "action", "ref", "hex"};
    static const char *const signalkeys[] = {"v", "id", "op", "action", "ref", "signal"};
    process_json_t *d = p->json;
    char action[24];
    if (!process_text(d, process_member(d, "action"), action, sizeof(action))) {
        return false;
    }
    if (!strcmp(action, "available")) {
        return process_keys(d, basic, 4) && process_jobs_status(p, GT_OK) &&
               process_append(p, ",\"available\":%s}", p->jobs ? "true" : "false");
    }
    if (!strcmp(action, "spawn")) {
        char program[129], root[65], policy[40];
        uint16_t array = process_member(d, "argv");
        if (!process_keys(d, spawn, 8) ||
            !process_text(d, process_member(d, "program"), program, sizeof(program)) ||
            !process_text(d, process_member(d, "root"), root, sizeof(root)) ||
            !process_text(d, process_member(d, "envPolicy"), policy, sizeof(policy)) ||
            strcmp(policy, "selected-user-environment") || d->nodes[array].kind != J_ARRAY ||
            !d->nodes[array].count || d->nodes[array].count > GT_EXT_PROCESS_ARGUMENTS_MAX) {
            return false;
        }
        const char *argv[GT_EXT_PROCESS_ARGUMENTS_MAX];
        size_t argc = 0, used = 0;
        for (uint16_t i = d->nodes[array].child; i; i = d->nodes[i].next) {
            const json_node_t *n = &d->nodes[i];
            if (n->kind != J_STRING || n->size > GT_EXT_PROCESS_ARGUMENT_MAX ||
                memchr(d->strings + n->text, 0, n->size) || (!argc && !n->size) ||
                n->size + 1 > GT_EXT_PROCESS_ARGUMENT_BYTES_MAX - used) {
                return false;
            }
            argv[argc++] = (const char *)d->strings + n->text;
            used += n->size + 1;
        }
        if (!p->jobs) {
            return process_jobs_status(p, GT_ERR_UNSUPPORTED) && process_append(p, "}");
        }
        process_reference_t *slot = process_empty(p->children);
        if (!slot || p->reference == UINT64_MAX) {
            return process_jobs_status(p, GT_PROCESS_JOBS_WAIT) && process_append(p, "}");
        }
        uint64_t native = 0;
        const char *version = NULL;
        gt_status_t s =
            gt__process_jobs_spawn(p->jobs, program, root, argv, argc, &native, &version);
        if (!process_jobs_status(p, s)) {
            return false;
        }
        if (s == GT_OK) {
            *slot = (process_reference_t){++p->reference, native};
            if (!process_append(p, ",\"ref\":\"%" PRIu64 "\",\"version\":", slot->local) ||
                !process_resource_string(p, version)) {
                return false;
            }
        }
        return process_append(p, "}");
    }
    bool read = !strcmp(action, "read"), write = !strcmp(action, "write"),
         signal = !strcmp(action, "signal");
    bool poll = !strcmp(action, "poll"), release = !strcmp(action, "release"),
         close = !strcmp(action, "closeStdin");
    uint64_t ref, maximum = 0;
    if ((!read && !write && !signal && !poll && !release && !close) ||
        !process_keys(d,
                      read     ? readkeys
                      : write  ? writekeys
                      : signal ? signalkeys
                               : refkeys,
                      read                ? 7
                      : (write || signal) ? 6
                                          : 5) ||
        !process_counter(d, process_member(d, "ref"), &ref)) {
        return false;
    }
    uint8_t bytes[GT_PROCESS_JOBS_IO_MAX];
    size_t size = 0;
    bool errors = false, kill = false;
    if (read) {
        uint16_t n = process_member(d, "stderr");
        if (!node_uint(d, process_member(d, "maxBytes"), &maximum) || !maximum ||
            maximum > GT_EXT_PROCESS_IO_MAX ||
            (d->nodes[n].kind != J_TRUE && d->nodes[n].kind != J_FALSE)) {
            return false;
        }
        errors = d->nodes[n].kind == J_TRUE;
    }
    if (write) {
        uint16_t n = process_member(d, "hex");
        if (d->nodes[n].kind != J_STRING || !d->nodes[n].size || d->nodes[n].size % 2 ||
            d->nodes[n].size > 2 * sizeof(bytes)) {
            return false;
        }
        const char *s = (const char *)d->strings + d->nodes[n].text;
        size = d->nodes[n].size / 2;
        for (size_t i = 0; i < size; i++) {
            unsigned v = 0;
            for (size_t k = 0; k < 2; k++) {
                char c = s[i * 2 + k];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                    return false;
                }
                v = v * 16 + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
            }
            bytes[i] = (uint8_t)v;
        }
    }
    if (signal) {
        char s[16];
        if (!process_text(d, process_member(d, "signal"), s, sizeof(s)) ||
            (strcmp(s, "terminate") && strcmp(s, "kill"))) {
            return false;
        }
        kill = !strcmp(s, "kill");
    }
    process_reference_t *slot = process_find(p->children, ref);
    if (!p->jobs || !slot) {
        return process_jobs_status(p, p->jobs ? GT_ERR_INVALID_ARG : GT_ERR_UNSUPPORTED) &&
               process_append(p, "}");
    }
    if (read || write) {
        size_t cap = maximum > sizeof(bytes) ? sizeof(bytes) : (size_t)maximum;
        gt_ext_process_io_t io =
            read ? gt__process_jobs_read(p->jobs, slot->native, errors, bytes, cap)
                 : gt__process_jobs_write(p->jobs, slot->native, bytes, size);
        static const char *const states[] = {"data", "wait", "eof", "closed", "error"};
        if ((unsigned)io.state >= 5 || !process_jobs_status(p, GT_OK) ||
            !process_append(p,
                            ",\"state\":\"%s\",\"count\":%zu,\"error\":%d",
                            states[io.state],
                            io.count,
                            io.error)) {
            return false;
        }
        if (read) {
            if (!process_append(p, ",\"hex\":\"")) {
                return false;
            }
            for (size_t i = 0; i < io.count; i++) {
                if (!process_append(p, "%02x", bytes[i])) {
                    return false;
                }
            }
            if (!process_append(p, "\"")) {
                return false;
            }
        }
        return process_append(p, "}");
    }
    if (poll) {
        gt_process_job_info_t info;
        gt_status_t s = gt__process_jobs_poll(p->jobs, slot->native, &info);
        return process_jobs_status(p, s) &&
               (s != GT_OK ||
                process_append(p,
                               ",\"exited\":%s,\"code\":%d,\"signal\":%d,\"reason\":\"%s\"",
                               info.process.exited ? "true" : "false",
                               info.process.exit_code,
                               info.process.term_signal,
                               info.reason)) &&
               process_append(p, "}");
    }
    gt_status_t s = release ? gt__process_jobs_release(p->jobs, slot->native)
                    : close ? gt__process_jobs_input_close(p->jobs, slot->native)
                            : gt__process_jobs_signal(p->jobs, slot->native, kill);
    if (s == GT_OK && release) {
        memset(slot, 0, sizeof(*slot));
    }
    return process_jobs_status(p, s) && process_append(p, "}");
}
