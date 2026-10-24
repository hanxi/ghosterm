/* Reuse deterministic encoder/allocator lifetime fixtures, not a fake pool. */
#define main checkpoint_fixture_main
#include "test_checkpoint_task.c"
#undef main
#include "session/attachment.h"
#include "allocation-test.h"
static void bind(gt_session_attachment_t *attachment,uint64_t generation,fixture_t *f) {
  gt_task_spec_t s=spec(f);s.generation=999;
  assert(gt__attachment_begin(attachment,generation,s.sequence)==GT_RESTORE_OK);
  assert(gt__attachment_submit(attachment,generation,&s,&f->id)==GT_TASK_OK);
}
static gt_attachment_retired_t reap(gt_session_attachment_t *a) {
  for(unsigned i=0;i<3000;i++) {
    gt_attachment_retired_t retired;
    gt_task_rc_t rc=gt__attachment_reap(a,&retired);
    assert(rc==GT_TASK_OK || rc==GT_TASK_BUSY);
    if(rc==GT_TASK_OK)return retired;
    struct timespec pause={0,1000000};nanosleep(&pause,NULL);
  }
  assert(!"attachment cleanup timeout");return (gt_attachment_retired_t){0};
}
static void consume(gt_session_attachment_t *a,uint64_t g,fixture_t *f) {
  gt_attachment_checkpoint_t lease;
  assert(gt__attachment_take(a,g,&lease)==GT_TASK_OK);
  assert(lease.task==f->id && lease.generation==g && lease.sequence==40 && *(unsigned *)lease.result==42);
  assert(gt__attachment_release(a,&lease)==GT_TASK_OK && !lease.result && !lease.task);
  wait_state(f,GT_TASK_FINISHED);
}
typedef struct {gt_session_attachment_t *attachment;gt_task_rc_t rc;uint64_t generation;} attach_attempt_t;
static void *attach_thread(void *context) {
  attach_attempt_t *a=context;a->rc=gt__attachment_attach(a->attachment,&a->generation);return NULL;
}
static void allocation_case(gt_checkpoint_tasks_t *pool) {
  allocation_test_budget_t b={.limit=1024*1024};gt_allocator_t a={&b,allocation_test_allocate,allocation_test_release};
  gt_allocator_t invalid={&b,allocation_test_allocate,NULL};
  assert(!gt__attachment_open_with_allocator(pool,8,3,&invalid) && !b.calls);
  for(size_t fail=1;fail<=3;fail++) {
    b.fail=b.calls+fail;assert(!gt__attachment_open_with_allocator(pool,8,3,&a));
    assert(!b.live && !b.count);
  }
  b.fail=0;gt_session_attachment_t *c=gt__attachment_open_with_allocator(pool,8,3,&a);assert(c);
  size_t live=b.live,calls=b.calls;assert(b.count==3 && live>8+3*sizeof(gt_restore_entry_t));
  uint64_t g=0;assert(gt__attachment_attach(c,&g)==GT_TASK_OK);
  assert(gt__attachment_close(c)==GT_TASK_BUSY && b.live==live && b.count==3);
  assert(gt__attachment_begin(c,g,0)==GT_RESTORE_OK);
  assert(gt__attachment_push(c,1,1,"012345678",9)==GT_RESTORE_LIMIT);
  assert(!gt__attachment_current(c,g) && b.live==live && b.calls==calls);
  uint64_t next=0;assert(gt__attachment_attach(c,&next)==GT_TASK_OK && next>g && b.live==live && b.calls==calls);
  assert(gt__attachment_detach(c,next)==GT_TASK_OK && b.live==live);
  assert(gt__attachment_close(c)==GT_TASK_OK && !b.live && !b.count);
}
static void prepare_live(gt_session_attachment_t *c,uint64_t g,fixture_t *f) {
  bind(c,g,f);wait_state(f,GT_TASK_READY);consume(c,g,f);gt_attachment_retired_t r=reap(c);assert(r.task==f->id);
  assert(gt__attachment_ready_snapshot(c,g,40)==GT_RESTORE_OK);uint64_t cut=0;
  assert(gt__attachment_live_begin(c,g,&cut)==GT_RESTORE_OK && cut==40);
  assert(gt__attachment_ready_live(c,g,40)==GT_RESTORE_OK);
}
static void ended_case(gt_checkpoint_tasks_t *pool,bool deferred) {
  gt_session_attachment_t *c=gt__attachment_open(pool,16,8);assert(c);fixture_t a,b;init(&a,pool,true);init(&b,pool,true);
  uint64_t g=0,next=0;assert(gt__attachment_attach(c,&g)==GT_TASK_OK);gt_attachment_restore_info_t info;
  if(deferred) {
    bind(c,g,&a);wait_state(&a,GT_TASK_READY);consume(c,g,&a);reap(c);
    assert(gt__attachment_push(c,41,1,"A",1)==GT_RESTORE_OK);
    assert(gt__attachment_end(c,g+1,41)==GT_RESTORE_STALE);
    assert(gt__attachment_end(c,g,40)==GT_RESTORE_PROTOCOL);
    assert(gt__attachment_restore_info(c,g,&info)==GT_RESTORE_OK && !info.session_ended && info.phase==GT_RESTORE_SNAPSHOT);
    assert(gt__attachment_end(c,g,41)==GT_RESTORE_WAIT);
    assert(gt__attachment_restore_info(c,g,&info)==GT_RESTORE_OK && info.session_ended && info.terminal_sequence==41 && info.events==1);
    assert(gt__attachment_ready_snapshot(c,g,40)==GT_RESTORE_OK);
    gt_restore_event_t e;char bytes[2];assert(gt__attachment_next(c,g,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && bytes[0]=='A');uint64_t cut=0;
    assert(gt__attachment_live_begin(c,g,&cut)==GT_RESTORE_OK && cut==41);
    assert(gt__attachment_end(c,g,41)==GT_RESTORE_WAIT);
    assert(gt__attachment_ready_live(c,g,41)==GT_RESTORE_OK && !gt__attachment_input_allowed(c,g,41));
    assert(gt__attachment_end(c,g,41)==GT_RESTORE_OK);
  } else {
    prepare_live(c,g,&a);assert(gt__attachment_input_allowed(c,g,40));
    assert(gt__attachment_push(c,41,1,"A",1)==GT_RESTORE_OK && gt__attachment_push(c,42,1,"B",1)==GT_RESTORE_OK);
    assert(gt__attachment_end(c,g,42)==GT_RESTORE_OK && gt__attachment_current(c,g) && !gt__attachment_input_allowed(c,g,40));
    gt_restore_event_t e;char bytes[2];
    assert(gt__attachment_next(c,g,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==41 && bytes[0]=='A');
    assert(gt__attachment_next(c,g,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==42 && bytes[0]=='B');
    assert(gt__attachment_detach(c,g)==GT_TASK_OK && gt__attachment_attach(c,&next)==GT_TASK_OK);
    assert(gt__attachment_restore_info(c,next,&info)==GT_RESTORE_OK && info.session_ended && info.terminal_sequence==42);
    assert(gt__attachment_begin(c,next,43)==GT_RESTORE_PROTOCOL && gt__attachment_current(c,next));
    prepare_live(c,next,&b);assert(!gt__attachment_input_allowed(c,next,40));
    assert(gt__attachment_end(c,next,42)==GT_RESTORE_WAIT);
    assert(gt__attachment_push(c,41,1,"A",1)==GT_RESTORE_OK && gt__attachment_push(c,42,1,"B",1)==GT_RESTORE_OK);
    assert(gt__attachment_push(c,43,1,"C",1)==GT_RESTORE_PROTOCOL);
    assert(gt__attachment_end(c,next,42)==GT_RESTORE_OK);
    assert(gt__attachment_end(c,g,42)==GT_RESTORE_STALE && gt__attachment_current(c,next));
    assert(gt__attachment_end(c,next,43)==GT_RESTORE_PROTOCOL);
    assert(gt__attachment_restore_info(c,next,&info)==GT_RESTORE_OK && info.session_ended && info.terminal_sequence==42 && info.phase==GT_RESTORE_ENDED && info.events==2);
    assert(gt__attachment_next(c,next,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==41);
    assert(gt__attachment_next(c,next,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==42);
    assert(gt__attachment_ready_live(c,next,40)==GT_RESTORE_PROTOCOL && gt__attachment_current(c,next));
    assert(gt__attachment_end(c,next,42)==GT_RESTORE_OK && !gt__attachment_input_allowed(c,next,42));
    g=next;
  }
  assert(gt__attachment_close(c)==GT_TASK_BUSY);
  assert(gt__attachment_detach(c,g)==GT_TASK_OK && gt__attachment_close(c)==GT_TASK_OK);destroy(&a);destroy(&b);
}
static void unknown_end_case(gt_checkpoint_tasks_t *pool) {
  gt_session_attachment_t *c=gt__attachment_open(pool,16,8);assert(c);uint64_t g=0,next=0;
  assert(gt__attachment_attach(c,&g)==GT_TASK_OK);
  assert(gt__attachment_end_unknown(c,g+1)==GT_RESTORE_STALE);
  gt_attachment_restore_info_t info;assert(gt__attachment_restore_info(c,g,&info)==GT_RESTORE_OK && !info.session_ended);
  assert(gt__attachment_end_unknown(c,g)==GT_RESTORE_OK && gt__attachment_current(c,g));
  assert(gt__attachment_end_unknown(c,g)==GT_RESTORE_OK);
  assert(gt__attachment_restore_info(c,g,&info)==GT_RESTORE_OK && info.session_ended && !info.has_terminal_sequence && !info.terminal_sequence && info.phase==GT_RESTORE_CAPTURING);
  assert(!gt__attachment_input_allowed(c,g,0));
  assert(gt__attachment_end(c,g,0)==GT_RESTORE_PROTOCOL);
  assert(gt__attachment_begin(c,g,0)==GT_RESTORE_PROTOCOL);
  assert(gt__attachment_push(c,0,1,"X",1)==GT_RESTORE_PROTOCOL);
  assert(gt__attachment_push(c,1,1,"X",1)==GT_RESTORE_PROTOCOL);
  assert(gt__attachment_current(c,g));
  assert(gt__attachment_detach(c,g)==GT_TASK_OK && gt__attachment_attach(c,&next)==GT_TASK_OK);
  assert(gt__attachment_end_unknown(c,g)==GT_RESTORE_STALE && gt__attachment_end_unknown(c,next)==GT_RESTORE_OK);
  assert(gt__attachment_begin(c,next,0)==GT_RESTORE_PROTOCOL);
  assert(gt__attachment_restore_info(c,next,&info)==GT_RESTORE_OK && info.session_ended && !info.has_terminal_sequence);
  assert(gt__attachment_detach(c,next)==GT_TASK_OK && gt__attachment_close(c)==GT_TASK_OK);
  c=gt__attachment_open(pool,16,8);assert(c);assert(gt__attachment_attach(c,&g)==GT_TASK_OK);
  assert(gt__attachment_end(c,g,0)==GT_RESTORE_WAIT);
  assert(gt__attachment_end_unknown(c,g)==GT_RESTORE_PROTOCOL);
  assert(gt__attachment_restore_info(c,g,&info)==GT_RESTORE_OK && info.session_ended && info.has_terminal_sequence && !info.terminal_sequence);
  assert(gt__attachment_detach(c,g)==GT_TASK_OK && gt__attachment_close(c)==GT_TASK_OK);
  c=gt__attachment_open(pool,16,8);assert(c);assert(gt__attachment_attach(c,&g)==GT_TASK_OK);
  assert(gt__attachment_begin(c,g,0)==GT_RESTORE_OK && gt__attachment_end_unknown(c,g)==GT_RESTORE_PROTOCOL);
  assert(gt__attachment_restore_info(c,g,&info)==GT_RESTORE_OK && !info.session_ended);
  assert(gt__attachment_detach(c,g)==GT_TASK_OK && gt__attachment_close(c)==GT_TASK_OK);
}
int main(int argc,char **argv) {
  assert(argc==2);
  bool pressure=!strcmp(argv[1],"backpressure");
  gt_checkpoint_tasks_t *pool=gt__tasks_open(pressure ? 1 : 2);assert(pool);
  if(!strcmp(argv[1],"ended") || !strcmp(argv[1],"end-wait")) {ended_case(pool,!strcmp(argv[1],"end-wait"));assert(gt__tasks_close(pool)==GT_TASK_OK);return 0;}
  if(!strcmp(argv[1],"unknown-end")) {unknown_end_case(pool);assert(gt__tasks_close(pool)==GT_TASK_OK);return 0;}
  if(!strcmp(argv[1],"allocator")) {allocation_case(pool);assert(gt__tasks_close(pool)==GT_TASK_OK);return 0;}
  gt_session_attachment_t *controller=gt__attachment_open(pool,64,16);assert(controller);
  fixture_t a,b;init(&a,pool,false);init(&b,pool,false);
  uint64_t g=0,next=0;
  if(!strcmp(argv[1],"invalid")) {
    gt_attachment_checkpoint_t lease;gt_attachment_retired_t retired;
    gt_task_spec_t s=spec(&a);uint64_t id=99;
    assert(gt__attachment_detach(controller,0)==GT_TASK_INVALID);
    assert(gt__attachment_take(controller,0,&lease)==GT_TASK_INVALID && !lease.result);
    assert(gt__attachment_submit(controller,0,&s,&id)==GT_TASK_INVALID && !id);
    assert(gt__attachment_reap(controller,&retired)==GT_TASK_BUSY);
    assert(!gt__attachment_current(controller,0) && !a.input_frees);
  } else if(!strcmp(argv[1],"race")) {
    attach_attempt_t attempts[2]={{.attachment=controller},{.attachment=controller}};
    pthread_t threads[2];
    for(unsigned i=0;i<2;i++)assert(!pthread_create(&threads[i],NULL,attach_thread,&attempts[i]));
    for(unsigned i=0;i<2;i++)assert(!pthread_join(threads[i],NULL));
    assert((attempts[0].rc==GT_TASK_OK)+(attempts[1].rc==GT_TASK_OK)==1);
    assert((attempts[0].rc==GT_TASK_BUSY)+(attempts[1].rc==GT_TASK_BUSY)==1);
    g=attempts[0].generation ? attempts[0].generation : attempts[1].generation;
    assert(g==1 && gt__attachment_current(controller,g));
    assert(gt__attachment_detach(controller,g)==GT_TASK_OK);
  } else {
    assert(gt__attachment_attach(controller,&g)==GT_TASK_OK && g);
    assert(gt__attachment_attach(controller,&next)==GT_TASK_BUSY && !next);
    assert(gt__attachment_close(controller)==GT_TASK_BUSY);
    if(!strcmp(argv[1],"nojob")) {
      assert(gt__attachment_detach(controller,g)==GT_TASK_OK);
      assert(!gt__attachment_current(controller,g));
    } else {
      a.fail=!strcmp(argv[1],"failure") || !strcmp(argv[1],"failure-stale");bind(controller,g,&a);wait_entered(&a);
      gt_task_spec_t duplicate=spec(&b);uint64_t rejected=99;
      assert(gt__attachment_submit(controller,g,&duplicate,&rejected)==GT_TASK_BUSY && !rejected);
      if(!strcmp(argv[1],"overflow") || !strcmp(argv[1],"early-ready")) {
        if(!strcmp(argv[1],"overflow")) {
          char oversized[65]={0};
          assert(gt__attachment_push(controller,41,1,oversized,sizeof(oversized))==GT_RESTORE_LIMIT);
        } else assert(gt__attachment_ready_snapshot(controller,g,40)==GT_RESTORE_PROTOCOL);
        assert(!gt__attachment_current(controller,g) && !gt__attachment_input_allowed(controller,g,40));
        gt_attachment_restore_info_t info;
        assert(gt__attachment_restore_info(controller,g,&info)==GT_RESTORE_OK && info.phase==GT_RESTORE_FAILED);
        assert(info.failure==(!strcmp(argv[1],"overflow") ? GT_RESTORE_LIMIT : GT_RESTORE_PROTOCOL));
        proceed(&a);wait_state(&a,GT_TASK_CANCELLED);
        assert(reap(controller).state==GT_TASK_CANCELLED && a.input_frees==1 && a.result_frees==1);
      } else if(!strcmp(argv[1],"borrowed")) {
        proceed(&a);wait_state(&a,GT_TASK_READY);
        gt_task_info_t info;assert(gt__tasks_info(pool,a.id,&info)==GT_TASK_OK && info.generation==g);
        gt_attachment_checkpoint_t lease,wrong;
        assert(gt__attachment_take(controller,g+1,&wrong)==GT_TASK_INVALID && !wrong.result);
        assert(gt__attachment_take(controller,g,&lease)==GT_TASK_OK);
        assert(gt__attachment_detach(controller,g)==GT_TASK_OK);
        assert(gt__attachment_take(controller,g,&wrong)==GT_TASK_INVALID && !wrong.result);
        assert(gt__attachment_attach(controller,&next)==GT_TASK_OK && next>g);
        assert(*(unsigned *)lease.result==42 && gt__tasks_close(pool)==GT_TASK_BUSY);
        wrong=lease;wrong.generation=next;
        assert(gt__attachment_release(controller,&wrong)==GT_TASK_INVALID);
        assert(gt__attachment_release(controller,&lease)==GT_TASK_OK);
        assert(gt__attachment_release(controller,&lease)==GT_TASK_INVALID);
        wait_state(&a,GT_TASK_FINISHED);
        gt_attachment_retired_t r=reap(controller);
        assert(r.task==a.id && r.generation==g && r.context==&a && r.state==GT_TASK_FINISHED);
        assert(gt__attachment_current(controller,next));
        assert(gt__attachment_detach(controller,next)==GT_TASK_OK);
      } else if(!strcmp(argv[1],"failure") || !strcmp(argv[1],"failure-stale")) {
        proceed(&a);wait_state(&a,GT_TASK_FAILED);
        bool stale=!strcmp(argv[1],"failure-stale");
        if(stale) {
          assert(gt__attachment_detach(controller,g)==GT_TASK_OK);
          assert(gt__attachment_attach(controller,&next)==GT_TASK_OK && next>g);
          bind(controller,next,&b);wait_entered(&b);
        }
        gt_attachment_retired_t r=reap(controller);
        assert(r.state==GT_TASK_FAILED && r.error==GT_ERR_IO && r.context==&a && r.generation==g);
        assert(a.input_frees==1 && a.result_frees==1);
        if(stale)assert(gt__attachment_current(controller,next));
        else {
          assert(!gt__attachment_current(controller,g) && !gt__attachment_input_allowed(controller,g,40));
          gt_attachment_restore_info_t info;
          assert(gt__attachment_restore_info(controller,g,&info)==GT_RESTORE_OK);
          assert(info.phase==GT_RESTORE_DETACHED && !info.bytes && !info.events);
          assert(gt__attachment_detach(controller,g)==GT_TASK_INVALID);
          assert(gt__attachment_attach(controller,&next)==GT_TASK_OK && next>g);
          bind(controller,next,&b);wait_entered(&b);
        }
        proceed(&b);wait_state(&b,GT_TASK_READY);consume(controller,next,&b);r=reap(controller);
        assert(r.task==b.id && r.state==GT_TASK_FINISHED && r.generation==next);
        assert(gt__attachment_current(controller,next));
        assert(gt__attachment_detach(controller,next)==GT_TASK_OK);
      } else if(!strcmp(argv[1],"ready")) {
        proceed(&a);wait_state(&a,GT_TASK_READY);
        assert(gt__attachment_detach(controller,g)==GT_TASK_OK);
        gt_attachment_checkpoint_t lease;
        assert(gt__attachment_take(controller,g,&lease)==GT_TASK_INVALID && !lease.result);
        gt_attachment_retired_t r=reap(controller);
        assert(r.state==GT_TASK_CANCELLED && r.error==GT_ERR_CANCELLED && a.result_frees==1);
      } else {
        assert(pressure || !strcmp(argv[1],"running"));
        assert(gt__attachment_detach(controller,g)==GT_TASK_OK);
        assert(!gt__attachment_current(controller,g));
        assert(gt__attachment_close(controller)==GT_TASK_BUSY);
        assert(gt__attachment_attach(controller,&next)==GT_TASK_OK && next>g);
        gt_task_spec_t s=spec(&b);
        assert(gt__attachment_begin(controller,next,s.sequence)==GT_RESTORE_OK);
        assert(gt__attachment_submit(controller,g,&s,&rejected)==GT_TASK_INVALID && !rejected);
        gt_task_rc_t rc=gt__attachment_submit(controller,next,&s,&b.id);
        assert(rc==(pressure ? GT_TASK_FULL : GT_TASK_OK));
        assert(!b.input_frees && !b.encodes);
        proceed(&a);wait_state(&a,GT_TASK_CANCELLED);
        gt_attachment_retired_t r=reap(controller);
        assert(r.task==a.id && r.generation==g && r.context==&a && r.state==GT_TASK_CANCELLED);
        assert(a.input_frees==1 && a.result_frees==1 && gt__attachment_current(controller,next));
        if(pressure)assert(gt__attachment_submit(controller,next,&s,&b.id)==GT_TASK_OK);
        wait_entered(&b);proceed(&b);wait_state(&b,GT_TASK_READY);
        consume(controller,next,&b);r=reap(controller);
        assert(r.task==b.id && r.context==&b && r.generation==next && r.state==GT_TASK_FINISHED);
        assert(gt__attachment_detach(controller,next)==GT_TASK_OK);
      }
    }
  }
  assert(gt__attachment_close(controller)==GT_TASK_OK);
  assert(gt__tasks_close(pool)==GT_TASK_OK);
  destroy(&a);destroy(&b);return 0;
}
