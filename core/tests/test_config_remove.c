#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "FAIL remove: %s\n", message); return 1; } } while (0)
static int equals(gt_config_t *cfg, const char *expected) {
  char *actual = gt_config_to_string(cfg);
  int same = actual && !strcmp(actual, expected); free(actual); return same;
}
static int example(const char *input, const char *alias, const char *expected) {
  gt_config_t *cfg = gt_config_parse(input, strlen(input));
  CHECK(cfg, "parse example");
  CHECK(gt_config_remove_host(cfg, alias) == GT_OK, "remove succeeds");
  CHECK(equals(cfg, expected), "non-target bytes survive");
  gt_config_host_info_t info;
  CHECK(gt_config_inspect_host(cfg, alias, &info) == GT_ERR_NOT_FOUND, "literal entry disappears");
  gt_config_free(cfg); return 0;
}
static int reject(const char *text, const char *alias, int status) {
  gt_config_t *cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg, "parse rejected example");
  CHECK(gt_config_remove_host(cfg, alias) == status && equals(cfg, text), "rejected edit is atomic");
  gt_config_free(cfg); return 0;
}
int main(int argc, char **argv) {
  CHECK(argc == 2, "project tmp required");
  CHECK(!example("# 中文 🔑\r\nHost demo\r\n User alice # own\r\n # keep standalone\r\n Unknown keep\r\n\r\nHost *\r\n Port 22", "demo",
                 "# 中文 🔑\r\n # keep standalone\r\n\r\nHost *\r\n Port 22"), "single block");
  CHECK(!example("Host before\n User bob\nHost demo\n User alice\nMatch user root\n Port 2222\nHost after\n User next\n", "demo",
                 "Host before\n User bob\nMatch user root\n Port 2222\nHost after\n User next\n"), "Match boundary");
  CHECK(!example("Host \"demo\"", "demo", ""), "last line no newline");
  CHECK(!example("Host other\n User keep\nHost demo\n User own", "demo", "Host other\n User keep\n"), "tail block");
  CHECK(!example("\tHoSt=  \"demo\"\tsibling # shared\r\n User alice\r\n", "DEMO", "\tHoSt=  sibling # shared\r\n User alice\r\n"), "first quoted token");
  CHECK(!example("Host left  de\\mo\tright # shared\n User alice\n", "demo", "Host left  right # shared\n User alice\n"), "middle escaped token");
  CHECK(!example("Host \"left \\\"quoted\\\"\"\t\"demo\" # shared\n User alice\n", "demo", "Host \"left \\\"quoted\\\"\" # shared\n User alice\n"), "last token quoted sibling");
  CHECK(!example("Host demo *.prod !excluded\n User alice\n", "demo", "Host *.prod !excluded\n User alice\n"), "retain wildcard and negation");
  CHECK(!reject("Host demo DEMO\n User me\n", "demo", GT_ERR_UNSUPPORTED), "repeated token");
  CHECK(!reject("Host demo\n User me\nHost DEMO\n User other\n", "demo", GT_ERR_UNSUPPORTED), "duplicate blocks");
  CHECK(!reject("Host demo\n", "missing", GT_ERR_NOT_FOUND), "missing");
  CHECK(!reject("Host *\n", "*", GT_ERR_INVALID_ARG), "wildcard");
  CHECK(!reject("Host demo\n", "!demo", GT_ERR_INVALID_ARG), "negative");
  CHECK(gt_config_remove_host(NULL, "demo") == GT_ERR_INVALID_ARG, "null config");
  CHECK(!reject("Host demo\n", NULL, GT_ERR_INVALID_ARG), "null alias");
  CHECK(!reject("Host demo\n", "", GT_ERR_INVALID_ARG), "empty alias");
  CHECK(!reject("Host demo\n", "a b", GT_ERR_INVALID_ARG), "whitespace alias");
  char directory[4096]; snprintf(directory, sizeof directory, "%s/config-remove-XXXXXX", argv[1]);
  CHECK(mkdtemp(directory), "isolated fixture directory");
  char include[8192], text[16384]; snprintf(include, sizeof include, "%s/hosts", directory);
  FILE *file = fopen(include, "w"); CHECK(file, "include fixture");
  fputs("Host included\n User retained\n", file); fclose(file);
  snprintf(text, sizeof text, "Include %s\nHost demo\n User own\n", include);
  gt_config_t *cfg = gt_config_parse(text, strlen(text)); CHECK(cfg, "parse include");
  CHECK(gt_config_remove_host(cfg, "included") == GT_ERR_UNSUPPORTED && equals(cfg, text), "included source");
  CHECK(!unlink(include), "vanish include");
  CHECK(gt_config_remove_host(cfg, "demo") == GT_ERR_IO && equals(cfg, text), "failed reparse is atomic");
  gt_config_free(cfg);
  file = fopen(include, "w"); CHECK(file, "restore include"); fputs("Host included\n User retained\n", file); fclose(file);
  snprintf(text, sizeof text, "Host demo\n Include %s\n User own\nHost after\n User keep\n", include);
  CHECK(!reject(text, "demo", GT_ERR_UNSUPPORTED), "contained include can affect other scopes");
  CHECK(!unlink(include) && !rmdir(directory), "fixture cleanup");
  puts("config_remove PASS"); return 0;
}
