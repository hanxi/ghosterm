#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN pending_fixture_prior_main
#include "../resource-snapshot-probe.c"
static const uint8_t pending_red[4]={255,0,0,255};
static uint64_t pending_begin(GhosttyTerminal t,bool rgb,uint32_t number) {
 uint64_t token=0;REQUIRE(ghostty_terminal_p0_pending_begin(t,7,number,1,1,rgb?3:4,&token)==0 && token!=0);return token;
}
static void pending_save(const char *dir,const char *name,const char *stage,const char *ext,const void *data,size_t n) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-%s.%s",dir,name,stage,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(data,1,n,f)==n);REQUIRE(fclose(f)==0);
}
static void pending_state(const char *dir,const char *name,const char *stage,GhosttyTerminal t) {
 Bytes b=capture(t);pending_save(dir,name,stage,"canonical",b.data,b.len);free(b.data);
 b=resource_capture(t);pending_save(dir,name,stage,"resources",b.data,b.len);free(b.data);
}
static void pending_word(uint8_t *p,uint64_t n) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(8*i));}
static void pending_step(const char *dir,const char *name,unsigned *step,GhosttyTerminal t,uint8_t kind,uint64_t token,const char *output,bool numbered) {
 uint8_t event[256]={0};event[0]=kind;size_t n=10;bool applied=false;
 if(kind==1) {n=strlen(output)+1;REQUIRE(n<=sizeof event);memcpy(event+1,output,n-1);feed(t,output);}
 else if(kind==2) {token=pending_begin(t,false,numbered?9:0);pending_word(event+1,token);event[9]=numbered;}
 else {
  if(kind==3) {REQUIRE(ghostty_terminal_p0_pending_ready(t,7,token,pending_red,4,&applied)==0);memcpy(event+10,pending_red,4);n=14;}
  else {REQUIRE(kind==4);REQUIRE(ghostty_terminal_p0_pending_cancel(t,7,token,&applied)==0);}
  pending_word(event+1,token);event[9]=applied;
 }
 char stage[64];REQUIRE(snprintf(stage,sizeof stage,"step%u",*step)<(int)sizeof stage);pending_save(dir,name,stage,"event",event,n);
 REQUIRE(snprintf(stage,sizeof stage,"after%u",*step)<(int)sizeof stage);pending_state(dir,name,stage,t);(*step)++;
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *names[]={"primary-ready","primary-cancel","alternate-ready","alternate-cancel","both","frame","active"};
 for(unsigned i=0;i<7;i++) {
  const char *name=names[i];bool alt=i==2||i==3,cancel=i==1||i==3;
  GhosttyTerminal t=new_terminal();configure_images(t,argv[1]);uint64_t seed=pending_begin(t,false,0);bool removed=false;
  REQUIRE(ghostty_terminal_p0_pending_cancel(t,7,seed,&removed)==0 && removed);
  uint64_t primary=pending_begin(t,false,0);feed(t,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");
  if(i==5)feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=77,q=2,m=1;AAD/\x1b\\");
  feed(t,"\x1b[?47h");uint64_t alternate=pending_begin(t,false,9);REQUIRE(primary!=alternate);
  feed(t,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");if(alt)feed(t,"\x1b[?47l");
  pending_state(argv[1],name,"initial",t);unsigned step=1;uint64_t token=alt||i==6?alternate:primary;
  pending_step(argv[1],name,&step,t,cancel?4:3,token,NULL,false);
  if(cancel)pending_step(argv[1],name,&step,t,3,token,NULL,false);
  if(i==4)pending_step(argv[1],name,&step,t,3,alternate,NULL,false);
  const char *output=alt||i==6?"\x1b[?47h":"\x1b[?47l";
  if(i==5)output="\x1b[?47l\x1b_Gm=0;/w==\x1b\\\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\";
  pending_step(argv[1],name,&step,t,1,0,output,false);
  uint8_t meta[16];pending_word(meta,resource_cap);pending_word(meta+8,step-1);pending_save(argv[1],name,"manifest","bin",meta,16);
  require_no_semantic_error(t);ghostty_terminal_free(t);printf("PASS pending screen fixture/%s\n",name);
 }
 return 0;
}
