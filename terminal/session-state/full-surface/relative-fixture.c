/* Authoritative headless graph, independently checked before GUI construction. */
#define GHOSTERM_RELATIVE_RESOURCE_MAIN relative_probe_prior_main
#include "../relative-resource-probe.c"
static void save_relative(const char *dir,const char *name,const char *ext,Bytes b) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s.%s",dir,name,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(b.data,1,b.len,f)==b.len);REQUIRE(fclose(f)==0);
}
static Bytes relative_resources(GhosttyTerminal t) {
 Bytes b={0};REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,NULL,0,&b.len)==5);
 b.data=malloc(b.len);REQUIRE(b.data);REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,b.data,b.len,&b.len)==0);return b;
}
static void generate_relative(const char *dir,const char *name) {
 GhosttyTerminal t=new_terminal();configure_images(t,dir);char command[512];
 unsigned root=strcmp(name,"forward")==0?90:strcmp(name,"internal")==0?0:9;
 REQUIRE(snprintf(command,sizeof command,"\x1b[3;5H\x1b_Ga=T,i=7,p=%u,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\",root)<(int)sizeof command);feed(t,command);
 if(strcmp(name,"static")!=0) {
  REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=p,i=7,p=10,P=7,Q=%u,H=2,V=3,q=2\x1b\\",root)<(int)sizeof command);feed(t,command);
  require_relative_position(t,10,6,5);
 }
 if(strcmp(name,"chain")==0 || strcmp(name,"forward")==0) {
  feed(t,"\x1b_Ga=p,i=7,p=5,P=7,Q=10,H=1,V=-1,q=2\x1b\\");require_relative_position(t,5,7,4);
 }
 Bytes initial=capture(t),resources=relative_resources(t);save_relative(dir,name,"initial",initial);save_relative(dir,name,"resources",resources);
 REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=d,d=i,i=7,p=%u,q=2\x1b\\",root)<(int)sizeof command);
 save_relative(dir,name,"delta",(Bytes){.data=(uint8_t*)command,.len=strlen(command)});feed(t,command);
 Bytes final=capture(t),after=relative_resources(t);save_relative(dir,name,"final",final);save_relative(dir,name,"after",after);
 free(initial.data);free(resources.data);free(final.data);free(after.data);ghostty_terminal_free(t);
 printf("PASS full relative fixture/%s\n",name);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *names[]={"static","relative","chain","forward","internal"};
 for(size_t i=0;i<sizeof names/sizeof names[0];i++)generate_relative(argv[1],names[i]);return 0;
}
