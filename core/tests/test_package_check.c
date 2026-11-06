#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "extension/package-check.h"
#include "extension/package-json.h"
#include "extension/memory.h"
#include "base/sha256.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",mode,__LINE__,#x);return 1;}}while(0)
typedef struct {size_t calls,fail,live;void *preflight_owner;bool wiped;atomic_bool *late_stop;bool *late_clock,*late_expired;} memory_t;
static void *alloc(void *p,size_t n){memory_t *m=p;if(++m->calls==m->fail)return NULL;void *out=malloc(n);if(out){if(m->calls==1)m->preflight_owner=out;m->live++;memset(out,0xa5,n);}return out;}
static void release(void *p,void *bytes,size_t n){memory_t *m=p;for(size_t i=0;i<n;i++)if(((unsigned char *)bytes)[i])m->wiped=false;bool preflight=bytes==m->preflight_owner;m->live--;free(bytes);if(preflight){if(m->late_stop)atomic_store(m->late_stop,true);if(m->late_clock)*m->late_clock=true;if(m->late_expired)*m->late_expired=true;}}
static int put(int fd,const char *name,const void *data,size_t n){int f=openat(fd,name,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);if(f<0)return -1;ssize_t w=write(f,data,n);int status=close(f);return w==(ssize_t)n && !status?0:-1;}
static char *file(const char *path,size_t *size){FILE *f=fopen(path,"rb");if(!f)return NULL;char *p=malloc(1048578);if(!p){fclose(f);return NULL;}*size=fread(p,1,1048577,f);bool ok=!ferror(f);if(fclose(f))ok=false;if(!ok){free(p);return NULL;}p[*size]=0;return p;}
static void quote(const char *p){putchar('"');for(;*p;p++){unsigned char c=(unsigned char)*p;if(c=='"' || c=='\\')printf("\\%c",c);else if(c<32)printf("\\u%04x",c);else putchar(c);}putchar('"');}
typedef struct {const char *mode;size_t calls,js_calls;atomic_bool *stop;bool late_clock,late_expired;} clock_tester_t;
static bool clock_test(void *opaque,uint64_t *now){
 clock_tester_t *t=opaque;if(t->late_clock)return false;if(t->late_expired){*now=1000;return true;}t->calls++;size_t calls=t->calls,threshold=6;
#ifdef GT_PACKAGE_CHECK_BOUNDARY
 test_caught_oom=!strcmp(t->mode,"caught-oom");test_destroy_error=!strcmp(t->mode,"destroy-error");
 if(!test_in_js){*now=0;return true;}calls=++t->js_calls;threshold=3;
#endif
 if(!strcmp(t->mode,"clock") || (!strcmp(t->mode,"js-clock") && calls>=threshold))return false;if(!strcmp(t->mode,"js-cancel") && calls>=threshold)atomic_store(t->stop,true);*now=(!strcmp(t->mode,"expired") && calls>=2) || (!strcmp(t->mode,"js-expired") && calls>=threshold)?1000:0;return true;
}
static const char manifest[]= "{\"schemaVersion\":1,\"id\":\"org.ghosterm.fixture\",\"pluginVersion\":\"0.1.0\",\"sdkApiRange\":\"^0.1.0\",\"bridgeProtocolRange\":[1],\"publisher\":\"fixture\",\"license\":\"MIT\",\"commands\":[{\"path\":[\"fixture\"],\"title\":\"Fixture\",\"argsSchema\":\"schemas/input.json\"}],\"client\":{\"runtime\":\"portable-js\",\"entry\":\"client/index.js\",\"requires\":[\"ui.resource\"],\"optional\":[\"ui.diff\"]},\"permissions\":[]}";
static const char host[]= "{\"side\":\"client\",\"sdkVersion\":\"0.1.1\",\"protocols\":[1],\"runtimes\":[\"portable-js\"],\"capabilities\":[\"ui.resource\"]}";
static int fixture(int fd,const char *mode){
 const char *names[]={"plugin.json","README","LICENSES/MIT.txt","client/index.js","schemas/input.json"};const char *texts[]={manifest,"fixture only","fixture license","throw Error('MUST_NOT_EXECUTE');export function activate(){}","{\"type\":\"object\"}"};
 if(mkdirat(fd,"LICENSES",0700) || mkdirat(fd,"client",0700) || mkdirat(fd,"schemas",0700))return -1;
 char changed[4096];strcpy(changed,manifest);
 if(!strcmp(mode,"duplicate"))snprintf(changed,sizeof(changed),"{\"schemaVersion\":1,%s",manifest+1);
 if(!strcmp(mode,"fractional")){const char *at=strstr(manifest,"1,");if(!at)return -1;size_t n=(size_t)(at-manifest);memcpy(changed,manifest,n);strcpy(changed+n,"1.00000000000000001");strcat(changed,at+1);}
 texts[0]=changed;char integrity[8192]="{\"schemaVersion\":1,\"members\":[";size_t used=strlen(integrity);
 for(size_t i=0;i<5;i++){size_t n=strlen(texts[i]);if(put(fd,names[i],texts[i],n))return -1;gt_sha256_t hash;char hex[65];gt__sha256_init(&hash);if(!gt__sha256_update(&hash,texts[i],n) || !gt__sha256_final(&hash,hex))return -1;if(!strcmp(mode,"hash") && i==3)hex[0]=hex[0]=='0'?'1':'0';int count=snprintf(integrity+used,sizeof(integrity)-used,"%s{\"path\":\"%s\",\"bytes\":%zu,\"sha256\":\"%s\"}",i?",":"",names[i],n,hex);if(count<0 || (size_t)count>=sizeof(integrity)-used)return -1;used+=(size_t)count;}
 strcat(integrity,"]}");return put(fd,"integrity.json",integrity,strlen(integrity));
}
static int clear(int fd){int copy=openat(fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(copy<0)return -1;DIR *d=fdopendir(copy);if(!d){close(copy);return -1;}struct dirent *e;int result=0;while((e=readdir(d))){if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;struct stat st;if(fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)){result=-1;break;}if(S_ISDIR(st.st_mode)){int child=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(child<0){result=-1;break;}result=clear(child);close(child);if(result || unlinkat(fd,e->d_name,AT_REMOVEDIR)){result=-1;break;}}else if(unlinkat(fd,e->d_name,0)){result=-1;break;}}if(closedir(d))result=-1;return result;}
int main(int argc,char **argv){
 const char *mode=argc>1?argv[1]:"invalid";gt_package_checked_t *checked=NULL;gt_package_check_report_t report;
 if(!strcmp(mode,"disabled")){CHECK(gt__package_check(NULL,NULL,NULL,&checked,&report)==GT_PACKAGE_CHECK_UNSUPPORTED && !checked && report.status==GT_PACKAGE_CHECK_UNSUPPORTED && !strcmp(report.code,"ENGINE_UNAVAILABLE"));return 0;}
 bool probe=!strcmp(mode,"probe");CHECK((probe && argc>=7) || (!probe && argc==3));char root[4096]={0};int fd;
 if(probe)fd=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);else{CHECK(snprintf(root,sizeof(root),"%s/package-check-XXXXXX",argv[2])>0 && mkdtemp(root));fd=open(root,O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(fd>=0 && fixture(fd,mode)==0);}CHECK(fd>=0);
 gt_package_capture_config_t capture={fd,GT_PACKAGE_MEMBERS_MAX,GT_PACKAGE_ENTRIES_MAX,GT_PACKAGE_MEMBER_BYTES_MAX,GT_PACKAGE_TOTAL_BYTES_MAX};gt_package_snapshot_t *snapshot=NULL;CHECK(gt__package_capture_open(&capture,NULL,&snapshot)==GT_PACKAGE_OK);
 const uint8_t *view=(void *)1;size_t view_size=99;CHECK(!gt__package_snapshot_view(snapshot,0,&view,&view_size) && !view && !view_size);
 gt_package_check_config_t incomplete={0};CHECK(gt__package_check(&snapshot,&incomplete,NULL,&checked,&report)==GT_PACKAGE_CHECK_INVALID && snapshot && !checked && !strcmp(report.code,"CAPTURE_NOT_READY"));
 gt_package_status_t state=GT_PACKAGE_WAIT;for(size_t i=0;i<50000 && state==GT_PACKAGE_WAIT;i++)state=gt__package_capture_step(snapshot);CHECK(state==GT_PACKAGE_OK);
 size_t host_size=strlen(host),registry_size=0;char *host_file=NULL,*registry_file=NULL;const void *facts=host;
 if(probe){if(strcmp(argv[3],"-")){host_file=file(argv[3],&host_size);CHECK(host_file);facts=host_file;}else{facts=NULL;host_size=0;}if(strcmp(argv[4],"-")){registry_file=file(argv[4],&registry_size);CHECK(registry_file);}}
 if(!strcmp(mode,"unknown")){facts=NULL;host_size=0;}
 atomic_bool stop;atomic_init(&stop,!strcmp(mode,"cancel"));clock_tester_t clock={.mode=mode,.stop=&stop};memory_t m={.wiped=true};if(!strcmp(mode,"allocation"))m.fail=1;
 if(!strcmp(mode,"late-cancel"))m.late_stop=&stop;if(!strcmp(mode,"late-clock"))m.late_clock=&clock.late_clock;if(!strcmp(mode,"late-expired"))m.late_expired=&clock.late_expired;
 if(probe && argc==8 && !strncmp(argv[7],"fail=",5))m.fail=(size_t)strtoull(argv[7]+5,NULL,10);
 gt_allocator_t a={&m,alloc,release};gt_package_check_config_t config={.host_json=facts,.host_size=host_size,.registry_json=registry_file,.registry_size=registry_size,.stop=&stop,.clock_context=&clock,.clock=clock_test};
 if(!strcmp(mode,"real-clock")){config.clock=NULL;config.cpu_ms=5000;}
 if(!strcmp(mode,"caught-oom"))config.memory_bytes=8388608;
 gt_package_check_status_t expected=GT_PACKAGE_CHECK_COMPATIBLE;const char *expected_code="COMPATIBLE";
 if(!strcmp(mode,"hash")){expected=GT_PACKAGE_CHECK_INVALID;expected_code="MEMBER_MISMATCH";}
 if(!strcmp(mode,"duplicate")){expected=GT_PACKAGE_CHECK_INVALID;expected_code="JSON_DUPLICATE";}
 if(!strcmp(mode,"fractional")){expected=GT_PACKAGE_CHECK_INVALID;expected_code="JSON_INTEGER";}
 if(!strcmp(mode,"unknown")){expected=GT_PACKAGE_CHECK_UNKNOWN;expected_code="HOST_UNKNOWN";}
 if(!strcmp(mode,"allocation")){expected=GT_PACKAGE_CHECK_MEMORY;expected_code="NATIVE_MEMORY";}
 if(!strcmp(mode,"caught-oom")){expected=GT_PACKAGE_CHECK_MEMORY;expected_code="ENGINE_MEMORY";}
 if(!strcmp(mode,"cancel") || !strcmp(mode,"js-cancel") || !strcmp(mode,"late-cancel")){expected=GT_PACKAGE_CHECK_CANCELLED;expected_code="CANCELLED";}
 if(!strcmp(mode,"clock") || !strcmp(mode,"js-clock") || !strcmp(mode,"late-clock")){expected=GT_PACKAGE_CHECK_IO;expected_code="CLOCK_IO";}
 if(!strcmp(mode,"expired") || !strcmp(mode,"js-expired") || !strcmp(mode,"late-expired")){expected=GT_PACKAGE_CHECK_LIMIT;expected_code="CPU_BUDGET";}
 if(probe){expected=(gt_package_check_status_t)strtol(argv[5],NULL,10);expected_code=argv[6];}
 if(!strcmp(mode,"invalid")){
  gt_package_check_config_t bad=config;bad.memory_bytes=134217729;CHECK(gt__package_check(&snapshot,&bad,&a,&checked,&report)==GT_PACKAGE_CHECK_INVALID && snapshot && !checked);
  bad=config;bad.stack_bytes=1048577;CHECK(gt__package_check(&snapshot,&bad,&a,&checked,&report)==GT_PACKAGE_CHECK_INVALID && snapshot && !checked);
  bad=config;bad.cpu_ms=5001;CHECK(gt__package_check(&snapshot,&bad,&a,&checked,&report)==GT_PACKAGE_CHECK_INVALID && snapshot && !checked);
  bad=config;bad.host_size=65537;CHECK(gt__package_check(&snapshot,&bad,&a,&checked,&report)==GT_PACKAGE_CHECK_LIMIT && !strcmp(report.code,"FACT_BYTES") && snapshot && !checked);
  bad=config;bad.registry_size=1048577;bad.registry_json="[]";CHECK(gt__package_check(&snapshot,&bad,&a,&checked,&report)==GT_PACKAGE_CHECK_LIMIT && snapshot && !checked);
  CHECK(gt__package_check(&snapshot,NULL,&a,&checked,&report)==GT_PACKAGE_CHECK_INVALID && snapshot && !checked);
  CHECK(gt__package_check(&snapshot,&config,&a,&checked,NULL)==GT_PACKAGE_CHECK_INVALID && snapshot && !checked);
 }
 void *reservation=NULL;size_t reserved=0;
 if(!strcmp(mode,"global-memory")){gt_plugin_memory_info_t usage=gt__plugin_memory_info();reserved=usage.limit-usage.used;reservation=gt__plugin_memory_alloc(NULL,reserved);CHECK(reservation);expected=GT_PACKAGE_CHECK_MEMORY;expected_code="NATIVE_MEMORY";}
 gt_package_check_status_t status=gt__package_check(&snapshot,&config,&a,&checked,&report);gt__plugin_memory_free(NULL,reservation,reserved);if(status!=expected || (strcmp(expected_code,"MEMORY") && strcmp(report.code,expected_code)))fprintf(stderr,"actual status=%u code=%s expected=%u %s\n",(unsigned)status,report.code,(unsigned)expected,expected_code);
 bool code_ok=!strcmp(report.code,expected_code) || (probe && m.fail && !strcmp(expected_code,"MEMORY") && (!strcmp(report.code,"NATIVE_MEMORY") || !strcmp(report.code,"ENGINE_MEMORY")));
 CHECK(status==expected && report.status==status && code_ok);
#ifdef GT_PACKAGE_CHECK_BOUNDARY
 if(!strcmp(mode,"caught-oom"))CHECK(test_injected_oom);
 else if(strcmp(mode,"destroy-error"))CHECK(test_in_js && !test_loop_finished);
#endif
 gt_package_check_info_t info;memset(&info,0xff,sizeof(info));
 if(status==GT_PACKAGE_CHECK_COMPATIBLE){
  CHECK(checked && !snapshot && gt__package_checked_info(checked,&info));CHECK(!info.authorization_granted && !info.publisher_authenticated);CHECK(info.protocol_count && info.requires_count<=16 && info.optional_missing_count<=16);
  if(!probe)CHECK(!strcmp(info.id,"org.ghosterm.fixture") && !strcmp(info.entry,"client/index.js") && !strcmp(info.version,"0.1.0") && !info.executor && !info.association_required && info.requires_count==1 && !strcmp(info.requires[0],"ui.resource") && info.optional_missing_count==1 && !strcmp(info.optional_missing[0],"ui.diff") && info.protocol_count==1 && info.protocols[0]==1);
  gt_package_member_t member;CHECK(gt__package_checked_member(checked,"plugin.json",&member,&view,&view_size));gt_sha256_t hash;char hex[65];gt__sha256_init(&hash);CHECK(gt__sha256_update(&hash,view,view_size) && gt__sha256_final(&hash,hex) && !strcmp(hex,info.manifest_digest));
  CHECK(gt__package_checked_member(checked,"integrity.json",&member,&view,&view_size));gt__sha256_init(&hash);CHECK(gt__sha256_update(&hash,view,view_size) && gt__sha256_final(&hash,hex) && !strcmp(hex,info.package_digest));
  if(probe && argc==8 && !strcmp(argv[7],"association=true"))CHECK(info.association_required);
  CHECK(gt__package_checked_member(checked,info.entry,&member,&view,&view_size));CHECK(view || !view_size);
  if(!strcmp(mode,"frozen")){CHECK(clear(fd)==0);CHECK(put(fd,"changed","evil",4)==0);CHECK(gt__package_checked_member(checked,info.entry,&member,&view,&view_size) && view_size==strlen("throw Error('MUST_NOT_EXECUTE');export function activate(){}") && !memcmp(view,"throw Error('MUST_NOT_EXECUTE');export function activate(){}",view_size));}
  CHECK(!gt__package_checked_member(checked,"../escape",&member,&view,&view_size) && !view && !view_size && !member.path);
  gt_package_status_t destroyed=GT_PACKAGE_OK;
#ifdef GT_PACKAGE_CHECK_BOUNDARY
  if(!strcmp(mode,"destroy-error"))destroyed=GT_PACKAGE_IO;
#endif
  CHECK(gt__package_checked_destroy(&checked)==destroyed && !checked);
 }else{size_t count;uint64_t bytes;CHECK(!checked && snapshot && gt__package_snapshot_count(snapshot,&count,&bytes));CHECK(!gt__package_checked_info(checked,&info) && !info.id[0]);CHECK(gt__package_snapshot_destroy(&snapshot)==GT_PACKAGE_OK && !snapshot);}
 CHECK(!m.live && m.wiped && !gt__plugin_memory_info().used);free(host_file);free(registry_file);if(!probe)CHECK(clear(fd)==0);CHECK(close(fd)==0);if(!probe)CHECK(rmdir(root)==0);
 printf("{\"status\":%u,\"code\":",(unsigned)status);quote(report.code);printf(",\"allocationCalls\":%zu",m.calls);
 if(status==GT_PACKAGE_CHECK_COMPATIBLE){printf(",\"id\":");quote(info.id);printf(",\"version\":");quote(info.version);printf(",\"entry\":");quote(info.entry);printf(",\"runtime\":");quote(info.runtime);printf(",\"variant\":");quote(info.variant);printf(",\"packageDigest\":");quote(info.package_digest);printf(",\"manifestDigest\":");quote(info.manifest_digest);printf(",\"executor\":%s,\"associationRequired\":%s,\"authorizationGranted\":false,\"publisherAuthenticated\":false,\"protocols\":[",info.executor?"true":"false",info.association_required?"true":"false");for(size_t i=0;i<info.protocol_count;i++)printf("%s%u",i?",":"",info.protocols[i]);printf("],\"optionalMissing\":[");for(size_t i=0;i<info.optional_missing_count;i++){if(i)putchar(',');quote(info.optional_missing[i]);}putchar(']');}puts("}");return 0;
}
