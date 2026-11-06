/* C11 caller for a PRIVATE Go ABI research library. No Ghosterm API changes. */
#define _POSIX_C_SOURCE 200809L
#include "plugin-go-abi.h"
#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

static void require(int value, const char *message) {
    if (!value) {
        fprintf(stderr, "FAIL C ABI: %s\n", message);
        exit(1);
    }
}

static uint64_t now_ns(void) {
    struct timespec t;
    require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "clock");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static void pause_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&t, &t) != 0) {
    }
}

static int fd_count(void) {
    DIR *dir = opendir("/dev/fd");
    require(dir != NULL, "fd observation");
    int n = -1; /* exclude the directory fd itself */
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
            ++n;
        }
    }
    require(closedir(dir) == 0, "fd observation close");
    return n;
}

static void memory(uint64_t *rss, unsigned *threads) {
    *rss = 0;
    *threads = 0;
#ifdef __APPLE__
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    require(task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) ==
                KERN_SUCCESS,
            "resident memory");
    *rss = info.resident_size;
    thread_act_array_t list;
    mach_msg_type_number_t n;
    require(task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS, "thread list");
    *threads = n;
    for (mach_msg_type_number_t i = 0; i < n; ++i) {
        require(mach_port_deallocate(mach_task_self(), list[i]) == KERN_SUCCESS,
                "thread port release");
    }
    require(vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(*list)) == KERN_SUCCESS,
            "thread array release");
#endif
}

static void observe(const char *phase) {
    uint64_t rss;
    unsigned threads;
    memory(&rss, &threads);
    printf("{\"phase\":\"%s\",\"rssBytes\":%" PRIu64 ",\"threads\":%u,\"fds\":%d}\n",
           phase,
           rss,
           threads,
           fd_count());
}

#ifndef PGABI_BASELINE
typedef struct job {
    uint64_t id;
    int mode, result;
    unsigned char input[4096], output[4096];
    uint32_t written;
    atomic_int complete;
} job;

static void *perform(void *argument) {
    job *j = argument;
    j->result = pgabi_run(
        j->id, j->mode, j->input, sizeof(j->input), j->output, sizeof(j->output), &j->written);
    atomic_store(&j->complete, 1);
    return NULL;
}

static pthread_t launch(job *j, uint64_t id, int mode) {
    memset(j, 0, sizeof(*j));
    j->id = id;
    j->mode = mode;
    for (unsigned i = 0; i < sizeof(j->input); ++i) {
        j->input[i] = (unsigned char)i;
    }
    memset(j->output, 0xaa, sizeof(j->output));
    atomic_init(&j->complete, 0);
    pthread_t thread;
    require(pthread_create(&thread, NULL, perform, j) == 0, "C thread create");
    return thread;
}

static void wait_active(void) {
    uint64_t deadline = now_ns() + UINT64_C(5000000000);
    pgabi_stats s;
    do {
        require(pgabi_snapshot(&s, 0) == PGABI_OK, "active snapshot");
        if (s.active == 1) {
            return;
        }
        pause_ms(1);
    } while (now_ns() < deadline);
    require(0, "operation did not start");
}

static void go_observe(const char *phase) {
    pgabi_stats s;
    require(pgabi_snapshot(&s, 1) == PGABI_OK, "Go snapshot");
    printf("{\"phase\":\"%s\",\"handles\":%" PRIu64 ",\"active\":%" PRIu64
           ",\"goroutines\":%" PRIu64 ",\"heapAllocBytes\":%" PRIu64 ",\"heapSysBytes\":%" PRIu64
           "}\n",
           phase,
           s.handles,
           s.active,
           s.goroutines,
           s.heap_alloc,
           s.heap_sys);
}

static void cancellation(int close_active) {
    uint64_t id = pgabi_new();
    require(id != 0, "cancel handle");
    job j;
    pthread_t thread = launch(&j, id, 1);
    wait_active();
    unsigned char b[8] = {0};
    uint32_t written = 99;
    require(pgabi_copy(id, b, sizeof(b), b, sizeof(b), &written) == PGABI_BUSY,
            "busy operation isolation");
    uint64_t start = now_ns();
    require((close_active ? pgabi_close(id) : pgabi_cancel(id)) == PGABI_OK, "cancel/close active");
    require(pthread_join(thread, NULL) == 0, "cancel thread join");
    uint64_t elapsed = now_ns() - start;
    require(j.result == PGABI_CANCELLED && j.written == 0, "cancel result/written");
    for (unsigned i = 0; i < sizeof(j.output); ++i) {
        require(j.output[i] == 0xaa, "late buffer write");
    }
    if (!close_active) {
        require(pgabi_copy(id, b, sizeof(b), b, sizeof(b), &written) == PGABI_CANCELLED,
                "cancelled owner reused");
        require(pgabi_close(id) == PGABI_OK, "cancelled owner close");
    }
    require(pgabi_close(id) == PGABI_INVALID && pgabi_cancel(id) == PGABI_INVALID,
            "invalidated owner");
    printf("{\"phase\":\"%s\",\"elapsedNs\":%" PRIu64 ",\"lateOutputBytes\":0}\n",
           close_active ? "close-active" : "cancel-active",
           elapsed);
}
#endif

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    pause_ms(200);
    observe("before-first-ABI-call");
