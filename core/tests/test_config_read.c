#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ENOENT 是首次使用的合法空态；fread 的 EISDIR 绝不能冒充空配置。 */
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  char directory[4096], missing[4096];
  if (snprintf(directory, sizeof directory, "%s/config-read-XXXXXX", argv[1]) >= (int)sizeof directory)
    return 2;
  if (!mkdtemp(directory)) { perror("mkdtemp"); return 2; }
  int failed = 0;
  gt_config_t *cfg = gt_config_parse_file(directory);
  if (cfg || !strstr(gt_last_error(), "read ")) {
    fprintf(stderr, "FAIL: read failure must not become empty config\n");
    failed = 1;
  }
  gt_config_free(cfg);
  if (snprintf(missing, sizeof missing, "%s/missing.conf", directory) >= (int)sizeof missing) {
    rmdir(directory);
    return 2;
  }
  cfg = gt_config_parse_file(missing);
  if (!cfg) {
    fprintf(stderr, "FAIL: missing config must remain a valid empty config\n");
    failed = 1;
  } else {
    char *text = gt_config_to_string(cfg);
    if (!text || *text) {
      fprintf(stderr, "FAIL: missing config must serialize to empty bytes\n");
      failed = 1;
    }
    free(text);
  }
  gt_config_free(cfg);
  if (rmdir(directory) != 0) { perror("rmdir"); return 2; }
  return failed;
}
