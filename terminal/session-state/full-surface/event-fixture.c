/* Authority states for ordered output/resize events, produced without any GUI. */
#define GHOSTERM_RESOURCE_HOOK_MAIN event_hooks_prior_main
#include "../resource-hooks-probe.c"
static const char event_on[]="\x1b[?2004hMODE-ON";
static const char event_off[]="\x1b[?2004lMODE-OFF";
static void save_event(const char *dir,const char *name,unsigned stage,const char *ext,const void *data,size_t len) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-%u.%s",dir,name,stage,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(data,1,len,f)==len);REQUIRE(fclose(f)==0);
}
static void save_event_state(const char *dir,const char *name,unsigned stage,GhosttyTerminal t) {
 Bytes b=capture(t);save_event(dir,name,stage,"canonical",b.data,b.len);free(b.data);
 size_t n=0;REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,NULL,0,&n)==5);
 uint8_t *data=malloc(n);REQUIRE(data);REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,data,n,&n)==0);
 save_event(dir,name,stage,"resources",data,n);free(data);
}
static void generate_event(const char *dir,const char *name) {
 GhosttyTerminal t=new_terminal();configure_images(t,dir);
 feed(t,"abcdefghijklmnopqrstuvwx0123456789abcdefghijklmnopqrstuvwx0123456789abcdefghijklmnopqrstuvwx0123456789\r\n");
 feed(t,"\x1b[3;5H\x1b_Ga=T,i=7,p=9,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\\x1b_Ga=p,i=7,p=10,P=7,Q=9,H=2,V=3,q=2\x1b\\");
 save_event_state(dir,name,0,t);unsigned stage=0;
 if(strcmp(name,"output")!=0) {
  REQUIRE(ghostty_terminal_resize(t,51,17,8,16)==GHOSTTY_SUCCESS);save_event_state(dir,name,++stage,t);
 }
 if(strcmp(name,"resize")!=0) {feed(t,event_on);save_event_state(dir,name,++stage,t);}
 if(strcmp(name,"output")!=0) {
  REQUIRE(ghostty_terminal_resize(t,90,30,8,16)==GHOSTTY_SUCCESS);save_event_state(dir,name,++stage,t);
 }
 if(strcmp(name,"resize")!=0) {feed(t,event_off);save_event_state(dir,name,++stage,t);}
 ghostty_terminal_free(t);printf("PASS ordered event fixture/%s\n",name);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);generate_event(argv[1],"resize");generate_event(argv[1],"output");generate_event(argv[1],"mixed");return 0;
}
