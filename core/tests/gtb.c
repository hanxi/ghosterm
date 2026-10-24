/*
 * gtb — writeback 测试驱动（薄壳，把 writeback.json 场景喂给 C 编辑 API）。
 * 仅测试用，不安装。
 *
 *   gtb edit <conf> <out> <alias> <reset 0|1> <spec>
 *
 * spec 为 tab 分隔的「分组」格式（value 可含空格，故用 tab 定界）：
 *   KW\t<Keyword>      开始一条编辑指令
 *   V\t<value>         给当前指令追加一个 token（可多行）
 *   DEL                当前指令删除（values=NULL）
 */
#include "ghosterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EDITS 64
#define MAX_VALS 64

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = calloc(1, (size_t)sz + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)sz, f);
  buf[got] = '\0';
  fclose(f);
  if (out_len) *out_len = got;
  return buf;
}

typedef struct {
  char *kw;
  char **vals; /* NULL 结尾；NULL = 删除 */
  size_t nv;
} spec_edit_t;

static int build_spec(const char *text, spec_edit_t *out, size_t *out_n) {
  size_t n = 0;
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    char line[2048];
    if (len >= sizeof line) len = sizeof line - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    if (len && line[len - 1] == '\r') line[--len] = '\0';

    char *tab = strchr(line, '\t');
    char *op = line;
    char *rest = NULL;
    if (tab) { *tab = '\0'; rest = tab + 1; }
    if (strcmp(op, "KW") == 0 && rest && n < MAX_EDITS) {
      out[n].kw = strdup(rest);
      out[n].vals = calloc(MAX_VALS, sizeof(char *));
      out[n].nv = 0;
      n++;
    } else if (strcmp(op, "V") == 0 && n > 0 && rest) {
      spec_edit_t *e = &out[n - 1];
      if (e->nv < MAX_VALS - 1) e->vals[e->nv++] = strdup(rest);
    } else if (strcmp(op, "DEL") == 0 && n > 0) {
      spec_edit_t *e = &out[n - 1];
      for (size_t i = 0; i < e->nv; i++) free(e->vals[i]);
      e->nv = 0;
      free(e->vals);
      e->vals = NULL; /* 删除标记 */
    }
    if (!nl) break;
    p = nl + 1;
  }
  *out_n = n;
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 7 || strcmp(argv[1], "edit") != 0) {
    fprintf(stderr, "usage: %s edit <conf> <out> <alias> <reset> <spec>\n", argv[0]);
    return 2;
  }
  size_t clen = 0, plen = 0;
  char *text = read_file(argv[2], &clen);
  char *spec = read_file(argv[6], &plen);
  if (!text || !spec) { fprintf(stderr, "gtb: read failed\n"); return 2; }

  gt_config_t *cfg = gt_config_parse(text, clen);
  if (!cfg) { fprintf(stderr, "gtb: parse: %s\n", gt_last_error()); return 1; }

  spec_edit_t se[MAX_EDITS];
  gt_config_edit_t edits[MAX_EDITS];
  size_t n = 0;
  build_spec(spec, se, &n);
  for (size_t i = 0; i < n; i++) {
    edits[i].keyword = se[i].kw;
    edits[i].values = (const char *const *)se[i].vals;
    if (se[i].vals) se[i].vals[se[i].nv] = NULL;
  }

  int rc = gt_config_edit(cfg, argv[4], edits, n, atoi(argv[5]) != 0);
  if (rc != GT_OK) {
    fprintf(stderr, "gtb: edit rc=%d: %s\n", rc, gt_last_error());
    return 1;
  }
  char *res = gt_config_to_string(cfg);
  FILE *f = fopen(argv[3], "wb");
  if (!f) { fprintf(stderr, "gtb: open out failed\n"); return 2; }
  fwrite(res, 1, strlen(res), f);
  fclose(f);

  for (size_t i = 0; i < n; i++) {
    free(se[i].kw);
    for (size_t k = 0; se[i].vals && se[i].vals[k]; k++) free(se[i].vals[k]);
    free(se[i].vals);
  }
  free(res);
  gt_config_free(cfg);
  free(text);
  free(spec);
  return 0;
}
