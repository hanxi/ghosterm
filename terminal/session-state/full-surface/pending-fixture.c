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
 REQUIRE(argc==2);const char *names[]={"ready","cancel","stale","deleted","begin","duplicate","frame","number"};
 for(unsigned i=0;i<8;i++) {
  const char *name=names[i];GhosttyTerminal t=new_terminal();configure_images(t,argv[1]);uint64_t token=0;
  /* Deliberately separate authority identity from a fresh GUI cache generation. */
  uint64_t seed=pending_begin(t,false,0);bool removed=false;
  REQUIRE(ghostty_terminal_p0_pending_cancel(t,7,seed,&removed)==0 && removed);
  if(i!=4) {token=pending_begin(t,false,i==7?9:0);feed(t,"\x1b_Ga=p,i=7,p=9,C=1,q=2\x1b\\");}
  if(i==6)feed(t,"\x1b_Ga=f,i=7,f=32,s=1,v=1,z=77,q=2,m=1;AAD/\x1b\\");
  pending_state(argv[1],name,"initial",t);unsigned step=1;
  if(i==4) {
   pending_step(argv[1],name,&step,t,2,0,NULL,false);
   /* Obtain the authority token from the event just written. */
   char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-step1.event",argv[1],name)<(int)sizeof path);
   FILE *f=fopen(path,"rb");REQUIRE(f);uint8_t event[10];REQUIRE(fread(event,1,10,f)==10 && fclose(f)==0);
   for(unsigned k=0;k<8;k++)token|=(uint64_t)event[k+1]<<(8*k);
  }
  if(i==1)pending_step(argv[1],name,&step,t,4,token,NULL,false);
  if(i==2)pending_step(argv[1],name,&step,t,1,0,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;AAD//w==\x1b\\",false);
  if(i==3)pending_step(argv[1],name,&step,t,1,0,"\x1b_Ga=d,d=I,i=7,q=2\x1b\\",false);
  pending_step(argv[1],name,&step,t,3,token,NULL,false);
  if(i==5) {pending_step(argv[1],name,&step,t,3,token,NULL,false);pending_step(argv[1],name,&step,t,4,token,NULL,false);}
  if(i==6)pending_step(argv[1],name,&step,t,1,0,"\x1b_Gm=0;/w==\x1b\\\x1b_Ga=a,i=7,c=2,s=1,q=2\x1b\\",false);
  uint8_t meta[16];pending_word(meta,resource_cap);pending_word(meta+8,step-1);pending_save(argv[1],name,"manifest","bin",meta,16);
  require_no_semantic_error(t);ghostty_terminal_free(t);printf("PASS pending fixture/%s\n",name);
 }
 return 0;
}
