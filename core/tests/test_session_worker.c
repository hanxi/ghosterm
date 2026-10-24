#include "session/worker.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "allocation-test.h"

typedef struct fixture {
  pthread_mutex_t lock;
  pthread_cond_t changed;
  pthread_t main;
  bool gate, opened, ready, reply, finished, fail_apply, backend_opened;
  unsigned completed, cancelled;
  gt_status_t result;
  gt_worker_reply_sink_t sink;
  bool replies, overflow, fail_write, in_apply, init_reply;
  unsigned writes;size_t delivered;
  gt_state_capture_t capture;
} fixture_t;
typedef struct { fixture_t *fixture; char bytes[4096]; size_t length;
                 unsigned events; gt_state_size_t size; } state_t;
static void wait_flag(fixture_t *f, const bool *flag) {
  struct timespec end;
  assert(!clock_gettime(CLOCK_REALTIME, &end));
  end.tv_sec += 5;
  pthread_mutex_lock(&f->lock);
  while (!*flag) assert(!pthread_cond_timedwait(&f->changed, &f->lock, &end));
  pthread_mutex_unlock(&f->lock);
}
static gt_status_t open_state(void *context, gt_worker_reply_sink_t sink, void **out) {
  fixture_t *f = context;
  f->sink = sink;
  assert(!pthread_equal(f->main, pthread_self()));
  pthread_mutex_lock(&f->lock);
  f->opened = true;
  pthread_cond_broadcast(&f->changed);
  while (!f->gate) pthread_cond_wait(&f->changed, &f->lock);
  pthread_mutex_unlock(&f->lock);
  if(f->init_reply)
    assert(f->sink.write(f->sink.context,(const uint8_t *)"x",1)==GT_ERR_INVALID_ARG);
  state_t *s = calloc(1, sizeof(*s));
  assert(s);
  s->fixture = f;
  *out = s;
  return GT_OK;
}
typedef struct {gt_io_backend_t base;gt_io_backend_t *real;fixture_t *fixture;} wrapped_t;
static int wrapped_read(gt_io_backend_t *b,uint8_t *p,size_t n,int timeout) {
  wrapped_t *w=(wrapped_t *)b;return w->real->ops->read(w->real,p,n,timeout);
}
static int wrapped_write(gt_io_backend_t *b,const uint8_t *p,size_t n) {
  wrapped_t *w=(wrapped_t *)b;
  assert(!w->fixture->in_apply);
  w->fixture->writes++;
  if(w->fixture->fail_write)return GT_ERR_IO;
  return w->real->ops->write(w->real,p,n);
}
static int wrapped_resize(gt_io_backend_t *b,uint16_t c,uint16_t r,uint32_t x,uint32_t y) {
  wrapped_t *w=(wrapped_t *)b;return w->real->ops->resize(w->real,c,r,x,y);
}
static void wrapped_close(gt_io_backend_t *b) {
  wrapped_t *w=(wrapped_t *)b;w->real->ops->close(w->real);free(w);
}
static const gt_io_ops_t wrapped_ops={.read=wrapped_read,.write=wrapped_write,
  .resize=wrapped_resize,.close=wrapped_close};
