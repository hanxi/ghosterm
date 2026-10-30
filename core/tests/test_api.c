/* test_api.c — ABI 冒烟：版本非空；未实现项必须显式报错（不得静默成功）。 */
#include "ghosterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail = 0, pass = 0;
#define CHECK(c, msg) do { if (c) pass++; else { fail++; printf("FAIL: %s\n", msg); } } while (0)

int main(void) {
  const char *v = gt_version();
  CHECK(v && *v, "gt_version() 非空");

  /* 三态枚举不得折叠 */
  gt_hostkey_decision_t d = GT_HOSTKEY_UNKNOWN;
  CHECK(d == GT_HOSTKEY_UNKNOWN, "hostkey 三态不得折叠");

  uint8_t preview[8] = {1}; size_t preview_size = 99;
  CHECK(gt_sftp_preview_controlled(NULL, ".", preview, sizeof preview, &preview_size,
      100, NULL, NULL) == GT_ERR_INVALID_ARG && !preview_size && !preview[0],
      "controlled preview rejects NULL and clears outputs");
  char browse_path[16] = "stale";
  gt_sftp_entry_t *browse_entries = NULL;
  size_t browse_count = 99;
  CHECK(gt_sftp_browse_controlled(NULL, ".", browse_path, sizeof browse_path,
      &browse_entries, &browse_count, 100, 100, NULL, NULL) == GT_ERR_INVALID_ARG &&
      !browse_path[0] && !browse_entries && browse_count == 0,
      "controlled browse rejects NULL and clears outputs");

  /* NULL 句柄的入口必须显式报错（铁律 #6：不静默成功）。
   * SFTP 已在 M4 实现：NULL 句柄是参数非法（INVALID_ARG），
   * 而不是 UNSUPPORTED——后者只属于「能力缺失」（libssh2 未编入）。 */
  gt_transport_t *sh = gt_transport_open_ssh_shell(NULL, NULL);
  CHECK(sh == NULL, "NULL 会话的 ssh shell transport 必须返回 NULL");
  CHECK(strlen(gt_last_error()) > 0, "失败必须留下可读错误");

  gt_status_t shell_status = GT_OK;
  CHECK(gt_transport_open_ssh_shell_controlled(NULL, NULL, 100, NULL, NULL, &shell_status) == NULL &&
        shell_status == GT_ERR_INVALID_ARG, "controlled shell rejects NULL with status");
  gt_ssh_opts_t shell_options = {.host="localhost", .username="fixture"};
  gt_ssh_t *shell_owner = gt_ssh_new(&shell_options);
  CHECK(shell_owner != NULL, "controlled shell argument test owner");
  shell_status = GT_OK;
  CHECK(gt_transport_open_ssh_shell_controlled(shell_owner, NULL, 0, NULL, NULL, &shell_status) == NULL &&
        shell_status == GT_ERR_INVALID_ARG, "spent shell budget cannot become default timeout");
  gt_ssh_close(shell_owner);

  gt_sftp_entry_t *entries = NULL;
  gt_status_t sftp_status = GT_OK;
  CHECK(gt_ssh_sftp_controlled(NULL, 100, NULL, NULL, &sftp_status) == NULL &&
        sftp_status == GT_ERR_INVALID_ARG, "controlled SFTP rejects NULL with status");
  gt_ssh_opts_t file_opts = {.host="localhost", .username="fixture"};
  gt_ssh_t *file_owner = gt_ssh_new(&file_opts);
  sftp_status = GT_OK;
  CHECK(gt_ssh_sftp_controlled(file_owner, 0, NULL, NULL, &sftp_status) == NULL &&
        sftp_status == GT_ERR_INVALID_ARG, "spent SFTP budget cannot become default timeout");
  gt_ssh_close(file_owner);
  size_t count = 0;
  int rc = gt_sftp_list(NULL, "/", &entries, &count);
  CHECK(rc == GT_ERR_INVALID_ARG, "NULL 句柄的 sftp_list 必须返回 INVALID_ARG");
  CHECK(entries == NULL && count == 0, "失败不得写出结果");
  CHECK(gt_sftp_stat(NULL, "/", &(gt_sftp_stat_t){0}) == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_stat 必须返回 INVALID_ARG");
  CHECK(gt_sftp_download(NULL, "/r", "/l") == 0,
        "NULL 句柄的 download 必须返回 0（无 job）");
  CHECK(gt_sftp_upload(NULL, "/l", "/r") == 0,
        "NULL 句柄的 upload 必须返回 0（无 job）");
  CHECK(gt_sftp_cancel(NULL, 0) == GT_ERR_INVALID_ARG,
        "job_id=0 的 cancel 必须返回 INVALID_ARG");
  CHECK(gt_sftp_cancel(NULL, 999) != GT_OK,
        "无此 job 的 cancel 不得返回 OK");

  /* 新增写侧/枚举入口：NULL 句柄一律 INVALID_ARG（不静默成功）。 */
  gt_sftp_stat_t st0;
  memset(&st0, 0, sizeof st0);
  CHECK(gt_sftp_lstat(NULL, "/", &st0) == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_lstat 必须返回 INVALID_ARG");
  char pbuf[64];
  CHECK(gt_sftp_readlink(NULL, "/", pbuf, sizeof pbuf) == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_readlink 必须返回 INVALID_ARG");
  CHECK(gt_sftp_realpath(NULL, "/", pbuf, sizeof pbuf) == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_realpath 必须返回 INVALID_ARG");
  CHECK(gt_sftp_mkdir(NULL, "/x", 0755) == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_mkdir 必须返回 INVALID_ARG");
  CHECK(gt_sftp_rmdir(NULL, "/x") == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_rmdir 必须返回 INVALID_ARG");
  CHECK(gt_sftp_remove(NULL, "/x") == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_remove 必须返回 INVALID_ARG");
  CHECK(gt_sftp_remove_all(NULL, "/x") == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_remove_all 必须返回 INVALID_ARG");
  CHECK(gt_sftp_rename(NULL, "/a", "/b") == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_rename 必须返回 INVALID_ARG");
  CHECK(gt_sftp_chmod(NULL, "/x", 0644) == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_chmod 必须返回 INVALID_ARG");
  CHECK(gt_sftp_walk(NULL, "/", -1, NULL, NULL) == GT_ERR_INVALID_ARG,
        "NULL 句柄的 sftp_walk 必须返回 INVALID_ARG");
  CHECK(gt_sftp_pause(NULL, 0, true) == GT_ERR_INVALID_ARG,
        "job_id=0 的 pause 必须返回 INVALID_ARG");

  /* exec：会话未连接 → NULL + 明确状态（不得静默成功）。 */
  {
    gt_status_t es = GT_ERR_INTERNAL;
    CHECK(gt_ssh_exec(NULL, "true", &es) == NULL && es != GT_OK,
          "NULL 会话的 exec 必须失败并给出状态码");
  }

  /* 编辑 API 的失败必须是可读错误、且不得破坏原配置（铁律 #6） */
  {
    gt_config_t *cfg = gt_config_parse("Host a\n  User x\n", strlen("Host a\n  User x\n"));
    CHECK(cfg != NULL, "parse 小样成功");
    if (cfg) {
      const char *vals[] = {"y", NULL};
      gt_config_edit_t e = {"User", vals};
      char *before = gt_config_to_string(cfg);
      int erc = gt_config_edit(cfg, "nope", &e, 1, false);
      CHECK(erc == GT_ERR_NOT_FOUND, "编辑不存在的 alias 返回 GT_ERR_NOT_FOUND");
      CHECK(strlen(gt_last_error()) > 0, "失败必须留下可读错误");
      char *after = gt_config_to_string(cfg);
      CHECK(before && after && strcmp(before, after) == 0, "编辑失败不得改动原配置");
      free(before); free(after);
      gt_config_free(cfg);
    }
    CHECK(gt_config_edit(NULL, "a", NULL, 0, false) == GT_ERR_INVALID_ARG, "edit(NULL) 返回 INVALID_ARG");
  }

  gt_ssh_opts_t so;
  memset(&so, 0, sizeof so);
  so.host = "example.invalid";
  so.username = "nobody";
  gt_ssh_t *ssh = gt_ssh_new(&so);
  CHECK(ssh != NULL, "gt_ssh_new 接受合法 opts");
  if (ssh) {
    /* 编入 libssh2 时对不可达目标返回 IO；未编入时返回 UNSUPPORTED。
     * 两种都要「非 0 + 可读错误」——不得静默成功（铁律 #6）。 */
    int src = gt_ssh_connect(ssh);
    CHECK(src != GT_OK, "ssh_connect 对不可达目标必须失败（不得静默成功）");
    CHECK(strlen(gt_last_error()) > 0, "ssh_connect 失败必须留下可读错误");
    /* 未握手就认证 → 显式 PROTOCOL，不得假装成功（认证与握手已拆开） */
    gt_ssh_t *fresh = gt_ssh_new(&so);
    CHECK(fresh && gt_ssh_authenticate(fresh) != GT_OK,
          "未握手的 authenticate 必须失败");
    gt_ssh_close(fresh);
    gt_ssh_close(ssh);
  }

  /* ---- 私钥是否需要口令（三态；只看文件头，不解密）----
   * 这条存在的理由：上层据它决定「问不问用户私钥口令」。先前是
   * 「文件读得出来就先弹口令框」，没口令的钥匙（实测 ~/.ssh/id_rsa）
   * 也照问——用户以为自己的钥匙出了问题。 */
  {
    /* 未加密的 PEM（PKCS#1）。 */
    CHECK(gt_ssh_key_needs_passphrase(
              "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n") == 0,
          "未加密的 PEM 私钥 → 0（不需要口令）");
    /* 传统 PEM 的加密标记。 */
    CHECK(gt_ssh_key_needs_passphrase(
              "-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\n"
              "DEK-Info: AES-128-CBC,0011\nAAAA\n-----END RSA PRIVATE KEY-----\n") == 1,
          "Proc-Type: 4,ENCRYPTED → 1（需要口令）");
    /* PKCS#8 加密。 */
    CHECK(gt_ssh_key_needs_passphrase(
              "-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n"
              "-----END ENCRYPTED PRIVATE KEY-----\n") == 1,
          "PKCS#8 ENCRYPTED → 1（需要口令）");
    /* 不是钥匙 → 三态里的 unknown，**不是** 0。 */
    CHECK(gt_ssh_key_needs_passphrase("hello world") == -1,
          "认不出来的内容 → -1（三态不折叠）");
    CHECK(gt_ssh_key_needs_passphrase(NULL) == -1, "NULL → -1");
  }

  printf("test_api: %d passed, %d failed\n", pass, fail);
  return fail ? 1 : 0;
}
