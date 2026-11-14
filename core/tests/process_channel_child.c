/* Disposable child used by public C / Swift / JNI transport tests. No shell,
 * network, user files or model. Packaged only in the Android test APK. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <signal.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef GT_PROCESS_CHANNEL_STANDALONE
#include "ghosterm.h"
#endif

static void exact(int fd, const void *bytes, size_t size) {
    size_t at = 0;
    while (at < size) {
        ssize_t n = write(fd, (const char *)bytes + at, size - at);
        if (n <= 0) {
            _exit(91);
        }
        at += (size_t)n;
    }
}

static void term(int number) {
    (void)number;
    _exit(37);
}

static void field(const char *text) {
    exact(1, text, strlen(text) + 1);
}

#ifndef GT_PROCESS_CHANNEL_STANDALONE
static int session(void) {
    gt_ext_channel_t *channel = NULL;
    gt_ext_session_t *owner = NULL;
    gt_ext_session_config_t c = {.required_count = 1,
                                 .available_count = 1,
                                 .budgets = {768, 65536, 262144},
                                 .handshake_ms = 5000,
                                 .consent_ms = 5000,
                                 .peer = {GT_EXT_PEER_LOCAL, "process-fixture"},
                                 .features = GT_EXT_FEATURE_REQUESTS};
    strcpy(c.plugin_id, "org.ghosterm.process-fixture");
    strcpy(c.plugin_version, "0.1.0");
    memset(c.client_digest, 'd', 64);
    memset(c.executor_digest, 'e', 64);
    strcpy(c.host_version, "0.1.0");
    strcpy(c.sdk_version, "1.0.0");
    strcpy(c.required[0], "ui.form");
    strcpy(c.available[0], "ui.form");
    gt_ext_rpc_method_t method = {.receiver = GT_EXT_EXECUTOR,
                                  .input_shape = GT_EXT_JSON_OBJECT,
                                  .output_shape = GT_EXT_JSON_OBJECT};
    strcpy(method.method, "fixture.echo");
    strcpy(method.capability, "ui.form");
    if (gt_ext_channel_open_fd(0, 1, &channel) != GT_OK ||
        gt_ext_session_open_rpc(&channel, GT_EXT_EXECUTOR, &c, &method, 1, &owner) != GT_OK) {
        return 92;
    }
    for (;;) {
        gt_ext_session_phase_t phase = gt_ext_session_step(owner, 1);
        if (phase >= GT_EXT_SESSION_ENDED) {
            /* Runtime may send revoke before closing its transport. Keep this
             * test child alive until the provider's TERM so the expected exit
             * is deterministic; protocol end alone is not process exit. */
            for (;;) {
                pause();
            }
        }
    }
}
#endif

int main(int argc, char **argv) {
    alarm(8); /* independent fault-fixture ceiling, never an expected test red */
    signal(SIGTERM, term);
    if (argc < 2 || isatty(0) || isatty(1) || isatty(2)) {
        return 90;
    }
    static const uint8_t diagnostic[] = {'D', 0, 255, '\n'};
    exact(2, diagnostic, sizeof(diagnostic));
    if (!strcmp(argv[1], "context")) {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            return 93;
        }
        for (int i = 0; i < argc; i++) {
            field(argv[i]);
        }
        field(cwd);
        field(getenv("GT_SELECTED") ? getenv("GT_SELECTED") : "missing");
        field(getenv("HOME") ? "ambient-home" : "no-home");
        field(getenv("PATH") ? "ambient-path" : "no-path");
        return 23;
    }
    if (!strcmp(argv[1], "tail")) {
        exact(1, "tail\0\377", 6);
        return 23;
    }
    if (!strcmp(argv[1], "signal")) {
        raise(SIGUSR1);
        return 94;
    }
    if (!strcmp(argv[1], "stderr")) {
        uint8_t bytes[8192];
        memset(bytes, 63, sizeof(bytes));
        exact(2, bytes, sizeof(bytes));
        return 23;
    }
#ifndef GT_PROCESS_CHANNEL_STANDALONE
    if (!strcmp(argv[1], "session")) {
        return session();
    }
#endif
    if (!strcmp(argv[1], "ignore")) {
        signal(SIGTERM, SIG_IGN);
    }
    if (!strcmp(argv[1], "hold-silent")) {
        int fd = open("ready", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            return 95;
        }
        exact(fd, "R", 1);
        close(fd);
        for (;;) {
            pause();
        }
    }
    if (!strcmp(argv[1], "eof-hold")) {
        close(1);
        for (;;) {
            pause();
        }
    }
    exact(1, "R", 1);
    if (!strcmp(argv[1], "echo")) {
        uint8_t bytes[16384];
        for (;;) {
            ssize_t n = read(0, bytes, sizeof(bytes));
            if (n <= 0) {
                return 24;
            }
            exact(1, bytes, (size_t)n);
        }
    }
    for (;;) {
        pause();
    }
}
