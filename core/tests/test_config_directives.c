#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "ghosterm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c, label)                                                                            \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL directives: %s\n", label);                                       \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int lexical(void) {
    const char *text = "User global\r\nHost\tdemo sibling\r\n\tHostName = \"host name\" # keep\r\n"
                       " IdentityFile=\"/keys/中文 #one\"\r\n # middle\r\n User first\r\n"
                       " UnknownField a \\\"b\\\"\r\n User second\r\n IdentityFile other\r\n"
                       "Host next\r\n Port 2222\r\n";
    gt_config_t *cfg = gt_config_parse(text, strlen(text));
    CHECK(cfg, "parse exact source");
    gt_config_directive_info_t item;
    const char *keys[] = {
        "hostname", "identityfile", "user", "unknownfield", "user", "identityfile"};
    for (size_t i = 0; i < 6; i++) {
        CHECK(gt_config_directive_at(cfg, "DEMO", i, &item) == GT_OK, "own directive count/order");
        CHECK(!strcmp(item.keyword, keys[i]) && !item.included && !*item.source_path,
              "native keyword and in-memory provenance");
    }
    CHECK(gt_config_directive_at(cfg, "demo", 0, &item) == GT_OK && item.argument_count == 1 &&
              !strcmp(item.arguments[0], "host name") && item.source_line == 3 &&
              !strcmp(item.raw, "\tHostName = \"host name\" # keep\r\n"),
          "native scanner handles tabs equals quotes comments and CRLF");
    CHECK(gt_config_directive_at(cfg, "demo", 1, &item) == GT_OK && item.render_per_line &&
              !strcmp(item.arguments[0], "/keys/中文 #one"),
          "decoded cumulative identity");
    CHECK(gt_config_directive_at(cfg, "sibling", 5, &item) == GT_OK &&
              !strcmp(item.arguments[0], "other"),
          "sibling sees same physical block");
    CHECK(gt_config_directive_at(cfg, "demo", 6, &item) == GT_ERR_NOT_FOUND && !item.keyword &&
              !item.arguments && !item.argument_count && !item.raw && !item.source_path &&
              !item.source_line && !item.included && !item.render_per_line,
          "end clears every borrowed output");
    CHECK(gt_config_directive_at(NULL, "demo", 0, &item) == GT_ERR_INVALID_ARG && !item.keyword,
          "invalid config clears output");
    CHECK(gt_config_directive_at(cfg, "", 0, &item) == GT_ERR_INVALID_ARG, "empty alias");
    CHECK(gt_config_directive_at(cfg, "demo", 0, NULL) == GT_ERR_INVALID_ARG, "null output");
    CHECK(gt_config_directive_at(cfg, "missing", 0, &item) == GT_ERR_NOT_FOUND, "missing alias");
    char *roundtrip = gt_config_to_string(cfg);
    CHECK(roundtrip && !strcmp(roundtrip, text), "queries cannot rewrite original bytes");
    free(roundtrip);
    gt_config_free(cfg);
    return 0;
}

static int guarded(void) {
    const char *text = "Host dup\n User first\nHost dup\n User second\n";
    gt_config_t *cfg = gt_config_parse(text, strlen(text));
    gt_config_directive_info_t item;
    CHECK(cfg && gt_config_directive_at(cfg, "dup", 0, &item) == GT_ERR_UNSUPPORTED && !item.raw,
          "duplicate blocks cannot masquerade as one editable source");
    gt_config_free(cfg);
    text = "Host demo\n User first\nMatch all\n User conditional\n";
    cfg = gt_config_parse(text, strlen(text));
    CHECK(cfg && gt_config_directive_at(cfg, "demo", 0, &item) == GT_ERR_UNSUPPORTED && !item.raw,
          "conditional rules reject before any plausible prefix");
    gt_config_free(cfg);
    cfg = gt_config_parse("Host empty\n", 11);
    CHECK(cfg && gt_config_directive_at(cfg, "empty", 0, &item) == GT_ERR_NOT_FOUND,
          "an actually empty block has no directives");
    gt_config_free(cfg);
    return 0;
}

