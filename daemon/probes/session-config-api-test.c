#define _DEFAULT_SOURCE
#include <ghostty.h>
#include "gui-budget.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {const ghostty_p0_allocator_s *base;size_t calls,live,freed;} fail_second;
static uint8_t *fail_alloc(void *ctx,size_t n,uint8_t a,uintptr_t r){fail_second *f=ctx;if(++f->calls==2)return NULL;uint8_t *p=f->base->vtable->alloc(f->base->ctx,n,a,r);if(p)f->live++;return p;}
static bool fail_resize(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r){(void)ctx;(void)p;(void)n;(void)a;(void)to;(void)r;return false;}
static uint8_t *fail_remap(void *ctx,uint8_t *p,size_t n,uint8_t a,size_t to,uintptr_t r){(void)ctx;(void)p;(void)n;(void)a;(void)to;(void)r;return NULL;}
static void fail_free(void *ctx,uint8_t *p,size_t n,uint8_t a,uintptr_t r){fail_second *f=ctx;REQUIRE(f->live);f->base->vtable->free(f->base->ctx,p,n,a,r);f->live--;f->freed++;}
static const ghostty_p0_allocator_vtable_s fail_vtable={fail_alloc,fail_resize,fail_remap,fail_free};
static void value(ghostty_config_t c,const char *mode){ghostty_config_color_s color={0};const char *title=NULL;REQUIRE(ghostty_config_get(c,&color,"background",10));if(!strcmp(mode,"default")){REQUIRE(color.r==0x28&&color.g==0x2c&&color.b==0x34);return;}REQUIRE(color.r==0xab&&color.g==0xcd&&color.b==0xef);REQUIRE(ghostty_config_get(c,&title,"title",5));if(!strcmp(mode,"edge")){REQUIRE(title&&strlen(title)==4086);for(size_t i=0;i<4086;i++)REQUIRE(title[i]=='x');}else REQUIRE(title&&!strcmp(title,"owned config allocation"));}
int main(int argc,char **argv){REQUIRE(argc==3&&!ghostty_init(0,NULL));if(!strcmp(argv[2],"null")){REQUIRE(!ghostty_p0_config_new(NULL)&&!ghostty_p0_config_clone(NULL,NULL)&&!ghostty_p0_config_load_file(NULL,NULL)&&!ghostty_p0_config_finalize(NULL));return 0;}const size_t mib=1024*1024;p0_gui_pool *pool=NULL;p0_gui_reservation *root=NULL,*copy=NULL;p0_gui_limits limits={.total=128*mib,.general=8*mib,.pages=8*mib,.wire=8*mib,.surfaces=1};REQUIRE(p0_gui_pool_create(limits,&pool)==P0_GUI_OK);REQUIRE(p0_gui_reserve(pool,!strcmp(argv[2],"load-capacity")?mib:8*mib,&root)==P0_GUI_OK);const ghostty_p0_allocator_s *host=p0_gui_reserved_allocator(root);
ghostty_config_t config=NULL;
if(!strcmp(argv[2],"ctor-second")){fail_second failure={.base=host};ghostty_p0_allocator_s selected={&failure,&fail_vtable};REQUIRE(!ghostty_p0_config_new(&selected));REQUIRE(failure.calls==2 && !failure.live && failure.freed==1 && p0_gui_reserved_idle(root));goto cleanup;}

REQUIRE(!ghostty_p0_config_new(NULL));REQUIRE(!ghostty_p0_config_clone(NULL,host));REQUIRE(!ghostty_p0_config_finalize(NULL));REQUIRE(!ghostty_p0_config_load_file(NULL,argv[1]));
config=ghostty_p0_config_new(host);REQUIRE(config&&!p0_gui_reserved_idle(root));REQUIRE(p0_gui_unreserve(root)==P0_GUI_BUSY);REQUIRE(!ghostty_p0_config_clone(config,NULL));REQUIRE(!ghostty_p0_config_load_file(config,NULL));REQUIRE(!ghostty_p0_config_load_file(config,"relative.conf"));ghostty_config_free(config);REQUIRE(p0_gui_reserved_idle(root));config=ghostty_p0_config_new(host);REQUIRE(config);
if(!strcmp(argv[2],"finalize-invalid")){ghostty_config_load_file(config,argv[1]);REQUIRE(!ghostty_p0_config_finalize(config));goto cleanup;}
bool valid=strcmp(argv[2],"invalid")!=0 && strcmp(argv[2],"load-capacity")!=0;REQUIRE(ghostty_p0_config_load_file(config,argv[1])==valid);
if(!strcmp(argv[2],"load-capacity"))REQUIRE(!p0_gui_reserved_healthy(root));
if(valid){REQUIRE(ghostty_p0_config_finalize(config));value(config,argv[2]);if(!strcmp(argv[2],"capacity")){p0_gui_reservation *failed=NULL;REQUIRE(p0_gui_reserve(pool,(size_t)sysconf(_SC_PAGESIZE),&failed)==P0_GUI_OK);REQUIRE(!ghostty_p0_config_new(p0_gui_reserved_allocator(failed)));REQUIRE(!p0_gui_reserved_healthy(failed)&&p0_gui_reserved_idle(failed));REQUIRE(p0_gui_unreserve(failed)==P0_GUI_OK);REQUIRE(p0_gui_reserved_healthy(root));value(config,argv[2]);}
if(!strcmp(argv[2],"public-clone")){ghostty_config_t clone=ghostty_config_clone(config);REQUIRE(clone);ghostty_config_free(config);config=NULL;REQUIRE(!p0_gui_reserved_idle(root)&&p0_gui_unreserve(root)==P0_GUI_BUSY);value(clone,argv[2]);ghostty_config_free(clone);}else {
REQUIRE(p0_gui_reserve(pool,8*mib,&copy)==P0_GUI_OK);ghostty_config_t clone=ghostty_p0_config_clone(config,p0_gui_reserved_allocator(copy));REQUIRE(clone&&!p0_gui_reserved_idle(copy));ghostty_config_free(config);config=NULL;REQUIRE(p0_gui_reserved_idle(root)&&p0_gui_unreserve(root)==P0_GUI_OK);root=NULL;value(clone,argv[2]);ghostty_config_free(clone);REQUIRE(p0_gui_reserved_idle(copy)&&p0_gui_unreserve(copy)==P0_GUI_OK);}}
cleanup:
if(config)ghostty_config_free(config);if(root)REQUIRE(p0_gui_reserved_idle(root)&&p0_gui_unreserve(root)==P0_GUI_OK);p0_gui_usage u=p0_gui_pool_read(pool);REQUIRE(!u.live&&!u.committed&&!u.reservations);REQUIRE(p0_gui_pool_close(pool)==P0_GUI_OK);printf("PASS process/config API/%s: explicit result and complete owner reclamation\n",argv[2]);return 0;}
