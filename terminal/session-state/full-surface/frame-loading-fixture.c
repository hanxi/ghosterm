#define GHOSTERM_RESOURCE_HOOK_MAIN frame_fixture_prior_main
#include "../resource-hooks-probe.c"
static void frame_save(const char *dir,const char *name,const char *stage,const char *ext,const void *data,size_t n) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-%s.%s",dir,name,stage,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(data,1,n,f)==n);REQUIRE(fclose(f)==0);
}
static void frame_state(const char *dir,const char *name,const char *stage,GhosttyTerminal t) {
 Bytes b=capture(t);frame_save(dir,name,stage,"canonical",b.data,b.len);free(b.data);size_t n=0;
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,NULL,0,&n)==5);uint8_t *data=malloc(n);REQUIRE(data);
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,data,n,&n)==0);frame_save(dir,name,stage,"resources",data,n);free(data);
}
static void frame_step(const char *dir,const char *name,unsigned *step,GhosttyTerminal t,const char *output,uint64_t at) {
 uint8_t event[1024];size_t n=9;char stage[64];
 if(output) {event[0]=1;n=strlen(output)+1;REQUIRE(n<=sizeof event);memcpy(event+1,output,n-1);feed(t,output);}
 else {event[0]=0;for(unsigned i=0;i<8;i++)event[i+1]=(uint8_t)(at>>(8*i));uint64_t delay=0;REQUIRE(ghostty_terminal_p0_animation_tick(t,at,&delay)==GHOSTTY_SUCCESS);}
 REQUIRE(snprintf(stage,sizeof stage,"step%u",*step)<(int)sizeof stage);frame_save(dir,name,stage,"event",event,n);
 REQUIRE(snprintf(stage,sizeof stage,"after%u",*step)<(int)sizeof stage);frame_state(dir,name,stage,t);(*step)++;
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *names[]={"append","number","edit","stale","tick-after","missing","blend","overwrite","background","png","zlib","quiet","continuation","negative-gap","static"};
 for(unsigned index=0;index<15;index++) {
  const char *name=names[index];bool number=index==1,edit=index==2,stale=index==3,tick_after=index==4,missing=index==5;
  bool blend=index==6,overwrite=index==7,background=index==8,png=index==9,zlib=index==10,quiet=index==11,continuation=index==12,negative=index==13,control=index==14;
  GhosttyTerminal t=new_terminal();configure_images(t,argv[1]);uint32_t id=number?1:7;
  if(number)feed(t,"\x1b_Ga=T,I=9,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  else if(background)feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=2,v=1,q=2;/wAA//8AAP8=\x1b\\");
  else if(png||zlib)feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;AP8A/w==\x1b\\");
  else feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  if(edit||stale||tick_after)feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=40,q=2;AP8A/w==\x1b\\");
  if(edit)feed(t,"\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\");
  uint64_t delay=0,limit=1024*1024;
  if(stale||tick_after) {feed(t,"\x1b_Ga=a,i=7,r=1,z=20,c=1,s=3,q=2\x1b\\");REQUIRE(ghostty_terminal_p0_animation_tick(t,0,&delay)==GHOSTTY_SUCCESS);}
  Replies replies={0};enable_replies(t,&replies);char command[512],tail[512]="\x1b_Gm=0;/w==\x1b\\";
  if(!control) {
   if(png) {
    REQUIRE(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void*)ghostty_p0_decode_png)==GHOSTTY_SUCCESS);
    char first[256],last[256];base64(png_pixel,3,first,sizeof first);base64(png_pixel+3,sizeof png_pixel-3,last,sizeof last);
    REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=f,i=7,f=100,z=77,q=0,m=1;%s\x1b\\",first)<(int)sizeof command);
    REQUIRE(snprintf(tail,sizeof tail,"\x1b_Gm=0;%s\x1b\\",last)<(int)sizeof tail);
   } else if(zlib) {strcpy(command,"\x1b_Ga=f,i=7,f=32,s=1,v=1,o=z,z=77,q=0,m=1;eJz7\x1b\\");strcpy(tail,"\x1b_Gm=0;z8DwHwAE/wH/\x1b\\");}
   else {
    REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=f,%s,f=32,s=1,v=1,z=%d,q=%u,m=1%s%s%s;AAD/\x1b\\",number?"I=9":"i=7",negative?-7:77,quiet?2u:0u,edit?",r=2":"",(blend||overwrite)?",c=1":"",background?",x=1,X=1,Y=16711935":overwrite?",X=1":"")<(int)sizeof command);
    if(blend||overwrite)strcpy(tail,"\x1b_Gm=0;gA==\x1b\\");
   }
   feed(t,command);REQUIRE(replies.calls==0);
   if(stale)REQUIRE(ghostty_terminal_p0_animation_tick(t,20,&delay)==GHOSTTY_SUCCESS);
   if(missing) {limit=1;REQUIRE(ghostty_terminal_set(t,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&limit)==GHOSTTY_SUCCESS);REQUIRE(!image7(t));}
   if(continuation) {feed(t,"\x1b_Gm=");strcpy(tail,"0;/w==\x1b\\");}
  }
  frame_state(argv[1],name,"initial",t);unsigned step=1;
  if(control)frame_step(argv[1],name,&step,t,"X",0);
  else {
   if(tick_after)frame_step(argv[1],name,&step,t,NULL,20);
   frame_step(argv[1],name,&step,t,tail,0);REQUIRE(replies.calls==(quiet?0u:1u));
   if(stale||tick_after||missing) {
    const char *expected="\x1b_Gi=7;ENOENT: image not found\x1b\\";REQUIRE(replies.len==strlen(expected) && memcmp(replies.data,expected,replies.len)==0);
    if(!missing)frame_step(argv[1],name,&step,t,NULL,60);
   } else {
    REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=a,i=%u,c=2,s=1,q=2\x1b\\",id)<(int)sizeof command);frame_step(argv[1],name,&step,t,command,0);
    if(!negative) {
     REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=a,i=%u,r=1,z=20,s=3,q=2\x1b\\",id)<(int)sizeof command);frame_step(argv[1],name,&step,t,command,0);
     frame_step(argv[1],name,&step,t,NULL,0);frame_step(argv[1],name,&step,t,NULL,77);
    }
   }
  }
  uint8_t meta[16];for(unsigned i=0;i<8;i++) {meta[i]=(uint8_t)(limit>>(8*i));meta[i+8]=(uint8_t)((uint64_t)(step-1)>>(8*i));}
  frame_save(argv[1],name,"manifest","bin",meta,sizeof meta);require_no_semantic_error(t);ghostty_terminal_free(t);printf("PASS frame fixture/%s\n",name);
 }
 return 0;
}
