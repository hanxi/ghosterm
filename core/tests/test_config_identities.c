#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c, label) do { if (!(c)) { fprintf(stderr, "FAIL identities: %s\n", label); return 1; } } while (0)

static int sources(void) {
  const char *text = "IdentityFile ~/.ssh/global\r\nHost demo sibling\r\n"
      "\tIdentityFile=\"/keys/中文 #1\"\r\n # keep\r\n User deploy\r\n"
      " IdentityFile second\r\nHost * !other\r\n IdentityFile wildcard\r\n"
      "Host unrelated\r\n IdentityFile excluded\r\n";
  gt_config_t *cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg, "parse");
  const char *values[] = {"~/.ssh/global", "/keys/中文 #1", "second", "wildcard"};
  const size_t lines[] = {1, 3, 6, 8};
  gt_config_identity_info_t item;
  for (size_t i = 0; i < 4; i++) {
    CHECK(gt_config_identity_at(cfg, "DEMO", i, &item) == GT_OK, "all cumulative entries");
    CHECK(strcmp(item.value, values[i]) == 0, "decoded values and source order");
    CHECK(item.source_line == lines[i] && !*item.source_path, "line and memory source");
    CHECK(item.explicit_value == (i == 1 || i == 2), "own block versus inheritance");
    CHECK(item.effective && !item.included, "effective main entries");
    CHECK(i ? *item.source_header : !*item.source_header, "global versus Host header");
  }
  CHECK(gt_config_identity_at(cfg, "demo", 4, &item) == GT_ERR_NOT_FOUND && !item.value && !item.effective,
        "end zeroes output");
  CHECK(gt_config_identity_at(cfg, "absent", 0, &item) == GT_ERR_NOT_FOUND, "missing host");
  CHECK(gt_config_identity_at(NULL, "demo", 0, &item) == GT_ERR_INVALID_ARG && !item.value, "null config");
  CHECK(gt_config_identity_at(cfg, "", 0, &item) == GT_ERR_INVALID_ARG, "empty alias");
  CHECK(gt_config_identity_at(cfg, "demo", 0, NULL) == GT_ERR_INVALID_ARG, "null output");
  char *before = gt_config_to_string(cfg);
  CHECK(before && strcmp(before, text) == 0, "query preserves original bytes"); free(before);
  const char *replacement[] = {"new-key", NULL};
  gt_config_edit_t edit = {"IdentityFile", replacement};
  CHECK(gt_config_edit(cfg, "demo", &edit, 1, false) == GT_OK, "identity sparse edit");
  CHECK(gt_config_identity_at(cfg, "demo", 1, &item) == GT_OK && item.explicit_value && strcmp(item.value, "new-key") == 0,
        "edited view reparses");
  char *after = gt_config_to_string(cfg);
  CHECK(after && strstr(after, " # keep\r\n User deploy\r\n") && strstr(after, "IdentityFile wildcard\r\n"),
        "edit preserves unrelated and inherited lines"); free(after);
  edit.values = NULL;
  CHECK(gt_config_edit(cfg, "demo", &edit, 1, false) == GT_OK, "restore inheritance");
  CHECK(gt_config_identity_at(cfg, "demo", 1, &item) == GT_OK && !item.explicit_value && strcmp(item.value, "wildcard") == 0,
        "deletion retains inherited entries");
  CHECK(gt_config_identity_at(cfg, "demo", 2, &item) == GT_ERR_NOT_FOUND, "deleted explicit entries absent");
  gt_config_free(cfg);
  return 0;
}

static int exclusions(void) {
  const char *text = "Host demo !demo\n IdentityFile own\nHost * !demo\n IdentityFile excluded\n"
      "Host demo\n IdentityFile later\n";
  gt_config_t *cfg = gt_config_parse(text, strlen(text));
  gt_config_identity_info_t item;
  CHECK(cfg && gt_config_identity_at(cfg, "demo", 0, &item) == GT_OK && item.explicit_value && !item.effective,
        "negated literal entry is explicit but ineffective");
  CHECK(gt_config_identity_at(cfg, "demo", 1, &item) == GT_OK && !item.explicit_value && item.effective &&
        strcmp(item.value, "later") == 0, "negated rules excluded and duplicate block separate");
  CHECK(gt_config_identity_at(cfg, "demo", 2, &item) == GT_ERR_NOT_FOUND, "excluded rules absent");
  gt_config_free(cfg);
  const char *invalid[] = {
      "Host demo\n IdentityFile valid\nHost other\n Match all\n IdentityFile conditional\n",
      "Host demo\n IdentityFile valid\n IdentityFile two arguments\n",
      "Host demo\n IdentityFile\n"
  };
  for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
    cfg = gt_config_parse(invalid[i], strlen(invalid[i]));
    CHECK(cfg && gt_config_identity_at(cfg, "demo", 0, &item) == GT_ERR_UNSUPPORTED && !item.value,
          "unsupported never publishes partial entries");
    gt_config_free(cfg);
  }
  text = "Host demo\nHost other\n IdentityFile two arguments\n";
  cfg = gt_config_parse(text, strlen(text));
  CHECK(cfg && gt_config_identity_at(cfg, "demo", 0, &item) == GT_ERR_NOT_FOUND,
        "no defaults or unrelated invalid entries");
  gt_config_free(cfg);
  return 0;
}

static int includes(const char *root) {
  char directory[4096], child[4200], text[8500];
  snprintf(directory, sizeof directory, "%s/config-identities-XXXXXX", root);
  CHECK(mkdtemp(directory), "temporary directory");
  snprintf(child, sizeof child, "%s/child.conf", directory);
  FILE *file = fopen(child, "w");
  CHECK(file, "include file");
  CHECK(fputs("# source\nIdentityFile \"/keys/包含 🔑\"\n", file) >= 0 && fclose(file) == 0, "write include");
  snprintf(text, sizeof text, "Host demo sibling\n Include %s\n IdentityFile local\n", child);
  gt_config_t *cfg = gt_config_parse(text, strlen(text));
  gt_config_identity_info_t item;
  CHECK(cfg && gt_config_identity_at(cfg, "demo", 0, &item) == GT_OK, "inline Include entry");
  CHECK(item.included && !item.explicit_value && item.effective, "Include is not editable explicit");
  CHECK(strcmp(item.source_path, child) == 0 && item.source_line == 2 && strcmp(item.value, "/keys/包含 🔑") == 0,
        "included file source and value");
  const char *values[] = {"replacement", NULL};
  gt_config_edit_t edit = {"IdentityFile", values};
  CHECK(gt_config_edit(cfg, "demo", &edit, 1, false) == GT_OK, "edit beside Include");
  CHECK(gt_config_identity_at(cfg, "demo", 0, &item) == GT_OK && item.included && strcmp(item.source_path, child) == 0,
        "Include source survives reparse");
  CHECK(gt_config_identity_at(cfg, "sibling", 1, &item) == GT_OK && item.explicit_value && strcmp(item.value, "replacement") == 0,
        "shared block alias sees same identities");
  gt_config_free(cfg);
  unlink(child); rmdir(directory);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  int result = sources() || exclusions() || includes(argv[1]);
  if (!result) puts("CONFIG IDENTITIES PASS");
  return result;
}
