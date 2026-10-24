/* P0 history policy/notification evidence, not production session UI. */
#include <ghostty/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define OK(x) CHECK((x)==GHOSTTY_SUCCESS)
typedef struct{size_t notices;} Watch;
static void pruned(void *context){Watch *w=context;w->notices++;}
static GhosttyTerminal make(Watch *w,size_t bytes,size_t lines){GhosttyTerminal t=NULL;OK(ghostty_terminal_new(NULL,&t,80,24));OK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,&bytes));OK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,&lines));OK(ghostty_terminal_p0_set_history_observer(t,w,pruned));return t;}
static void text(GhosttyTerminal t,const char *s){ghostty_terminal_vt_write(t,(const uint8_t *)s,strlen(s));}
static void rows(GhosttyTerminal t,unsigned first,unsigned n){for(unsigned i=first;i<first+n;i++){char b[32];snprintf(b,sizeof b,"ROW%06u\r\n",i);text(t,b);}}
static size_t retained(GhosttyTerminal t){size_t n=0;OK(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_SCROLLBACK_ROWS,&n));return n;}
static void contents(GhosttyTerminal t,const char *latest){GhosttyFormatterTerminalOptions o={.size=sizeof o,.emit=GHOSTTY_FORMATTER_FORMAT_PLAIN,.trim=true};GhosttyFormatter f=NULL;OK(ghostty_formatter_terminal_new(NULL,&f,t,o));uint8_t b[65536];size_t n=0;OK(ghostty_formatter_format_buf(f,b,sizeof b-1,&n));b[n]=0;CHECK(!strstr((char *)b,"ROW000000") && strstr((char *)b,latest));ghostty_formatter_free(f);bool failed=true;OK(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR,&failed));CHECK(!failed);}
static void *capture(GhosttyTerminal t,size_t *n){CHECK(ghostty_snapshot_encode_buf(t,NULL,0,n)==GHOSTTY_OUT_OF_SPACE);void *p=malloc(*n);CHECK(p);OK(ghostty_snapshot_encode_buf(t,p,*n,n));return p;}
int main(int argc,char **argv){CHECK(argc==2);const char *name=argv[1];Watch w={0};const size_t mib=1024*1024;size_t bytes=!strcmp(name,"lines")?64*mib:mib,lines=!strcmp(name,"bytes")?100000:100;GhosttyTerminal t=make(&w,bytes,lines);CHECK(ghostty_terminal_p0_set_history_observer(NULL,NULL,NULL)==GHOSTTY_INVALID_VALUE);
if(!strcmp(name,"clear")||!strcmp(name,"no-prune")){rows(t,0,100);CHECK(!w.notices);text(t,"\033[3J\033c");CHECK(!w.notices && !retained(t));if(strcmp(name,"no-prune")){rows(t,100,2400);CHECK(w.notices);}}
else if(!strcmp(name,"same-chunk")){char *b=malloc(65536);CHECK(b);size_t n=0;for(unsigned i=0;i<2400;i++)n+=(size_t)snprintf(b+n,65536-n,"ROW%06u\r\n",i);memcpy(b+n,"\033c",2);n+=2;ghostty_terminal_vt_write(t,(const uint8_t *)b,n);free(b);CHECK(w.notices && !retained(t));size_t before=w.notices;rows(t,2400,2400);CHECK(w.notices>before);}
else if(!strcmp(name,"lower")){bytes=64*mib;lines=100000;OK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,&bytes));OK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,&lines));rows(t,0,2400);CHECK(!w.notices);lines=100;OK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,&lines));CHECK(w.notices);size_t before=w.notices;lines=100000;OK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES,&lines));rows(t,2400,2400);CHECK(w.notices==before);bytes=mib;OK(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES,&bytes));CHECK(w.notices>before);contents(t,"ROW004799");}
else{CHECK(!strcmp(name,"bytes")||!strcmp(name,"lines")||!strcmp(name,"both")||!strcmp(name,"import")||!strcmp(name,"unregister"));rows(t,0,2400);CHECK(w.notices && retained(t)>0 && retained(t)<2376);contents(t,"ROW002399");
if(!strcmp(name,"import")){size_t n=0;void *p=capture(t,&n);size_t before=w.notices;GhosttySnapshotDecoder d=NULL;GhosttyTerminal copy=NULL;OK(ghostty_snapshot_decoder_new_buf(NULL,&d,p,n));OK(ghostty_snapshot_decoder_decode(d,&copy));ghostty_snapshot_decoder_free(d);free(p);rows(copy,2400,2400);CHECK(w.notices==before);rows(t,2400,2400);CHECK(w.notices>before);size_t a=0,b=0;void *pa=capture(t,&a),*pb=capture(copy,&b);CHECK(a==b&&!memcmp(pa,pb,a));free(pa);free(pb);contents(copy,"ROW004799");ghostty_terminal_free(copy);}
if(!strcmp(name,"unregister")){size_t before=w.notices;OK(ghostty_terminal_p0_set_history_observer(t,NULL,NULL));rows(t,2400,2400);CHECK(w.notices==before);contents(t,"ROW004799");}}
size_t notices=w.notices;ghostty_terminal_free(t);CHECK(w.notices==notices);printf("PASS history-policy/%s notices=%zu\n",name,w.notices);return 0;}
