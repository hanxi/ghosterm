#include "import-fixture.h"
#include "budget-posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MIB (1024u * 1024u)
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL fixture:%d: %s\n", __LINE__, #x);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static p0_budget budget;
static p0_budget_scope source, scratch, gui;
static GhosttyTerminal terminal;
static import_fixture data;
static bool healthy(void *ctx) {
  return p0_budget_result(ctx, GHOSTTY_SUCCESS) == GHOSTTY_SUCCESS;
}
static import_parts capture(void) {
  uint8_t *c = NULL, *r = NULL;
  size_t cn = 0, rn = 0;
  const GhosttyAllocator *a = p0_budget_allocator(&scratch, P0_BUDGET_SCRATCH);
  GhosttyResult status = ghostty_snapshot_p0_encode_alloc(terminal, a, &c, &cn);
  if (status != GHOSTTY_SUCCESS)
    fprintf(stderr, "encode status=%d source_fault=%d scratch_fault=%d\n",
            status, p0_budget_scope_fault(&source),
            p0_budget_scope_fault(&scratch));
  CHECK(status == GHOSTTY_SUCCESS);
  CHECK(!ghostty_terminal_p0_resources_encode_alloc(terminal, a, 16 * MIB, &r,
                                                    &rn));
  return (import_parts){c, r, cn, rn};
}
void import_fixture_open(import_fixture *out, const char *mode) {
  bool container = !strncmp(mode,"container-",10);if(container)mode+=10;
  bool image = !strcmp(mode, "image"), utf8 = !strcmp(mode, "utf8");
  CHECK(p0_budget_init(&budget, 224 * MIB));
  CHECK(p0_budget_scope_admit(&source, &budget, 64 * MIB) &&
        p0_budget_scope_admit(&scratch, &budget, 64 * MIB) &&
        p0_budget_scope_admit(&gui, &budget, 96 * MIB));
  CHECK(ghostty_terminal_p0_new_with_page_allocator(
            p0_budget_allocator(&source, P0_BUDGET_GENERAL),
            p0_budget_allocator(&source, P0_BUDGET_PAGES), &terminal, 80,
            24) == GHOSTTY_SUCCESS);
  if(container){GhosttyColorRgb bg={0,0,0},fg={255,255,255};
    CHECK(ghostty_terminal_set(terminal,GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,&bg)==GHOSTTY_SUCCESS && ghostty_terminal_set(terminal,GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,&fg)==GHOSTTY_SUCCESS);
    CHECK(p0_budget_resize_cells(&source,terminal,80,24,10,20)==GHOSTTY_SUCCESS);
  }
  size_t continuation = MIB;
  CHECK(ghostty_terminal_set(terminal,
                             GHOSTTY_TERMINAL_OPT_CONTINUATION_MAX_BYTES,
                             &continuation) == GHOSTTY_SUCCESS);
  uint64_t cap = MIB;
  CHECK(ghostty_terminal_set(terminal,
                             GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT,
                             &cap) == GHOSTTY_SUCCESS);
  if (!strcmp(mode, "deny-file")) {
    char cwd[4096], tmp[4200];
    CHECK(getcwd(cwd, sizeof(cwd)));
    CHECK(snprintf(tmp, sizeof(tmp), "%s/tmp", cwd) > 0);
    GhosttyString directory = {.ptr = (const uint8_t *)tmp, .len = strlen(tmp)};
    CHECK(ghostty_terminal_set(
              terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_TEMP_FILE,
              &directory) == GHOSTTY_SUCCESS);
  }
  const char *prefix =
      image ? "\033_Ga=T,i=7,f=32,s=1,v=1,q=2;/wAA/w==\033\\IMAGE α\033[3"
            : "\033[48;2;255;0;0m          \033[0mALPHA α\033[?2004h\033[3";
  if (utf8)
    prefix = "\033[48;2;255;0;0m          \033[0mALPHA α\xf0\x9f";
  CHECK(p0_budget_feed(&source, terminal, (const uint8_t *)prefix,
                       strlen(prefix)) == GHOSTTY_SUCCESS);
  data.initial = capture();
  static const uint8_t delta[] =
      "1m中文\033[0m\r\n\033[48;2;0;255;0m          "
      "\033[0m\033[5n\033]52;c;eA==\a\033]777;notify;title;body\a";
  static const uint8_t utf8_delta[] =
      "\x8c\x8d\r\n\033[48;2;0;255;0m          \033[0m";
  data.delta = utf8 ? utf8_delta : delta;
  data.delta_len = utf8 ? sizeof(utf8_delta) - 1 : sizeof(delta) - 1;
  CHECK(p0_budget_feed(&source, terminal, data.delta, data.delta_len) ==
        GHOSTTY_SUCCESS);
  data.final = capture();
  ghostty_terminal_free(terminal);
  terminal = NULL;
  CHECK(p0_budget_scope_close(&source));
  data.general = p0_budget_allocator(&gui, P0_BUDGET_GENERAL);
  data.pages = p0_budget_allocator(&gui, P0_BUDGET_PAGES);
  data.scope = &gui;
  data.healthy = healthy;
  *out = data;
}
void import_fixture_close(void) {
  const GhosttyAllocator *a = p0_budget_allocator(&scratch, P0_BUDGET_SCRATCH);
  ghostty_free(a, (void *)data.initial.canonical, data.initial.canonical_len);
  ghostty_free(a, (void *)data.initial.resources, data.initial.resources_len);
  ghostty_free(a, (void *)data.final.canonical, data.final.canonical_len);
  ghostty_free(a, (void *)data.final.resources, data.final.resources_len);
  CHECK(p0_budget_scope_close(&scratch) && p0_budget_scope_close(&gui) &&
        p0_budget_destroy(&budget));
}

void import_fixture_revoke(void) {
  CHECK(p0_budget_scope_set_limit(&gui, p0_budget_scope_read(&gui).live));
}
