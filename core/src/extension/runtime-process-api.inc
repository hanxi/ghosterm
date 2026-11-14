/* Public opaque Runtime adaptation for the checked POSIX Node/Exec owner. */
static gt_status_t runtime_execution_environment(const gt_ext_process_environment_t *values,
                                                 size_t count,
                                                 char bytes[16384],
                                                 const char *pointers[32]) {
    if (count > 32 || (count && !values)) {
        return GT_ERR_INVALID_ARG;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (!values[i].name || !values[i].value) {
            return GT_ERR_INVALID_ARG;
        }
        size_t n = strnlen(values[i].name, 129), v = strnlen(values[i].value, 4097);
        if (!n || n > 128 || v > 4096 || n + v + 2 > 16384 - used) {
            return GT_ERR_INVALID_ARG;
        }
        for (size_t k = 0; k < n; k++) {
            char c = values[i].name[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                  (k && c >= '0' && c <= '9'))) {
                return GT_ERR_INVALID_ARG;
            }
        }
        pointers[i] = bytes + used;
        memcpy(bytes + used, values[i].name, n);
        bytes[used + n] = '=';
        memcpy(bytes + used + n + 1, values[i].value, v + 1);
        used += n + v + 2;
    }
    return gt__ext_process_environment_valid(pointers, count) ? GT_OK : GT_ERR_INVALID_ARG;
}

static gt_status_t runtime_process_open(gt_ext_session_t **session,
                                        gt_ext_package_t *package,
                                        const gt_ext_runtime_config_t *config,
                                        bool associated,
                                        gt_ext_runtime_t **out) {
    gt_package_check_info_t package_info;
    const gt_ext_runtime_execution_t *e = config->execution;
    if (config->role != GT_EXT_EXECUTOR ||
        !gt__package_checked_info(package->checked, &package_info) ||
        (strcmp(package_info.runtime, "node") && strcmp(package_info.runtime, "exec"))) {
        return GT_ERR_UNSUPPORTED;
    }
    if (config->memory_bytes || config->stack_bytes || config->cpu_ms || config->jobs_per_step) {
        return GT_ERR_UNSUPPORTED;
    }
    /* Validate the separate native launch approval before reading the SDK. */
    if (!e->execution_authorized) {
        return GT_ERR_AUTH;
    }
    bool node = !strcmp(package_info.runtime, "node");
    if (!node && (e->node_executable || e->node_sha256 || e->node_sdk_catalog_sha256 ||
                  e->node_sdk_directory_fd != -1)) {
        return GT_ERR_INVALID_ARG;
    }
    if (node && (e->node_sdk_directory_fd < 0 || !e->node_executable || !e->node_sha256 ||
                 !e->node_sdk_catalog_sha256)) {
        return GT_ERR_INVALID_ARG;
    }
    char environment[16384] = {0}, business_environment[16384] = {0};
    const char *variables[32] = {0}, *business_variables[32] = {0};
    gt_status_t status =
        runtime_execution_environment(e->environment, e->environment_count, environment, variables);
    if (status == GT_OK) {
        status = runtime_execution_environment(config->process_environment,
                                               config->process_environment_count,
                                               business_environment,
                                               business_variables);
    }
    gt_package_snapshot_t *sdk = NULL;
    gt_ext_runtime_t *r = NULL;
    if (status != GT_OK) {
        goto done;
    }
    if (node) {
        gt_package_capture_config_t capture = {
            e->node_sdk_directory_fd, 1024, 4096, 16777216, 67108864};
        gt_package_status_t captured = gt__package_capture_open(&capture, NULL, &sdk);
        while (captured == GT_PACKAGE_OK || captured == GT_PACKAGE_WAIT) {
            captured = gt__package_capture_step(sdk);
            if (captured != GT_PACKAGE_WAIT) {
                break;
            }
        }
        if (captured != GT_PACKAGE_OK) {
            status = captured == GT_PACKAGE_INVALID ? GT_ERR_INVALID_ARG : GT_ERR_IO;
            goto done;
        }
    }
    r = gt__plugin_memory_calloc(1, sizeof(*r));
    if (!r) {
        status = GT_ERR_IO;
        goto done;
    }
    gt_runtime_root_t roots[GT_EXT_RUNTIME_ROOTS_MAX];
    for (size_t i = 0; i < config->root_count; i++) {
        roots[i] = (gt_runtime_root_t){
            config->roots[i].id, config->roots[i].directory_fd, config->roots[i].byte_limit};
    }
    gt_process_runtime_config_t c = {
        .stage_parent_fd = e->stage_parent_fd,
        .stage_parent_path = e->stage_parent_path,
        .execution_authorized = e->execution_authorized,
        .command_associated = associated,
        .node_executable = e->node_executable,
        .node_sha256 = e->node_sha256,
        .sdk_catalog_sha256 = e->node_sdk_catalog_sha256,
        .invocation = config->invocation,
        .environment = variables,
        .environment_count = e->environment_count,
        .roots = roots,
        .root_count = config->root_count,
        .activation_ms = config->activation_ms,
        .lifetime_ms = e->lifetime_ms,
        .stop_grace_ms = e->stop_grace_ms,
        .stdout_bytes = e->stdout_bytes,
        .stderr_bytes = e->stderr_bytes,
        .process = {.programs = config->process_programs,
                    .program_count = config->process_program_count,
                    .environment = business_variables,
                    .environment_count = config->process_environment_count,
                    .lifetime_ms = config->process_lifetime_ms,
                    .output_bytes = config->process_output_bytes}};
    status = gt__process_runtime_open(session, package->checked, &sdk, &c, &r->process);
    if (status != GT_OK) {
        gt__plugin_memory_free(NULL, r, sizeof(*r));
        r = NULL;
        goto done;
    }
    r->process_mode = true;
    *out = r;
done:
    gt__secure_erase(environment, sizeof(environment));
    gt__secure_erase(business_environment, sizeof(business_environment));
    if (sdk) {
        (void)gt__package_snapshot_destroy(&sdk);
    }
    return status;
}

