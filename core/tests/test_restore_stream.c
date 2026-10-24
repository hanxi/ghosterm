#include "session/restore-stream.h"
#include <assert.h>
#include <string.h>
#include "allocation-test.h"
static void start(gt_restore_stream_t *s,uint64_t generation,uint64_t sequence) {
  gt__restore_reset(s,generation);
  assert(gt__restore_begin(s,generation,sequence)==GT_RESTORE_OK);
  assert(gt__restore_offer(s,generation,sequence)==GT_RESTORE_OK);
}
static void ended_case(void) {
  gt_restore_stream_t s={0};assert(gt__restore_init(&s,16,8));start(&s,7,40);
  assert(gt__restore_end(&s,7,40)==GT_RESTORE_WAIT && s.phase==GT_RESTORE_SNAPSHOT);
  assert(gt__restore_ready_snapshot(&s,7,40)==GT_RESTORE_OK);uint64_t cut=0;
  assert(gt__restore_live_begin(&s,7,&cut)==GT_RESTORE_OK && cut==40);
  assert(gt__restore_ready_live(&s,7,40)==GT_RESTORE_OK);
  assert(gt__restore_push(&s,41,1,"A",1)==GT_RESTORE_OK && gt__restore_push(&s,42,1,"B",1)==GT_RESTORE_OK);
  assert(gt__restore_end(&s,8,42)==GT_RESTORE_STALE && s.phase==GT_RESTORE_LIVE);
  assert(gt__restore_end(&s,7,41)==GT_RESTORE_PROTOCOL && s.phase==GT_RESTORE_LIVE);
  assert(gt__restore_end(&s,7,43)==GT_RESTORE_WAIT && s.phase==GT_RESTORE_LIVE);
  assert(gt__restore_end(&s,7,42)==GT_RESTORE_OK && s.phase==GT_RESTORE_ENDED && s.count==2 && s.used==2);
  assert(!gt__restore_input_allowed(&s,7,40));
  assert(gt__restore_push(&s,43,1,"C",1)==GT_RESTORE_PROTOCOL && s.count==2 && s.last==42);
  assert(gt__restore_ready_live(&s,7,40)==GT_RESTORE_PROTOCOL && s.phase==GT_RESTORE_ENDED && s.count==2);
  assert(gt__restore_ready_snapshot(&s,7,40)==GT_RESTORE_PROTOCOL && s.phase==GT_RESTORE_ENDED);
  assert(gt__restore_begin(&s,7,42)==GT_RESTORE_PROTOCOL && gt__restore_offer(&s,7,40)==GT_RESTORE_PROTOCOL && s.count==2);
  assert(gt__restore_live_begin(&s,7,&cut)==GT_RESTORE_WAIT && !cut);
  gt_restore_event_t e;char b[2];
  assert(gt__restore_next(&s,7,&e,b,sizeof(b))==GT_RESTORE_OK && e.sequence==41 && b[0]=='A');
  assert(gt__restore_next(&s,7,&e,b,sizeof(b))==GT_RESTORE_OK && e.sequence==42 && b[0]=='B');
  assert(gt__restore_next(&s,7,&e,b,sizeof(b))==GT_RESTORE_WAIT && !s.count && s.phase==GT_RESTORE_ENDED);
  assert(gt__restore_end(&s,7,42)==GT_RESTORE_OK && gt__restore_end(&s,7,43)==GT_RESTORE_PROTOCOL && s.phase==GT_RESTORE_ENDED);
  gt__restore_detach(&s);assert(gt__restore_end(&s,7,42)==GT_RESTORE_STALE);gt__restore_destroy(&s);
}
static void allocation_case(void) {
  allocation_test_budget_t b={.limit=1024*1024};
  gt_allocator_t a={&b,allocation_test_allocate,allocation_test_release};gt_restore_stream_t s={0};
  gt_allocator_t invalid={&b,NULL,allocation_test_release};
  assert(!gt__restore_init_with_allocator(&s,8,3,&invalid) && !b.calls);
  for(size_t fail=1;fail<=2;fail++) {
    b.fail=b.calls+fail;assert(!gt__restore_init_with_allocator(&s,8,3,&a));
    assert(!b.live && !b.count && !s.bytes && !s.entries);gt__restore_destroy(&s);
  }
  b.fail=0;b.limit=7;assert(!gt__restore_init_with_allocator(&s,8,3,&a) && !b.live);
  b.limit=1024*1024;assert(gt__restore_init_with_allocator(&s,8,3,&a));
  size_t expected=8+3*sizeof(*s.entries),calls=b.calls;
  assert(b.live==expected && b.count==2);start(&s,1,0);
  assert(gt__restore_push(&s,1,1,"01234567",8)==GT_RESTORE_OK);
  assert(gt__restore_push(&s,2,1,"X",1)==GT_RESTORE_LIMIT && !s.count && b.live==expected);
  gt__restore_detach(&s);start(&s,2,0);
  assert(gt__restore_push(&s,1,1,"new",3)==GT_RESTORE_OK);
  assert(gt__restore_ready_snapshot(&s,2,0)==GT_RESTORE_OK);
  gt_restore_event_t e;char bytes[8];assert(gt__restore_next(&s,2,&e,bytes,sizeof(bytes))==GT_RESTORE_OK);
  assert(e.length==3 && !memcmp(bytes,"new",3) && b.live==expected && b.calls==calls);
  gt__restore_destroy(&s);assert(!b.live && !b.count && !s.bytes && !s.entries);
  gt__restore_destroy(&s);gt__restore_destroy(NULL);
}
int main(int argc,char **argv) {
  assert(argc==2);gt_restore_stream_t s={0};
  if(!strcmp(argv[1],"ended")){ended_case();return 0;}
  if(!strcmp(argv[1],"allocator")) {allocation_case();return 0;}
  if(!strcmp(argv[1],"invalid")) {
    assert(!gt__restore_init(&s,0,3));assert(!gt__restore_init(&s,8,0));
    assert(!gt__restore_init(&s,SIZE_MAX,3));return 0;
  }
  assert(gt__restore_init(&s,8,3));
  gt_restore_event_t e;char bytes[8];uint64_t cut=99;
  if(!strcmp(argv[1],"flow")) {
    start(&s,7,40);
    assert(gt__restore_push(&s,41,1,"abc",3)==GT_RESTORE_OK);
    assert(gt__restore_push(&s,42,2,"xy",2)==GT_RESTORE_OK);
    assert(!gt__restore_input_allowed(&s,7,40));
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_WAIT);
    assert(gt__restore_ready_snapshot(&s,7,40)==GT_RESTORE_OK);
    assert(gt__restore_live_begin(&s,7,&cut)==GT_RESTORE_WAIT && !cut);
    assert(gt__restore_next(&s,7,&e,bytes,2)==GT_RESTORE_SPACE && e.length==3 && s.count==2);
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_OK);
    assert(e.generation==7 && e.sequence==41 && e.kind==1 && e.length==3 && !memcmp(bytes,"abc",3));
    assert(gt__restore_push(&s,43,1,"DE",2)==GT_RESTORE_OK);
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_OK);
    assert(e.sequence==42 && e.kind==2 && !memcmp(bytes,"xy",2));
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_WAIT);
    assert(gt__restore_live_begin(&s,7,&cut)==GT_RESTORE_OK && cut==42);
    assert(!gt__restore_input_allowed(&s,7,42));
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==43 && !memcmp(bytes,"DE",2));
    assert(gt__restore_ready_live(&s,7,42)==GT_RESTORE_OK);
    assert(gt__restore_input_allowed(&s,7,42) && gt__restore_input_allowed(&s,7,43));
    assert(!gt__restore_input_allowed(&s,7,41) && !gt__restore_input_allowed(&s,7,44) && !gt__restore_input_allowed(&s,8,42));
    assert(gt__restore_push(&s,44,1,"WXYZ",4)==GT_RESTORE_OK);
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==44 && !memcmp(bytes,"WXYZ",4));
    assert(!s.used && !s.count);
  } else if(!strcmp(argv[1],"empty")) {
    start(&s,7,40);assert(!gt__restore_input_allowed(&s,7,40));
    assert(gt__restore_ready_snapshot(&s,7,40)==GT_RESTORE_OK);
    assert(!gt__restore_input_allowed(&s,7,40));
    assert(gt__restore_live_begin(&s,7,&cut)==GT_RESTORE_OK && cut==40);
    assert(!gt__restore_input_allowed(&s,7,40));
    assert(gt__restore_ready_live(&s,7,40)==GT_RESTORE_OK && gt__restore_input_allowed(&s,7,40));
    gt__restore_detach(&s);assert(!gt__restore_input_allowed(&s,7,40));
  } else if(!strcmp(argv[1],"limits")) {
    start(&s,7,0);assert(gt__restore_push(&s,1,1,"oversized",9)==GT_RESTORE_LIMIT);
    assert(s.phase==GT_RESTORE_FAILED && s.failure==GT_RESTORE_LIMIT && !s.used && !s.count);
    start(&s,8,0);
    for(uint64_t i=1;i<=3;i++)assert(gt__restore_push(&s,i,2,NULL,0)==GT_RESTORE_OK);
    assert(gt__restore_push(&s,4,2,NULL,0)==GT_RESTORE_LIMIT);
    assert(!s.used && !s.count);
  } else if(!strcmp(argv[1],"protocol")) {
    start(&s,7,40);assert(gt__restore_push(&s,42,1,"gap",3)==GT_RESTORE_PROTOCOL);
    assert(!s.count && s.phase==GT_RESTORE_FAILED);
    start(&s,8,UINT64_MAX);assert(gt__restore_push(&s,0,1,NULL,0)==GT_RESTORE_PROTOCOL);
    start(&s,9,40);assert(gt__restore_push(&s,41,1,NULL,1)==GT_RESTORE_PROTOCOL);
    start(&s,10,40);assert(gt__restore_ready_snapshot(&s,9,40)==GT_RESTORE_STALE);
    assert(s.phase==GT_RESTORE_SNAPSHOT);
    assert(gt__restore_ready_snapshot(&s,10,39)==GT_RESTORE_PROTOCOL);
  } else if(!strcmp(argv[1],"phases")) {
    start(&s,7,40);assert(gt__restore_ready_live(&s,7,40)==GT_RESTORE_PROTOCOL);
    start(&s,8,40);assert(gt__restore_ready_snapshot(&s,8,40)==GT_RESTORE_OK);
    assert(gt__restore_ready_live(&s,8,40)==GT_RESTORE_PROTOCOL);
    gt__restore_reset(&s,9);assert(gt__restore_begin(&s,9,40)==GT_RESTORE_OK);
    assert(gt__restore_ready_snapshot(&s,9,40)==GT_RESTORE_PROTOCOL);
    start(&s,10,40);assert(gt__restore_ready_snapshot(&s,10,40)==GT_RESTORE_OK);
    assert(gt__restore_live_begin(&s,10,&cut)==GT_RESTORE_OK);
    assert(gt__restore_ready_live(&s,10,41)==GT_RESTORE_PROTOCOL);
    start(&s,11,40);assert(gt__restore_begin(&s,11,40)==GT_RESTORE_PROTOCOL);
  } else if(!strcmp(argv[1],"interrupt")) {
    /* 恢复到一半掉线：队列里还留着没送出的事件，这一代不能再取事件、
     * 不能进 LIVE，也不能授权输入；下一代从头开始，不继承任何残留。 */
    start(&s,7,40);
    for(uint64_t i=41;i<=43;i++)assert(gt__restore_push(&s,i,1,"ab",2)==GT_RESTORE_OK);
    assert(gt__restore_ready_snapshot(&s,7,40)==GT_RESTORE_OK);
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==41);
    assert(s.count==2 && s.dequeued==41 && s.cut==43);
    gt__restore_detach(&s);
    assert(s.phase==GT_RESTORE_DETACHED && !s.count && !s.used);
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_STALE);
    assert(gt__restore_live_begin(&s,7,&cut)==GT_RESTORE_STALE && !cut);
    assert(gt__restore_ready_live(&s,7,43)==GT_RESTORE_STALE);
    assert(!gt__restore_input_allowed(&s,7,41) && !gt__restore_input_allowed(&s,7,43));
    gt__restore_reset(&s,8);
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_STALE);
    assert(gt__restore_begin(&s,8,40)==GT_RESTORE_OK && !s.count && s.dequeued==40);
    assert(gt__restore_offer(&s,8,40)==GT_RESTORE_OK);
    assert(gt__restore_push(&s,41,1,"cd",2)==GT_RESTORE_OK);
    assert(gt__restore_ready_snapshot(&s,8,40)==GT_RESTORE_OK);
    assert(gt__restore_next(&s,8,&e,bytes,sizeof(bytes))==GT_RESTORE_OK && e.sequence==41);
  } else if(!strcmp(argv[1],"reset")) {
    start(&s,7,40);assert(gt__restore_push(&s,41,1,"old",3)==GT_RESTORE_OK);
    gt__restore_detach(&s);assert(!s.count && !s.used);
    assert(gt__restore_push(&s,42,1,"ignored",7)==GT_RESTORE_IDLE);
    gt__restore_reset(&s,8);assert(gt__restore_push(&s,50,1,"ignored",7)==GT_RESTORE_IDLE);
    assert(gt__restore_next(&s,7,&e,bytes,sizeof(bytes))==GT_RESTORE_STALE);
    assert(gt__restore_begin(&s,8,100)==GT_RESTORE_OK && !s.count);
  } else assert(!"unknown case");
  gt__restore_destroy(&s);return 0;
}