#ifndef PGABI_BASELINE
    uint64_t id = pgabi_new();
    require(id != 0, "new");
    unsigned char input[64], output[64];
    uint32_t written;
    memset(input, 0x5c, sizeof(input));
    memset(output, 0xaa, sizeof(output));
    require(pgabi_copy(id, input, sizeof(input), output, 1, &written) == PGABI_BUFFER &&
                written == sizeof(input),
            "buffer limit");
    require(output[0] == 0xaa, "buffer failure changed output");
    require(pgabi_copy(id, NULL, 1, output, sizeof(output), &written) == PGABI_INVALID &&
                written == 0,
            "null input");
    require(pgabi_copy(id, input, 65537, output, sizeof(output), &written) == PGABI_LIMIT,
            "message limit");
    require(pgabi_copy(0, input, sizeof(input), output, sizeof(output), &written) == PGABI_INVALID,
            "invalid handle");
    require(pgabi_snapshot(NULL, 0) == PGABI_INVALID, "null snapshot");
    go_observe("Go-initialized");
    observe("after-first-ABI-call");
    uint64_t start = now_ns();
    const unsigned iterations = 10000;
    for (unsigned i = 0; i < iterations; ++i) {
        require(pgabi_copy(id, input, sizeof(input), output, sizeof(output), &written) == PGABI_OK,
                "copy loop");
    }
    require(written == sizeof(input) && memcmp(input, output, sizeof(input)) == 0, "copy content");
    printf(
        "{\"phase\":\"C-Go-C-copy\",\"iterations\":%u,\"bytesPerCall\":%zu,\"nsPerCall\":%" PRIu64
        "}\n",
        iterations,
        sizeof(input),
        (now_ns() - start) / iterations);
    job j;
    pthread_t thread = launch(&j, id, 0);
    uint64_t peak_rss = 0;
    unsigned peak_threads = 0;
    uint64_t until = now_ns() + UINT64_C(60000000000);
    while (!atomic_load(&j.complete)) {
        uint64_t rss;
        unsigned threads;
        memory(&rss, &threads);
        if (rss > peak_rss) {
            peak_rss = rss;
        }
        if (threads > peak_threads) {
            peak_threads = threads;
        }
        require(now_ns() < until, "real network operation deadline");
        pause_ms(10);
    }
    require(pthread_join(thread, NULL) == 0, "network thread join");
    if (j.result != PGABI_OK) {
        fprintf(stderr, "network ABI status %d\n", j.result);
    }
    require(j.result == PGABI_OK && j.written == sizeof(j.input) &&
                memcmp(j.input, j.output, sizeof(j.input)) == 0,
            "actual C ABI paired DERP transfer");
    printf(
        "{\"phase\":\"real-paired-DERP-roundtrip\",\"bytes\":%zu,\"peakSampledRssBytes\":%" PRIu64
        ",\"peakSampledThreads\":%u}\n",
        sizeof(j.input),
        peak_rss,
        peak_threads);
    require(pgabi_close(id) == PGABI_OK, "network owner close");
    require(pgabi_copy(id, input, sizeof(input), output, sizeof(output), &written) == PGABI_INVALID,
            "closed handle write");
    cancellation(0);
    cancellation(1);
    pause_ms(200);
    int warm_fds = fd_count();
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t next = pgabi_new();
        require(next > id, "non-reused handle");
        id = next;
        require(pgabi_close(next) == PGABI_OK, "owner cycle close");
    }
    require(fd_count() == warm_fds, "non-network owner cycles leaked descriptors");
    pgabi_stats final;
    require(pgabi_snapshot(&final, 1) == PGABI_OK && final.handles == 0 && final.active == 0,
            "owner registry not empty");
    go_observe("after-all-owners-closed");
    observe("after-all-owners-closed");
    puts("{\"result\":\"pass\",\"networkRuntime\":\"in-process-Go\",\"helperProcess\":false}");
#else
    /* Same C observation code, independently linked without the Go archive. */
    require(now_ns() > 0, "baseline clock");
    puts("{\"result\":\"baseline\"}");
#endif
    return 0;
}
