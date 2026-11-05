/* Public headers/calls only, including the managed-SSH/RPC fixture. */
#define EXT_RPC_MAIN ext_resource_rpc_fixture_main
#include "test_ext_rpc.c"
#undef EXT_RPC_MAIN
#include <sys/stat.h>
typedef struct {gt_ext_resource_store_t *s;char path[4096];} res_store_t;
typedef struct {rpc_pair_t rpc;res_store_t h,e;} res_pair_t;
static const char *res_root;
static uint64_t res_last_handle;
static bool res_keep_result;
static res_store_t res_store(uint64_t bytes){
 res_store_t s={0};CHECK(snprintf(s.path,sizeof(s.path),"%s/public-resource-XXXXXX",res_root)>0 && mkdtemp(s.path));int fd=open(s.path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(fd>=0);gt_ext_resource_store_config_t c={fd,bytes,bytes<262144?bytes:262144,64};CHECK(gt_ext_resource_store_open(&c,&s.s)==GT_EXT_RESOURCE_OK && !close(fd));return s;
}
static gt_ext_resource_usage_t res_usage(res_store_t *s){gt_ext_resource_usage_t u;CHECK(gt_ext_resource_store_usage(s->s,&u));return u;}
static void res_store_drop(res_store_t *s){CHECK(gt_ext_resource_store_destroy(&s->s)==GT_EXT_RESOURCE_OK && !s->s && !rmdir(s->path));}
static gt_ext_resource_descriptor_t res_descriptor(size_t size,bool unknown){
 gt_ext_resource_descriptor_t d={.size=size,.size_known=!unknown};if(unknown)d.size=0;
 strcpy(d.sha256,size==3?"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad":size?"31a1f9dea0169551092d05e8bf4a446228c8c3eb4c9b713c66adcb7fd53c89be":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");strcpy(d.mime,"application/octet-stream");strcpy(d.display_name,"../展示/\"snapshot\\.bin");return d;
}
static void res_open(res_pair_t *f,const char *mode){
 memset(f,0,sizeof(*f));f->h=res_store(!strcmp(mode,"quota")?3:1048576);f->e=res_store(!strcmp(mode,"quota")?3:1048576);int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));gt_ext_channel_t *h=ext_channel(fd[0]),*e=ext_channel(fd[1]);gt_ext_session_config_t c=ext_config();c.features=GT_EXT_FEATURE_RESOURCES|GT_EXT_FEATURE_REQUESTS;c.required_count=2;c.available_count=3;strcpy(c.required[1],"resource.receive");strcpy(c.available[2],"resource.receive");gt_ext_rpc_method_t m[4];rpc_methods(m);
 if(!strcmp(mode,"denied"))c.required_count=1;if(!strcmp(mode,"unsupported")){c.required_count=1;c.available_count=2;}
 CHECK(gt_ext_session_open_resources(&h,GT_EXT_HOST,&c,m,4,f->h.s,!strcmp(mode,"quota")?3:1048576,&f->rpc.pair.h)==GT_OK && !h);CHECK(gt_ext_session_open_resources(&e,GT_EXT_EXECUTOR,&c,m,4,f->e.s,!strcmp(mode,"quota")?3:1048576,&f->rpc.pair.e)==GT_OK && !e);
 memset(m,0,sizeof(m));ext_allow(&f->rpc.pair);CHECK(gt_ext_session_lease(f->rpc.pair.h,&f->rpc.h) && gt_ext_session_lease(f->rpc.pair.e,&f->rpc.e));
}
static void res_drop(res_pair_t *f){rpc_drop(&f->rpc);res_store_drop(&f->h);res_store_drop(&f->e);}
static gt_ext_transfer_result_t res_result(res_pair_t *f,bool rev,uint64_t handle){gt_ext_transfer_result_t r;CHECK(gt_ext_session_resource_result(rev?f->rpc.pair.e:f->rpc.pair.h,rev?&f->rpc.e:&f->rpc.h,handle,&r)==GT_EXT_RESOURCE_OK);return r;}
static gt_ext_resource_snapshot_t res_send(res_pair_t *f,bool rev,size_t size,bool unknown,const char *failure){
 gt_ext_session_t *s=rev?f->rpc.pair.e:f->rpc.pair.h,*receiver=rev?f->rpc.pair.h:f->rpc.pair.e;gt_ext_lease_t *l=rev?&f->rpc.e:&f->rpc.h,*rl=rev?&f->rpc.h:&f->rpc.e;gt_ext_resource_descriptor_t d=res_descriptor(size,unknown);uint64_t handle=0;
 if(!strcmp(failure,"digest"))memset(d.sha256,'0',64);CHECK(gt_ext_session_send_resource(s,l,&d,&handle)==GT_EXT_RESOURCE_OK && handle);memset(&d,0,sizeof(d));
 size_t at=0,expected=!strcmp(failure,"length")?2:size;for(unsigned steps=0;at<expected;steps++){
  CHECK(steps<100000);uint8_t data[768];size_t n=expected-at;if(n>sizeof(data))n=sizeof(data);for(size_t i=0;i<n;i++)data[i]=size==3?(uint8_t)"abc"[at+i]:(uint8_t)((at+i)%251);size_t accepted=0;gt_ext_resource_status_t status=gt_ext_session_write_resource(s,l,handle,data,n,&accepted);CHECK(status==GT_EXT_RESOURCE_OK || status==GT_EXT_RESOURCE_WAIT);at+=accepted;memset(data,0,sizeof(data));rpc_pump(&f->rpc);
 }
 CHECK(gt_ext_session_finish_resource(s,l,handle,false)==GT_EXT_RESOURCE_INVALID);gt_ext_resource_status_t finish=gt_ext_session_finish_resource(s,l,handle,true);if(*failure){CHECK(finish==GT_EXT_RESOURCE_INVALID);gt_ext_transfer_result_t r=res_result(f,rev,handle);CHECK(!r.confirmed);CHECK(gt_ext_session_cancel_resource(s,l,handle)==GT_EXT_RESOURCE_OK);return (gt_ext_resource_snapshot_t){0};}CHECK(finish==GT_EXT_RESOURCE_OK);
 gt_ext_transfer_result_t r={0};for(unsigned steps=0;!r.confirmed;steps++){CHECK(steps<100000);rpc_pump(&f->rpc);r=res_result(f,rev,handle);}CHECK(r.state==GT_EXT_TRANSFER_SUCCEEDED && r.accepted==size);
 gt_ext_resource_snapshot_t snap;CHECK(gt_ext_session_take_resource(receiver,rl,&snap)==GT_EXT_RESOURCE_OK && snap.owner && snap.handle && snap.descriptor.size_known && snap.descriptor.size==size);CHECK(!strcmp(snap.descriptor.display_name,"../展示/\"snapshot\\.bin"));res_last_handle=handle;if(!res_keep_result)CHECK(gt_ext_session_forget_resource(s,l,handle)==GT_EXT_RESOURCE_OK);return snap;
}
static void res_verify(res_store_t *s,const gt_ext_lease_t *l,const gt_ext_resource_snapshot_t *snap,size_t size){
 gt_ext_resource_info_t i;CHECK(gt_ext_resource_store_info(s->s,l,snap,&i)==GT_EXT_RESOURCE_OK && i.published && i.received==size && i.reserved==size && !strcmp(i.descriptor.sha256,res_descriptor(size,false).sha256));uint8_t data[16384];size_t at=0;
 do{size_t n=999;CHECK(gt_ext_resource_store_read(s->s,l,snap,at,data,sizeof(data),&n)==GT_EXT_RESOURCE_OK && n<=sizeof(data));for(size_t j=0;j<n;j++)CHECK(data[j]==(size==3?(uint8_t)"abc"[at+j]:(uint8_t)((at+j)%251)));at+=n;if(!n)break;}while(at<=size);CHECK(at==size);
}
static void res_native(const char *mode){
 if(!strcmp(mode,"invalid")){
  gt_ext_resource_store_t *store=(void *)1;CHECK(gt_ext_resource_store_open(NULL,&store)==GT_EXT_RESOURCE_INVALID && !store);gt_ext_resource_usage_t u;memset(&u,1,sizeof(u));CHECK(!gt_ext_resource_store_usage(NULL,&u) && !u.sessions && !u.reserved_bytes);size_t n=1;CHECK(gt_ext_resource_store_read(NULL,NULL,NULL,0,NULL,0,&n)==GT_EXT_RESOURCE_INVALID && !n);CHECK(gt_ext_resource_store_retain(NULL,NULL,NULL)==GT_EXT_RESOURCE_INVALID && gt_ext_resource_store_release(NULL,NULL,NULL)==GT_EXT_RESOURCE_INVALID && gt_ext_resource_store_destroy(NULL)==GT_EXT_RESOURCE_INVALID);gt_ext_resource_info_t i;memset(&i,1,sizeof(i));CHECK(gt_ext_resource_store_info(NULL,NULL,NULL,&i)==GT_EXT_RESOURCE_INVALID && !i.published && !i.received);return;
 }
 if(!strcmp(mode,"constructor") || !strcmp(mode,"features") || !strcmp(mode,"store-pending")){
  res_store_t store=res_store(1048576);int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));gt_ext_channel_t *c=ext_channel(fd[0]);gt_ext_session_config_t cfg=ext_config();gt_ext_session_t *s=(void *)1;gt_ext_rpc_method_t m[4];rpc_methods(m);
  CHECK(gt_ext_session_open_resources(&c,GT_EXT_HOST,&cfg,NULL,0,store.s,1048576,&s)==GT_ERR_UNSUPPORTED && !s && c && !res_usage(&store).sessions);cfg.features=GT_EXT_FEATURE_RESOURCES;
  CHECK(gt_ext_session_open_resources(&c,GT_EXT_HOST,&cfg,m,4,store.s,1048576,&s)==GT_ERR_INVALID_ARG && !s && c);cfg.features|=4;CHECK(gt_ext_session_open_resources(&c,GT_EXT_HOST,&cfg,NULL,0,store.s,1048576,&s)==GT_ERR_UNSUPPORTED && !s && c);cfg.features=GT_EXT_FEATURE_RESOURCES;
  CHECK(gt_ext_session_open_resources(&c,GT_EXT_HOST,&cfg,NULL,0,store.s,1048577,&s)==GT_ERR_INVALID_ARG && !s && c);CHECK(gt_ext_session_open_resources(&c,GT_EXT_HOST,&cfg,NULL,0,store.s,0,&s)==GT_ERR_INVALID_ARG && !s && c);CHECK(gt_ext_session_open_resources(&c,GT_EXT_HOST,&cfg,NULL,0,store.s,1048576,&s)==GT_OK && !c && res_usage(&store).sessions==1);CHECK(gt_ext_resource_store_destroy(&store.s)==GT_EXT_RESOURCE_WAIT && store.s);gt_ext_lease_t l={0};uint64_t handle=7;CHECK(gt_ext_session_request(s,&l,"fixture.echo","{}",2,1000,&handle)==GT_EXT_RPC_UNSUPPORTED && !handle);CHECK(gt_ext_session_close(s,0)==GT_EXT_RELEASED);CHECK(gt_ext_resource_store_destroy(&store.s)==GT_EXT_RESOURCE_WAIT && store.s);CHECK(gt_ext_session_destroy(&s)==GT_OK);res_store_drop(&store);CHECK(!close(fd[1]));return;
 }
 res_pair_t f;res_open(&f,mode);res_keep_result=!strcmp(mode,"confirmed-stop");bool rev=!strcmp(mode,"reverse");gt_ext_session_t *s=rev?f.rpc.pair.e:f.rpc.pair.h;gt_ext_lease_t *l=rev?&f.rpc.e:&f.rpc.h;
 if(!strcmp(mode,"denied") || !strcmp(mode,"unsupported") || !strcmp(mode,"scope")){
  gt_ext_lease_t bad=*l;if(!strcmp(mode,"scope"))bad.epoch++;uint64_t handle=9;gt_ext_resource_descriptor_t d=res_descriptor(3,false);gt_ext_resource_status_t expected=!strcmp(mode,"unsupported")?GT_EXT_RESOURCE_UNSUPPORTED:GT_EXT_RESOURCE_DENIED;
  CHECK(gt_ext_session_send_resource(s,&bad,&d,&handle)==expected && !handle);gt_ext_resource_snapshot_t snap;memset(&snap,1,sizeof(snap));if(!strcmp(mode,"scope"))CHECK(gt_ext_session_take_resource(s,&bad,&snap)==GT_EXT_RESOURCE_DENIED && !snap.handle && !snap.token[0]);res_drop(&f);return;
 }
 if(!strcmp(mode,"queued-cancel") || !strcmp(mode,"lost") || !strcmp(mode,"backpressure")){
  gt_ext_resource_descriptor_t d=res_descriptor(262144,false);uint64_t handle=0;CHECK(gt_ext_session_send_resource(s,l,&d,&handle)==GT_EXT_RESOURCE_OK);uint64_t extra=99;CHECK(gt_ext_session_send_resource(s,l,&d,&extra)==GT_EXT_RESOURCE_WAIT && !extra);
  if(!strcmp(mode,"lost")){for(unsigned j=0;j<100;j++)rpc_pump(&f.rpc);gt_ext_session_stop(s);gt_ext_session_step(s,0);CHECK(res_result(&f,false,handle).state==GT_EXT_TRANSFER_UNKNOWN);}else{
   if(!strcmp(mode,"backpressure")){uint8_t bytes[768]={0};size_t n=0;unsigned writes=0;for(;writes<100;writes++){gt_ext_resource_status_t st=gt_ext_session_write_resource(s,l,handle,bytes,sizeof(bytes),&n);CHECK(st==GT_EXT_RESOURCE_OK || st==GT_EXT_RESOURCE_WAIT);if(st==GT_EXT_RESOURCE_WAIT){CHECK(!n);break;}}CHECK(writes<100);}
   CHECK(gt_ext_session_cancel_resource(s,l,handle)==GT_EXT_RESOURCE_OK);CHECK(res_result(&f,false,handle).state==GT_EXT_TRANSFER_CANCELLED && !res_result(&f,false,handle).confirmed);
  }res_drop(&f);return;
 }
 size_t size=!strcmp(mode,"large")?262144:!strcmp(mode,"empty")?0:3;gt_ext_resource_snapshot_t snap=res_send(&f,rev,size,!strcmp(mode,"unknown-size"),(!strcmp(mode,"digest") || !strcmp(mode,"length"))?mode:"");
 if(!strcmp(mode,"digest") || !strcmp(mode,"length")){res_drop(&f);return;}res_store_t *store=rev?&f.h:&f.e;gt_ext_lease_t *rl=rev?&f.rpc.h:&f.rpc.e;res_verify(store,rl,&snap,size);CHECK(res_usage(store).reserved_bytes==size);
 if(!strcmp(mode,"retain")){CHECK(gt_ext_resource_store_retain(store->s,rl,&snap)==GT_EXT_RESOURCE_OK);gt_ext_resource_snapshot_t second=snap;CHECK(gt_ext_resource_store_release(store->s,rl,&snap)==GT_EXT_RESOURCE_OK && !snap.handle && res_usage(store).resources==1);snap=second;gt_ext_resource_info_t i;CHECK(gt_ext_resource_store_info(store->s,rl,&snap,&i)==GT_EXT_RESOURCE_OK && i.references==1);}
 if(!strcmp(mode,"quota")){uint64_t handle=0;gt_ext_resource_descriptor_t d=res_descriptor(3,false);CHECK(gt_ext_session_send_resource(f.rpc.pair.h,&f.rpc.h,&d,&handle)==GT_EXT_RESOURCE_OK);for(unsigned k=0;k<200;k++)rpc_pump(&f.rpc);gt_ext_transfer_result_t r=res_result(&f,false,handle);CHECK(r.state==GT_EXT_TRANSFER_FAILED && r.confirmed);}
 if(res_keep_result){gt_ext_session_stop(s);gt_ext_session_step(s,0);gt_ext_transfer_result_t r=res_result(&f,rev,res_last_handle);CHECK(r.confirmed && r.state==GT_EXT_TRANSFER_SUCCEEDED && r.accepted==size);}
 gt_ext_resource_info_t saved;CHECK(gt_ext_resource_store_info(store->s,rl,&snap,&saved)==GT_EXT_RESOURCE_OK);
 gt_ext_lease_t bad=*rl;bad.token[0]^=1;gt_ext_resource_info_t i;memset(&i,1,sizeof(i));CHECK(gt_ext_resource_store_info(store->s,&bad,&snap,&i)==GT_EXT_RESOURCE_DENIED && !i.published && !i.descriptor.sha256[0]);size_t n=99;char b;CHECK(gt_ext_resource_store_read(store->s,&bad,&snap,0,&b,1,&n)==GT_EXT_RESOURCE_DENIED && !n);gt_ext_resource_snapshot_t forged=snap;forged.token[0]^=1;CHECK(gt_ext_resource_store_release(store->s,rl,&forged)==GT_EXT_RESOURCE_NOT_FOUND && !forged.handle && res_usage(store).resources==1);
 if(!strcmp(mode,"after-close") || !strcmp(mode,"copy") || !strcmp(mode,"confirmed-stop")){rpc_drop(&f.rpc);CHECK(gt_ext_resource_store_destroy(&store->s)==GT_EXT_RESOURCE_WAIT && store->s);res_verify(store,rl,&snap,size);CHECK(saved.descriptor.size==size && saved.published);}
 CHECK(gt_ext_resource_store_release(store->s,rl,&snap)==GT_EXT_RESOURCE_OK && !snap.handle && !snap.token[0]);if(f.rpc.pair.h)rpc_drop(&f.rpc);res_store_drop(&f.h);res_store_drop(&f.e);
}
/* The SSH fixture uses public constructors and independently verifies bytes on
 * both peers. RPC done cannot be sent/answered before resource confirmation. */
