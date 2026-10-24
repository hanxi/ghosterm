#define GHOSTERM_RESOURCE_HOOK_MAIN animation_fixture_prior_main
#include "../resource-hooks-probe.c"
static void animation_save(const char *dir,const char *name,const char *stage,const char *ext,const void *data,size_t n) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-%s.%s",dir,name,stage,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(data,1,n,f)==n);REQUIRE(fclose(f)==0);
}
static void animation_state(const char *dir,const char *name,const char *stage,GhosttyTerminal t) {
 Bytes b=capture(t);animation_save(dir,name,stage,"canonical",b.data,b.len);free(b.data);size_t n=0;
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,NULL,0,&n)==5);uint8_t *data=malloc(n);REQUIRE(data);
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,data,n,&n)==0);animation_save(dir,name,stage,"resources",data,n);free(data);
}
static void animation_make(GhosttyTerminal t,bool animated) {
 feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");if(!animated)return;
 feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=40,q=2;AP8A/w==\x1b\\\x1b_Ga=f,i=7,f=32,s=1,v=1,z=60,q=2;AAD//w==\x1b\\\x1b_Ga=a,i=7,r=1,z=20,c=1,s=3,q=2\x1b\\");
}
static void animation_step(const char *dir,const char *name,unsigned step,GhosttyTerminal t,uint64_t at,const char *output) {
 char stage[64];uint8_t record[128];size_t n=9;
 if(output) {record[0]=1;n=strlen(output)+1;REQUIRE(n<=sizeof record);memcpy(record+1,output,n-1);feed(t,output);}
 else {record[0]=0;for(unsigned i=0;i<8;i++)record[i+1]=(uint8_t)(at>>(8*i));uint64_t delay=0;REQUIRE(ghostty_terminal_p0_animation_tick(t,at,&delay)==GHOSTTY_SUCCESS);}
 REQUIRE(snprintf(stage,sizeof stage,"step%u",step)<(int)sizeof stage);animation_save(dir,name,stage,"event",record,n);
 REQUIRE(snprintf(stage,sizeof stage,"after%u",step)<(int)sizeof stage);animation_state(dir,name,stage,t);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *names[]={"running","stopped","finite","loading","gapless","static","screens","continuation"};
 for(unsigned i=0;i<8;i++) {
  GhosttyTerminal t=new_terminal();configure_images(t,argv[1]);Replies replies={0};enable_replies(t,&replies);animation_make(t,i!=5);
  if(i==1)feed(t,"\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\");
  if(i==2)feed(t,"\x1b_Ga=a,i=7,v=2,q=2\x1b\\");
  if(i==3)feed(t,"\x1b_Ga=a,i=7,s=2,q=2\x1b\\");
  if(i==4)feed(t,"\x1b_Ga=a,i=7,r=1,z=-1,q=2\x1b\\\x1b_Ga=a,i=7,r=2,z=-1,q=2\x1b\\");
  if(i==6) {feed(t,"\x1b[?1049h");animation_make(t,true);}
  if(i==7)feed(t,"\x1b[1;");
  uint64_t delay=0;REQUIRE(ghostty_terminal_p0_animation_tick(t,0,&delay)==GHOSTTY_SUCCESS);animation_state(argv[1],names[i],"initial",t);
  unsigned step=1;animation_step(argv[1],names[i],step++,t,20,NULL);
  if(i==7)animation_step(argv[1],names[i],step++,t,0,"32mZ");
  animation_step(argv[1],names[i],step++,t,60,NULL);
  if(i==6)animation_step(argv[1],names[i],step++,t,0,"\x1b[?1049l");
  animation_step(argv[1],names[i],step++,t,120,NULL);
  REQUIRE(replies.calls==0);require_no_semantic_error(t);ghostty_terminal_free(t);printf("PASS animation fixture/%s\n",names[i]);
 }
 return 0;
}
