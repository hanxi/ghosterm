/* Real SFTP worker/bytes plus narrowly scoped local libc fault injection.
 * Production sftp.c is compiled unchanged here; no test hooks ship in Core. */
#include "ghosterm.h"
#include "internal.h"
#include <errno.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *mode, *local_path;
static FILE *tracked;
static atomic_int injected, local_opens, local_closes, remote_closes;
static FILE *file_open(const char *, const char *);
static int file_close(FILE *);
static int file_seek(FILE *, off_t, int);
static off_t file_tell(FILE *);
static int tracked_remote_close(LIBSSH2_SFTP_HANDLE *);
static ssize_t remote_write(LIBSSH2_SFTP_HANDLE *, const char *, size_t);
#define fopen file_open
#define fclose file_close
#define fseeko file_seek
#define ftello file_tell
#define libssh2_sftp_write remote_write
#define libssh2_sftp_close_handle tracked_remote_close
#include "../src/sftp.c"
#undef fopen
#undef fclose
#undef fseeko
#undef ftello
#undef libssh2_sftp_write
#undef libssh2_sftp_close_handle

static bool is(const char *value) { return !strcmp(mode, value); }
static FILE *file_open(const char *path, const char *flags) {
    if (!strcmp(path, local_path) && is("download-local-open")) {
        atomic_fetch_add(&injected, 1); errno = EACCES; return NULL;
    }
    FILE *file = fopen(path, flags);
    if (file && !strcmp(path, local_path)) { tracked = file; atomic_fetch_add(&local_opens, 1); }
    return file;
}
static int file_close(FILE *file) {
    if (file == tracked) {
        tracked = NULL; atomic_fetch_add(&local_closes, 1);
        if (is("download-local-close") || is("upload-local-close")) {
            /* Force a real libc EBADF, including the buffered download flush.
             * No descriptor is opened/reused between close and fclose. */
            if (close(fileno(file))) abort();
            int result = fclose(file);
            if (result != EOF) abort();
            atomic_fetch_add(&injected, 1);
            return result;
        }
    }
    return fclose(file);
}
static int file_seek(FILE *file, off_t offset, int whence) {
    if (file == tracked && ((is("upload-seek-end") && whence == SEEK_END) ||
                           (is("upload-seek-start") && whence == SEEK_SET))) {
        atomic_fetch_add(&injected, 1); errno = EIO; return -1;
    }
    return fseeko(file, offset, whence);
}
static off_t file_tell(FILE *file) {
    if (file == tracked && is("upload-tell")) {
        atomic_fetch_add(&injected, 1); errno = EIO; return -1;
    }
    return ftello(file);
}
static int tracked_remote_close(LIBSSH2_SFTP_HANDLE *handle) {
    atomic_fetch_add(&remote_closes, 1);
    return libssh2_sftp_close_handle(handle);
}
static ssize_t remote_write(LIBSSH2_SFTP_HANDLE *handle, const char *buffer, size_t length) {
    if (is("upload-zero-write") && atomic_fetch_add(&injected, 1) == 0) return 0;
    return libssh2_sftp_write(handle, buffer, length);
}
static int failures;
#define CHECK(c, text) do { if (!(c)) { puts("FAIL: " text); failures++; } } while (0)
static const char content[] = "copy\0传输\xff\n";
static const char sentinel[] = "existing target must survive preflight failures and queued cancellation";
static void put(const char *path, const char *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, size, f) != size || fclose(f)) abort();
}
static bool equal(const char *path, const char *data, size_t size) {
    char buffer[256]; FILE *f = fopen(path, "rb"); if (!f) return false;
    size_t count = fread(buffer, 1, sizeof buffer, f);
    bool result = !ferror(f) && feof(f) && count == size && !memcmp(buffer, data, size);
    fclose(f); return result;
}
static gt_hostkey_decision_t trust(void *u, const char *h, uint16_t p, const char *t, const char *f) {
    (void)u; (void)h; (void)p; (void)t; (void)f; return GT_HOSTKEY_ACCEPT;
}
static gt_ssh_t *connect_fixture(char **argv) {
    char known[4096]; snprintf(known, sizeof known, "%s/transfer-known", argv[5]);
    gt_ssh_opts_t options = {.host=argv[1], .port=(uint16_t)atoi(argv[2]), .username=argv[3], .known_hosts_path=known};
    gt_ssh_t *ssh = gt_ssh_new(&options);
    FILE *f = fopen(argv[4], "rb"); if (!f || !ssh) abort();
    char key[16384] = {0}; size_t n = fread(key, 1, sizeof key - 1, f);
    if (ferror(f) || !feof(f) || !n || fclose(f)) abort();
    int status = gt_ssh_auth_private_key(ssh, key, NULL);
    volatile char *wipe = key; for (size_t i=0;i<sizeof key;i++) wipe[i]=0;
    if (status != GT_OK) abort();
    gt_ssh_set_hostkey_policy(ssh, trust, NULL);
    if (gt_ssh_connect(ssh) != GT_OK) { puts("FAIL: transfer authentication"); exit(1); }
    return ssh;
}
static int wait_job(uint64_t id, char *detail, size_t capacity) {
    if (!id) { puts("FAIL: job not submitted"); failures++; return -1; }
    int64_t deadline = mono_ms()+5000;
    while (mono_ms()<deadline) {
        gt_event_t events[32]; size_t count = gt_core_poll_events(events, 32);
        for (size_t i=0;i<count;i++) if (events[i].kind == GT_EVENT_JOB_DONE && events[i].job_id == id) {
            snprintf(detail, capacity, "%s", events[i].message); return events[i].code;
        }
        struct timespec delay = {.tv_nsec=1000000}; nanosleep(&delay,NULL);
    }
    puts("FAIL: transfer completion not received"); failures++; return -1;
}
static void run_case(char **argv, const char *name, bool peer) {
    mode=name; tracked=NULL; atomic_store(&injected,0); atomic_store(&local_opens,0); atomic_store(&local_closes,0); atomic_store(&remote_closes,0);
    char local[4096], remote[4096], detail[512]={0};
    snprintf(local,sizeof local,"%s/%s-local",argv[5],mode);
    snprintf(remote,sizeof remote,"%s/%s-remote",argv[5],mode);
    local_path=local;
    bool upload=!strncmp(mode,"upload-",7);
    bool empty=strstr(mode,"empty")!=NULL;
    bool cancelled=strstr(mode,"cancelled")!=NULL;
    bool okay=is("upload-normal") || is("download-normal") || is("upload-zero-write") || empty;
    size_t size=empty ? 0 : sizeof content-1;
    put(local,upload ? content : sentinel,upload ? size : sizeof sentinel-1);
    if (!peer && !is("download-missing")) put(remote,upload ? sentinel : content,upload ? sizeof sentinel-1 : size);
    gt_ssh_t *ssh=connect_fixture(argv); gt_sftp_t *sftp=gt_ssh_sftp(ssh);
    if (!sftp) abort();
    if (cancelled) {
        /* Pause the actual worker before dequeue, enqueue and cancel through
         * the public API, then release. No timing assumptions or fake jobs. */
        pthread_mutex_lock(&sftp->qlock);
        sftp->quit=true;
        pthread_cond_signal(&sftp->qcond);
        pthread_mutex_unlock(&sftp->qlock);
        pthread_join(sftp->worker,NULL);
        sftp->quit=false;
    }
    uint64_t id=upload ? gt_sftp_upload(sftp,local,remote) : gt_sftp_download(sftp,remote,local);
    if (cancelled) {
        CHECK(gt_sftp_cancel(sftp,id)==GT_OK,"queued cancellation accepted");
        if (pthread_create(&sftp->worker,NULL,worker_main,sftp)) abort();
    }
    int result=wait_job(id,detail,sizeof detail);
    CHECK(result==(okay ? GT_JOB_OK : cancelled ? GT_JOB_CANCELLED : GT_JOB_FAILED),"transfer result matches IO and close outcomes");
    if (strstr(mode,"close")) CHECK(strstr(detail,"close failed")!=NULL,"close failure is visible in completion detail");
    gt_sftp_close(sftp); gt_ssh_close(ssh);
    CHECK(atomic_load(&local_opens)==atomic_load(&local_closes),"each local stream is closed once");
    bool no_remote = cancelled || is("download-missing") || is("download-open-deny") ||
        is("upload-seek-end") || is("upload-seek-start") || is("upload-tell");
    CHECK(atomic_load(&remote_closes)==(no_remote ? 0 : 1),"each opened remote handle is closed once");
    if (!peer && okay) CHECK(equal(upload ? remote : local,content,size),"exact destination bytes with no old tail");
    if (is("download-missing") || cancelled || is("download-open-deny")) {
        CHECK(equal(upload ? remote : local,sentinel,sizeof sentinel-1),"preflight failure or queued cancellation preserves target");
        CHECK(atomic_load(&local_opens)==0,"no local stream opened before admission");
    }
    if (is("upload-seek-end") || is("upload-seek-start") || is("upload-tell"))
        CHECK(equal(remote,sentinel,sizeof sentinel-1),"source positioning failure preserves remote target");
    if (strstr(mode,"local-") || strstr(mode,"seek-") || is("upload-tell") )
        CHECK(atomic_load(&injected)==1,"intended local fault occurred exactly once");
    if (is("upload-zero-write")) CHECK(atomic_load(&injected)==2,"zero write retries unchanged payload");
    if (!failures) printf("PASS transfer %s\n",mode);
}
int main(int argc,char **argv) {
    if (argc!=6 && argc!=7) return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc==7) run_case(argv,argv[6],strstr(argv[6],"-deny")!=NULL);
    else {
        const char *cases[]={"upload-normal","download-normal","upload-empty","download-empty","download-missing",
            "download-local-open","download-local-close","upload-local-close","upload-seek-end","upload-seek-start",
            "upload-tell","upload-zero-write","upload-cancelled","download-cancelled"};
        for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) run_case(argv,cases[i],false);
    }
    if (!failures) puts("SFTP TRANSFER PASS");
    return failures ? 1 : 0;
}
