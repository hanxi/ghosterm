/* Explicit development ProcessService configuration, no ambient resolution. */
static struct {
    const char *root;
    int root_fd;
    gt_ext_runtime_root_t workspace;
    gt_ext_process_program_mapping_t programs[16];
    gt_ext_process_environment_t environment[32];
    size_t program_count, environment_count;
    uint32_t lifetime;
    uint64_t output;
} kit_process = {.root_fd = -1};

static uint64_t kit_process_number(const char *s, uint64_t maximum) {
    char *end;
    unsigned long long value = strtoull(s, &end, 10);
    kit_check(*s && !*end && value <= maximum, "process configuration number");
    return value;
}

static int kit_process_parse(int argc, char **argv, int offset) {
    kit_check(argc >= offset + 4, "process configuration fields");
    kit_process.root = argv[offset++];
    kit_process.lifetime = (uint32_t)kit_process_number(argv[offset++], 600000);
    kit_process.output = kit_process_number(argv[offset++], 1048576);
    kit_process.program_count = (size_t)kit_process_number(argv[offset++], 16);
    kit_check(kit_process.program_count && argc >= offset + (int)kit_process.program_count * 4 + 1,
              "process mappings present");
    for (size_t i = 0; i < kit_process.program_count; i++) {
        kit_process.programs[i] =
            (gt_ext_process_program_mapping_t){.name = argv[offset],
                                               .executable = argv[offset + 1],
                                               .verified_version = argv[offset + 2],
                                               .verified_sha256 = argv[offset + 3]};
        offset += 4;
    }
    kit_process.environment_count = (size_t)kit_process_number(argv[offset++], 32);
    kit_check(argc >= offset + (int)kit_process.environment_count * 2,
              "selected process environment fields");
    for (size_t i = 0; i < kit_process.environment_count; i++) {
        kit_process.environment[i] = (gt_ext_process_environment_t){argv[offset], argv[offset + 1]};
        offset += 2;
    }
    return offset;
}

static void kit_process_inputs_close(void) {
    if (kit_process.root_fd >= 0) {
        close(kit_process.root_fd);
        kit_process.root_fd = -1;
    }
}

static void kit_process_declarations(const gt_ext_package_metadata_t *metadata) {
    const char *caps[] = {"process.spawn", "process.pipes"};
    for (size_t i = 0; i < 2; i++) {
        bool found = false;
        for (size_t j = 0; j < metadata->requires_count; j++) {
            found |= !strcmp(metadata->requires[j], caps[i]);
        }
        kit_check(found, "explicit Executor process declarations");
    }
}

static void kit_process_capabilities(gt_ext_session_config_t *config) {
    const char *caps[] = {"process.spawn", "process.pipes"};
    for (size_t i = 0; i < 2; i++) {
        strcpy(config->required[config->required_count++], caps[i]);
        strcpy(config->available[config->available_count++], caps[i]);
    }
}

static void kit_process_apply(gt_ext_runtime_config_t *options) {
    kit_check(!atexit(kit_process_inputs_close), "process input cleanup registration");
    kit_process.root_fd = open(kit_process.root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    kit_check(kit_process.root_fd >= 0, "explicit business workspace directory");
    kit_process.workspace = (gt_ext_runtime_root_t){"workspace", kit_process.root_fd, 262144};
    options->roots = &kit_process.workspace;
    options->root_count = 1;
    options->process_programs = kit_process.programs;
    options->process_program_count = kit_process.program_count;
    options->process_environment = kit_process.environment;
    options->process_environment_count = kit_process.environment_count;
    options->process_lifetime_ms = kit_process.lifetime;
    options->process_output_bytes = kit_process.output;
}
