#define GHOSTERM_RESOURCE_HOOK_MAIN clear_hooks_prior_main
#include "../resource-hooks-probe.c"
static void clear_save(const char *dir,const char *name,const char *stage,const char *ext,const void *data,size_t len) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-%s.%s",dir,name,stage,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(data,1,len,f)==len);REQUIRE(fclose(f)==0);
}
static void clear_state(const char *dir,const char *name,const char *stage,GhosttyTerminal t) {
 Bytes b=capture(t);clear_save(dir,name,stage,"canonical",b.data,b.len);free(b.data);size_t n=0;
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,NULL,0,&n)==5);uint8_t *data=malloc(n);REQUIRE(data);
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,data,n,&n)==0);clear_save(dir,name,stage,"resources",data,n);free(data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *names[]={"normal","prompt","alternate","keep-history"};
 for(unsigned i=0;i<4;i++) {
  GhosttyTerminal t=new_terminal();configure_images(t,argv[1]);feed(t,"\x1b[?2004h");
  if(i==3)feed(t,"\x1b_Ga=T,i=8,p=10,f=32,s=1,v=1,q=2;AP8A/w==\x1b\\");
  for(unsigned row=0;row<70;row++)feed(t,"HISTORY-LINE\r\n");
  if(i==2)feed(t,"\x1b[?1049h");
  feed(t,"\x1b[10;1HKEEP-BELOW\x1b[3;5H\x1b_Ga=T,i=7,p=9,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  if(i==1) {feed(t,"\x1b]133;A\x1b\\PROMPT>");bool prompt=false;REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_CURSOR_AT_PROMPT,&prompt)==GHOSTTY_SUCCESS && prompt);}
  clear_state(argv[1],names[i],"initial",t);Replies replies={0};enable_replies(t,&replies);uint32_t outcome=99;
  REQUIRE(ghostty_terminal_p0_clear(t,i!=3,&outcome)==GHOSTTY_SUCCESS);REQUIRE(outcome==(i==1?2u:i==2?0u:1u));
  REQUIRE(replies.calls==(i==1?1u:0u));REQUIRE(replies.len==(i==1?1u:0u));if(i==1)REQUIRE(replies.data[0]==0x0c);
  if(i==0 || i==3)REQUIRE(!image7(t));else if(i==2)REQUIRE(image7(t));
  if(i==3) {
   GhosttyKittyGraphics graphics=NULL;
   REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)==GHOSTTY_SUCCESS);
   REQUIRE(ghostty_kitty_graphics_image(graphics,8)!=NULL);
  }
  clear_state(argv[1],names[i],"after",t);clear_save(argv[1],names[i],"outcome","bin",&outcome,sizeof outcome);
  ghostty_terminal_free(t);printf("PASS clear fixture/%s: owner FF calls=%u\n",names[i],replies.calls);
 }
 return 0;
}
