#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include <sys/socket.h>
#ifdef GT_HAVE_LIBSSH2
int route_fixture_connect(int,const struct sockaddr *,socklen_t);
/* Real production connector, substituting only its native dial boundary.
 * The client is forbidden to connect anywhere except the first SSH port. */
#define connect route_fixture_connect
#include "../src/ssh.c"
#undef connect
#endif
#define main route_base_main
#include "test_bridge_ssh.c"
#undef main
#include "extension/route.h"
#ifdef GT_HAVE_LIBSSH2
#include <arpa/inet.h>
#include <dirent.h>
static unsigned dial_calls;
static uint16_t permitted_port;
int route_fixture_connect(int fd,const struct sockaddr *address,socklen_t n) {
  dial_calls++;
  if(address->sa_family!=AF_INET || ntohs(((const struct sockaddr_in *)address)->sin_port)!=permitted_port) {
    errno=EACCES;return -1;
  }
  return connect(fd,address,n);
}
static gt_bridge_route_info_t route_info(gt_bridge_stream_t *r){gt_bridge_route_info_t i;CHECK(gt__bridge_route_info(r,&i));return i;}
static gt_ssh_t *route_material(char **argv,size_t i,bool credentials) {
  char identity[64];CHECK(snprintf(identity,sizeof(identity),"route-hop-%zu.invalid",i)>0);
  gt_ssh_opts_t options={.host=identity,.port=(uint16_t)atoi(argv[6+i]),.username=argv[3],.known_hosts_path=argv[4]};
  gt_ssh_t *s=gt_ssh_new(&options);CHECK(s);
  if(credentials)CHECK(gt_ssh_auth_private_key(s,private_key,NULL)==GT_OK);
  return s;
}
typedef struct {size_t live,calls,fail;} route_budget_t;
typedef union {max_align_t align;size_t size;} route_block_t;
static void *route_allocate(void *ctx,size_t n) {
  route_budget_t *b=ctx;if(++b->calls==b->fail)return NULL;
  route_block_t *p=malloc(sizeof(*p)+n);CHECK(p);p->size=n;b->live+=n;return p+1;
}
static void route_release(void *ctx,void *p,size_t n){route_budget_t *b=ctx;route_block_t *a=(route_block_t *)p-1;CHECK(a->size==n && b->live>=n);b->live-=n;free(a);}
static void route_close(gt_bridge_stream_t *r,route_budget_t *b) {
  uint64_t start=now();
  for(;;){gt_bridge_close_t status=gt__bridge_stream_close(r,0);if(status==GT_BRIDGE_RELEASED)break;CHECK(status==GT_BRIDGE_DRAINING && now()-start<1000);}
  CHECK(gt__bridge_stream_close(r,0)==GT_BRIDGE_RELEASED && route_info(r).phase==GT_BRIDGE_ROUTE_CLOSED);
  CHECK(gt__bridge_stream_write(r,"late",4,0).state==GT_BRIDGE_CLOSED);
  CHECK(gt__bridge_stream_destroy(r)==GT_OK && !b->live && now()-start<1000);
}
static void until_route(gt_bridge_stream_t *r,gt_bridge_route_phase_t phase) {
  uint64_t end=now()+6000;
  for(;;){gt_bridge_route_phase_t p=gt__bridge_route_step(r,25);if(p==phase)break;CHECK(p<GT_BRIDGE_ROUTE_FAILED && !(phase==GT_BRIDGE_ROUTE_FAILED && p==GT_BRIDGE_ROUTE_READY) && now()<end);}
}
static size_t descriptors(void) {
  DIR *d=opendir("/dev/fd");CHECK(d);size_t count=0;struct dirent *e;
  while((e=readdir(d)))if(strcmp(e->d_name,".") && strcmp(e->d_name,".."))count++;
  CHECK(!closedir(d));return count;
}
static void stall_barrier(gt_bridge_stream_t *r,size_t hop) {
  uint64_t end=now()+5000;
  while(route_info(r).hop!=hop){CHECK(gt__bridge_route_step(r,10)<GT_BRIDGE_ROUTE_FAILED && now()<end);}
  puts("ROUTE_STALL_BARRIER");fflush(stdout);
  struct pollfd input={0,POLLIN,0};
  while(poll(&input,1,0)==0) {
    CHECK(gt__bridge_route_step(r,0)<GT_BRIDGE_ROUTE_FAILED && now()<end);delay(1);
  }
  uint8_t byte;CHECK(exact_read(0,&byte,1) && byte=='R');
}
static void run_route(int argc,char **argv) {
  CHECK(argc>=8);size_t count=(size_t)atoi(argv[2]);CHECK(count>=2 && count<=8 && argc==(int)(6+count));
  const char *mode=argv[1];permitted_port=(uint16_t)atoi(argv[6]);dial_calls=0;
  uint8_t header[4];CHECK(exact_read(0,header,4));size_t keylen=((size_t)header[0]<<24)|((size_t)header[1]<<16)|((size_t)header[2]<<8)|header[3];
  CHECK(keylen && keylen<8192);private_key=calloc(1,keylen+1);CHECK(private_key && exact_read(0,private_key,keylen));
  route_budget_t budget={0};gt_allocator_t allocator={&budget,route_allocate,route_release};
  gt_ssh_t *material[8]={0};gt_bridge_route_hop_t hops[8]={0};char via[8][16];
  for(size_t i=0;i<count;i++) {
    material[i]=route_material(argv,i,(strcmp(mode,"credentials") && strcmp(mode,"wrong-auth")) || i+1<count);
    if(!strcmp(mode,"wrong-auth") && i+1==count)CHECK(gt_ssh_auth_password(material[i],"wrong-fixture-password")==GT_OK);
    hops[i].connector=(gt_ssh_connector_config_t){.family=i?0:4,.address={127,0,0,1},.timeout_ms=3000,.clock=clock_read};
    if(i){memcpy(via[i],"127.0.0.1",10);hops[i].via_host=via[i];}
    if(!strcmp(mode,"credentials") && i+1==count)hops[i].connector.defer_credentials=true;
  }
  size_t before=descriptors();gt_bridge_stream_t *route=NULL;
  gt_bridge_route_options_t options={!strcmp(mode,"deadline")?1000:10000,{3000,262144}};
  if(!strcmp(mode,"allocation")) {
    for(size_t fail=1;fail<=2;fail++) {
      budget.calls=0;budget.fail=fail;
      CHECK(gt__bridge_route_open(material,hops,count,argv[5],&options,&allocator,&route)==GT_ERR_IO && !route && material[0] && material[1] && !budget.live && descriptors()==before);
    }
    budget.fail=0;
    gt_bridge_route_hop_t bad[8];memcpy(bad,hops,sizeof(bad));bad[1].connector.family=4;
    CHECK(gt__bridge_route_open(material,bad,count,argv[5],&options,&allocator,&route)==GT_ERR_INVALID_ARG && !route && material[0]);
    gt_ssh_t *alias[8]={material[0],material[0]};
    CHECK(gt__bridge_route_open(alias,hops,2,argv[5],&options,&allocator,&route)==GT_ERR_INVALID_ARG && !route && alias[0]==material[0] && alias[1]==material[0]);
    CHECK(gt__bridge_route_open(material,hops,0,argv[5],&options,&allocator,&route)==GT_ERR_INVALID_ARG && !route && material[0]);
    CHECK(gt__bridge_route_open(material,hops,9,argv[5],&options,&allocator,&route)==GT_ERR_INVALID_ARG && !route && material[0]);
    memcpy(bad,hops,sizeof(bad));bad[1].connector.clock_context=&budget;
    CHECK(gt__bridge_route_open(material,bad,count,argv[5],&options,&allocator,&route)==GT_ERR_INVALID_ARG && !route && material[1]);
    memcpy(bad,hops,sizeof(bad));bad[1].via_host="bad host";
    CHECK(gt__bridge_route_open(material,bad,count,argv[5],&options,&allocator,&route)==GT_ERR_INVALID_ARG && !route && material[1]);
  }
  CHECK(gt__bridge_route_open(material,hops,count,argv[5],&options,&allocator,&route)==GT_OK && route && budget.live);
  for(size_t i=0;i<count;i++)CHECK(!material[i]);
  /* Route must own configuration strings/POD after construction. */
  for(size_t i=1;i<count;i++){memset(via[i],'x',9);hops[i].connector.family=4;}
  if(!strcmp(mode,"cancel-auth") || !strcmp(mode,"deadline"))stall_barrier(route,count-1);
  if(!strcmp(mode,"wrong-pin") || !strcmp(mode,"wrong-auth") || !strcmp(mode,"forward-deny") || !strcmp(mode,"middle-deny") || !strcmp(mode,"deadline")) {
    until_route(route,GT_BRIDGE_ROUTE_FAILED);gt_bridge_route_info_t info=route_info(route);
    if(!strcmp(mode,"wrong-pin"))CHECK(info.hop==count-1 && info.error==GT_ERR_HOST_KEY && info.authentication[count-1].host==GT_SSH_HOST_MISMATCH);
    if(!strcmp(mode,"wrong-auth"))CHECK(info.hop==count-1 && info.error==GT_ERR_AUTH && info.authentication[count-1].host==GT_SSH_HOST_KNOWN);
    if(!strcmp(mode,"forward-deny"))CHECK(info.hop==0 && info.failure_phase==GT_BRIDGE_ROUTE_FORWARD && info.error==GT_ERR_IO && info.authentication[1].host==GT_SSH_HOST_UNCHECKED);
    if(!strcmp(mode,"middle-deny"))CHECK(info.hop==1 && info.failure_phase==GT_BRIDGE_ROUTE_FORWARD && info.error==GT_ERR_IO && info.authentication[2].host==GT_SSH_HOST_UNCHECKED);
    if(!strcmp(mode,"deadline"))CHECK(info.hop==count-1 && info.deadline && info.error==GT_ERR_IO);
  } else if(!strcmp(mode,"unknown-accept") || !strcmp(mode,"unknown-reject") || !strcmp(mode,"credentials")) {
    uint64_t end=now()+6000;
    for(;;) {
      gt_bridge_route_phase_t p=gt__bridge_route_step(route,25);gt_bridge_route_info_t info=route_info(route);
      CHECK(p<GT_BRIDGE_ROUTE_READY && now()<end);
      if(info.hop==count-1 && info.authentication[count-1].phase==(!strcmp(mode,"credentials")?GT_SSH_CONNECT_CREDENTIALS:GT_SSH_CONNECT_HOSTKEY))break;
    }
    if(!strcmp(mode,"credentials")) {
      gt_ssh_t *fresh=route_material(argv,count-1,true);
      CHECK(gt__bridge_route_supply(route,&fresh)==GT_OK && !fresh);
      until_route(route,GT_BRIDGE_ROUTE_READY);ready(route);roundtrip(route,65536);
    } else {
      CHECK(route_info(route).authentication[count-1].host==GT_SSH_HOST_UNKNOWN);
      CHECK(gt__bridge_route_decide(route,GT_HOSTKEY_UNKNOWN)==GT_OK);
      CHECK(route_info(route).decisions[count-1]==GT_HOSTKEY_UNKNOWN && !route_info(route).authentication[count-1].auth_methods);
      bool accept=!strcmp(mode,"unknown-accept");
      CHECK(gt__bridge_route_decide(route,accept?GT_HOSTKEY_ACCEPT:GT_HOSTKEY_REJECT)==(accept?GT_OK:GT_ERR_HOST_KEY));
      CHECK(route_info(route).decisions[count-1]==(accept?GT_HOSTKEY_ACCEPT:GT_HOSTKEY_REJECT));
      if(accept){until_route(route,GT_BRIDGE_ROUTE_READY);ready(route);roundtrip(route,65536);}
      else CHECK(route_info(route).phase==GT_BRIDGE_ROUTE_FAILED && route_info(route).error==GT_ERR_HOST_KEY);
    }
  } else if(!strcmp(mode,"cancel-auth")) {
    uint64_t end=now()+5000;
    while(route_info(route).hop!=count-1){CHECK(gt__bridge_route_step(route,25)<GT_BRIDGE_ROUTE_FAILED && now()<end);}
    pthread_t thread;CHECK(!pthread_create(&thread,NULL,stop_later,route));uint64_t start=now();
    CHECK(gt__bridge_route_step(route,5000)==GT_BRIDGE_ROUTE_STOPPED);
    CHECK(!pthread_join(thread,NULL) && now()-start<1000 && route_info(route).error==GT_ERR_CANCELLED);
  } else {
    until_route(route,GT_BRIDGE_ROUTE_READY);gt_bridge_route_info_t info=route_info(route);
    for(size_t i=0;i<count;i++) {
      CHECK(info.authentication[i].host==GT_SSH_HOST_KNOWN && info.authentication[i].phase==GT_SSH_CONNECT_READY);
      for(size_t j=0;j<i;j++)CHECK(strcmp(info.authentication[i].fingerprint,info.authentication[j].fingerprint));
    }
    ready(route);roundtrip(route,65536);
    if(!strcmp(mode,"cancel-read")) {
      pthread_t thread;CHECK(!pthread_create(&thread,NULL,stop_later,route));uint8_t byte;uint64_t start=now();
      gt_bridge_io_t r=gt__bridge_stream_read(route,&byte,1,5000);
      CHECK(r.state==GT_BRIDGE_CLOSED && r.error==GT_ERR_CANCELLED && now()-start<1000);CHECK(!pthread_join(thread,NULL));
    } else for(int i=0;i<4;i++)roundtrip(route,65536);
  }
  CHECK(dial_calls==1);route_close(route,&budget);CHECK(descriptors()==before);
  gt__secure_erase(private_key,keylen);free(private_key);private_key=NULL;
  printf("PASS bridge_route_%s_%zu\n",mode,count);
}
#endif
int main(int argc,char **argv) {
  if(argc==3 && !strcmp(argv[1],"--executor")){executor(argv[2]);return 0;}
#ifdef GT_HAVE_LIBSSH2
  run_route(argc,argv);
#else
  CHECK(argc==2 && !strcmp(argv[1],"unsupported"));gt_ssh_t *owned[2]={(gt_ssh_t *)1,(gt_ssh_t *)2};
  gt_bridge_stream_t *r=(gt_bridge_stream_t *)1;
  CHECK(gt__bridge_route_open(owned,NULL,2,"fixed",NULL,NULL,&r)==GT_ERR_UNSUPPORTED && !r && owned[0]==(gt_ssh_t *)1);
  puts("PASS bridge_route_unsupported");
#endif
  return 0;
}
