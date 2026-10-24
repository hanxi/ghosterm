/* P0 backend/worker measurement, not GUI or production daemon performance. */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "session/worker.h"
#include <ghostty/vt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL performance:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define BULK (4u*1024u*1024u)
#define SAMPLES 128u
static double clock_ms(void) {struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static void pause_ms(unsigned ms) {struct timespec t={(time_t)(ms/1000),(long)(ms%1000)*1000000};while(nanosleep(&t,&t))CHECK(errno==EINTR);}
static void send_all(int fd,const void *data,size_t n) {const unsigned char *p=data;while(n){ssize_t k=write(fd,p,n);if(k<0 && errno==EINTR)continue;CHECK(k>0);p+=k;n-=(size_t)k;}}
static unsigned char pattern(size_t n) {return n%80==79?'\n':(unsigned char)('A'+n%26);}
static void producer(void) {
    struct termios t;CHECK(!tcgetattr(STDIN_FILENO,&t));cfmakeraw(&t);CHECK(!tcsetattr(STDIN_FILENO,TCSANOW,&t));
    send_all(STDOUT_FILENO,"R",1);
    for(;;){unsigned char command;ssize_t n=read(STDIN_FILENO,&command,1);if(n<0 && errno==EINTR)continue;CHECK(n==1);
        if(command=='B'){unsigned char block[16384];for(size_t offset=0;offset<BULK;offset+=sizeof(block)){for(size_t i=0;i<sizeof(block);i++)block[i]=pattern(offset+i);send_all(STDOUT_FILENO,block,sizeof(block));}}
        else if(command=='Q')return;
        else {CHECK(command>='a' && command<='z');send_all(STDOUT_FILENO,&command,1);}
    }
}
typedef struct {
    pthread_mutex_t lock;pthread_cond_t changed;
    const char *executable;gt_session_worker_t *worker;gt_io_backend_t *direct;
    GhosttyTerminal terminal;unsigned phase;size_t received;unsigned char echo;
    bool closed,finished;gt_status_t result;unsigned events;size_t applied;
} bench;
static gt_io_backend_t *open_backend(void *context) {
    bench *b=context;const char *args[]={"producer",NULL};
    gt_pty_opts_t options={.executable=b->executable,.argv=args,.cmd_kind=GT_PTY_CMD_DIRECT,.cols=80,.rows=24};
    return gt__pty_open(&options);
}
static gt_status_t open_state(void *context,gt_worker_reply_sink_t sink,void **state) {
    (void)sink;bench *b=context;CHECK(ghostty_terminal_new(NULL,&b->terminal,80,24)==GHOSTTY_SUCCESS);*state=b;return GT_OK;
}
static void consume(bench *b,const unsigned char *bytes,size_t n) {
    CHECK(!pthread_mutex_lock(&b->lock));
    for(size_t i=0;i<n;i++){
        unsigned char expected=b->phase==0?'R':b->phase==1?pattern(b->received):b->echo;
        CHECK(bytes[i]==expected);b->received++;
        CHECK(b->received<=(b->phase==1?BULK:1));
    }
    CHECK(!pthread_cond_broadcast(&b->changed));CHECK(!pthread_mutex_unlock(&b->lock));
}
static gt_status_t apply(void *state,const gt_state_event_t *event) {
    bench *b=state;CHECK(event->kind==GT_STATE_OUTPUT);ghostty_terminal_vt_write(b->terminal,event->payload,event->length);
    b->events++;b->applied+=event->length;consume(b,event->payload,event->length);return GT_OK;
}
static gt_status_t freeze(void *state,void **out) {(void)state;*out=NULL;return GT_ERR_UNSUPPORTED;}
static void snapshot(GhosttyTerminal terminal,unsigned char **out,size_t *length) {
    CHECK(ghostty_snapshot_encode_buf(terminal,NULL,0,length)==GHOSTTY_OUT_OF_SPACE);CHECK(*length>0 && *length<16u*1024u*1024u);
    *out=malloc(*length);CHECK(*out);CHECK(ghostty_snapshot_encode_buf(terminal,*out,*length,length)==GHOSTTY_SUCCESS);
}
static void close_state(void *state) {
    bench *b=state;CHECK(b->applied==1+BULK+SAMPLES && b->events>0);
    /* Independent oracle uses different chunk boundaries; measured delivery
     * cannot skip VT application while still passing byte-stream validation. */
    GhosttyTerminal oracle=NULL;CHECK(ghostty_terminal_new(NULL,&oracle,80,24)==GHOSTTY_SUCCESS);
    ghostty_terminal_vt_write(oracle,(const unsigned char *)"R",1);
    unsigned char block[4096];for(size_t offset=0;offset<BULK;offset+=sizeof(block)){for(size_t i=0;i<sizeof(block);i++)block[i]=pattern(offset+i);ghostty_terminal_vt_write(oracle,block,sizeof(block));}
    for(unsigned i=0;i<SAMPLES;i++){unsigned char value=(unsigned char)('a'+i%26);ghostty_terminal_vt_write(oracle,&value,1);}
    unsigned char *actual=NULL,*expected=NULL;size_t an=0,en=0;snapshot(b->terminal,&actual,&an);snapshot(oracle,&expected,&en);
    CHECK(an==en && !memcmp(actual,expected,an));free(actual);free(expected);ghostty_terminal_free(oracle);ghostty_terminal_free(b->terminal);b->terminal=NULL;b->closed=true;
}
static void finished(void *context,gt_status_t result) {
    bench *b=context;CHECK(!pthread_mutex_lock(&b->lock));b->result=result;b->finished=true;CHECK(!pthread_cond_broadcast(&b->changed));CHECK(!pthread_mutex_unlock(&b->lock));
}
static void direct_read(bench *b) {
    unsigned char bytes[16384];int n=b->direct->ops->read(b->direct,bytes,sizeof(bytes),10);
    if(n==GT_TRANSPORT_READ_TIMEOUT)return;
    CHECK(n>0);gt_state_event_t event={GT_STATE_OUTPUT,bytes,(size_t)n};CHECK(apply(b,&event)==GT_OK);
}
static void wait_received(bench *b,size_t expected) {
    double deadline=clock_ms()+10000;
    if(b->direct){while(b->received<expected){CHECK(clock_ms()<deadline);direct_read(b);}return;}
    struct timespec until;CHECK(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=10;
    CHECK(!pthread_mutex_lock(&b->lock));while(b->received<expected && !b->finished)CHECK(!pthread_cond_timedwait(&b->changed,&b->lock,&until));CHECK(b->received==expected && !b->finished);CHECK(!pthread_mutex_unlock(&b->lock));
}
static void command(bench *b,unsigned phase,unsigned char c) {
    CHECK(!pthread_mutex_lock(&b->lock));b->phase=phase;b->received=0;b->echo=c;CHECK(!pthread_mutex_unlock(&b->lock));
    if(b->direct)CHECK(b->direct->ops->write(b->direct,&c,1)==GT_OK);
    else CHECK(gt__worker_submit_internal(b->worker,GT_WORKER_INPUT,&c,1,NULL,NULL)==GT_WORKER_ACCEPTED);
}
static double cpu_ms(const struct rusage *r) {return (r->ru_utime.tv_sec+r->ru_stime.tv_sec)*1000.0+(r->ru_utime.tv_usec+r->ru_stime.tv_usec)/1000.0;}
static int compare(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return(x>y)-(x<y);}
int main(int argc,char **argv) {
    CHECK(argc==2);if(!strcmp(argv[1],"producer")){producer();return 0;}
    if(!strcmp(argv[1],"oracle")) {
        GhosttyTerminal a=NULL,b=NULL;CHECK(ghostty_terminal_new(NULL,&a,80,24)==GHOSTTY_SUCCESS);CHECK(ghostty_terminal_new(NULL,&b,80,24)==GHOSTTY_SUCCESS);
        ghostty_terminal_vt_write(a,(const unsigned char *)"control\033[2;3HX",14);ghostty_terminal_vt_write(b,(const unsigned char *)"control\033[2;3HX",14);
        unsigned char *ap=NULL,*bp=NULL;size_t an=0,bn=0;snapshot(a,&ap,&an);snapshot(b,&bp,&bn);CHECK(an==bn && !memcmp(ap,bp,an));free(ap);free(bp);ghostty_terminal_free(a);ghostty_terminal_free(b);puts("PASS independent oracle control");return 0;
    }
    bool worker=!strcmp(argv[1],"worker");CHECK(worker || !strcmp(argv[1],"direct"));
    bench b={.executable=argv[0]};CHECK(!pthread_mutex_init(&b.lock,NULL));CHECK(!pthread_cond_init(&b.changed,NULL));
    if(worker){gt_worker_config_t config={.context=&b,.open_backend=open_backend,.open_state=open_state,.close_state=close_state,.state_ops={apply,freeze},.finished=finished};b.worker=gt__worker_start(&config);CHECK(b.worker);}
    else{void *state=NULL;CHECK(open_state(&b,(gt_worker_reply_sink_t){0},&state)==GT_OK && state==&b);b.direct=open_backend(&b);CHECK(b.direct);}
    wait_received(&b,1);
    struct rusage before,after;CHECK(!getrusage(RUSAGE_SELF,&before));double start=clock_ms();
    if(worker)pause_ms(1000);else while(clock_ms()-start<1000){unsigned char bytes[32];CHECK(b.direct->ops->read(b.direct,bytes,sizeof(bytes),10)==GT_TRANSPORT_READ_TIMEOUT);}
    double idle_wall=clock_ms()-start;CHECK(!getrusage(RUSAGE_SELF,&after));double idle_cpu=cpu_ms(&after)-cpu_ms(&before);
    start=clock_ms();command(&b,1,'B');wait_received(&b,BULK);double bulk_ms=clock_ms()-start;
    double latency[SAMPLES];for(unsigned i=0;i<SAMPLES;i++){
        /* Decouple sends from read completion so this does not only measure
         * the fastest just-completed-read scheduling window. */
        pause_ms(1+i%7);start=clock_ms();command(&b,2,(unsigned char)('a'+i%26));wait_received(&b,1);latency[i]=clock_ms()-start;
    }
    command(&b,2,'Q');
    if(worker){struct timespec until;CHECK(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec+=10;CHECK(!pthread_mutex_lock(&b.lock));while(!b.finished)CHECK(!pthread_cond_timedwait(&b.changed,&b.lock,&until));CHECK(!pthread_mutex_unlock(&b.lock));gt__worker_join(b.worker);CHECK(b.result==GT_OK && b.closed);}
    else{unsigned char bytes[32];int n;do{n=b.direct->ops->read(b.direct,bytes,sizeof(bytes),10);}while(n==GT_TRANSPORT_READ_TIMEOUT);CHECK(n==GT_TRANSPORT_READ_EOF);b.direct->ops->close(b.direct);close_state(&b);}
    CHECK(!getrusage(RUSAGE_SELF,&after));qsort(latency,SAMPLES,sizeof(*latency),compare);
    printf("{\"mode\":\"%s\",\"bulk_bytes\":%u,\"bulk_ms\":%.6f,\"latency_samples\":%u,\"latency_p50_ms\":%.6f,\"latency_p95_ms\":%.6f,\"latency_p99_ms\":%.6f,\"idle_wall_ms\":%.6f,\"idle_cpu_ms\":%.6f,\"peak_rss_native\":%ld,\"applied_bytes\":%zu,\"snapshot_equal\":true}\n",argv[1],BULK,bulk_ms,SAMPLES,latency[63],latency[121],latency[126],idle_wall,idle_cpu,after.ru_maxrss,b.applied);
    CHECK(!pthread_cond_destroy(&b.changed));CHECK(!pthread_mutex_destroy(&b.lock));return 0;
}
