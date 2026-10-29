#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "FAIL rename: %s\n", label); return 1; } } while (0)

static int equals(gt_config_t *cfg, const char *text) {
  char *actual = gt_config_to_string(cfg);
  int same = actual && strcmp(actual, text) == 0;
  free(actual); return same;
}

static int spelling(void) {
  const char *original = "# 注释 🔑\r\n\tHoSt=  \"demo\"\tsibling # shared\r\n\tUser=alice\r\n  FutureOption preserve\r\nHost other\n  Port 2200";
  const char *expected = "# 注释 🔑\r\n\tHoSt=  renamed\tsibling # shared\r\n\tUser=alice\r\n  FutureOption preserve\r\nHost other\n  Port 2200";
  gt_config_t *cfg = gt_config_parse(original, strlen(original));
  CHECK(cfg && gt_config_rename_host(cfg, "demo", "demo") == GT_OK && equals(cfg, original), "same-name preserves quotes");
  CHECK(cfg && gt_config_rename_host(cfg, "DEMO", "renamed") == GT_OK, "quoted rename succeeds");
  CHECK(equals(cfg, expected), "only alias token changes");
  gt_config_host_info_t info;
  CHECK(gt_config_inspect_host(cfg, "demo", &info) == GT_ERR_NOT_FOUND, "old alias removed");
  CHECK(gt_config_inspect_host(cfg, "renamed", &info) == GT_OK && info.pattern_count == 2 && info.source_line == 2,
        "new metadata and sibling survive");
  gt_resolved_t *sibling = gt_config_resolve(cfg, "sibling", NULL);
  CHECK(sibling && strcmp(sibling->user, "alice") == 0, "sibling resolves unchanged");
  gt_resolved_free(sibling);
  CHECK(gt_config_rename_host(cfg, "renamed", "RENAMED") == GT_OK, "case-only rename succeeds");
  CHECK(strcmp(gt_config_host_pattern(cfg, 0), "RENAMED") == 0, "case-only spelling changes");
  CHECK(gt_config_rename_host(cfg, "RENAMED", "RENAMED") == GT_OK, "same-name no-op");
  const char *values[] = {"bob", NULL};
  gt_config_edit_t edit = {"User", values};
  CHECK(gt_config_edit(cfg, "RENAMED", &edit, 1, false) == GT_OK, "edit after rename");
  CHECK(gt_config_inspect_host(cfg, "RENAMED", &info) == GT_OK && strcmp(info.user.explicit_value, "bob") == 0,
        "renamed block remains editable");
  gt_config_free(cfg);
  original = "Host left de\\mo right # keep demo\n  User me\n";
  expected = "Host left new-host right # keep demo\n  User me\n";
  cfg = gt_config_parse(original, strlen(original));
  CHECK(cfg && gt_config_rename_host(cfg, "demo", "new-host") == GT_OK && equals(cfg, expected), "escaped middle token");
  gt_config_free(cfg);
  original = "Host \"demo\"";
  cfg = gt_config_parse(original, strlen(original));
  CHECK(cfg && gt_config_rename_host(cfg, "demo", "new") == GT_OK && equals(cfg, "Host new"), "no-final-newline preserved");
  gt_config_free(cfg); return 0;
}

