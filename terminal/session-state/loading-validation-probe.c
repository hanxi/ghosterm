#define GHOSTERM_RESOURCE_SNAPSHOT_MAIN validation_static_prior_main
#include "resource-snapshot-probe.c"
static void validation_word(uint8_t *p,uint64_t value) {for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(value>>(8*i));}
static void validation_check(const char *mode) {
 GhosttyTerminal source=new_terminal();configure_images(source,".");
 feed(source,"\x1b_Ga=T,i=8,p=10,C=1,f=32,s=1,v=1,q=2;/wAA/w==\x1b\\");
 feed(source,"\x1b_Ga=T,i=7,p=9,C=1,f=32,s=1,v=1,q=2,m=1;/wAA\x1b\\");
 Bytes canonical=capture(source),resources=resource_capture(source);REQUIRE(resources.len==519);
 /* All offsets are part of this exact experimental version, not C layouts. */
 size_t offset=88;uint64_t value=0;int expected=1;
 if(strcmp(mode,"presence")==0) {offset=80;value=2;}
 else if(strcmp(mode,"format")==0) {offset=120;value=99;}
 else if(strcmp(mode,"compression")==0) {offset=128;value=99;}
 else if(strcmp(mode,"flags")==0) {offset=136;value=4;}
 else if(strcmp(mode,"quiet")==0) {offset=144;value=99;}
 else if(strcmp(mode,"display")==0) {offset=176;value=2;}
 else if(strcmp(mode,"wide-field")==0) {offset=184;value=UINT64_C(0x100000000);}
 else if(strcmp(mode,"cursor")==0) {offset=312;value=2;}
 else if(strcmp(mode,"virtual")==0) {offset=320;value=2;}
 else if(strcmp(mode,"dimension")==0) {offset=104;value=10001;expected=4;}
 else if(strcmp(mode,"length")==0) {offset=328;value=UINT64_MAX;expected=4;}
 else if(strcmp(mode,"truncated")==0) {offset=328;value=4;}
 else REQUIRE(strcmp(mode,"zero-id")==0);
 offset+=180; /* completed image + pinned placement precede LoadingImage */
 validation_word(resources.data+offset,value);little32_write(resources.data+12,crc32c(resources.data+16,resources.len-16));
 SnapshotAlloc state={0};const GhosttyAllocatorVtable table={snapshot_alloc,png_resize,png_remap,png_free};
 GhosttyAllocator allocator={.ctx=&state,.vtable=&table};GhosttySnapshotDecoder decoder=NULL;GhosttyTerminal copy=NULL;
 REQUIRE(ghostty_snapshot_decoder_new_buf(&allocator,&decoder,canonical.data,canonical.len)==GHOSTTY_SUCCESS);
 REQUIRE(ghostty_snapshot_decoder_decode(decoder,&copy)==GHOSTTY_SUCCESS);ghostty_snapshot_decoder_free(decoder);
 Bytes before=resource_capture(copy);size_t live_before=state.base.live;state.calls=0;
 REQUIRE(ghostty_terminal_p0_resources_decode(copy,resources.data,resources.len,resource_cap)==expected);
 REQUIRE(state.calls==0 && state.base.live==live_before);
 Bytes after=resource_capture(copy);REQUIRE(before.len==after.len && memcmp(before.data,after.data,before.len)==0);
 require_equal_state(source,copy);require_no_semantic_error(copy);ghostty_terminal_free(copy);REQUIRE(state.base.live==0);
 ghostty_terminal_free(source);free(canonical.data);free(resources.data);free(before.data);free(after.data);
}
int main(int argc,char **argv) {
 REQUIRE(argc==2);if(strcmp(argv[1],"static")==0)return validation_static_prior_main(argc,argv);
 validation_check(argv[1]);printf("PASS loading validation/%s\n",argv[1]);return 0;
}
