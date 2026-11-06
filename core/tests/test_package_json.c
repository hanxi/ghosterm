#include "extension/package-json.h"
#include "extension/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",mode,__LINE__,#x);return 1;}}while(0)
typedef struct {size_t live,used,peak,calls,fail;bool wiped;} memory_t;
static void *alloc(void *p,size_t n){memory_t *m=p;if(++m->calls==m->fail)return NULL;void *out=malloc(n);if(out){m->live++;m->used+=n;if(m->used>m->peak)m->peak=m->used;}return out;}
static void release(void *p,void *bytes,size_t n){memory_t *m=p;for(size_t i=0;i<n;i++)if(((unsigned char *)bytes)[i])m->wiped=false;m->live--;m->used-=n;free(bytes);}
int main(int argc,char **argv){
 const char *mode=argc>1?argv[1]:"valid";memory_t m={.wiped=true};gt_allocator_t a={&m,alloc,release};gt_package_json_t *d=NULL;CHECK(gt__package_json_open(&a,&d));size_t owner_bytes=m.used;
 const char *raw="{\"a\":1,\"inner\":{\"a\":2},\"text\":\"\\ud83d\\ude00\"}";size_t size=strlen(raw);char *owned=NULL;bool integers=true;gt_package_json_error_t expected=GT_PACKAGE_JSON_OK;
 if(!strcmp(mode,"duplicate")){raw="{\"a\":1,\"\\u0061\":2}";expected=GT_PACKAGE_JSON_DUPLICATE;}
 else if(!strcmp(mode,"surrogate")){raw="\"\\ud800\"";expected=GT_PACKAGE_JSON_UTF8;}
 else if(!strcmp(mode,"utf8")){raw="\"\xed\xa0\x80\"";expected=GT_PACKAGE_JSON_UTF8;}
 else if(!strcmp(mode,"bom")){raw="\xef\xbb\xbf{}";expected=GT_PACKAGE_JSON_SYNTAX;}
 else if(!strcmp(mode,"trailing")){raw="{} true";expected=GT_PACKAGE_JSON_SYNTAX;}
 else if(!strcmp(mode,"fractional")){raw="1.00000000000000001";expected=GT_PACKAGE_JSON_NUMBER;}
 else if(!strcmp(mode,"unsafe")){raw="9007199254740993";expected=GT_PACKAGE_JSON_NUMBER;}
 else if(!strcmp(mode,"negative")){raw="-1";expected=GT_PACKAGE_JSON_NUMBER;}
 else if(!strcmp(mode,"exact"))raw="[1e0,100e-2,-0,0e9999999999999999999,9007199254740991]";
 else if(!strcmp(mode,"syntax-only")){raw="[-1,1.0000000000000001,1e100000]";integers=false;}
 else if(!strcmp(mode,"empty-keys")){raw="{\"\":\"\",\"nested\":{\"\":0}}";}
 else if(!strcmp(mode,"small-memory")){CHECK(owner_bytes<1024);}
 else if(!strcmp(mode,"growth") || !strcmp(mode,"growth-duplicate") || !strcmp(mode,"oom")){
  owned=malloc(32768);CHECK(owned);size=(size_t)snprintf(owned,32768,"{\"nested\":[");
  for(unsigned i=0;i<300;i++)size+=(size_t)snprintf(owned+size,32768-size,"%s{\"k%u\":\"value%u\"}",i?",":"",i,i);
  size+=(size_t)snprintf(owned+size,32768-size,"],\"after\":17%s}",!strcmp(mode,"growth-duplicate")?",\"\\u006eested\":18":"");raw=owned;
  if(!strcmp(mode,"growth-duplicate"))expected=GT_PACKAGE_JSON_DUPLICATE;
 }
 else if(!strcmp(mode,"dense-string")){size=1024*1024;owned=malloc(size);CHECK(owned);memset(owned,'x',size);owned[0]=owned[size-1]='"';raw=owned;}
 else if(!strcmp(mode,"global-memory")){
  gt_plugin_memory_info_t info=gt__plugin_memory_info();size_t n=info.limit-info.used;void *hold=gt__plugin_memory_alloc(NULL,n);CHECK(hold);
  CHECK(gt__package_json_check(d,raw,strlen(raw),true)==GT_PACKAGE_JSON_MEMORY && m.used==owner_bytes);
  gt__plugin_memory_free(NULL,hold,n);
 }
 else if(!strcmp(mode,"large")){size=GT_PACKAGE_JSON_BYTES;owned=malloc(size);CHECK(owned);memset(owned,' ',size);memcpy(owned,"{}",2);raw=owned;}
 else if(!strcmp(mode,"bytes-limit")){size=GT_PACKAGE_JSON_BYTES+1;owned=malloc(size);CHECK(owned);memset(owned,' ',size);owned[0]='0';raw=owned;expected=GT_PACKAGE_JSON_LIMIT;}
 else if(!strcmp(mode,"nodes") || !strcmp(mode,"nodes-limit")){size_t count=GT_PACKAGE_JSON_NODES-1+(!strcmp(mode,"nodes-limit"));size=count*2+1;owned=malloc(size);CHECK(owned);owned[0]='[';for(size_t i=0;i<count;i++){owned[1+i*2]='0';owned[2+i*2]=i+1<count?',':']';}raw=owned;if(!strcmp(mode,"nodes-limit"))expected=GT_PACKAGE_JSON_LIMIT;}
 else if(!strcmp(mode,"depth") || !strcmp(mode,"depth-limit")){size_t depth=GT_PACKAGE_JSON_DEPTH+(!strcmp(mode,"depth-limit"));size=2*depth+1;owned=malloc(size);CHECK(owned);memset(owned,'[',depth);owned[depth]='0';memset(owned+depth+1,']',depth);raw=owned;if(!strcmp(mode,"depth-limit"))expected=GT_PACKAGE_JSON_LIMIT;}
 else CHECK(!strcmp(mode,"valid"));
 if(!owned)size=strlen(raw);size_t before=m.calls;CHECK(gt__package_json_check(d,raw,size,integers)==expected);size_t allocations=m.calls-before;CHECK(m.used==owner_bytes && m.live==1);
 if(!strcmp(mode,"oom"))for(size_t i=1;i<=allocations;i++){
  m.fail=m.calls+i;CHECK(gt__package_json_check(d,raw,size,true)==GT_PACKAGE_JSON_MEMORY && m.used==owner_bytes && m.live==1);m.fail=0;
  CHECK(gt__package_json_check(d,"{}",2,true)==GT_PACKAGE_JSON_OK && m.used==owner_bytes);
 }
 if(!strcmp(mode,"small-memory") || !strcmp(mode,"large"))CHECK(m.peak<4096);
 CHECK(gt__package_json_check(d,"{}",2,true)==GT_PACKAGE_JSON_OK);CHECK(gt__package_json_check(d,NULL,1,true)==GT_PACKAGE_JSON_SYNTAX);CHECK(gt__package_json_check(d,NULL,0,true)==GT_PACKAGE_JSON_SYNTAX);CHECK(gt__package_json_check(NULL,"{}",2,true)==GT_PACKAGE_JSON_SYNTAX);
 gt__package_json_destroy(&d);CHECK(!d && !m.live && !m.used && m.wiped && !gt__plugin_memory_info().used);free(owned);printf("PACKAGE JSON PASS %s peak=%zu owner=%zu allocations=%zu\n",mode,m.peak,owner_bytes,allocations);return 0;
}
