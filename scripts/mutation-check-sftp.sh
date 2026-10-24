#!/usr/bin/env bash
# 变异验证：证明 SFTP e2e 断言有牙（非空跑）。
#
# 变异目标（都是「会让断言变红的目标失效状态」，且都仍能编译——否则红的
# 是「编译失败」而不是「断言失败」，那种红不能证明断言有牙）：
#   F1  stat 的 size 直接返回 0（尺寸断言失效）；
#   F2  list 不拼文件名（文件名断言失效）；
#   F3  download 写空文件（内容一致性断言失效）；
#   F4  job 完成上报 code 恒 OK（三态折叠——FAILED 也报成功）；
#   F5  remove_all 不递归（递归删除断言失效）；
#   F6  chmod 丢 mode（权限断言失效）；
#   F7  walk 忽略 max_depth（深度断言失效）；
#   F8  传输忽略 pause 标志（「暂停期间不得完成」断言失效）。
# （exec 通道的变异见 scripts/mutation-check-exec.sh，目标是 ssh.c。）
#
# 纪律（照 AGENTS.md）：起跑前扫残留标记；trap 复原；回滚比对 sha256；
# 无 sshd 时整体 SKIP。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-sftp

TMP="$ROOT/tmp"
mkdir -p "$TMP"
SRC="$ROOT/core/src/sftp.c"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如「关闭标签页不停会话」的变异：读循环永远在 poll，deinit
# 就永远等下去）。没有超时的话脚本会一直挂着，而不是把那次判成红。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

if ! command -v sshd >/dev/null 2>&1 && [ ! -x /usr/sbin/sshd ]; then
  echo "SKIP mutation-check-sftp: 本机无 sshd，闸门会 SKIP，变异无法验证"
  exit 0
fi

restore_all() {
  # 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

if grep -q "$MARK" "$SRC" 2>/dev/null; then
  echo "检测到残留变异标记（$MARK）——工作区不干净，拒绝起跑（exit 2）" >&2
  exit 2
fi

BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutsftp-XXXXXX")"; BACKUP="$BACKUP.c"
cp "$SRC" "$BACKUP"

run_e2e() {
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    gt_timeout 300 ctest --test-dir "$ROOT/core/build" -R ssh_e2e --output-on-failure >/dev/null 2>&1 )
}
expect_e2e() {
  if run_e2e; then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → e2e $actual"; PASS=$((PASS+1));
  else echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1)); fi
}
mutate() { python3 "$1" "$SRC"; }
rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}

# 每个变异：old 必须仍能编译（否则红的是编译失败，证明不了断言有牙）。
mk() { # mk <name> <file> ; then heredoc provides python body via stdin
  cat > "$TMP/$1.py"
}

echo "== 基线 =="
expect_e2e green "基线"

# ---- F1：stat 尺寸归零 ----
echo "== F1：stat 的 size 归零 =="
mk gtmut_f1 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  out->size = (a->flags & LIBSSH2_SFTP_ATTR_SIZE) ? a->filesize : 0;"
new="  out->size = 0; /* GT_MUTATION_MARK */"
assert old in s, "F1 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f1.py"; expect_e2e red "F1 远端尺寸断言失效"; rollback

# ---- F2：list 丢文件名 ----
echo "== F2：list 丢文件名 =="
mk gtmut_f2 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    strcpy(e->full_path + off, name);"
new="    strcpy(e->full_path + off, \"x\"); /* GT_MUTATION_MARK */"
assert old in s, "F2 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f2.py"; expect_e2e red "F2 列目录条目名断言失效"; rollback

# ---- F3：download 写空文件 ----
echo "== F3：download 不写内容 =="
mk gtmut_f3 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { rc = GT_JOB_FAILED; break; }"
new="    if (fwrite(\"\", 1, 0, f) != (size_t)n) { rc = GT_JOB_FAILED; break; } /* GT_MUTATION_MARK */"
assert old in s, "F3 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f3.py"; expect_e2e red "F3 下载内容断言失效"; rollback

# ---- F4：job 完成码折叠成 OK ----
echo "== F4：job 完成码折叠成 OK =="
mk gtmut_f4 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="static void emit_job_done(uint64_t job_id, int result, const char *detail) {"
new="static void emit_job_done(uint64_t job_id, int result, const char *detail) {\n  result = GT_JOB_OK; /* GT_MUTATION_MARK: 三态折叠 */"
assert old in s, "F4 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f4.py"; expect_e2e red "F4 job 失败被报成成功（三态折叠）"; rollback

# ---- F5：remove_all 不递归（改成逐个 unlink → 目录删不掉） ----
echo "== F5：remove_all 不递归 =="
mk gtmut_f5 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    int r = remove_all_rec(sftp, ents[i].full_path, depth + 1);"
new="    int r = gt_sftp_remove(sftp, ents[i].full_path); /* GT_MUTATION_MARK */"
assert old in s, "F5 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f5.py"; expect_e2e red "F5 递归删除断言失效"; rollback

# ---- F6：chmod 丢 mode（恒定 0644，仍「用」到 mode 以免 -Werror） ----
echo "== F6：chmod 丢 mode =="
mk gtmut_f6 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  a.permissions = (unsigned long)(mode & 07777);"
new="  a.permissions = (unsigned long)((mode & 0) | 0644); /* GT_MUTATION_MARK */"
assert old in s, "F6 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f6.py"; expect_e2e red "F6 权限位断言失效"; rollback

# ---- F7：walk 忽略 max_depth ----
echo "== F7：walk 忽略 max_depth =="
mk gtmut_f7 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    if (e->stat.is_dir && (max_depth < 0 || depth < max_depth)) {"
new="    if (e->stat.is_dir) { (void)max_depth; /* GT_MUTATION_MARK */"
assert old in s, "F7 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f7.py"; expect_e2e red "F7 深度断言失效"; rollback

# ---- F8：传输忽略 pause 标志 ----
echo "== F8：传输忽略 pause =="
mk gtmut_f8 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="    if (atomic_load(&j->paused)) { usleep(20 * 1000); continue; }"
new="    if (atomic_load(&j->paused) && false) { usleep(20 * 1000); continue; } /* GT_MUTATION_MARK */"
assert old in s, "F8 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f8.py"; expect_e2e red "F8 暂停断言失效（暂停期间仍完成）"; rollback

echo "== 回滚复绿 =="
expect_e2e green "回滚后"

restore_all
trap - INT TERM
echo
echo "mutation-check-sftp: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