static gt_io_backend_t *open_backend(void *context) {
  fixture_t *f = context;
  assert(!pthread_equal(f->main, pthread_self()));
  f->backend_opened = true;
  const char *args[] = {"-c", "stty -echo; printf READY; read answer; stty size; printf REPLY; read answer", NULL};
  gt_pty_opts_t opts = {.executable = "/bin/sh", .argv = args,
    .cmd_kind = GT_PTY_CMD_DIRECT, .cols = 80, .rows = 24};
  gt_io_backend_t *real=gt__pty_open(&opts);
  if(!f->replies)return real;
  assert(real);
  wrapped_t *wrapped=calloc(1,sizeof(*wrapped));assert(wrapped);
  wrapped->base.ops=&wrapped_ops;wrapped->real=real;wrapped->fixture=f;
  return &wrapped->base;
}
static gt_status_t apply(void *context, const gt_state_event_t *event) {
  state_t *s = context;
  assert(!pthread_equal(s->fixture->main, pthread_self()));
  s->events++;
  if(s->fixture->replies && s->events==1) {
    fixture_t *f=s->fixture;f->in_apply=true;
    if(f->overflow) {
      static const uint8_t full[65536]={0};
      assert(f->sink.write(f->sink.context,full,sizeof(full))==GT_OK);
      /* Intentionally ignore the error: the worker must still stop. */
      (void)f->sink.write(f->sink.context,(const uint8_t *)"x",1);
    } else {
      assert(f->sink.write(f->sink.context,(const uint8_t *)"rep",3)==GT_OK);
      assert(f->sink.write(f->sink.context,(const uint8_t *)"ly\n",3)==GT_OK);
    }
    f->in_apply=false;
  }
  if (event->kind == GT_STATE_RESIZE) {
    assert(event->length == sizeof(s->size));
    memcpy(&s->size, event->payload, sizeof(s->size));
  } else {
    assert(event->kind == GT_STATE_OUTPUT);
    assert(s->length + event->length < sizeof(s->bytes));
    memcpy(s->bytes + s->length, event->payload, event->length);
    s->length += event->length;
  }
  return s->fixture->fail_apply ? GT_ERR_IO : GT_OK;
}
static gt_status_t freeze(void *context, void **out) {
  state_t *s = context;
  state_t *copy = malloc(sizeof(*copy));
  assert(copy);
  *copy = *s;
  *out = copy;
  return GT_OK;
}
static void output(void *context, const uint8_t *bytes, size_t length, uint64_t seq) {
  fixture_t *f = context;
  assert(seq);
  /* These tokens may be split: collect without assuming read boundaries. */
  static char collected[4096];
  static size_t used;
  assert(used + length < sizeof(collected));
  memcpy(collected + used, bytes, length);
  used += length;
  pthread_mutex_lock(&f->lock);
  f->delivered+=length;
  f->ready = strstr(collected, "READY") != NULL;
  f->reply = strstr(collected, "REPLY") != NULL;
  pthread_cond_broadcast(&f->changed);
  pthread_mutex_unlock(&f->lock);
}
static void completion(void *context, gt_status_t rc, gt_state_capture_t capture) {
  fixture_t *f = context;
  assert(!pthread_equal(f->main, pthread_self()));
  pthread_mutex_lock(&f->lock);
  f->completed++;
  if (rc == GT_ERR_CANCELLED) f->cancelled++;
  else assert(rc == GT_OK);
  if (capture.frozen) {
    assert(!f->capture.frozen);
    f->capture = capture;
  }
  pthread_cond_broadcast(&f->changed);
  pthread_mutex_unlock(&f->lock);
}
static void finished(void *context, gt_status_t rc) {
  fixture_t *f = context;
  pthread_mutex_lock(&f->lock);
  f->finished = true;
  f->result = rc;
  pthread_cond_broadcast(&f->changed);
  pthread_mutex_unlock(&f->lock);
}
static void wait_completions(fixture_t *f, unsigned n) {
  struct timespec end;
  assert(!clock_gettime(CLOCK_REALTIME, &end));
  end.tv_sec += 5;
  pthread_mutex_lock(&f->lock);
  while (f->completed < n)
    assert(!pthread_cond_timedwait(&f->changed, &f->lock, &end));
  pthread_mutex_unlock(&f->lock);
}
int main(int argc, char **argv) {
  fixture_t f = {.lock = PTHREAD_MUTEX_INITIALIZER,
                .changed = PTHREAD_COND_INITIALIZER, .main = pthread_self()};
  f.init_reply = argc>1 && !strcmp(argv[1],"reply-init");
  f.overflow = argc>1 && !strcmp(argv[1],"reply-overflow");
  f.fail_write = argc>1 && !strcmp(argv[1],"reply-write-failure");
  f.replies = f.overflow || f.fail_write || (argc>1 && !strcmp(argv[1],"replies"));
  f.fail_apply = argc > 1 && !strcmp(argv[1], "failure");
  bool pressure = argc > 1 && !strcmp(argv[1], "pressure");
  bool allocation=argc>1 && !strcmp(argv[1],"allocator");
  bool byte_pressure = allocation || (argc > 1 && !strcmp(argv[1], "bytes"));
  gt_worker_config_t config = {.context = &f, .open_backend = open_backend,
    .open_state = open_state, .close_state = free,
    .state_ops = {apply, freeze}, .output = output, .finished = finished};
  allocation_test_budget_t budget={.limit=1024*1024};gt_allocator_t domain={&budget,allocation_test_allocate,allocation_test_release};
  if(allocation) {
    gt_allocator_t invalid={&budget,allocation_test_allocate,NULL};
    assert(!gt__worker_start_with_allocator(&config,&invalid) && !budget.calls);
    budget.fail=1;assert(!gt__worker_start_with_allocator(&config,&domain) && !budget.live);
    budget.fail=0;budget.limit=1;assert(!gt__worker_start_with_allocator(&config,&domain) && !budget.live);
    budget.limit=1024*1024;
  }
  gt_session_worker_t *w = allocation?gt__worker_start_with_allocator(&config,&domain):gt__worker_start(&config);
  assert(w);
  wait_flag(&f, &f.opened);
  if(argc>1 && !strcmp(argv[1],"reply-thread"))
    assert(f.sink.write(f.sink.context,(const uint8_t *)"x",1)==GT_ERR_INVALID_ARG);
  if(allocation) {
    assert(budget.count==1 && budget.live>65536);size_t live=budget.live;budget.limit=live;
    assert(gt__worker_submit_internal(w,GT_WORKER_INPUT,"x",1,completion,&f)==GT_WORKER_NO_MEMORY);
    assert(budget.live==live && budget.count==1 && !f.completed);budget.limit=1024*1024;
  }
  if (pressure || byte_pressure) {
    unsigned total = pressure ? 16 : 1;
    char payload[65536] = {0};
    for (unsigned i = 0; i < total; i++)
      assert(gt__worker_submit_internal(w, byte_pressure ? GT_WORKER_INPUT : GT_WORKER_CAPTURE,
             byte_pressure ? payload : NULL, byte_pressure ? sizeof(payload) : 0,
             completion, &f) == GT_WORKER_ACCEPTED);
    assert(gt__worker_submit_internal(w, GT_WORKER_INPUT, "x", 1, completion, &f) == GT_WORKER_BACKPRESSURE);
    if(allocation)assert(budget.count==2 && budget.live>65536+sizeof(payload));
    gt__worker_stop(w);
    assert(gt__worker_submit_internal(w, GT_WORKER_CAPTURE, NULL, 0, completion, &f) == GT_WORKER_STOPPED);
    pthread_mutex_lock(&f.lock);
    f.gate = true;
    pthread_cond_broadcast(&f.changed);
    pthread_mutex_unlock(&f.lock);
    gt__worker_join(w);
    assert(f.completed == total && f.cancelled == total && f.finished);
    assert(!f.backend_opened);
    if(allocation)assert(!budget.live && !budget.count);
  } else {
    pthread_mutex_lock(&f.lock);
    f.gate = true;
    pthread_cond_broadcast(&f.changed);
    pthread_mutex_unlock(&f.lock);
    if(f.init_reply) {
      wait_flag(&f,&f.finished);
      assert(f.result==GT_ERR_INVALID_ARG && !f.backend_opened);
      gt__worker_join(w);
      pthread_cond_destroy(&f.changed);pthread_mutex_destroy(&f.lock);
      return 0;
    }
    if (f.replies) {
      if(f.overflow || f.fail_write) {
        wait_flag(&f,&f.finished);
        assert(f.result==(f.overflow ? GT_ERR_PROTOCOL : GT_ERR_IO));
        assert(f.delivered>0 && !f.reply); /* Applied bytes survive reply delivery failure. */
      } else wait_flag(&f,&f.reply);
      gt__worker_stop(w);gt__worker_join(w);
      assert(f.writes==(f.overflow ? 0u : 1u));
      pthread_cond_destroy(&f.changed);pthread_mutex_destroy(&f.lock);
      return 0;
    }
    if (f.fail_apply) {
      wait_flag(&f, &f.finished);
      assert(f.result == GT_ERR_IO && !f.ready && !f.reply);
      gt__worker_join(w);
      pthread_cond_destroy(&f.changed);
      pthread_mutex_destroy(&f.lock);
      return 0;
    }
    wait_flag(&f, &f.ready);
    gt_state_size_t size = {91, 31, 910, 620};
    assert(gt__worker_submit_internal(w, GT_WORKER_RESIZE, &size, sizeof(size), completion, &f) == GT_WORKER_ACCEPTED);
    assert(gt__worker_submit_internal(w, GT_WORKER_CAPTURE, NULL, 0, completion, &f) == GT_WORKER_ACCEPTED);
    wait_completions(&f, 2);
    state_t *before = f.capture.frozen;
    uint64_t boundary = f.capture.sequence;
    assert(before && before->events == boundary && strstr(before->bytes, "READY"));
    assert(before->size.cols == 91 && before->size.rows == 31);
    f.capture = (gt_state_capture_t){0};
    assert(gt__worker_submit_internal(w, GT_WORKER_INPUT, "go\n", 3, completion, &f) == GT_WORKER_ACCEPTED);
    wait_flag(&f, &f.reply);
    assert(gt__worker_submit_internal(w, GT_WORKER_CAPTURE, NULL, 0, completion, &f) == GT_WORKER_ACCEPTED);
    wait_completions(&f, 4);
    state_t *after = f.capture.frozen;
    assert(after && after->events == f.capture.sequence && f.capture.sequence > boundary);
    assert(strstr(after->bytes, "31 91") && strstr(after->bytes, "REPLY"));
    assert(!strstr(before->bytes, "REPLY") && before->events == boundary);
    gt__worker_stop(w);
    gt__worker_join(w);
    assert(f.finished && f.result == GT_OK);
    free(before);
    free(after);
  }
  pthread_cond_destroy(&f.changed);
  pthread_mutex_destroy(&f.lock);
  return 0;
}
