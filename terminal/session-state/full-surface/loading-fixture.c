#define GHOSTERM_RESOURCE_HOOK_MAIN loading_fixture_prior_main
#include "../resource-hooks-probe.c"
static void loading_save(const char *dir,const char *name,const char *stage,const char *ext,const void *data,size_t n) {
 char path[4096];REQUIRE(snprintf(path,sizeof path,"%s/%s-%s.%s",dir,name,stage,ext)<(int)sizeof path);
 FILE *f=fopen(path,"wb");REQUIRE(f);REQUIRE(fwrite(data,1,n,f)==n);REQUIRE(fclose(f)==0);
}
static void loading_state(const char *dir,const char *name,const char *stage,GhosttyTerminal t) {
 Bytes b=capture(t);loading_save(dir,name,stage,"canonical",b.data,b.len);free(b.data);size_t n=0;
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,NULL,0,&n)==5);uint8_t *data=malloc(n);REQUIRE(data);
 REQUIRE(ghostty_terminal_p0_resources_encode(t,1024*1024,data,n,&n)==0);loading_save(dir,name,stage,"resources",data,n);free(data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);const char *names[]={"direct","quiet","no-display","zlib","png","continuation","screens","overwrite","static","coexist","implicit"};
 for(unsigned i=0;i<11;i++) {
  GhosttyTerminal t=new_terminal();configure_images(t,argv[1]);Replies replies={0};enable_replies(t,&replies);
  feed(t,"\x1b[?2004h");
  if(i==7 || i==8)feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  if(i==9)feed(t,"\x1b_Ga=T,i=8,p=10,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
  char tail[512]="\x1b_Gm=0;/w==\x1b\\";
  if(i==8)strcpy(tail,"A");
  else if(i==10)feed(t,"\x1b_Ga=T,C=1,f=32,s=1,v=1,q=0,m=1;/wAA\x1b\\");
  else if(i==4) {
   REQUIRE(ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG,(const void*)ghostty_p0_decode_png)==GHOSTTY_SUCCESS);
   char first[256],last[256],command[512];base64(png_pixel,3,first,sizeof first);base64(png_pixel+3,sizeof png_pixel-3,last,sizeof last);
   REQUIRE(snprintf(command,sizeof command,"\x1b_Ga=T,i=7,p=9,C=1,f=100,q=2,m=1;%s\x1b\\",first)<(int)sizeof command);feed(t,command);
   REQUIRE(snprintf(tail,sizeof tail,"\x1b_Gm=0;%s\x1b\\",last)<(int)sizeof tail);
  } else if(i==3) {feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,o=z,q=2,m=1;eJz7\x1b\\");strcpy(tail,"\x1b_Gm=0;z8DwHwAE/wH/\x1b\\");}
  else if(i==2)feed(t,"\x1b_Ga=t,i=7,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
  else if(i==1 || i==6)feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
  else if(i==7)feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;AP8A\x1b\\");
  else feed(t,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=0,m=1;/wAA\x1b\\");
  if(i==5) {feed(t,"\x1b_Gm=");strcpy(tail,"0;/w==\x1b\\");}
  if(i==6)feed(t,"\x1b[?1049h\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;AP8A\x1b\\");
  REQUIRE((image7(t)!=NULL)==(i==8));REQUIRE(replies.calls==0);
  loading_state(argv[1],names[i],"initial",t);loading_save(argv[1],names[i],"step1","output",tail,strlen(tail));feed(t,tail);
  GhosttyKittyGraphicsImage final_image=image7(t);
  if(i==10) {GhosttyKittyGraphics graphics=NULL;REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)==GHOSTTY_SUCCESS);final_image=ghostty_kitty_graphics_image(graphics,2147483647u);}
  REQUIRE(final_image);const uint8_t *pixels=NULL;size_t n=0;const uint8_t red[]={255,0,0,255},green[]={0,255,0,255};
  REQUIRE(ghostty_kitty_graphics_image_get(final_image,GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR,&pixels)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_kitty_graphics_image_get(final_image,GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN,&n)==GHOSTTY_SUCCESS && n==4);
  REQUIRE(memcmp(pixels,(i==6||i==7)?green:red,4)==0);loading_state(argv[1],names[i],"after1",t);
  REQUIRE(replies.calls==((i==0||i==5||i==9)?1u:0u));
  if(i==6) {const char *second="\x1b[?1049l\x1b_Gm=0;/w==\x1b\\";loading_save(argv[1],names[i],"step2","output",second,strlen(second));feed(t,second);require_red_pixel(t);loading_state(argv[1],names[i],"after2",t);}
  if(i==10) {const char *second="\x1b_Ga=t,f=32,s=1,v=1,q=0;AP8A/w==\x1b\\";loading_save(argv[1],names[i],"step2","output",second,strlen(second));feed(t,second);GhosttyKittyGraphics graphics=NULL;REQUIRE(ghostty_terminal_get(t,GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,&graphics)==GHOSTTY_SUCCESS);REQUIRE(ghostty_kitty_graphics_image(graphics,2147483648u));REQUIRE(replies.calls==0);loading_state(argv[1],names[i],"after2",t);}
  require_no_semantic_error(t);ghostty_terminal_free(t);printf("PASS loading fixture/%s\n",names[i]);
 }
 return 0;
}
