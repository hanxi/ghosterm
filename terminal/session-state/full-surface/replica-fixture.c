/* Synthetic transport containers for the native Surface bridge. No user data. */
#include "import-fixture.h"
#include "checkpoint-wire.h"
#include <ghostty/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL replica fixture:%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void save(const char *directory,const char *name,const void *bytes,size_t n){char path[2048];CHECK(snprintf(path,sizeof(path),"%s/%s",directory,name)>0);FILE *f=fopen(path,"wb");CHECK(f && fwrite(bytes,1,n,f)==n && !fclose(f));}
static void container(const char *directory,const char *name,import_parts p){
  GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal terminal=NULL;CHECK(ghostty_snapshot_decoder_new_buf(NULL,&decoder,p.canonical,p.canonical_len)==GHOSTTY_SUCCESS);bool retain=true;size_t limit=1024*1024;CHECK(ghostty_snapshot_decoder_set(decoder,GHOSTTY_SNAPSHOT_DECODER_OPT_RETAIN_CONTINUATION,&retain)==GHOSTTY_SUCCESS && ghostty_snapshot_decoder_set(decoder,GHOSTTY_SNAPSHOT_DECODER_OPT_MAX_CONTINUATION_BYTES,&limit)==GHOSTTY_SUCCESS && ghostty_snapshot_decoder_decode(decoder,&terminal)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
  GhosttyColorRgb bg,fg;CHECK(ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND_DEFAULT,&bg)==GHOSTTY_SUCCESS && ghostty_terminal_get(terminal,GHOSTTY_TERMINAL_DATA_COLOR_FOREGROUND_DEFAULT,&fg)==GHOSTTY_SUCCESS);ghostty_terminal_free(terminal);
  gt_vt_snapshot_view_t v={.canonical=p.canonical,.canonical_bytes=p.canonical_len,.resources=p.resources,.resource_bytes=p.resources_len,.profile=GT_VT_PROFILE_INLINE,.cell_width=10,.cell_height=20,.history_bytes=1024*1024,.history_lines=1000,.continuation_bytes=1024*1024,.image_bytes=1024*1024,.light={{bg.r,bg.g,bg.b},{fg.r,fg.g,fg.b}},.dark={{bg.r,bg.g,bg.b},{fg.r,fg.g,fg.b}}};size_t n;CHECK(gt_vt_snapshot_size(&v,GT_VT_SNAPSHOT_LIMIT,&n)==GT_OK);uint8_t *out=malloc(n);CHECK(out && gt_vt_snapshot_write(&v,out,n,&n)==GT_OK);save(directory,name,out,n);free(out);
}
int main(int argc,char **argv){CHECK(argc==3);import_fixture fixture;char mode[64];CHECK(snprintf(mode,sizeof(mode),"container-%s",argv[1])>0);import_fixture_open(&fixture,mode);container(argv[2],"initial.gtvc",fixture.initial);container(argv[2],"final.gtvc",fixture.final);save(argv[2],"delta.bin",fixture.delta,fixture.delta_len);import_fixture_close();puts("PASS synthetic replica checkpoint containers");return 0;}
