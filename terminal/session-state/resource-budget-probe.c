/* Raw external I/O success is not the final graphics commit result. */
#define GHOSTERM_RESOURCE_HOOK_MAIN resource_hooks_previous_main
#include "resource-hooks-probe.c"
int main(int argc,char **argv) {
  REQUIRE(argc==3);
  bool divergent=strcmp(argv[1],"divergent")==0;
  bool aligned_accept=strcmp(argv[1],"aligned-accept")==0;
  REQUIRE(divergent || aligned_accept || strcmp(argv[1],"aligned-reject")==0);
  char path[4096],encoded[8192],command[9000];
  REQUIRE(snprintf(path,sizeof path,"%s/tty-graphics-protocol-XXXXXX",argv[2])<(int)sizeof path);
  int fd=mkstemp(path);REQUIRE(fd>=0);
  const uint8_t red[]={255,0,0,255};fd_write_all(fd,red,sizeof red);REQUIRE(close(fd)==0);
  GhosttyTerminal authority=new_terminal();configure_images(authority,argv[2]);
  uint64_t source_limit=aligned_accept?4:3;
  REQUIRE(ghostty_terminal_set(authority,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&source_limit)==GHOSTTY_SUCCESS);
  Bytes initial=capture(authority);GhosttyTerminal replica=restore(initial);configure_images(replica,argv[2]);
  uint64_t replica_limit=divergent?4:source_limit;
  REQUIRE(ghostty_terminal_set(replica,GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,&replica_limit)==GHOSTTY_SUCCESS);
  ResourceEvent event={0};
  REQUIRE(ghostty_terminal_p0_resource_hooks(authority,&event,observe_resource,NULL)==GHOSTTY_SUCCESS);
  REQUIRE(ghostty_terminal_p0_resource_hooks(replica,&event,NULL,resolve_resource)==GHOSTTY_SUCCESS);
  base64((const uint8_t*)path,strlen(path),encoded,sizeof encoded);
  REQUIRE(snprintf(command,sizeof command,"\x1b[3;5H\x1b_Ga=T,i=7,f=32,s=1,v=1,t=t,q=2;%s\x1b\\\x1b[4;6H",encoded)<(int)sizeof command);
  feed(authority,command);
  REQUIRE(event.observed==1 && event.present && event.status==0 && event.data_len==4);
  REQUIRE(access(path,F_OK)<0 && errno==ENOENT);
  feed(replica,command);
  REQUIRE(event.resolved==1 && !event.present);
  REQUIRE(!semantic_error(authority) && !semantic_error(replica));
  require_equal_state(authority,replica);
  if(aligned_accept) {require_red_pixel(authority);require_red_pixel(replica);require_placement_at(authority,4,2);require_placement_at(replica,4,2);}
  else {REQUIRE(image7(authority)==NULL);if(divergent) {require_red_pixel(replica);require_placement_at(replica,4,2);}else REQUIRE(image7(replica)==NULL);}
  free(initial.data);ghostty_terminal_free(replica);ghostty_terminal_free(authority);
  if(divergent) {puts("LIMITATION raw I/O success: source quota rejects image, replica accepts; canonical state and semantic flags still match");return 3;}
  printf("PASS resource-budget/%s\n",argv[1]);return 0;
}
