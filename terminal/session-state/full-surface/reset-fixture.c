#define GHOSTERM_RESOURCE_HOOK_MAIN reset_hooks_prior_main
#include "../resource-hooks-probe.c"
static void save_reset(const char *dir,const char *name,const char *stage,const char *ext,const void *data,size_t len) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-%s.%s",dir,name,stage,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(data,1,len,f)==len);REQUIRE(fclose(f)==0);
}
static void reset_state(const char *dir,const char *name,const char *stage,GhosttyTerminal t) {
 Bytes b=capture(t);save_reset(dir,name,stage,"canonical",b.data,b.len);free(b.data);
 size_t n=0;REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,NULL,0,&n)==5);uint8_t *data=malloc(n);REQUIRE(data);
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,data,n,&n)==0);save_reset(dir,name,stage,"resources",data,n);free(data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *names[]={"reset","continuation"};
 for(unsigned i=0;i<2;i++) {
  GhosttyTerminal t=new_terminal();configure_images(t,argv[1]);
  feed(t,"\x1b[?2004h\x1b[3;5H\x1b_Ga=T,i=7,p=9,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\BEFORE");
  if(i)feed(t,"\x1b[1;");reset_state(argv[1],names[i],"initial",t);
  ghostty_terminal_reset(t);REQUIRE(!image7(t));reset_state(argv[1],names[i],"reset",t);
  if(i) {feed(t,"32mZ");reset_state(argv[1],names[i],"final",t);}
  ghostty_terminal_free(t);printf("PASS reset fixture/%s\n",names[i]);
 }
 return 0;
}
