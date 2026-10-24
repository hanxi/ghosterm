/* Same real framed socket fixture as the shared coordinator's existing tests. */
#define main coordinator_baseline_main
#include "test_session_client.c"
#undef main
static void pending_lookup(void){
 for(unsigned unknown=0;unknown<2;unknown++){
  fixture f;setup(&f,false);f.config.create=NULL;f.config.pending_nonce[0]=9;f.config.instance[0]=1;open_client(&f);f.config.pending_nonce[0]=17;
  CHECK(info(&f).has_admitted_create);hello(&f,0,1);creation(&f,true,unknown?GT_CREATION_UNKNOWN:GT_CREATION_ACCEPTED);
  if(unknown)CHECK(info(&f).phase==GT_SESSION_FAILED && info(&f).error==GT_ERR_NOT_FOUND && f.streams==1);
  else{attach(&f,false);data_hello(&f);live(&f,false);CHECK(info(&f).session==5 && f.starts==1);}
  finish(&f);
 }
}
static void admitted_before_wire(void){
 fixture f;setup(&f,false);open_client(&f);CHECK(!info(&f).has_admitted_create);
 gt_wire_frame_t r=frame(&f,0);CHECK(r.header.type==GT_MSG_HELLO);CHECK(gt__connection_release_frame(f.peer[0]));f.a[0].quota=0;
 uint8_t p[80]={0};p[5]=GT_ROLE_MANAGEMENT;p[8]=2;p[48]=1;p[64]=9;peer_send(&f,0,GT_MSG_HELLO_REPLY,1,p,80);
 f.a[0].quota=1;r=frame(&f,0);CHECK(r.header.type==GT_MSG_HELLO_ACK);CHECK(gt__connection_release_frame(f.peer[0]));f.a[0].quota=0;pump(&f,3);
 CHECK(info(&f).has_admitted_create && info(&f).instance[0]==1 && !info(&f).session);
 CHECK(!gt__connection_frame(f.peer[0],&r));f.a[0].quota=SIZE_MAX;(void)request(&f,GT_MSG_CREATE);finish(&f);
}
static void list_reply(fixture *f,size_t count){
 uint64_t id=request(f,GT_MSG_LIST);uint8_t p[8+2*GT_CONTROL_INFO_BYTES]={0};CHECK(count<=2);put(p+4,count,4);
 for(size_t i=0;i<count;i++){gt_managed_info_t row={.id=5+i,.phase=GT_MANAGED_WORKER};CHECK(gt__control_info_encode(&row,p+8+i*GT_CONTROL_INFO_BYTES));}
 peer_send(f,0,GT_MSG_LIST_REPLY,id,p,8+count*GT_CONTROL_INFO_BYTES);
}
static void catalog_open(fixture *f){setup(f,true);f->config.session=0;f->config.catalog_only=true;f->config.attachment=(gt_client_config_t){0};open_client(f);hello(f,0,1);}
static void catalog(void){
 fixture f;catalog_open(&f);gt_managed_info_t rows[2];memset(rows,0x5a,sizeof(rows));size_t count=99;
 CHECK(gt__session_client_catalog(f.client,rows,2,&count)==GT_ERR_NOT_FOUND && !count);list_reply(&f,2);
 CHECK(info(&f).phase==GT_SESSION_CATALOG && f.streams==1 && !f.snapshots && !info(&f).has_admitted_create);
 CHECK(gt__session_client_catalog(f.client,rows,1,&count)==GT_ERR_INVALID_ARG && count==2 && rows[0].id==UINT64_C(0x5a5a5a5a5a5a5a5a));
 CHECK(gt__session_client_catalog(f.client,rows,2,&count)==GT_OK && count==2 && rows[0].id==5 && rows[1].id==6);
 gt_user_request_t r=input();gt_worker_stamp_t stamp;CHECK(gt__session_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED);
 CHECK(gt__session_client_catalog_refresh(f.client)==GT_CLIENT_QUEUED);CHECK(gt__session_client_catalog(f.client,rows,2,&count)==GT_ERR_NOT_FOUND && !count);
 list_reply(&f,0);CHECK(gt__session_client_catalog(f.client,NULL,0,&count)==GT_OK && !count && f.streams==1);
 f.now=20;uint64_t id=request(&f,GT_MSG_PING);peer_send(&f,0,GT_MSG_PING_REPLY,id,NULL,0);CHECK(info(&f).phase==GT_SESSION_CATALOG);
 f.now=1000;CHECK(gt__session_client_catalog_refresh(f.client)==GT_CLIENT_QUEUED);id=request(&f,GT_MSG_PING);peer_send(&f,0,GT_MSG_PING_REPLY,id,NULL,0);list_reply(&f,0);CHECK(info(&f).phase==GT_SESSION_CATALOG);finish(&f);
}
static void catalog_network(void){
 fixture f;catalog_open(&f);list_reply(&f,1);f.link=GT_SESSION_LINK_RETRY;f.link_error=GT_ERR_IO;pump(&f,1);
 gt_managed_info_t row;size_t count=99;CHECK(gt__session_client_catalog(f.client,&row,1,&count)==GT_ERR_NOT_FOUND && !count);
 f.link=GT_SESSION_LINK_READY;f.link_error=GT_OK;f.now=10;hello(&f,0,1);list_reply(&f,0);CHECK(f.streams==2 && info(&f).phase==GT_SESSION_CATALOG && !f.snapshots);finish(&f);
}
static void termination_reply(fixture *f,gt_control_status_t status){
 uint64_t id=request(f,GT_MSG_TERMINATE);uint8_t p[12]={0};put(p,status,4);put(p+4,5,8);peer_send(f,0,GT_MSG_TERMINATE_REPLY,id,p,sizeof(p));
}
static void terminate_known(void){
 for(unsigned status=0;status<=GT_CONTROL_INTERNAL;status++){
  fixture f;setup(&f,true);open_client(&f);connect_live(&f,true,false);CHECK(gt__session_client_terminate(f.client,5)==GT_CLIENT_QUEUED);
  CHECK(info(&f).stop.state==GT_CLIENT_STOP_PENDING && info(&f).stop.session==5 && info(&f).stop.request && info(&f).input_locked);
  gt_worker_stamp_t stamp;gt_user_request_t r=input();CHECK(gt__session_client_submit(f.client,&r,&stamp)==GT_CLIENT_LOCKED && !stamp.command_id);
  CHECK(gt__session_client_terminate(f.client,5)==GT_CLIENT_BACKPRESSURE);termination_reply(&f,(gt_control_status_t)status);
  CHECK(info(&f).stop.state==GT_CLIENT_STOP_KNOWN && info(&f).stop.status==status && info(&f).input_locked==(status==GT_CONTROL_OK));
  CHECK(info(&f).phase==GT_SESSION_LIVE && !info(&f).attachment.has_terminal);finish(&f);
 }
}
static void terminate_unknown(void){
 for(unsigned cancel_now=0;cancel_now<3;cancel_now++){
  fixture f;setup(&f,true);open_client(&f);connect_live(&f,true,false);CHECK(gt__session_client_terminate(f.client,5)==GT_CLIENT_QUEUED);(void)request(&f,GT_MSG_TERMINATE);
  if(cancel_now==1){gt__session_client_cancel(f.client);CHECK(info(&f).phase==GT_SESSION_CLOSED && info(&f).stop.state==GT_CLIENT_STOP_UNKNOWN && info(&f).stop.error==GT_ERR_CANCELLED);}
  else{
   if(cancel_now==2){gt__connection_destroy(f.peer[0]);f.peer[0]=NULL;}else{f.link=GT_SESSION_LINK_RETRY;f.link_error=GT_ERR_IO;}
   pump(&f,1);CHECK(info(&f).phase==GT_SESSION_RECOVERING && info(&f).stop.state==GT_CLIENT_STOP_UNKNOWN && info(&f).stop.error==GT_ERR_IO);
   f.link=GT_SESSION_LINK_READY;f.link_error=GT_OK;f.now=10;f.generation=8;connect_live(&f,true,false);CHECK(info(&f).stop.state==GT_CLIENT_STOP_UNKNOWN && info(&f).input_locked);
   gt_wire_frame_t r;pump(&f,5);CHECK(!gt__connection_frame(f.peer[0],&r));
  }
  finish(&f);
 }
}
static void management_validation(void){
 fixture f;setup(&f,false);gt_allocator_t a={&f,admit,refund};f.config.catalog_only=true;CHECK(gt__session_client_open(&f.config,&a,&f.client)==GT_ERR_INVALID_ARG && !f.client);
 f.config.catalog_only=false;f.config.create=NULL;f.config.pending_nonce[0]=9;CHECK(gt__session_client_open(&f.config,&a,&f.client)==GT_ERR_INVALID_ARG && !f.client);
 f.config.instance[0]=1;open_client(&f);CHECK(gt__session_client_terminate(f.client,5)==GT_CLIENT_LOCKED && gt__session_client_terminate(f.client,0)==GT_CLIENT_INVALID);
 CHECK(gt__session_client_catalog_refresh(f.client)==GT_CLIENT_LOCKED);size_t count=9;CHECK(gt__session_client_catalog(f.client,NULL,0,&count)==GT_ERR_UNSUPPORTED && !count);finish(&f);
}
int main(int argc,char **argv){CHECK(argc==2);if(!strcmp(argv[1],"pending"))pending_lookup();else if(!strcmp(argv[1],"admission"))admitted_before_wire();else if(!strcmp(argv[1],"catalog"))catalog();else if(!strcmp(argv[1],"catalog-network"))catalog_network();else if(!strcmp(argv[1],"stop"))terminate_known();else if(!strcmp(argv[1],"stop-unknown"))terminate_unknown();else{CHECK(!strcmp(argv[1],"validation"));management_validation();}printf("PASS session management %s\n",argv[1]);return 0;}
