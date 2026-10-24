/* Experimental fixture: real external I/O finishes before any GUI exists. */
#define GHOSTERM_RESOURCE_HOOK_MAIN resource_hooks_baseline_main
#include "resource-hooks-probe.c"
#undef GHOSTERM_RESOURCE_HOOK_MAIN
static void save(const char *dir,const char *name,const char *ext,const void *p,size_t n) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s.%s",dir,name,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(p,1,n,f)==n);REQUIRE(fclose(f)==0);
}
static void generate(const char *dir,const char *name) {
 char path[4096],encoded[8192],command[9000];
 REQUIRE(snprintf(path,sizeof path,"%s/tty-graphics-protocol-XXXXXX",dir)<(int)sizeof path);
 int fd=mkstemp(path);REQUIRE(fd>=0);
 bool png=strstr(name,"png")!=NULL,failed=strcmp(name,"resource-failure")==0;
 const uint8_t red[]={255,0,0,255};fd_write_all(fd,png?png_pixel:red,png?sizeof png_pixel:sizeof red);REQUIRE(close(fd)==0);
 if(failed)REQUIRE(unlink(path)==0);
 GhosttyTerminal t=new_terminal();configure_images(t,dir);feed(t,"\x1b[?2004h");
 ResourceEvent event={0};REQUIRE(ghostty_terminal_p0_resource_hooks(t,&event,observe_resource,NULL)==GHOSTTY_SUCCESS);
#ifdef GHOSTERM_P0_SOURCE_QUOTA
 bool quota_reject=strcmp(name,"resource-reject")==0;
 uint64_t source_limit=quota_reject?3:1024*1024;
 REQUIRE(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&source_limit)==GHOSTTY_SUCCESS);
#else
 const bool quota_reject=false;
#endif
#ifdef GHOSTERM_P0_STATIC_RESOURCES
 if(strncmp(name,"static-",7)==0) {
   if(strcmp(name,"static-rgb")==0)feed(t,"\x1b[3;5H\x1b_Ga=T,i=7,f=24,s=1,v=1,q=2;/wAA\x1b\\");
   else if(png) {
     char pixel[1024],initial_command[1200];base64(png_pixel,sizeof png_pixel,pixel,sizeof pixel);
     REQUIRE(snprintf(initial_command,sizeof initial_command,"\x1b[3;5H\x1b_Ga=T,i=7,f=100,s=1,v=1,q=2;%s\x1b\\",pixel)<(int)sizeof initial_command);feed(t,initial_command);
   } else feed(t,"\x1b[3;5H\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
   if(strcmp(name,"static-screens")==0)feed(t,"\x1b[?1049h\x1b[8;9H\x1b_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
 }
 size_t resource_len=0;REQUIRE(ghostty_terminal_p0_resources_encode(t,16*1024*1024,NULL,0,&resource_len)==5);
 uint8_t *resources=malloc(resource_len);REQUIRE(resources);
 REQUIRE(ghostty_terminal_p0_resources_encode(t,16*1024*1024,resources,resource_len,&resource_len)==0);
 save(dir,name,"resources",resources,resource_len);free(resources);
#endif
 Bytes initial=capture(t);save(dir,name,"initial",initial.data,initial.len);
 base64((const uint8_t*)path,strlen(path),encoded,sizeof encoded);
 REQUIRE(snprintf(command,sizeof command,"%s\x1b[3;5H\x1b_Ga=T,i=7,f=%u,s=1,v=1,t=t,q=2;%s\x1b\\\x1b[4;6H",strcmp(name,"static-screens")==0?"\x1b[?1049l":"",png?100u:32u,encoded)<(int)sizeof command);
 feed(t,command);REQUIRE(event.observed==1 && event.present && event.status==(failed?1u:0u));
 REQUIRE(!semantic_error(t));REQUIRE(access(path,F_OK)<0 && errno==ENOENT);
 if(!failed && !quota_reject) {require_red_pixel(t);require_placement_at(t,4,2);}
 else REQUIRE(image7(t)==NULL);
 save(dir,name,"delta",command,strlen(command));Bytes final=capture(t);save(dir,name,"final",final.data,final.len);
#ifdef GHOSTERM_P0_STATIC_RESOURCES
 if(strcmp(name,"static-screens")==0) {feed(t,"\x1b[?47h");require_red_pixel(t);require_placement_at(t,8,7);Bytes alternate=capture(t);save(dir,name,"alternate",alternate.data,alternate.len);free(alternate.data);}
#endif
 uint8_t wire[20+sizeof event.path+sizeof event.data];uint32_t fields[]={event.medium,event.status,(uint32_t)event.path_len,(uint32_t)event.data_len};
 for(size_t i=0;i<4;i++)for(size_t j=0;j<4;j++)wire[4*i+j]=(uint8_t)(fields[i]>>(8*j));
 size_t header=16;
#ifdef GHOSTERM_P0_SOURCE_QUOTA
 for(size_t j=0;j<4;j++)wire[16+j]=(uint8_t)(source_limit>>(8*j));header=20;
#endif
 memcpy(wire+header,event.path,event.path_len);memcpy(wire+header+event.path_len,event.data,event.data_len);
 save(dir,name,"event",wire,header+event.path_len+event.data_len);
 free(initial.data);free(final.data);ghostty_terminal_free(t);printf("PASS resource fixture/%s\n",name);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);REQUIRE(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void*)ghostty_p0_decode_png)==GHOSTTY_SUCCESS);
 generate(argv[1],"resource-rgba");generate(argv[1],"resource-png");generate(argv[1],"resource-failure");
#ifdef GHOSTERM_P0_SOURCE_QUOTA
 generate(argv[1],"resource-reject");
#ifdef GHOSTERM_P0_STATIC_RESOURCES
 generate(argv[1],"static-rgba");generate(argv[1],"static-png");generate(argv[1],"static-rgb");generate(argv[1],"static-screens");
#endif
#endif
 return 0;
}
