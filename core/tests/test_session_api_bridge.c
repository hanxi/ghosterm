/* The public/private translation boundary, independent from the real protocol
 * tests. Only coordinator is replaced; both real providers still construct. */
#include "ghosterm.h"
#include "ipc/session-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL session ABI bridge:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static gt_session_client_config_t config;static gt_session_client_info_t source;
static gt_session_t *session;static unsigned calls,closed,cancelled;
static uint64_t encoded_generation,encoded_sequence;static gt_user_request_t request;
static int marker;static bool inside;
gt_status_t gt__session_client_open(const gt_session_client_config_t *c,const gt_allocator_t *a,gt_session_client_t **out){(void)a;config=*c;*out=(void *)&marker;return GT_OK;}
bool gt__session_client_info(const gt_session_client_t *c,gt_session_client_info_t *out){CHECK(c==(void *)&marker);*out=source;return true;}
gt_session_phase_t gt__session_client_step(gt_session_client_t *c){CHECK(c==(void *)&marker);if(!inside){inside=true;CHECK(gt_session_step(session)==GT_SESSION_STATE_FAILED);CHECK(!gt_session_close(session));CHECK(gt_session_terminate(session,5)==GT_SESSION_SUBMIT_INVALID && gt_session_catalog_refresh(session)==GT_SESSION_SUBMIT_INVALID);size_t n=99;CHECK(gt_session_catalog(session,NULL,0,&n)==GT_ERR_INVALID_ARG && !n);gt_session_stamp_t stamp;CHECK(gt_session_input(session,41,43,"x",1,&stamp)==GT_SESSION_SUBMIT_INVALID);gt_session_info_t i;CHECK(!gt_session_info(session,&i) && !i.session);inside=false;}return source.phase;}
void gt__session_client_cancel(gt_session_client_t *c){CHECK(c==(void *)&marker);cancelled++;}
void gt__session_client_close(gt_session_client_t *c){CHECK(c==(void *)&marker);closed++;}
gt_client_submit_t gt__session_client_submit_at(gt_session_client_t *c,uint64_t g,uint64_t s,const gt_user_request_t *r,gt_worker_stamp_t *out){CHECK(c==(void *)&marker);encoded_generation=g;encoded_sequence=s;request=*r;*out=(gt_worker_stamp_t){g,81,s};return GT_CLIENT_QUEUED;}
static gt_managed_info_t catalog_rows[2];static uint64_t stopped_target;static unsigned refreshes;
gt_status_t gt__session_client_catalog(const gt_session_client_t *c,gt_managed_info_t *rows,size_t cap,size_t *n){CHECK(c==(void *)&marker && cap==64);*n=2;memcpy(rows,catalog_rows,sizeof(catalog_rows));return GT_OK;}
gt_client_submit_t gt__session_client_catalog_refresh(gt_session_client_t *c){CHECK(c==(void *)&marker);refreshes++;return GT_CLIENT_QUEUED;}
gt_client_submit_t gt__session_client_terminate(gt_session_client_t *c,uint64_t target){CHECK(c==(void *)&marker);stopped_target=target;return GT_CLIENT_QUEUED;}
static gt_session_sink_result_t snapshot(void *c,uint64_t g,uint64_t s,uint32_t f,const uint8_t *p,size_t n,bool ro,gt_status_t *error){CHECK(c==&marker && g==41 && s==43 && f==7 && n==3 && !memcmp(p,"A\0B",3) && ro);*error=GT_OK;calls++;return GT_SESSION_SINK_BUSY;}
static gt_session_sink_result_t event(void *c,uint64_t g,uint64_t s,const gt_session_event_t *e,gt_status_t *error){CHECK(c==&marker && g==41 && s==43);calls++;*error=GT_OK;if(e->kind==GT_SESSION_OUTPUT)CHECK(e->length==3 && !memcmp(e->bytes,"A\0B",3));else if(e->kind==GT_SESSION_RESIZE)CHECK(e->size.cols==80 && e->size.rows==24 && e->size.px_width==800 && e->size.px_height==600 && !e->length);else if(e->kind==GT_SESSION_STYLE)CHECK(e->style.dark==1 && e->style.background[2]==5 && e->style.palette[0][2]==8 && !e->length);else CHECK(e->kind==GT_SESSION_COLOR && e->dark && !e->length);return GT_SESSION_SINK_ACCEPTED;}
static gt_status_t applied(void *c,gt_session_applied_t *out){CHECK(c==&marker);*out=(gt_session_applied_t){true,41,0};return GT_OK;}
static void invalidated(void *c,uint64_t g,gt_status_t e){CHECK(c==&marker && g==41 && e==GT_ERR_IO);calls++;}
static void receipt(void *c,const gt_session_receipt_t *r){CHECK(c==&marker && r->stamp.generation==41 && r->stamp.command_id==47 && r->stamp.sequence==43 && r->status==GT_ERR_AUTH && r->delivery==GT_SESSION_OUTCOME_UNKNOWN && r->rejected && r->rejection==GT_USER_REJECT_UNSUPPORTED && r->kind==GT_SESSION_COMMAND_COLOR);calls++;}
static void open_session(void){gt_session_create_t create={.nonce={1},.process={.executable="/bin/sh",.cwd="/",.cols=80,.rows=24}};gt_session_options_t o={.kind=GT_SESSION_LOCAL,.proxy="/fixture/proxy",.daemon="/fixture/daemon",.runtime="/fixture/runtime",.compatibility={1},.create=&create,.snapshot_format=7,.sink={&marker,snapshot,event,applied,invalidated,receipt}};CHECK(gt_session_open(&o,&session)==GT_OK);}
int main(void){
  open_session();CHECK(config.attachment.sink.context && config.attachment.snapshot_limit==16*1024*1024 && config.recovery_timeout_ms==60000);
  const gt_session_state_t phases[]={GT_SESSION_STATE_STARTING,GT_SESSION_STATE_RECOVERING,GT_SESSION_STATE_RESTORING,GT_SESSION_STATE_LIVE,GT_SESSION_STATE_READONLY,GT_SESSION_STATE_ENDED,GT_SESSION_STATE_FAILED,GT_SESSION_STATE_CLOSED,GT_SESSION_STATE_CATALOG};
  for(unsigned i=0;i<9;i++){source.phase=(gt_session_phase_t)i;CHECK(gt_session_step(session)==phases[i]);gt_session_info_t info;CHECK(gt_session_info(session,&info) && info.state==phases[i]);}
  source=(gt_session_client_info_t){.phase=GT_SESSION_LIVE,.instance={9},.session=31,.generation=41,.attempts=3,.busy_replies=2,.has_attachment=true,.attachment={.has_sequence=true,.sequence=43,.has_terminal=true,.state_error=GT_ERR_UNSUPPORTED,.terminal={.has_result=true,.result={.kind=GT_WORKER_END_EOF,.status=GT_ERR_IO,.termination_status=GT_ERR_NOT_FOUND,.exit_code=777}}}};
  gt_session_info_t info;CHECK(gt_session_info(session,&info) && !info.readonly && info.has_sequence && info.sequence==43 && info.session==31 && info.generation==41 && info.instance[0]==9 && info.attempts==3 && info.busy_replies==2 && info.has_terminal && !info.has_exit_code && !info.exit_code && info.terminal_kind==GT_WORKER_END_EOF && info.terminal_status==GT_ERR_IO && info.termination_status==GT_ERR_NOT_FOUND && info.state_error==GT_ERR_UNSUPPORTED);
  source.attachment.terminal.result.has_exit_code=true;source.attachment.terminal.result.exit_code=7;CHECK(gt_session_info(session,&info) && info.has_exit_code && info.exit_code==7);
  gt_client_sink_t *sink=&config.attachment.sink;gt_status_t error=GT_ERR_INTERNAL;CHECK(sink->snapshot(sink->context,41,43,7,(const uint8_t *)"A\0B",3,true,&error)==GT_CLIENT_SINK_BUSY && error==GT_OK && calls==1);
  gt_state_event_t e={GT_STATE_OUTPUT,"A\0B",3};CHECK(sink->event(sink->context,41,43,&e,&error)==GT_CLIENT_SINK_ACCEPTED && error==GT_OK);
  gt_state_size_t size={80,24,800,600};unsigned char unaligned[sizeof(size)+1];memcpy(unaligned+1,&size,sizeof(size));e=(gt_state_event_t){GT_STATE_RESIZE,unaligned+1,sizeof(size)};CHECK(sink->event(sink->context,41,43,&e,&error)==GT_CLIENT_SINK_ACCEPTED);
  gt_state_color_scheme_t color={1};e=(gt_state_event_t){GT_STATE_COLOR_SCHEME,&color,sizeof(color)};CHECK(sink->event(sink->context,41,43,&e,&error)==GT_CLIENT_SINK_ACCEPTED);CHECK(calls==4);
  color.dark=2;CHECK(sink->event(sink->context,41,43,&e,&error)==GT_CLIENT_SINK_FAILED && error==GT_ERR_PROTOCOL && calls==4);e.kind=999;CHECK(sink->event(sink->context,41,43,&e,&error)==GT_CLIENT_SINK_FAILED && error==GT_ERR_UNSUPPORTED && calls==4);
  gt_client_applied_t a;CHECK(sink->applied(sink->context,&a)==GT_OK && a.has_sequence && a.generation==41 && a.sequence==0);sink->invalidated(sink->context,41,GT_ERR_IO);CHECK(calls==5);
  gt_user_reply_t r={.kind=GT_WORKER_COLOR_SCHEME,.stamp={41,47,43},.status=GT_ERR_AUTH,.delivery=GT_USER_OUTCOME_UNKNOWN,.rejected=true,.rejection=GT_USER_REJECT_UNSUPPORTED};sink->result(sink->context,&r);CHECK(calls==6);
  gt_session_stamp_t stamp;CHECK(gt_session_input(session,41,43,"A\0B",3,&stamp)==GT_SESSION_SUBMIT_QUEUED && stamp.generation==41 && stamp.command_id==81 && stamp.sequence==43 && encoded_generation==41 && encoded_sequence==43 && request.kind==GT_WORKER_INPUT && request.length==3 && !memcmp(request.input,"A\0B",3));
  CHECK(gt_session_resize(session,41,43,(gt_session_size_t){80,24,800,600},&stamp)==GT_SESSION_SUBMIT_QUEUED && request.kind==GT_WORKER_RESIZE && request.size.cols==80 && request.size.rows==24 && request.size.px_width==800 && request.size.px_height==600);
  CHECK(gt_session_color(session,41,43,true,&stamp)==GT_SESSION_SUBMIT_QUEUED && request.kind==GT_WORKER_COLOR_SCHEME && request.color.dark==1);
  gt_session_style_t style={.dark=1,.background={3,4,5},.palette={{6,7,8}}};
  CHECK(gt_session_style(session,41,43,&style,&stamp)==GT_SESSION_SUBMIT_QUEUED && request.kind==GT_WORKER_STYLE && request.style.dark==1 && request.style.background[2]==5 && request.style.palette[0][2]==8);
  e=(gt_state_event_t){GT_STATE_STYLE,&style,sizeof(style)};CHECK(sink->event(sink->context,41,43,&e,&error)==GT_CLIENT_SINK_ACCEPTED && calls==7);
  CHECK(gt_session_style(session,41,43,NULL,&stamp)==GT_SESSION_SUBMIT_INVALID && !stamp.command_id);
  style.dark=2;CHECK(gt_session_style(session,41,43,&style,&stamp)==GT_SESSION_SUBMIT_INVALID && !stamp.command_id);
  catalog_rows[0]=(gt_managed_info_t){.id=3,.phase=GT_MANAGED_WORKER,.result={.has_exit_code=true,.exit_code=777}};
  catalog_rows[1]=(gt_managed_info_t){.id=5,.phase=GT_MANAGED_RETAINED,.termination_requested=true,.has_result=true,.result={.kind=GT_WORKER_END_STOPPED,.has_sequence=true,.sequence=43,.has_exit_code=true,.exit_code=7,.status=GT_ERR_IO,.termination_status=GT_ERR_UNSUPPORTED}};
  gt_session_row_t rows[2];size_t count=99;CHECK(gt_session_catalog(session,rows,1,&count)==GT_ERR_INVALID_ARG && count==2 && !rows[0].session);
  CHECK(gt_session_catalog(session,rows,2,&count)==GT_OK && count==2 && rows[0].session==3 && !rows[0].has_result && !rows[0].has_exit_code && !rows[0].exit_code && rows[1].session==5 && rows[1].phase==GT_MANAGED_RETAINED && rows[1].terminating && rows[1].has_result && rows[1].has_exit_code && rows[1].exit_code==7 && rows[1].has_sequence && rows[1].sequence==43 && rows[1].status==GT_ERR_IO && rows[1].termination_status==GT_ERR_UNSUPPORTED);
  catalog_rows[1].result.has_exit_code=false;catalog_rows[1].result.exit_code=777;CHECK(gt_session_catalog(session,rows,2,&count)==GT_OK && rows[1].has_result && !rows[1].has_exit_code && !rows[1].exit_code);
  CHECK(gt_session_catalog_refresh(session)==GT_SESSION_SUBMIT_QUEUED && refreshes==1);CHECK(gt_session_terminate(session,31)==GT_SESSION_SUBMIT_QUEUED && stopped_target==31);
  source.has_admitted_create=true;source.input_locked=true;source.stop=(gt_client_stop_t){.state=GT_CLIENT_STOP_UNKNOWN,.session=31,.request=81,.status=GT_CONTROL_OK,.error=GT_ERR_IO};
  CHECK(gt_session_info(session,&info) && info.has_admitted_create && info.readonly && info.stop.state==GT_SESSION_STOP_UNKNOWN && info.stop.session==31 && info.stop.request==81 && info.stop.error==GT_ERR_IO);
  source.stop.state=GT_CLIENT_STOP_KNOWN;source.stop.status=GT_CONTROL_UNSUPPORTED;CHECK(gt_session_info(session,&info) && info.stop.state==GT_SESSION_STOP_KNOWN && info.stop.status==GT_SESSION_CONTROL_UNSUPPORTED);
  gt_ssh_t *unused_ssh=(void *)&marker;CHECK(gt_session_auth_adopt(session,1,&unused_ssh)==GT_ERR_UNSUPPORTED && unused_ssh==(void *)&marker);
  gt_session_cancel(session);gt_session_cancel(session);CHECK(cancelled==1);CHECK(gt_session_close(session) && closed==1 && cancelled==1);puts("PASS session public ABI field translation and borrowed binary callbacks");return 0;
}
