#include "session/program-status.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
static gt_program_status_record_t record(const char *id) {
  return (gt_program_status_record_t){id,"builder","编译","进行中",1,0,25};
}
static void add(gt_program_status_store_t *s,const char *id) {
  gt_program_status_record_t r=record(id);assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
}
static gt_program_status_record_t at(gt_program_status_store_t *s,size_t i,const char *id) {
  gt_program_status_record_t r;assert(gt__program_status_at(s,i,&r));assert(!strcmp(r.id,id));return r;
}
typedef struct {void *pointer;size_t bytes;} allocation_t;
typedef struct {allocation_t slots[32];size_t calls,fail,live,peak,count,limit;} allocation_budget_t;
static void *budget_allocate(void *context,size_t n) {
  allocation_budget_t *b=context;b->calls++;
  if(b->calls==b->fail || n>b->limit-b->live)return NULL;
  assert(b->count<32);void *p=malloc(n);assert(p);
  b->slots[b->count++]=(allocation_t){p,n};b->live+=n;if(b->live>b->peak)b->peak=b->live;return p;
}
static void budget_release(void *context,void *p,size_t n) {
  allocation_budget_t *b=context;size_t i=0;
  while(i<b->count && b->slots[i].pointer!=p)i++;
  assert(i<b->count && b->slots[i].bytes==n && b->live>=n);
  b->live-=n;b->slots[i]=b->slots[--b->count];free(p);
}
static void allocator_case(void) {
  allocation_budget_t b={.limit=1024*1024};
  gt_status_allocator_t a={&b,budget_allocate,budget_release};
  gt_status_allocator_t invalid={&b,NULL,budget_release};
  assert(!gt__program_status_open_with_allocator(8,4096,&invalid) && !b.calls);
  invalid=(gt_status_allocator_t){&b,budget_allocate,NULL};
  assert(!gt__program_status_open_with_allocator(8,4096,&invalid) && !b.calls);
  for(size_t fail=1;fail<=2;fail++) {
    b.fail=b.calls+fail;
    assert(!gt__program_status_open_with_allocator(8,4096,&a) && !b.live && !b.count);
  }
  b.fail=0;gt_program_status_store_t *s=gt__program_status_open_with_allocator(8,4096,&a);
  assert(s && b.count==2);size_t fixed=b.live;add(s,"parent");add(s,"parent/done");
  assert(b.count==4 && b.live==fixed+gt__program_status_bytes(s));
  gt_program_status_record_t r=at(s,1,"parent/done");r.state=2;r.progress=-1;
  assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
  size_t before=b.live,used=gt__program_status_bytes(s);r=record("parent");r.message="replacement";
  b.limit=b.live;assert(gt__program_status_apply(s,&r)==GT_STATUS_NO_MEMORY);
  assert(b.live==before && gt__program_status_bytes(s)==used && !strcmp(at(s,0,"parent").message,"进行中"));
  b.limit=1024*1024;
  for(size_t fail=1;fail<=4;fail++) {
    b.fail=b.calls+fail;gt_program_status_store_t *copy=(void *)1;
    assert(gt__program_status_copy(s,&copy)==GT_STATUS_NO_MEMORY && !copy);
    assert(b.live==before && b.count==4 && gt__program_status_count(s)==2);
  }
  b.fail=0;gt_program_status_store_t *copy=NULL;
  assert(gt__program_status_copy(s,&copy)==GT_STATUS_OK && b.live==2*before && b.count==8);
  assert(gt__program_status_semantic(s,1)==1 && gt__program_status_count(copy)==2);
  r=(gt_program_status_record_t){.id="",.state=5};assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
  assert(b.live==fixed+before && b.count==6);
  gt__program_status_close(s);assert(b.live==before && b.count==4);
  gt__program_status_close(copy);assert(!b.live && !b.count && b.peak>=2*before);
}
static void domains_case(void) {
  allocation_budget_t source={.limit=1024*1024},destination={.limit=1024*1024};
  gt_status_allocator_t a={&source,budget_allocate,budget_release},b={&destination,budget_allocate,budget_release};
  gt_program_status_store_t *s=gt__program_status_open_with_allocator(8,4096,&a);assert(s);add(s,"one");add(s,"two");
  size_t before=source.live;gt_program_status_store_t *copy=(void *)1;
  gt_status_allocator_t invalid={&destination,NULL,budget_release};
  assert(gt__program_status_copy_with_allocator(s,&invalid,&copy)==GT_STATUS_INVALID && !copy && !destination.calls);
  assert(gt__program_status_copy_with_allocator(NULL,&b,&copy)==GT_STATUS_INVALID && !copy);
  assert(gt__program_status_copy_with_allocator(s,&b,NULL)==GT_STATUS_INVALID);
  for(size_t fail=1;fail<=4;fail++) {
    destination.fail=destination.calls+fail;copy=(void *)1;
    assert(gt__program_status_copy_with_allocator(s,&b,&copy)==GT_STATUS_NO_MEMORY && !copy);
    assert(!destination.live && !destination.count && source.live==before && source.count==4);
  }
  destination.fail=0;destination.limit=1;
  assert(gt__program_status_copy_with_allocator(s,&b,&copy)==GT_STATUS_NO_MEMORY && !copy && !destination.live);
  destination.limit=1024*1024;
  assert(gt__program_status_copy_with_allocator(s,&b,&copy)==GT_STATUS_OK);
  assert(source.live==before && destination.live==before && source.count==4 && destination.count==4);
  gt_program_status_store_t *plain=NULL;
  assert(gt__program_status_copy_with_allocator(s,NULL,&plain)==GT_STATUS_OK && source.live==before && destination.live==before);
  gt__program_status_close(s);assert(!source.live && !source.count && destination.live==before);
  at(copy,0,"one");at(copy,1,"two");add(copy,"three");
  gt__program_status_close(copy);assert(!destination.live && !destination.count);
  at(plain,0,"one");gt__program_status_close(plain);
}
int main(int argc,char **argv) {
  assert(argc==2);gt_program_status_store_t *s=gt__program_status_open(8,4096);assert(s);
  if(!strcmp(argv[1],"domains")) {
    domains_case();
  } else if(!strcmp(argv[1],"allocator")) {
    allocator_case();
  } else if(!strcmp(argv[1],"prompt")) {
    const unsigned states[]={1,3,2,4,0};const char *ids[]={"a","b","a/done","b/error","idle"};
    for(unsigned i=0;i<5;i++) {
      gt_program_status_record_t r=record(ids[i]);r.state=states[i];r.progress=-1;
      assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
    }
    gt_program_status_store_t *copy=NULL;assert(gt__program_status_copy(s,&copy)==GT_STATUS_OK);
    size_t before=gt__program_status_bytes(s);
    for(unsigned kind=0;kind<6;kind++)if(kind!=1)assert(!gt__program_status_semantic(s,kind) && gt__program_status_count(s)==5 && gt__program_status_bytes(s)==before);
    assert(gt__program_status_semantic(s,1)==2 && gt__program_status_count(s)==3 && gt__program_status_bytes(s)<before);
    at(s,0,"a/done");at(s,1,"b/error");at(s,2,"idle");
    assert(gt__program_status_count(copy)==5 && gt__program_status_bytes(copy)==before);
    assert(!gt__program_status_semantic(s,1) && !gt__program_status_semantic(NULL,1));
    gt__program_status_close(copy);
  } else if(!strcmp(argv[1],"copy")) {
    char id[]="task",text[]="original";gt_program_status_record_t r={id,text,text,text,3,2,-1};
    assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);memset(text,'X',sizeof(text)-1);id[0]='X';
    r=at(s,0,"task");assert(!strcmp(r.app,"original") && !strcmp(r.title,"original") && !strcmp(r.message,"original"));
    gt_program_status_store_t *copy=NULL;assert(gt__program_status_copy(s,&copy)==GT_STATUS_OK && copy);
    add(s,"task");gt__program_status_close(s);s=copy;
    r=at(s,0,"task");assert(r.state==3 && r.kind==2 && r.progress==-1 && !strcmp(r.message,"original"));
  } else if(!strcmp(argv[1],"clear")) {
    const char *ids[]={"a","a/b","ab","a/b/c","a-b","b/a"};
    for(unsigned i=0;i<6;i++)add(s,ids[i]);
    gt_program_status_record_t clear={.id="a",.state=5};
    assert(gt__program_status_apply(s,&clear)==GT_STATUS_OK && gt__program_status_count(s)==3);
    at(s,0,"ab");at(s,1,"a-b");at(s,2,"b/a");
    clear.id="absent";size_t bytes=gt__program_status_bytes(s);
    assert(gt__program_status_apply(s,&clear)==GT_STATUS_OK && gt__program_status_bytes(s)==bytes);
    clear.id="";assert(gt__program_status_apply(s,&clear)==GT_STATUS_OK);
    assert(!gt__program_status_count(s) && !gt__program_status_bytes(s));
  } else if(!strcmp(argv[1],"aliases")) {
    add(s,"a");add(s,"a/b");add(s,"a/c");
    gt_program_status_record_t r=at(s,0,"a");r.progress=90;
    assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
    r=at(s,0,"a");assert(r.progress==90);r.state=5;
    assert(gt__program_status_apply(s,&r)==GT_STATUS_OK && !gt__program_status_count(s));
  } else if(!strcmp(argv[1],"limits")) {
    gt__program_status_close(s);s=gt__program_status_open(1,64);assert(s);add(s,"a");
    gt_program_status_record_t r=record("b");size_t bytes=gt__program_status_bytes(s);
    assert(gt__program_status_apply(s,&r)==GT_STATUS_LIMIT && gt__program_status_bytes(s)==bytes);
    r.id="a";r.message="0123456789012345678901234567890123456789012345678901234567890123456789";
    assert(gt__program_status_apply(s,&r)==GT_STATUS_LIMIT && gt__program_status_bytes(s)==bytes);
    r=at(s,0,"a");assert(!strcmp(r.message,"进行中"));
    r=record("a");r.message="";assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
    assert(gt__program_status_bytes(s)==bytes-strlen("进行中"));
    r=(gt_program_status_record_t){.id="",.state=5};assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
    add(s,"b");at(s,0,"b");
  } else if(!strcmp(argv[1],"states")) {
    for(unsigned state=0;state<5;state++) {
      gt_program_status_record_t r=record("a");r.state=state;r.kind=state==3?3:0;r.progress=state==1 || state==3 ? 100:-1;
      assert(gt__program_status_apply(s,&r)==GT_STATUS_OK);
      gt_program_status_record_t stored=at(s,0,"a");
      assert(stored.state==state && stored.kind==r.kind && stored.progress==r.progress);
      assert(!strcmp(stored.app,r.app) && !strcmp(stored.title,r.title) && !strcmp(stored.message,r.message));
      assert(gt__program_status_count(s)==1);
    }
    add(s,"b");at(s,0,"a");at(s,1,"b");
  } else {
    assert(!strcmp(argv[1],"invalid"));
    assert(!gt__program_status_open(0,1) && !gt__program_status_open(1025,1) && !gt__program_status_open(1,0));
    gt_program_status_record_t r=record("a");r.state=6;
    assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID);
    r=record("a");r.progress=101;assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID);
    r.progress=-2;assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID);
    r=record("a");r.state=0;assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID);
    r=record("a");r.kind=1;assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID);
    r=record(NULL);assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID);
    r=record("a");r.message=NULL;assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID);
    char long_id[4098];memset(long_id,'x',sizeof(long_id));long_id[4097]=0;r=record(long_id);
    assert(gt__program_status_apply(s,&r)==GT_STATUS_INVALID && !gt__program_status_count(s));
    gt_program_status_store_t *copy=(void *)1;
    assert(gt__program_status_copy(NULL,&copy)==GT_STATUS_INVALID && !copy);
    assert(!gt__program_status_at(s,0,&r) && !r.id);
  }
  gt__program_status_close(s);gt__program_status_close(NULL);return 0;
}
