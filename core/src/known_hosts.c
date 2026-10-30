#include "ghosterm.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>

struct gt_known_hosts {
  char *text;
  gt_known_host_entry_t *entries;
  size_t count;
};

/* Shared lexical reader. Preserve the established auth field-shape policy;
 * richer key validation belongs to a separately reviewed authentication change. */
bool gt__kh_split_line(char *line, gt_known_host_entry_t *out) {
  memset(out, 0, sizeof *out);
  out->marker = out->hosts = out->key_type = out->key_base64 = "";
  char *p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '#' || *p == '\n' || *p == '\r' || !*p) return false;
  char *nl = strpbrk(p, "\r\n");
  if (nl) *nl = 0;
  const char **fields[4];
  size_t count = 0;
  if (*p == '@') fields[count++] = &out->marker;
  fields[count++] = &out->hosts;
  fields[count++] = &out->key_type;
  fields[count++] = &out->key_base64;
  for (size_t i = 0; i < count && *p; i++) {
    *fields[i] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (!*p) break;
    *p++ = 0;
    while (*p == ' ' || *p == '\t') p++;
  }
  out->hashed = out->hosts[0] == '|';
  out->recognized = *out->hosts && *out->key_type && strlen(out->key_base64) >= 16 &&
      (strpbrk(out->key_base64, "/=") != NULL || strncmp(out->key_type, "ssh-", 4) == 0);
  return true;
}

gt_known_hosts_t *gt_known_hosts_parse(const char *text, size_t length) {
  if (!text || length > 1024 * 1024 || memchr(text, 0, length)) {
    gt__set_error("known_hosts snapshot: invalid text or size (maximum 1 MiB)");
    return NULL;
  }
  gt_known_hosts_t *snapshot = gt__alloc(sizeof *snapshot);
  if (!snapshot) return NULL;
  memset(snapshot, 0, sizeof *snapshot);
  snapshot->text = gt__strndup(text, length);
  if (!snapshot->text) { gt_known_hosts_free(snapshot); return NULL; }
  size_t start = 0, line = 1, capacity = 0;
  while (start < length) {
    char *newline = memchr(snapshot->text + start, '\n', length - start);
    size_t end = newline ? (size_t)(newline - snapshot->text) + 1 : length;
    if (newline) *newline = 0;
    gt_known_host_entry_t entry;
    if (gt__kh_split_line(snapshot->text + start, &entry)) {
      if (snapshot->count == 4096) {
        gt__set_error("known_hosts snapshot: too many records (maximum 4096)");
        gt_known_hosts_free(snapshot); return NULL;
      }
      if (snapshot->count == capacity) {
        size_t next = capacity ? capacity * 2 : 16;
        void *entries = gt__realloc(snapshot->entries, next * sizeof entry);
        if (!entries) { gt_known_hosts_free(snapshot); return NULL; }
        snapshot->entries = entries; capacity = next;
      }
      entry.source_line = line;
      entry.byte_start = start; entry.byte_end = end;
      snapshot->entries[snapshot->count++] = entry;
    }
    start = end; line++;
  }
  return snapshot;
}
size_t gt_known_hosts_count(const gt_known_hosts_t *snapshot) { return snapshot ? snapshot->count : 0; }
gt_status_t gt_known_hosts_entry_at(const gt_known_hosts_t *snapshot, size_t index, gt_known_host_entry_t *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!snapshot || !out) return GT_ERR_INVALID_ARG;
  if (index >= snapshot->count) return GT_ERR_NOT_FOUND;
  *out = snapshot->entries[index];
  return GT_OK;
}
void gt_known_hosts_free(gt_known_hosts_t *snapshot) {
  if (!snapshot) return;
  free(snapshot->entries); free(snapshot->text); free(snapshot);
}