static int included(const char *root) {
    char directory[4096], child[4096], mainfile[4096], text[8192];
    CHECK(snprintf(directory, sizeof directory, "%s/directives-XXXXXX", root) <
              (int)sizeof directory,
          "temporary root length");
    CHECK(mkdtemp(directory), "owned temporary directory");
    CHECK(snprintf(child, sizeof child, "%s/child", directory) < (int)sizeof child, "child length");
    CHECK(snprintf(mainfile, sizeof mainfile, "%s/config", directory) < (int)sizeof mainfile,
          "main length");
    FILE *file = fopen(child, "wb");
    CHECK(file && fputs("User inherited\n", file) >= 0 && fclose(file) == 0, "write include");
    CHECK(snprintf(text, sizeof text, "Host main\n Include %s\n User local\n", child) <
              (int)sizeof text,
          "main text length");
    file = fopen(mainfile, "wb");
    CHECK(file && fputs(text, file) >= 0 && fclose(file) == 0, "write main");
    gt_config_t *cfg = gt_config_parse_file(mainfile);
    gt_config_directive_info_t item;
    CHECK(cfg && gt_config_directive_at(cfg, "main", 0, &item) == GT_OK &&
              !strcmp(item.keyword, "include"),
          "owned Include line remains visible");
    CHECK(gt_config_directive_at(cfg, "main", 1, &item) == GT_OK &&
              !strcmp(item.arguments[0], "local") && !item.included &&
              !strcmp(item.source_path, mainfile),
          "included expansion is not an owned main row");
    CHECK(gt_config_directive_at(cfg, "main", 2, &item) == GT_ERR_NOT_FOUND,
          "no inherited row can be mistaken for editable content");
    gt_config_free(cfg);
    CHECK(unlink(child) == 0 && unlink(mainfile) == 0 && rmdir(directory) == 0, "fixture cleanup");
    return 0;
}

static int failure_atomic(void) {
    const char original[] = "Host demo\n User before\n";
    gt_config_t *cfg = gt_config_parse(original, strlen(original));
    CHECK(cfg, "atomic edit base");
    const char *bad[] = {"\"unterminated", NULL};
    gt_config_edit_t edit = {.keyword = "User", .values = bad};
    CHECK(gt_config_edit(cfg, "demo", &edit, 1, false) != GT_OK, "invalid candidate is refused");
    char *observed = gt_config_to_string(cfg);
    CHECK(observed && !strcmp(observed, original), "failed parse preserves original tree bytes");
    free(observed);
    const char *injected[] = {"name\nHost injected", NULL};
    edit.values = injected;
    CHECK(gt_config_edit(cfg, "demo", &edit, 1, false) == GT_ERR_INVALID_ARG,
          "physical directive injection rejected");
    gt_config_free(cfg);
    const char nul[] = "Host demo\n User name\0suffix\n";
    CHECK(!gt_config_parse(nul, sizeof nul - 1), "embedded NUL cannot silently truncate");
    CHECK(!gt_config_parse("Host demo\n User \"bad\n", strlen("Host demo\n User \"bad\n")),
          "unterminated quote rejected");
    size_t length = 8192;
    char *source = malloc(length + 40);
    CHECK(source, "long fixture allocation");
    strcpy(source, "Host demo\n IdentityFile ");
    size_t prefix = strlen(source);
    memset(source + prefix, 'x', length);
    source[prefix + length] = '\n';
    source[prefix + length + 1] = 0;
    cfg = gt_config_parse(source, strlen(source));
    gt_config_directive_info_t info;
    CHECK(cfg && gt_config_directive_at(cfg, "demo", 0, &info) == GT_OK &&
              strlen(info.arguments[0]) == length,
          "long token is complete, never silently truncated");
    observed = gt_config_to_string(cfg);
    CHECK(observed && !strcmp(observed, source), "long source exact roundtrip");
    free(observed);
    free(source);
    gt_config_free(cfg);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2 || lexical() || guarded() || failure_atomic() || included(argv[1])) {
        return 1;
    }
    puts("config directives: native lexical view and ownership guards passed");
    return 0;
}
