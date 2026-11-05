/* Public header/API contract and managed SSH fixture; no private source fields. */
#define EXT_RESOURCE_MAIN ext_source_resource_fixture_main
#include "test_ext_resource.c"
#undef EXT_RESOURCE_MAIN
typedef struct {res_pair_t pair;char directory[4096],file[4096];int root;} pub_source_fixture_t;
static void pub_pair(res_pair_t *f,const char *mode){
 memset(f,0,sizeof(*f));f->h=res_store(1048576);f->e=res_store(1048576);int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));gt_ext_channel_t *h=ext_channel(fd[0]),*e=ext_channel(fd[1]);
 gt_ext_session_config_t c=ext_config();c.features=GT_EXT_FEATURE_REQUESTS|GT_EXT_FEATURE_RESOURCES;c.required_count=3;c.available_count=4;strcpy(c.required[1],"resource.receive");strcpy(c.required[2],"file.readScoped");strcpy(c.available[2],"resource.receive");strcpy(c.available[3],"file.readScoped");gt_ext_rpc_method_t m[4];rpc_methods(m);
 if(!strcmp(mode,"ungranted"))c.required_count=2;if(!strcmp(mode,"unsupported")){c.required_count=2;c.available_count=3;}
 if(!strcmp(mode,"feature")){c.features=GT_EXT_FEATURE_REQUESTS;CHECK(gt_ext_session_open_rpc(&h,GT_EXT_HOST,&c,m,4,&f->rpc.pair.h)==GT_OK);CHECK(gt_ext_session_open_rpc(&e,GT_EXT_EXECUTOR,&c,m,4,&f->rpc.pair.e)==GT_OK);}
 else{CHECK(gt_ext_session_open_resources(&h,GT_EXT_HOST,&c,m,4,f->h.s,1048576,&f->rpc.pair.h)==GT_OK);CHECK(gt_ext_session_open_resources(&e,GT_EXT_EXECUTOR,&c,m,4,f->e.s,1048576,&f->rpc.pair.e)==GT_OK);}
 memset(m,0,sizeof(m));ext_allow(&f->rpc.pair);CHECK(gt_ext_session_lease(f->rpc.pair.h,&f->rpc.h) && gt_ext_session_lease(f->rpc.pair.e,&f->rpc.e));
}
static void pub_file(pub_source_fixture_t *f,size_t size,const char *name){
 CHECK(snprintf(f->directory,sizeof(f->directory),"%s/public-source-XXXXXX",res_root)>0 && mkdtemp(f->directory));CHECK(snprintf(f->file,sizeof(f->file),"%s/%s",f->directory,name)>0);
 f->root=open(f->directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(f->root>=0);int fd=open(f->file,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(fd>=0);uint8_t bytes[16384];
 for(size_t at=0;at<size;){size_t n=size-at;if(n>sizeof(bytes))n=sizeof(bytes);for(size_t i=0;i<n;i++)bytes[i]=size==3?(uint8_t)"abc"[at+i]:(uint8_t)((at+i)%251);CHECK(write(fd,bytes,n)==(ssize_t)n);at+=n;}CHECK(!close(fd));
}
static gt_ext_source_info_t pub_info(gt_ext_source_t *s){gt_ext_source_info_t i;CHECK(gt_ext_source_info(s,&i));return i;}
static void pub_drop(gt_ext_source_t **s){for(unsigned i=0;i<100;i++){gt_ext_source_status_t status=gt_ext_source_destroy(s);CHECK(status==GT_EXT_SOURCE_OK || status==GT_EXT_SOURCE_WAIT);if(status==GT_EXT_SOURCE_OK){CHECK(!*s);return;}}CHECK(false);}
static void pub_verify(res_store_t *store,const gt_ext_lease_t *lease,gt_ext_resource_snapshot_t *snap,size_t size,const char *name){
 gt_ext_resource_info_t info;CHECK(gt_ext_resource_store_info(store->s,lease,snap,&info)==GT_EXT_RESOURCE_OK && info.published && info.received==size && info.reserved==size && !strcmp(info.descriptor.display_name,name) && !strcmp(info.descriptor.sha256,res_descriptor(size,false).sha256));
 uint8_t bytes[16384];size_t at=0;while(true){size_t n=99;CHECK(gt_ext_resource_store_read(store->s,lease,snap,at,bytes,sizeof(bytes),&n)==GT_EXT_RESOURCE_OK);if(!n)break;for(size_t j=0;j<n;j++)CHECK(bytes[j]==(size==3?(uint8_t)"abc"[at+j]:(uint8_t)((at+j)%251)));at+=n;}CHECK(at==size);
}
static gt_ext_source_t *pub_open(pub_source_fixture_t *f,const char *name,uint64_t limit){gt_ext_source_t *s=NULL;gt_ext_source_config_t c={f->root,name,"application/octet-stream",limit};CHECK(gt_ext_source_open(f->pair.rpc.pair.h,&f->pair.rpc.h,&c,&s)==GT_EXT_SOURCE_OK && s);return s;}
static void pub_ready(gt_ext_source_t *s){for(unsigned i=0;i<100;i++){gt_ext_source_phase_t phase=gt_ext_source_step(s);CHECK(phase==GT_EXT_SOURCE_HASHING || phase==GT_EXT_SOURCE_READY);if(phase==GT_EXT_SOURCE_READY)return;}CHECK(false);}
static void pub_fixture_drop(pub_source_fixture_t *f){if(f->root>=0)CHECK(!close(f->root));CHECK(!unlink(f->file) && !rmdir(f->directory));res_drop(&f->pair);}
static void pub_native(const char *mode){
 if(!strcmp(mode,"invalid")){
  gt_ext_source_t *s=(void *)1;gt_ext_source_config_t c={-1,NULL,NULL,0};CHECK(gt_ext_source_open(NULL,NULL,&c,&s)==GT_EXT_SOURCE_INVALID && !s);gt_ext_source_info_t i;memset(&i,1,sizeof(i));CHECK(!gt_ext_source_info(NULL,&i) && !i.has_transfer && !i.accepted && !i.descriptor.sha256[0]);CHECK(gt_ext_source_step(NULL)==GT_EXT_SOURCE_FAILED && gt_ext_source_send(NULL)==GT_EXT_SOURCE_INVALID && gt_ext_source_close(NULL)==GT_EXT_SOURCE_INVALID && gt_ext_source_destroy(NULL)==GT_EXT_SOURCE_INVALID);gt_ext_source_cancel(NULL);return;
 }
 size_t size=!strcmp(mode,"large") || !strcmp(mode,"cancel-sent") || !strcmp(mode,"lost")?262144:!strcmp(mode,"empty")?0:3;const char *name=!strcmp(mode,"literal")?"$HOME;$(touch no).bin":"file.bin";
 pub_source_fixture_t f={.root=-1};pub_pair(&f.pair,mode);pub_file(&f,size,name);gt_ext_source_t *s=NULL;gt_ext_source_config_t c={f.root,name,"application/octet-stream",262144};gt_ext_lease_t lease=f.pair.rpc.h;
 gt_ext_source_status_t rejected=GT_EXT_SOURCE_OK;
 if(!strcmp(mode,"scope")){lease.epoch++;rejected=GT_EXT_SOURCE_DENIED;}else if(!strcmp(mode,"feature") || !strcmp(mode,"unsupported"))rejected=GT_EXT_SOURCE_UNSUPPORTED;else if(!strcmp(mode,"ungranted"))rejected=GT_EXT_SOURCE_UNSUPPORTED;
 else if(!strcmp(mode,"parent")){c.relative_path="../file.bin";rejected=GT_EXT_SOURCE_DENIED;}else if(!strcmp(mode,"absolute")){c.relative_path=f.file;rejected=GT_EXT_SOURCE_INVALID;}else if(!strcmp(mode,"missing")){c.relative_path="missing";rejected=GT_EXT_SOURCE_NOT_FOUND;}
 else if(!strcmp(mode,"utf8")){c.relative_path="\xff";rejected=GT_EXT_SOURCE_UNSUPPORTED;}else if(!strcmp(mode,"control")){c.relative_path="file\n.bin";rejected=GT_EXT_SOURCE_UNSUPPORTED;}else if(!strcmp(mode,"mime")){c.mime="BAD MIME";rejected=GT_EXT_SOURCE_INVALID;}else if(!strcmp(mode,"limit")){c.byte_limit=2;rejected=GT_EXT_SOURCE_LIMIT_REACHED;}
 else if(!strcmp(mode,"symlink") || !strcmp(mode,"fifo")){CHECK(!unlink(f.file));if(!strcmp(mode,"symlink"))CHECK(!symlink("unread-target",f.file));else CHECK(!mkfifo(f.file,0600));rejected=GT_EXT_SOURCE_UNSUPPORTED;}
 if(rejected!=GT_EXT_SOURCE_OK){CHECK(gt_ext_source_open(f.pair.rpc.pair.h,&lease,&c,&s)==rejected && !s);CHECK(!res_usage(&f.pair.e).resources);pub_fixture_drop(&f);return;}
 if(!strcmp(mode,"capacity")){
  gt_ext_source_t *all[GT_EXT_SOURCE_LIMIT];for(unsigned i=0;i<GT_EXT_SOURCE_LIMIT;i++)all[i]=pub_open(&f,name,262144);
  s=(void *)1;CHECK(gt_ext_source_open(f.pair.rpc.pair.h,&lease,&c,&s)==GT_EXT_SOURCE_LIMIT_REACHED && !s);pub_drop(&all[0]);s=pub_open(&f,name,262144);pub_drop(&s);for(unsigned i=1;i<GT_EXT_SOURCE_LIMIT;i++)pub_drop(&all[i]);pub_fixture_drop(&f);return;
 }
 char copied_path[64]="file.bin",copied_mime[64]="application/octet-stream";
 if(!strcmp(mode,"copy")){c.relative_path=copied_path;c.mime=copied_mime;}CHECK(gt_ext_source_open(f.pair.rpc.pair.h,&lease,&c,&s)==GT_EXT_SOURCE_OK && s);memset(copied_path,0,sizeof(copied_path));memset(copied_mime,0,sizeof(copied_mime));CHECK(!close(f.root));f.root=-1;
 CHECK(pub_info(s).phase==GT_EXT_SOURCE_HASHING && !pub_info(s).confirmed && !pub_info(s).has_transfer && pub_info(s).hashed==0);
 if(!strcmp(mode,"session-pin")){
  CHECK(gt_ext_session_close(f.pair.rpc.pair.h,0)==GT_EXT_DRAINING && gt_ext_session_destroy(&f.pair.rpc.pair.h)==GT_ERR_INVALID_ARG && f.pair.rpc.pair.h);CHECK(gt_ext_source_close(s)==GT_EXT_SOURCE_OK);pub_drop(&s);pub_fixture_drop(&f);return;
 }
 if(!strcmp(mode,"cancel-before")){gt_ext_source_cancel(s);CHECK(pub_info(s).phase==GT_EXT_SOURCE_STOPPED && pub_info(s).error==GT_EXT_SOURCE_CANCELLED && !pub_info(s).has_transfer);pub_drop(&s);pub_fixture_drop(&f);return;}
 CHECK(gt_ext_source_send(s)==GT_EXT_SOURCE_WAIT);pub_ready(s);CHECK(pub_info(s).hashed==size && !strcmp(pub_info(s).descriptor.sha256,res_descriptor(size,false).sha256));
 if(!strcmp(mode,"change-ready")){int fd=open(f.file,O_WRONLY|O_CLOEXEC);CHECK(fd>=0 && pwrite(fd,"Z",1,0)==1 && !close(fd));CHECK(gt_ext_source_send(s)==GT_EXT_SOURCE_CHANGED && pub_info(s).phase==GT_EXT_SOURCE_FAILED && !pub_info(s).has_transfer);pub_drop(&s);pub_fixture_drop(&f);return;}
 CHECK(gt_ext_source_send(s)==GT_EXT_SOURCE_OK);
 if(!strcmp(mode,"cancel-sent") || !strcmp(mode,"lost")){
  for(unsigned j=0;j<40;j++){gt_ext_source_step(s);rpc_pump(&f.pair.rpc);}if(!strcmp(mode,"lost")){gt_ext_session_stop(f.pair.rpc.pair.h);gt_ext_session_step(f.pair.rpc.pair.h,0);gt_ext_source_step(s);CHECK(pub_info(s).phase==GT_EXT_SOURCE_UNCERTAIN && pub_info(s).error==GT_EXT_SOURCE_UNKNOWN && pub_info(s).transfer_state==GT_EXT_TRANSFER_UNKNOWN && !pub_info(s).confirmed);}
  else{gt_ext_source_cancel(s);for(unsigned j=0;j<2000 && pub_info(s).phase==GT_EXT_SOURCE_CANCELLING;j++){rpc_pump(&f.pair.rpc);gt_ext_source_step(s);}CHECK(pub_info(s).phase==GT_EXT_SOURCE_STOPPED && pub_info(s).error==GT_EXT_SOURCE_CANCELLED);}
  pub_drop(&s);pub_fixture_drop(&f);return;
 }
 uint64_t rpc=rpc_start(&f.pair.rpc,false,"{\"text\":\"concurrent\"}",10000);for(unsigned step=0;pub_info(s).phase!=GT_EXT_SOURCE_SUCCEEDED;step++){CHECK(step<100000);gt_ext_source_step(s);rpc_pump(&f.pair.rpc);}
 gt_ext_source_info_t saved=pub_info(s);CHECK(saved.confirmed && saved.transfer_state==GT_EXT_TRANSFER_SUCCEEDED && saved.has_transfer && saved.read_bytes==size && saved.accepted==size && saved.transfer_accepted==size);
 gt_ext_rpc_request_t request=rpc_take(&f.pair.rpc,false);rpc_reply(f.pair.rpc.pair.e,&f.pair.rpc.e,&request);CHECK(rpc_complete(&f.pair.rpc,false,rpc).confirmed);
 gt_ext_resource_snapshot_t snap;CHECK(gt_ext_session_take_resource(f.pair.rpc.pair.e,&f.pair.rpc.e,&snap)==GT_EXT_RESOURCE_OK);pub_verify(&f.pair.e,&f.pair.rpc.e,&snap,size,name);
 if(!strcmp(mode,"confirmed-stop")){gt_ext_session_stop(f.pair.rpc.pair.h);gt_ext_session_step(f.pair.rpc.pair.h,0);gt_ext_source_step(s);CHECK(pub_info(s).confirmed && pub_info(s).phase==GT_EXT_SOURCE_SUCCEEDED);}
 pub_drop(&s);CHECK(saved.descriptor.size==size && saved.confirmed);CHECK(gt_ext_resource_store_release(f.pair.e.s,&f.pair.rpc.e,&snap)==GT_EXT_RESOURCE_OK);pub_fixture_drop(&f);
}
static gt_ext_source_t *pub_network_source;
static pub_source_fixture_t pub_network_file;
static bool pub_network_source_done;
static gt_ext_session_t *pub_network_adopt(gt_ext_channel_t **c,gt_ext_role_t role,gt_ext_session_config_t *cfg,const gt_ext_rpc_method_t *m,size_t count){
 network_store=res_store(1048576);cfg->features|=GT_EXT_FEATURE_RESOURCES;cfg->required_count=3;cfg->available_count=4;strcpy(cfg->required[1],"resource.receive");strcpy(cfg->required[2],"file.readScoped");strcpy(cfg->available[2],"resource.receive");strcpy(cfg->available[3],"file.readScoped");gt_ext_session_t *s=NULL;CHECK(gt_ext_session_open_resources(c,role,cfg,m,count,network_store.s,1048576,&s)==GT_OK && !*c);return s;
}
static bool pub_network_progress(gt_ext_session_t *s,const gt_ext_lease_t *l){
 if(!pub_network_source && !pub_network_source_done){pub_file(&pub_network_file,262144,"file.bin");gt_ext_source_config_t c={pub_network_file.root,"file.bin","application/octet-stream",262144};CHECK(gt_ext_source_open(s,l,&c,&pub_network_source)==GT_EXT_SOURCE_OK);CHECK(!close(pub_network_file.root));pub_network_file.root=-1;}
 if(pub_network_source){gt_ext_source_phase_t phase=gt_ext_source_step(pub_network_source);if(phase==GT_EXT_SOURCE_READY)CHECK(gt_ext_source_send(pub_network_source)==GT_EXT_SOURCE_OK);else CHECK(phase==GT_EXT_SOURCE_HASHING || phase==GT_EXT_SOURCE_SENDING || phase==GT_EXT_SOURCE_RECEIPT || phase==GT_EXT_SOURCE_SUCCEEDED);
  if(phase==GT_EXT_SOURCE_SUCCEEDED){gt_ext_source_info_t i=pub_info(pub_network_source);CHECK(i.confirmed && i.has_transfer && i.hashed==262144 && i.read_bytes==262144 && i.accepted==262144 && i.transfer_state==GT_EXT_TRANSFER_SUCCEEDED);pub_drop(&pub_network_source);CHECK(!unlink(pub_network_file.file) && !rmdir(pub_network_file.directory));pub_network_source_done=true;char path[4096];CHECK(snprintf(path,sizeof(path),"%s/source-proof-XXXXXX",res_root)>0);int fd=mkstemp(path);const char *proof="{\"hashed\":262144,\"read\":262144,\"accepted\":262144,\"confirmed\":true}\n";CHECK(fd>=0 && write(fd,proof,strlen(proof))==(ssize_t)strlen(proof) && !close(fd));}}
 if(!network_snapshot.handle && !network_received){gt_ext_resource_status_t status=gt_ext_session_take_resource(s,l,&network_snapshot);CHECK(status==GT_EXT_RESOURCE_OK || status==GT_EXT_RESOURCE_WAIT);}
 if(network_snapshot.handle){pub_verify(&network_store,l,&network_snapshot,262144,"file.bin");CHECK(gt_ext_resource_store_release(network_store.s,l,&network_snapshot)==GT_EXT_RESOURCE_OK);network_received=true;}
 bool done=pub_network_source_done && network_received;if(done && !network_recorded){char path[4096];CHECK(snprintf(path,sizeof(path),"%s/resource-proof-XXXXXX",res_root)>0);int fd=mkstemp(path);const char *proof="{\"received\":262144,\"confirmed\":true}\n";CHECK(fd>=0 && write(fd,proof,strlen(proof))==(ssize_t)strlen(proof) && !close(fd));network_recorded=true;}return done;
}
#ifndef EXT_SOURCE_MAIN
#define EXT_SOURCE_MAIN main
#endif
int EXT_SOURCE_MAIN(int argc,char **argv){
 CHECK(argc>=2);res_root=argc==3?argv[2]:getenv("TMPDIR");CHECK(res_root);size_t before=ext_fds();
 if(!strcmp(argv[1],"--executor")){rpc_adopt_hook=pub_network_adopt;rpc_progress_hook=pub_network_progress;rpc_executor(argv[2]);res_store_drop(&network_store);}
#ifdef GT_HAVE_LIBSSH2
 else if(argc>3){rpc_adopt_hook=pub_network_adopt;rpc_progress_hook=pub_network_progress;rpc_network(argc,argv);res_store_drop(&network_store);}
#endif
 else{pub_native(argv[1]);printf("PASS ext_source_%s\n",argv[1]);}
 CHECK(ext_fds()+(!strcmp(argv[1],"--executor")?2u:0u)==before);return 0;
}