static int rejected(void) {
  const char *text = "Host demo sibling\n  User me\nHost occupied\n";
  gt_config_t *cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg, "parse rejections");
  CHECK(gt_config_rename_host(cfg, "demo", "SIBLING") == GT_ERR_INVALID_ARG, "reject sibling collision");
  CHECK(gt_config_rename_host(cfg, "demo", "OCCUPIED") == GT_ERR_INVALID_ARG, "reject other block collision");
  const char *bad[] = {"", "x y", "x\ny", "x#y", "!deny", "a*", "a?", "a[b]", ".hidden", "a=b", "x\\y", "\"a\"", "中文"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    CHECK(gt_config_rename_host(cfg, "demo", bad[i]) == GT_ERR_INVALID_ARG, "reject invalid new alias");
  char long_name[130]; memset(long_name, 'a', 129); long_name[129] = '\0';
  CHECK(gt_config_rename_host(cfg, "demo", long_name) == GT_ERR_INVALID_ARG, "reject overlong alias");
  CHECK(gt_config_rename_host(NULL, "demo", "new") == GT_ERR_INVALID_ARG &&
        gt_config_rename_host(cfg, NULL, "new") == GT_ERR_INVALID_ARG &&
        gt_config_rename_host(cfg, "demo", NULL) == GT_ERR_INVALID_ARG, "reject null arguments");
  CHECK(gt_config_rename_host(cfg, "missing", "new") == GT_ERR_NOT_FOUND, "missing alias");
  CHECK(equals(cfg, text), "rejections preserve original");
  gt_config_free(cfg);
  text = "Host demo\n  User a\nHost DEMO\n  User b\n";
  cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg && gt_config_rename_host(cfg, "demo", "new") == GT_ERR_UNSUPPORTED, "reject duplicate blocks");
  CHECK(equals(cfg, text), "duplicate block bytes preserved"); gt_config_free(cfg);
  text = "Host demo DEMO sibling\n";
  cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg && gt_config_rename_host(cfg, "demo", "new") == GT_ERR_UNSUPPORTED, "reject repeated header token");
  CHECK(equals(cfg, text), "repeated header bytes preserved"); gt_config_free(cfg);
  text = "Host *.prod\n";
  cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg && gt_config_rename_host(cfg, "*.prod", "new") == GT_ERR_INVALID_ARG, "reject pattern source");
  gt_config_free(cfg); return 0;
}

static int write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "wb"); if (!file) return -1;
  int ok = fwrite(text, 1, strlen(text), file) == strlen(text);
  return fclose(file) == 0 && ok ? 0 : -1;
}

static int includes(const char *directory) {
  char child[4096], mainfile[4096], text[8192], expected[8192];
  snprintf(child, sizeof child, "%s/child.conf", directory);
  snprintf(mainfile, sizeof mainfile, "%s/config", directory);
  CHECK(write_text(child, "Host included\n  User child\n") == 0, "write child");
  snprintf(text, sizeof text, "Include %s\nHost demo\n  User main\n", child);
  snprintf(expected, sizeof expected, "Include %s\nHost new\n  User main\n", child);
  CHECK(write_text(mainfile, text) == 0, "write main");
  gt_config_t *cfg = gt_config_parse_file(mainfile);
  CHECK(cfg && gt_config_rename_host(cfg, "included", "new") == GT_ERR_UNSUPPORTED, "reject included source");
  CHECK(gt_config_rename_host(cfg, "demo", "INCLUDED") == GT_ERR_INVALID_ARG, "reject included collision");
  CHECK(equals(cfg, text), "include rejections preserve bytes");
  CHECK(unlink(child) == 0, "remove include after parse");
  CHECK(gt_config_rename_host(cfg, "demo", "new") == GT_ERR_IO, "reparse failure is visible");
  CHECK(equals(cfg, text), "reparse failure preserves original");
  CHECK(write_text(child, "Host included\n  User child\n") == 0, "restore include");
  CHECK(gt_config_rename_host(cfg, "demo", "new") == GT_OK && equals(cfg, expected), "main rename with include");
  gt_config_host_info_t info;
  CHECK(gt_config_inspect_host(cfg, "new", &info) == GT_OK && strcmp(info.source_path, mainfile) == 0,
        "rename preserves source path");
  gt_config_free(cfg); unlink(child); unlink(mainfile); return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  char directory[4096]; snprintf(directory, sizeof directory, "%s/config-rename-XXXXXX", argv[1]);
  if (!mkdtemp(directory)) return 2;
  int failed = spelling() || rejected() || includes(directory);
  rmdir(directory); return failed;
}