static res_store_t network_store;
static uint64_t network_handle;
static size_t network_written,network_read;
static bool network_finish,network_received,network_recorded;
static gt_ext_resource_snapshot_t network_snapshot;
static gt_ext_session_t *res_network_adopt(gt_ext_channel_t **c,gt_ext_role_t role,gt_ext_session_config_t *cfg,const gt_ext_rpc_method_t *m,size_t count){
 network_store=res_store(1048576);cfg->features|=GT_EXT_FEATURE_RESOURCES;cfg->required_count=2;cfg->available_count=3;strcpy(cfg->required[1],"resource.receive");strcpy(cfg->available[2],"resource.receive");gt_ext_session_t *s=NULL;CHECK(gt_ext_session_open_resources(c,role,cfg,m,count,network_store.s,1048576,&s)==GT_OK && !*c);return s;
}
static bool res_network_progress(gt_ext_session_t *s,const gt_ext_lease_t *l){
 if(!network_handle){gt_ext_resource_descriptor_t d=res_descriptor(262144,false);CHECK(gt_ext_session_send_resource(s,l,&d,&network_handle)==GT_EXT_RESOURCE_OK && network_handle);}
 if(network_written<262144){uint8_t data[768];size_t n=262144-network_written;if(n>sizeof(data))n=sizeof(data);for(size_t i=0;i<n;i++)data[i]=(uint8_t)((network_written+i)%251);size_t accepted=0;gt_ext_resource_status_t status=gt_ext_session_write_resource(s,l,network_handle,data,n,&accepted);CHECK(status==GT_EXT_RESOURCE_OK || status==GT_EXT_RESOURCE_WAIT);network_written+=accepted;}
 if(network_written==262144 && !network_finish){CHECK(gt_ext_session_finish_resource(s,l,network_handle,true)==GT_EXT_RESOURCE_OK);network_finish=true;}
 if(!network_snapshot.handle && !network_received){gt_ext_resource_status_t status=gt_ext_session_take_resource(s,l,&network_snapshot);CHECK(status==GT_EXT_RESOURCE_OK || status==GT_EXT_RESOURCE_WAIT);}
 if(network_snapshot.handle){uint8_t data[16384];size_t n=0;CHECK(gt_ext_resource_store_read(network_store.s,l,&network_snapshot,network_read,data,sizeof(data),&n)==GT_EXT_RESOURCE_OK);for(size_t i=0;i<n;i++)CHECK(data[i]==(uint8_t)((network_read+i)%251));network_read+=n;if(!n){CHECK(network_read==262144);CHECK(gt_ext_resource_store_release(network_store.s,l,&network_snapshot)==GT_EXT_RESOURCE_OK);network_received=true;}}
 gt_ext_transfer_result_t r;CHECK(gt_ext_session_resource_result(s,l,network_handle,&r)==GT_EXT_RESOURCE_OK);bool done=network_received && r.confirmed && r.state==GT_EXT_TRANSFER_SUCCEEDED;
 if(done && !network_recorded){char path[4096];CHECK(snprintf(path,sizeof(path),"%s/resource-proof-XXXXXX",res_root)>0);int fd=mkstemp(path);CHECK(fd>=0);const char *proof="{\"received\":262144,\"confirmed\":true}\n";CHECK(write(fd,proof,strlen(proof))==(ssize_t)strlen(proof) && !close(fd));network_recorded=true;}return done;
}
#ifndef EXT_RESOURCE_MAIN
#define EXT_RESOURCE_MAIN main
#endif
int EXT_RESOURCE_MAIN(int argc,char **argv){
 CHECK(argc>=2);res_root=argc==3?argv[2]:getenv("TMPDIR");CHECK(res_root && res_root[0]);size_t before=ext_fds();
 if(!strcmp(argv[1],"--executor")){rpc_adopt_hook=res_network_adopt;rpc_progress_hook=res_network_progress;rpc_executor(argv[2]);res_store_drop(&network_store);}
#ifdef GT_HAVE_LIBSSH2
 else if(argc>3){rpc_adopt_hook=res_network_adopt;rpc_progress_hook=res_network_progress;rpc_network(argc,argv);res_store_drop(&network_store);}
#endif
 else {res_native(argv[1]);printf("PASS ext_resource_%s\n",argv[1]);}
 CHECK(ext_fds()+(!strcmp(argv[1],"--executor")?2u:0u)==before);return 0;
}
