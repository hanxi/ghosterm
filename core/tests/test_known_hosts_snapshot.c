#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "FAIL snapshot: %s\n", label); return 1; } } while (0)
int main(void) {
  const char *text = "# 注释\r\n\t\r\n"
      "  @revoked [demo]:2222,other ssh-ed25519 AAAAAAAAAAAAAAAA note\r\n"
      "@cert-authority *.example ecdsa-sha2-nistp256 AAAA/AAAAAAAAAAA\n"
      "|1|salt|hash ssh-rsa AAAAAAAAAAAAAAAA\n"
      "@future host ssh-new AAAAAAAAAAAAAAAA\n"
      "broken\n"
      "demo ssh-ed25519 AAAAAAAAAAAAAAAA";
  gt_known_hosts_t *snapshot = gt_known_hosts_parse(text, strlen(text));
  CHECK(snapshot && gt_known_hosts_count(snapshot) == 6, "visible records include malformed");
  const char *markers[] = {"@revoked", "@cert-authority", "", "@future", "", ""};
  size_t offset = (size_t)(strstr(text, "  @revoked") - text);
  for (size_t i = 0; i < 6; i++) {
    gt_known_host_entry_t item;
    CHECK(gt_known_hosts_entry_at(snapshot, i, &item) == GT_OK, "query");
    CHECK(item.source_line == i + 3, "physical line numbers");
    CHECK(item.byte_start == offset, "original byte start");
    const char *newline = strchr(text + offset, '\n');
    size_t end = newline ? (size_t)(newline - text) + 1 : strlen(text);
    CHECK(item.byte_end == end, "range includes original newline");
    CHECK(strcmp(item.marker, markers[i]) == 0, "markers never folded to trust");
    CHECK(item.recognized == (i != 4), "field shape not silent skip");
    CHECK(item.hashed == (i == 2), "hashed classified without matching");
    if (i == 0) CHECK(!strcmp(item.hosts, "[demo]:2222,other") && !strcmp(item.key_type,"ssh-ed25519") &&
                     !strcmp(item.key_base64,"AAAAAAAAAAAAAAAA"), "host list and key fields");
    if (i == 1) CHECK(!strcmp(item.key_type,"ecdsa-sha2-nistp256"), "ecdsa not filtered by ssh prefix");
    if (i == 4) CHECK(!strcmp(item.hosts,"broken") && !*item.key_type && !*item.key_base64, "malformed fields visible");
    offset = end;
  }
  gt_known_host_entry_t item;
  memset(&item, 0xff, sizeof item);
  CHECK(gt_known_hosts_entry_at(snapshot,6,&item)==GT_ERR_NOT_FOUND && !item.marker && !item.byte_end, "end clears output");
  CHECK(gt_known_hosts_entry_at(NULL,0,&item)==GT_ERR_INVALID_ARG && !item.hosts, "null snapshot");
  CHECK(gt_known_hosts_entry_at(snapshot,0,NULL)==GT_ERR_INVALID_ARG, "null output");
  gt_known_hosts_free(snapshot); gt_known_hosts_free(NULL);
  char copy[]="a ssh-rsa AAAAAAAAAAAAAAAA";
  snapshot=gt_known_hosts_parse(copy,strlen(copy)); memset(copy,'x',sizeof(copy)-1);
  CHECK(snapshot && gt_known_hosts_entry_at(snapshot,0,&item)==GT_OK && !strcmp(item.hosts,"a"), "snapshot owns token storage");
  gt_known_hosts_free(snapshot);
  snapshot=gt_known_hosts_parse("",0); CHECK(snapshot && !gt_known_hosts_count(snapshot),"empty snapshot"); gt_known_hosts_free(snapshot);
  CHECK(!gt_known_hosts_parse(NULL,0),"null input rejected");
  CHECK(!gt_known_hosts_parse("a\0b",3),"embedded NUL rejected");
  CHECK(!gt_known_hosts_parse("",1024*1024+1),"size checked before read");
  char *many=malloc(4097*2+1); CHECK(many,"allocate");
  for(size_t i=0;i<4097;i++){many[2*i]='x';many[2*i+1]='\n';}many[4097*2]=0;
  snapshot=gt_known_hosts_parse(many,4096*2);CHECK(snapshot && gt_known_hosts_count(snapshot)==4096,"limit boundary");gt_known_hosts_free(snapshot);
  CHECK(!gt_known_hosts_parse(many,4097*2),"record limit explicit failure");free(many);
  puts("known_hosts snapshot PASS");return 0;
}
