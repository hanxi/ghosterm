#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "FAIL metadata: %s\n", label); return 1; } } while (0)

static int scalar_sources(void) {
  const char *text = "User inherited\nHost demo sibling\n  HostName example.test\n  User explicit\nHost *\n  Port 2222\n";
  gt_config_t *cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg, "parse");
  gt_config_host_info_t info;
  CHECK(gt_config_inspect_host(cfg, "DEMO", &info) == GT_OK, "inspect case-insensitive alias");
  CHECK(info.pattern_count == 2 && info.matching_blocks == 1 && !info.included, "shared block ownership");
  CHECK(strcmp(info.header, "Host demo sibling\n") == 0 && info.source_line == 2, "header location");
  CHECK(strcmp(info.user.explicit_value, "explicit") == 0 && !info.user.effective_is_explicit,
        "explicit value can lose to earlier rule");
  CHECK(info.user.source_line == 1 && !*info.user.source_header && !*info.user.source_path,
        "global winning source");
  CHECK(info.hostname.effective_is_explicit && info.hostname.source_line == 3, "local winning source");
  CHECK(info.port.explicit_value == NULL && info.port.source_line == 6 &&
        strcmp(info.port.source_header, "Host *\n") == 0, "wildcard winning source");
  CHECK(gt_config_inspect_host(cfg, "missing", &info) == GT_ERR_NOT_FOUND, "missing alias");
  CHECK(gt_config_inspect_host(NULL, "demo", &info) == GT_ERR_INVALID_ARG, "invalid arguments");
  gt_config_free(cfg);
  text = "Host plain\n";
  cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg && gt_config_inspect_host(cfg, "plain", &info) == GT_OK, "default inspect");
  CHECK(info.port.source_line == 0 && !info.port.effective_is_explicit && info.port.explicit_value == NULL,
        "defaults are not explicit");
  gt_config_free(cfg);
  return 0;
}

static int ambiguous_blocks(void) {
  const char *text = "Host dup\n  User first\nHost DUP\n  User second\n";
  gt_config_t *cfg = gt_config_parse(text, strlen(text));
  gt_config_host_info_t info;
  CHECK(cfg && gt_config_inspect_host(cfg, "dup", &info) == GT_OK, "ambiguous inspect");
  CHECK(info.matching_blocks == 2, "duplicate block count");
  const char *values[] = {"changed", NULL};
  gt_config_edit_t edit = {"User", values};
  CHECK(gt_config_edit(cfg, "dup", &edit, 1, false) == GT_ERR_UNSUPPORTED, "reject ambiguous edit");
  CHECK(gt_config_edit(cfg, "dup", &edit, 1, true) == GT_ERR_UNSUPPORTED, "reject ambiguous reset");
  char *serialized = gt_config_to_string(cfg);
  CHECK(serialized && strcmp(serialized, text) == 0, "ambiguous rejection preserves bytes");
  free(serialized); gt_config_free(cfg);
  return 0;
}

static int write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (!file) return -1;
  int ok = fwrite(text, 1, strlen(text), file) == strlen(text);
  return fclose(file) == 0 && ok ? 0 : -1;
}

static int included_sources(const char *directory) {
  char child[4096], mainfile[4096], text[8192];
  snprintf(child, sizeof child, "%s/child.conf", directory);
  snprintf(mainfile, sizeof mainfile, "%s/config", directory);
  CHECK(write_text(child, "Host remote sibling\n  User included\n") == 0, "write included fixture");
  snprintf(text, sizeof text, "# root\nInclude %s\nHost local\n  Port 2222\n", child);
  CHECK(write_text(mainfile, text) == 0, "write main fixture");
  gt_config_t *cfg = gt_config_parse_file(mainfile);
  gt_config_host_info_t info;
  CHECK(cfg && gt_config_inspect_host(cfg, "remote", &info) == GT_OK, "included inspect");
  CHECK(info.included && strcmp(info.source_path, child) == 0 && info.source_line == 1,
        "included header provenance");
  CHECK(info.user.source_included && strcmp(info.user.source_path, child) == 0 && info.user.source_line == 2,
        "included field provenance");
  CHECK(info.user.explicit_value == NULL && !info.user.effective_is_explicit, "included value is read-only");
  const char *values[] = {"2200", NULL};
  gt_config_edit_t edit = {"Port", values};
  CHECK(gt_config_edit(cfg, "remote", &edit, 1, false) == GT_ERR_UNSUPPORTED, "reject included edit");
  CHECK(gt_config_edit(cfg, "remote", &edit, 1, true) == GT_ERR_UNSUPPORTED, "reject included reset");
  char *serialized = gt_config_to_string(cfg);
  CHECK(serialized && strcmp(serialized, text) == 0, "included rejection preserves main bytes");
  free(serialized);
  CHECK(gt_config_edit(cfg, "local", &edit, 1, false) == GT_OK, "main block remains editable");
  CHECK(gt_config_inspect_host(cfg, "local", &info) == GT_OK && strcmp(info.source_path, mainfile) == 0,
        "main path survives edit");
  gt_config_free(cfg);

  CHECK(write_text(child, "# included defaults\nUser from-include\n") == 0, "write inline include");
  snprintf(text, sizeof text, "Host inline\nInclude %s\n  Port 22\n", child);
  cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg && gt_config_inspect_host(cfg, "inline", &info) == GT_OK, "inline inspect");
  CHECK(!info.included && info.user.source_included && info.user.source_line == 2 &&
        info.user.explicit_value == NULL, "included field within main block");
  gt_config_free(cfg);
  unlink(child); unlink(mainfile);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  char directory[4096];
  snprintf(directory, sizeof directory, "%s/config-metadata-XXXXXX", argv[1]);
  if (!mkdtemp(directory)) return 2;
  int failed = scalar_sources() || ambiguous_blocks() || included_sources(directory);
  rmdir(directory);
  return failed;
}
