/* Deterministic real-SFTP ownership race. Compile the unmodified production
 * implementation with test-only scheduling wrappers and AddressSanitizer.
 * Only the submitting thread pauses after unlocking; the real worker completes
 * IO and actually frees the job before submit_job is allowed to return. */
#include "ghosterm.h"
#include "internal.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_t submitter;
static atomic_bool armed, released;
static _Atomic(void *) submitted_job;
static void *submit_alloc(size_t size);
static void submit_free(void *pointer);
static int submit_unlock(pthread_mutex_t *mutex);
#define gt__alloc submit_alloc
#define free submit_free
#define pthread_mutex_unlock submit_unlock
#include "../src/sftp.c"
#undef gt__alloc
#undef free
#undef pthread_mutex_unlock

static void *submit_alloc(size_t size) {
    void *pointer = gt__alloc(size);
    if (atomic_load(&armed) && pthread_equal(pthread_self(), submitter) &&
        !atomic_load(&submitted_job)) {
        if (size != sizeof(sftp_job_t) || !pointer) abort();
        atomic_store(&submitted_job, pointer);
    }
    return pointer;
}
static void submit_free(void *pointer) {
    bool job = pointer && pointer == atomic_load(&submitted_job);
    free(pointer);
    if (job) atomic_store(&released, true);
}
static int submit_unlock(pthread_mutex_t *mutex) {
    int result = pthread_mutex_unlock(mutex);
    if (pthread_equal(pthread_self(), submitter) && atomic_exchange(&armed, false)) {
        int64_t deadline = mono_ms() + 5000;
        while (!atomic_load(&released)) {
            if (mono_ms() >= deadline) {
                fputs("FAIL: real worker did not free submitted job\n", stderr);
                exit(2);
            }
            struct timespec delay = {.tv_nsec = 1000000};
            nanosleep(&delay, NULL);
        }
    }
    return result;
}
static gt_hostkey_decision_t trust(void *u, const char *h, uint16_t p,
                                  const char *t, const char *f) {
    (void)u; (void)h; (void)p; (void)t; (void)f;
    return GT_HOSTKEY_ACCEPT;
}
static void prepare(void) {
    atomic_store(&submitted_job, NULL);
    atomic_store(&released, false);
    atomic_store(&armed, true);
}
static int verify(uint64_t id) {
    if (!id || !atomic_load(&released)) return 1;
    gt_event_t events[32];
    size_t count = gt_core_poll_events(events, 32);
    int found = 0;
    for (size_t i = 0; i < count; i++)
        if (events[i].kind == GT_EVENT_JOB_DONE && events[i].job_id == id &&
            events[i].code == GT_JOB_OK) found++;
    return found != 1;
}
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    submitter = pthread_self();
    char known[4096], source[4096], remote[4096], target[4096];
    snprintf(known, sizeof known, "%s/known", argv[5]);
    snprintf(source, sizeof source, "%s/submit-source", argv[5]);
    snprintf(remote, sizeof remote, "%s/submit-remote", argv[5]);
    snprintf(target, sizeof target, "%s/submit-target", argv[5]);
    char key[65536];
    FILE *file = fopen(argv[4], "rb");
    if (!file) return 2;
    size_t length = fread(key, 1, sizeof key - 1, file);
    if (ferror(file) || !feof(file) || !length) return 2;
    fclose(file); key[length] = 0;
    gt_ssh_opts_t options = {.host = argv[1], .port = (uint16_t)atoi(argv[2]),
                            .username = argv[3], .known_hosts_path = known};
    gt_ssh_t *ssh = gt_ssh_new(&options);
    if (!ssh) return 2;
    int status = gt_ssh_auth_private_key(ssh, key, NULL);
    volatile char *wipe = key;
    for (size_t i = 0; i < sizeof key; i++) wipe[i] = 0;
    if (status) return 2;
    gt_ssh_set_hostkey_policy(ssh, trust, NULL);
    if (gt_ssh_connect(ssh)) return 2;
    gt_sftp_t *sftp = gt_ssh_sftp(ssh);
    if (!sftp) return 2;
    file = fopen(source, "wb");
    if (!file || fwrite("job", 1, 3, file) != 3 || fclose(file)) return 2;
    prepare();
    if (verify(gt_sftp_upload(sftp, source, remote))) return 1;
    prepare();
    if (verify(gt_sftp_download(sftp, remote, target))) return 1;
    atomic_store(&submitted_job, NULL);
    gt_sftp_close(sftp); gt_ssh_close(ssh);
    file = fopen(target, "rb");
    char data[4] = {0};
    if (!file || fread(data, 1, sizeof data, file) != 3 || memcmp(data, "job", 3)) return 1;
    fclose(file);
    puts("SFTP SUBMIT PASS: real upload/download IDs survive worker release");
    return 0;
}