static bool runtime_process_info(const gt_ext_runtime_t *r, gt_ext_runtime_info_t *out) {
    if (!r->process) {
        *out = r->final_process_info;
        return true;
    }
    gt_process_runtime_info_t i;
    if (!gt__process_runtime_info(r->process, &i)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->external_process = true;
    out->phase =
        i.phase == GT_PROCESS_RUNTIME_CREATED  ? GT_EXT_RUNTIME_CREATED
        : i.phase == GT_PROCESS_RUNTIME_ACTIVE ? GT_EXT_RUNTIME_ACTIVE
        : i.phase == GT_PROCESS_RUNTIME_CLOSED ? GT_EXT_RUNTIME_CLOSED
        : i.phase == GT_PROCESS_RUNTIME_STOPPING
            ? (i.error == GT_ERR_CANCELLED ? GT_EXT_RUNTIME_STOPPED : GT_EXT_RUNTIME_FAILED)
            : GT_EXT_RUNTIME_STARTING;
    out->error = i.error == GT_OK              ? GT_EXT_RUNTIME_NONE
                 : i.error == GT_ERR_CANCELLED ? GT_EXT_RUNTIME_CANCELLED
                 : i.error == GT_ERR_PROTOCOL  ? GT_EXT_RUNTIME_PROTOCOL
                                               : GT_EXT_RUNTIME_IO;
    out->execution_error = i.error;
    out->cleanup_error = i.cleanup_error;
    out->session_end = i.session_end;
    out->session_error = i.session_error;
    out->activated = i.activated;
    out->child_exited = i.child_exited;
    out->probe_verified = i.probe_verified;
    out->exit_code = i.exit_code;
    out->term_signal = i.term_signal;
    out->group_signal_error = i.group_signal_error;
    out->stdout_bytes = i.stdout_bytes;
    out->stderr_bytes = i.stderr_bytes;
    out->steps = r->process_steps;
    out->engine_released = i.phase == GT_PROCESS_RUNTIME_CLOSED;
    return true;
}
