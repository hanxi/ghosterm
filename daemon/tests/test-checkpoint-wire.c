#include "checkpoint-wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
/* Independent fixed vector: Python struct.pack + zlib.crc32, not this codec. */
static const uint8_t vector[]={
  0x47,0x54,0x56,0x43,0,1,0,128,0,0,0,3,0,0,0,1,
  0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,2,0,0,0,10,0,0,0,20,
  1,2,3,4,5,6,7,8,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
  0,0,0,0,0,0,4,0,0,0,0,0,0,0,16,0,
  1,2,3,4,5,6,7,8,9,10,11,12,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0x94,0x04,0xf1,0xe7,'a','b','c','X','Y'};
static gt_vt_snapshot_view_t view(void) {
  return (gt_vt_snapshot_view_t){.canonical=(const uint8_t *)"abc",.canonical_bytes=3,
    .resources=(const uint8_t *)"XY",.resource_bytes=2,.profile=1,.cell_width=10,.cell_height=20,
    .history_bytes=UINT64_C(0x0102030405060708),.history_lines=UINT64_C(0x1112131415161718),
    .continuation_bytes=1024,.image_bytes=4096,.light={{1,2,3},{4,5,6}},.dark={{7,8,9},{10,11,12}},
    .is_dark=true,.history_pruned=true};
}
static void check_view(gt_vt_snapshot_view_t v) {
  CHECK(v.canonical_bytes==3 && v.resource_bytes==2 && !memcmp(v.canonical,"abc",3) && !memcmp(v.resources,"XY",2));
  CHECK(v.profile==1 && v.cell_width==10 && v.cell_height==20 && v.history_bytes==UINT64_C(0x0102030405060708) && v.history_lines==UINT64_C(0x1112131415161718));
  CHECK(v.continuation_bytes==1024 && v.image_bytes==4096 && v.is_dark && v.history_pruned);
  CHECK(v.light.background[0]==1 && v.light.foreground[2]==6 && v.dark.background[0]==7 && v.dark.foreground[2]==12);
}
static void vectors(void) {
  CHECK(sizeof(vector)==133 && GT_VT_SNAPSHOT_HEADER==128 && GT_VT_SNAPSHOT_FORMAT==0x47545643);
  gt_vt_snapshot_view_t v=view(),decoded;size_t n=0;uint8_t bytes[133];
  CHECK(gt_vt_snapshot_size(&v,133,&n)==GT_OK && n==133);
  CHECK(gt_vt_snapshot_write(&v,bytes,sizeof(bytes),&n)==GT_OK && n==sizeof(vector) && !memcmp(bytes,vector,sizeof(vector)));
  CHECK(gt_vt_snapshot_parse(vector,sizeof(vector),133,&decoded)==GT_OK);check_view(decoded);
  CHECK(decoded.canonical==vector+128 && decoded.resources==vector+131);
}
static void checksum(uint8_t *bytes,size_t n) {
  /* Separate bitwise IEEE implementation, deliberately not nibble-table based. */
  memset(bytes+124,0,4);uint32_t crc=UINT32_MAX;
  for(size_t i=0;i<n;i++){crc^=bytes[i];for(unsigned b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xedb88320:0);}
  crc=~crc;for(unsigned i=0;i<4;i++)bytes[127-i]=(uint8_t)(crc>>(8*i));
}
static void rejected(const void *bytes,size_t n,gt_status_t expected) {
  gt_vt_snapshot_view_t v;memset(&v,0xa5,sizeof(v));
  CHECK(gt_vt_snapshot_parse(bytes,n,64*1024*1024,&v)==expected);
  const uint8_t *p=(const uint8_t *)&v;for(size_t i=0;i<sizeof(v);i++)CHECK(!p[i]);
}
static void corruption(void) {
  uint8_t bytes[134];memcpy(bytes,vector,sizeof(vector));bytes[128]^=1;
  rejected(bytes,sizeof(vector),GT_ERR_PROTOCOL);
  for(size_t i=0;i<sizeof(vector);i++) {
    memcpy(bytes,vector,sizeof(vector));bytes[i]^=0x80;
    rejected(bytes,sizeof(vector),(i==4 || i==5 || (i>=12 && i<16))?GT_ERR_UNSUPPORTED:GT_ERR_PROTOCOL);
  }
}
static void malformed(void) {
  uint8_t bytes[134];
  memcpy(bytes,vector,sizeof(vector));bytes[0]='X';checksum(bytes,sizeof(vector));rejected(bytes,sizeof(vector),GT_ERR_PROTOCOL);
  for(size_t n=0;n<sizeof(vector);n++)rejected(vector,n,GT_ERR_PROTOCOL);
  memcpy(bytes,vector,sizeof(vector));bytes[133]=0;checksum(bytes,sizeof(bytes));rejected(bytes,sizeof(bytes),GT_ERR_PROTOCOL);
  const size_t offsets[]={6,7,8,9,10,11,16,23,24,31,32,36,40,48,56,84,100,123};
  for(size_t i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
    memcpy(bytes,vector,sizeof(vector));size_t off=offsets[i];
    if(off==11)bytes[off]=4;
    else if(off==23 || off==31)bytes[off]=0;
    else if(off==40 || off==48 || off==56)memset(bytes+off,0,8);
    else if(off==32 || off==36)bytes[off]=1;
    else bytes[off]^=0x80;
    checksum(bytes,sizeof(vector));rejected(bytes,sizeof(vector),GT_ERR_PROTOCOL);
  }
  memcpy(bytes,vector,sizeof(vector));memset(bytes+16,255,8);checksum(bytes,sizeof(vector));rejected(bytes,sizeof(vector),GT_ERR_PROTOCOL);
  memcpy(bytes,vector,sizeof(vector));memset(bytes+24,255,8);checksum(bytes,sizeof(vector));rejected(bytes,sizeof(vector),GT_ERR_PROTOCOL);
  memcpy(bytes,vector,sizeof(vector));memset(bytes+16,0,8);bytes[31]=5;checksum(bytes,sizeof(vector));rejected(bytes,sizeof(vector),GT_ERR_PROTOCOL);
  memcpy(bytes,vector,sizeof(vector));bytes[23]=5;memset(bytes+24,0,8);checksum(bytes,sizeof(vector));rejected(bytes,sizeof(vector),GT_ERR_PROTOCOL);
}
static void limits(void) {
  gt_vt_snapshot_view_t v=view(),out;size_t n=99;uint8_t bytes[133];memset(bytes,0xa5,sizeof(bytes));
  CHECK(gt_vt_snapshot_size(&v,132,&n)==GT_ERR_IO && !n);
  CHECK(gt_vt_snapshot_write(&v,bytes,132,&n)==GT_ERR_IO && !n && bytes[0]==0xa5 && bytes[132]==0xa5);
  CHECK(gt_vt_snapshot_parse(vector,sizeof(vector),132,&out)==GT_ERR_PROTOCOL);
  CHECK(gt_vt_snapshot_size(&v,(size_t)64*1024*1024+1,&n)==GT_ERR_INVALID_ARG && !n);
  v.canonical_bytes=(size_t)64*1024*1024-129;v.resource_bytes=1;
  CHECK(gt_vt_snapshot_size(&v,64*1024*1024,&n)==GT_OK && n==(size_t)64*1024*1024);
  v.canonical_bytes++;CHECK(gt_vt_snapshot_size(&v,64*1024*1024,&n)==GT_ERR_IO && !n);
  v.canonical_bytes=SIZE_MAX;CHECK(gt_vt_snapshot_size(&v,133,&n)!=GT_OK && !n);
  v=view();v.resource_bytes=SIZE_MAX;CHECK(gt_vt_snapshot_size(&v,133,&n)!=GT_OK && !n);
}
static void policy(void) {
  gt_vt_snapshot_view_t v=view();size_t n=99;
  v.profile=2;CHECK(gt_vt_snapshot_size(&v,133,&n)==GT_ERR_UNSUPPORTED && !n);
  v=view();v.continuation_bytes=0;CHECK(gt_vt_snapshot_size(&v,133,&n)==GT_ERR_INVALID_ARG && !n);
  v=view();v.cell_height=65536;CHECK(gt_vt_snapshot_size(&v,133,&n)==GT_ERR_INVALID_ARG && !n);
  CHECK(gt_vt_snapshot_size(NULL,133,&n)==GT_ERR_INVALID_ARG && !n);
  CHECK(gt_vt_snapshot_write(NULL,NULL,0,&n)==GT_ERR_INVALID_ARG && !n);
  CHECK(gt_vt_snapshot_parse(NULL,0,133,&v)==GT_ERR_INVALID_ARG && !v.canonical);
  CHECK(gt_vt_snapshot_parse(vector,sizeof(vector),0,&v)==GT_ERR_INVALID_ARG && !v.canonical);
  uint8_t bytes[133];memcpy(bytes,vector,sizeof(bytes));bytes[15]=2;checksum(bytes,sizeof(bytes));rejected(bytes,sizeof(bytes),GT_ERR_UNSUPPORTED);
  memcpy(bytes,vector,sizeof(bytes));bytes[5]=2;checksum(bytes,sizeof(bytes));rejected(bytes,sizeof(bytes),GT_ERR_UNSUPPORTED);
}
int main(int argc,char **argv) {
  CHECK(argc==2);
  if(!strcmp(argv[1],"vectors"))vectors();
  else if(!strcmp(argv[1],"corruption"))corruption();
  else if(!strcmp(argv[1],"malformed"))malformed();
  else if(!strcmp(argv[1],"limits"))limits();
  else {CHECK(!strcmp(argv[1],"policy"));policy();}
  printf("PASS checkpoint wire %s\n",argv[1]);return 0;
}
