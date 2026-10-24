#!/usr/bin/env bash
# 变异验证：证明「本地 shell 是登录 shell」与「关得快」这两组断言有牙
# （core/tests/test_transport.c 的第 9、10 节）。
#
# 为什么需要这两组：
#   1. GUI（Finder/Dock）启动的进程环境里 PATH 只有 /usr/bin:/bin:/usr/sbin:/sbin，
#      而 macOS 的 PATH 由 /etc/zprofile 里的 path_helper 补全——zprofile 只对
#      **登录 shell** 生效。不把 shell 起成登录 shell，用户 ~/.zshrc 里的
#      brew / go / gls 全是「command not found」。
#   2. close 必须快：交互式 shell **忽略 SIGHUP**（这是它的常规行为），所以
#      关闭路径的宽限必须短并果断升级 SIGKILL。宽限写长了不报错，只是
#      「关一个标签页卡 2 秒」。
#
#   L1 不做登录 shell 处理（argv[0] 不带 `-`）→ 断言红；
#   L2 argv[0] 漏掉前导 `-`（照抄 basename 的自然写法）→ 断言红；
#   L3 close 只发单个 child（不打整组）→ 关闭慢 → 断言红；
#   L4 回退成「给了 executable 就不当交互 shell」（分屏 brew 丢失的原版
#      形态）→ 断言红；
#   L5 `-l` 白名单恒假 → fish 那条红。
#
# 历史：这里原先是验 login(1) 包装的（不包装 / 丢 argv / argv 槽数不足）。
# 那套方案被整个换掉了——/usr/bin/login 是 setuid root，非 root 父进程杀不掉
# 它，close 会卡在 pthread_join（详见 core/src/io/pty_posix.c 的长注释）。
# 换法之后那三条的目标不复存在，故整组重写。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap 复原（EXIT 也复原：
# 被 kill -9 时 INT/TERM 不跑，变异会留在源码里）；回滚后比对起跑前 sha256。
# 注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
SRC="$ROOT/core/src/io/pty_posix.c"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如宽限写长的变异：close 要等满 2 秒；更糟的写法会等不到头）。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

mkdir -p "$TMP"

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
  echo "检测到残留变异标记（${MARK}）——拒绝起跑（exit 2）" >&2; exit 2
fi
BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutlogin-XXXXXX")"; BACKUP="$BACKUP.c"
cp "$SRC" "$BACKUP"

run_transport() { # 第 2 个参数是临时目录（§9 要干净 HOME + 两个假 shell）
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    timeout 120 "$ROOT/core/build/test_transport" "$TMP" >/dev/null 2>&1 )
}
run_config() { # 指向性对照：另一组核心断言（ssh_config）在同样变异下仍绿
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    timeout 120 "$ROOT/core/build/test_config" "$ROOT/core/tests/fixtures" >/dev/null 2>&1 )
}
expect() { # ${1}=期望 red|green ${2}=描述 ${3}=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}

echo "== 基线 =="
expect green "pty 断言基线" run_transport
expect green "对照（ssh_config）基线" run_config

# ---- L1：不做登录 shell 处理 ----
echo "== L1：不做登录 shell 处理（argv[0] 不带 \`-\`） =="
# 变异必须**仍然可编译**：直接让 macos_login_argv 返回 NULL 会让它变成
# 未使用函数，被 -Werror 拦下 → 构建失败 → 对照组也红（那不是指向性问题，
# 是变异不可编译）。所以保留函数体、让它提前返回。
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = '  *out_dash0 = dash0;'
new = '  pty_free(dash0); *out_dash0 = NULL; /* GT_MUTATION_MARK：不做登录 shell */'
assert old in s, "L1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "L1 默认 shell 不再是登录 shell" run_transport
expect green "L1 指向性（ssh_config 不受影响）" run_config
rollback

# ---- L2：argv[0] 漏掉前导 `-` ----
echo "== L2：argv[0] 漏掉前导 \`-\` =="
# 「照抄 basename」是不查手册就会写出来的形态，但那样 shell 不会把自己当
# 登录 shell——正是要防的。
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = '  snprintf(dash0, n, "-%s", base);'
new = '  snprintf(dash0, n, "%s", base); /* GT_MUTATION_MARK：漏了前导 - */'
assert old in s, "L2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "L2 argv[0] 缺前导 \`-\`（不是登录 shell）" run_transport
rollback

# ---- L3：close 时只发单个 child（不打整组） ----
echo "== L3：close 时只发单个 child（不打整组） =="
# 登录 zsh 会把它的子进程放进**它自己的**组，只给 shell 发信号收不掉它们；
# 从端被后代持有 → close 要等到 SIGKILL 生效甚至更久（原先实测 2.3 秒）。
# 这条变异的形态就是「退回只发单个」——很自然，因为「别打到自己组」这条
# 顾虑容易让人干脆不发整组。
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (!group_is_ours && pg > 0) kill(-pg, SIGHUP);\n    kill(t->child, SIGHUP);"
new = "    kill(t->child, SIGHUP); /* GT_MUTATION_MARK：不打整组 */"
assert old in s, "L3(a) 锚点缺失"
s = s.replace(old, new, 1)
old2 = "      if (!group_is_ours && pg > 0) kill(-pg, SIGKILL);\n      kill(t->child, SIGKILL);"
new2 = "      kill(t->child, SIGKILL); /* GT_MUTATION_MARK：不打整组 */"
assert old2 in s, "L3(b) 锚点缺失"
open(p, 'w').write(s.replace(old2, new2, 1))
PYEOF
expect red "L3 只发单个 child（后代拖住从端）" run_transport
rollback

# ---- L4：回退成「给了 executable 就不当交互 shell」 ----
echo "== L4：拿「给没给 executable」当判据（分屏 brew 丢失的原版） =="
# 这就是被修掉的那个 bug 的原形：新建标签页不传 exe 所以是好的，分屏传了
# 偏好里的 shell 所以是坏的——同一个 app 两种行为，屏幕上还不报错。
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = '  bool is_shell = (o->cmd_kind == GT_PTY_CMD_SHELL);'
new = ('  bool is_shell = (o->cmd_kind == GT_PTY_CMD_SHELL) &&\n'
       '                  !(o->executable && *o->executable);'
       ' /* GT_MUTATION_MARK：拿给没给 exe 当判据 */')
assert old in s, "L4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "L4 指定 shell 的那格不再是登录 shell" run_transport
expect green "L4 指向性（ssh_config 不受影响）" run_config
rollback

# ---- L5：`-l` 白名单恒假 ----
echo "== L5：\`-l\` 白名单恒假 =="
# fish 官方只文档化 `-l`，argv[0] 前缀没有保证；白名单塌成恒假 = fish 用户
# 静默回到短 PATH（铁律 #6：不许静默降级）。
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = '  const char *base = strrchr(shell, \'/\');'
new = '  return false; /* GT_MUTATION_MARK：白名单恒假 */\n' + old
assert old in s, "L5 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "L5 名单内的 shell 拿不到 -l" run_transport
rollback

echo "== 回滚复绿 =="
expect green "回滚后 pty 断言复绿" run_transport
expect green "回滚后 ssh_config 断言仍绿" run_config

restore_all
echo
echo "mutation-check-login-shell: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
