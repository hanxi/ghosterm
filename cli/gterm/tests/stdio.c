#define _POSIX_C_SOURCE 200809L
#include "ghosterm.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static pid_t child;
static void cleanup(void) { if (child > 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); } }
static uint64_t now(void) { struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t)); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void idle(void) { struct timespec t={0,1000000}; nanosleep(&t,NULL); }
int main(int argc,char **argv) {
    CHECK(argc==5); CHECK(!atexit(cleanup));
    const char *binary=argv[1],*directory=argv[2],*package_digest=argv[3],*mode=argv[4];
    char client_digest[65];memset(client_digest,'d',64);client_digest[64]=0;
    int fd[2];CHECK(!socketpair(AF_UNIX,SOCK_STREAM,0,fd));
    child=fork();CHECK(child>=0);
    if(!child){
        close(fd[0]);CHECK(dup2(fd[1],0)==0 && dup2(fd[1],1)==1);close(fd[1]);
        const char *trusted=!strcmp(mode,"digest")?client_digest:package_digest;
        execl(binary,binary,"relay","--stdio","--trust-development-digest",trusted,"--client-digest",client_digest,"--method","fixture.echo","--",directory,!strcmp(mode,"command")?"other":"fixture","中文","","';throw Error('injected');//",(char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    gt_ext_channel_t *channel=NULL;CHECK(gt_ext_channel_open_fd(fd[0],fd[0],&channel)==GT_OK);
    gt_ext_session_config_t c={.required_count=1,.available_count=1,.budgets={768,65536,262144},.handshake_ms=5000,.consent_ms=60000,.peer={GT_EXT_PEER_LOCAL,"test-explicit-child"},.features=GT_EXT_FEATURE_REQUESTS};
    strcpy(c.plugin_id,"org.ghosterm.fixture");strcpy(c.plugin_version,"0.1.0");strcpy(c.sdk_version,"1.0.0");strcpy(c.host_version,"0.1.0");strcpy(c.client_digest,client_digest);strcpy(c.executor_digest,package_digest);strcpy(c.required[0],"session.rpc");strcpy(c.available[0],"session.rpc");
    if(!strcmp(mode,"peer-digest"))strcpy(c.client_digest,package_digest);
    gt_ext_rpc_method_t method={.method="fixture.echo",.capability="session.rpc",.receiver=GT_EXT_EXECUTOR,.input_shape=GT_EXT_JSON_OBJECT,.output_shape=GT_EXT_JSON_OBJECT};
    gt_ext_session_t *session=NULL;CHECK(gt_ext_session_open_rpc(&channel,GT_EXT_HOST,&c,&method,1,&session)==GT_OK);
    uint64_t deadline=now()+10000,consent_at=0,handle=0;bool confirmed=false,decided=false,signalled=false;
    gt_ext_lease_t lease;int status=0;bool ended=false;
    while(now()<deadline){
        gt_ext_session_step(session,0);gt_ext_session_info_t info;CHECK(gt_ext_session_info(session,&info));
        if(info.phase==GT_EXT_SESSION_CONSENT && !decided){
            if(!consent_at)consent_at=now();
            /* Withhold approval for a measurable interval. No RPC/activation is admitted. */
            if(now()-consent_at>=100){
                if(!strcmp(mode,"cancel")){CHECK(!kill(child,SIGTERM));signalled=true;decided=true;}
                else {CHECK(gt_ext_session_decide(session,info.consent_token,!strcmp(mode,"deny")?GT_EXT_DECISION_DENY:GT_EXT_DECISION_ALLOW)==GT_OK);decided=true;}
            }
        }
        if(info.phase==GT_EXT_SESSION_ACTIVE && !handle){
            CHECK(strcmp(mode,"deny") && strcmp(mode,"cancel"));CHECK(gt_ext_session_lease(session,&lease));
            const char *input="{\"text\":\"stdio\",\"count\":17}";
            CHECK(gt_ext_session_request(session,&lease,"fixture.echo",input,strlen(input),3000,&handle)==GT_EXT_RPC_OK && handle);
        }
        if(handle && !confirmed){
            gt_ext_rpc_result_t result;CHECK(gt_ext_session_request_result(session,&lease,handle,&result)==GT_EXT_RPC_OK);
            if(result.confirmed){CHECK(result.state==GT_EXT_RPC_SUCCEEDED && strstr(result.outcome,"stdio"));confirmed=true;CHECK(gt_ext_session_forget_request(session,&lease,handle)==GT_EXT_RPC_OK);gt_ext_session_revoke(session);}
        }
        pid_t waited=waitpid(child,&status,WNOHANG);CHECK(waited>=0);
        if(waited==child){ended=true;child=0;break;}idle();
    }
    CHECK(ended);if(!WIFEXITED(status)){fprintf(stderr,"ABNORMAL child status %d\n",status);return 2;}
    int expected=!strcmp(mode,"digest") || !strcmp(mode,"deny")?6:!strcmp(mode,"command")?2:!strcmp(mode,"peer-digest")?7:!strcmp(mode,"cancel")?130:!strcmp(mode,"dispose-error")?5:0;
    CHECK(WEXITSTATUS(status)==expected);
    if(!strcmp(mode,"digest") || !strcmp(mode,"command")){gt_ext_session_info_t info;CHECK(gt_ext_session_info(session,&info) && !info.received);}
    if(!strcmp(mode,"roundtrip") || !strcmp(mode,"dispose-error"))CHECK(confirmed && decided && consent_at);
    if(!strcmp(mode,"cancel"))CHECK(signalled && !handle);
    for(unsigned i=0;i<100;i++)if(gt_ext_session_close(session,1)==GT_EXT_RELEASED)break;
    CHECK(gt_ext_session_destroy(&session)==GT_OK);printf("PASS stdio %s\n",mode);return 0;
}
