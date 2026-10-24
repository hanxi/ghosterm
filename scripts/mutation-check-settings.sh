#!/usr/bin/env bash
# 变异验证：证明**设置真的生效**那批断言有牙（TerminalConfigTests）。
#
# 为什么单独一个脚本：设置页最容易变成摆设——文件写了、界面刷了、引擎没动。
# 这里的每个变异都造出「看起来成功、其实没生效」的状态，断言必须变红。
#
#   S1 配置加载后不检查诊断（坏配置也放行）；
#   S2 reload 不真的把新配置交给 app（设置改了没反应）；
#   S3 逐行合并把注释行当普通行（用户手写的注释被吃掉）；
#   S4 写侧不做外部修改检测（静默覆盖别人的编辑）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-settings

TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
SHIM="$ROOT/apple/GhostermTerminal/Sources/CGhostty/shim.c"
SETTINGS="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/SettingsStore.swift"
SURF="$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurface.swift"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如「关闭标签页不停会话」的变异：读循环永远在 poll，deinit
# 就永远等下去）。没有超时的话脚本会一直挂着，而不是把那次判成红。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP_SHIM=""; BACKUP_SETTINGS=""; BACKUP_SURF=""

mkdir -p "$TMP"

if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-settings: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-settings: 缺 GhosttyKit"; exit 0
fi

restore_all() {
# 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  # **三个文件逐个查**（不是看某一个）：变异可能只落在其中一个上，
  # 先前这里写的是 `$SRC`——那个变量在本脚本里根本不存在，于是在
  # `set -u` 下 EXIT trap 自己报错、脚本以 1 退出（闸门看着是「失败」，
  # 其实变异全绿）。一并修掉：trap 里不许再依赖不存在的变量。
  local dirty=0
  for f in "$SHIM" "$SETTINGS" "$SURF"; do
    grep -q "$MARK" "$f" 2>/dev/null && dirty=1
  done
  [ "$dirty" -eq 1 ] || return 0
  [ -n "$BACKUP_SHIM" ] && [ -f "$BACKUP_SHIM" ] && cp "$BACKUP_SHIM" "$SHIM" && rm -f "$BACKUP_SHIM"
  [ -n "$BACKUP_SETTINGS" ] && [ -f "$BACKUP_SETTINGS" ] && cp "$BACKUP_SETTINGS" "$SETTINGS" && rm -f "$BACKUP_SETTINGS"
  [ -n "$BACKUP_SURF" ] && [ -f "$BACKUP_SURF" ] && cp "$BACKUP_SURF" "$SURF" && rm -f "$BACKUP_SURF"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "$SHIM" "$SETTINGS" "$SURF"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（$MARK）于 $f——拒绝起跑（exit 2）" >&2; exit 2
  fi
done

SHA_SHIM="$(shasum -a 256 "$SHIM" | awk '{print $1}')"
SHA_SETTINGS="$(shasum -a 256 "$SETTINGS" | awk '{print $1}')"
SHA_SURF="$(shasum -a 256 "$SURF" | awk '{print $1}')"
BACKUP_SHIM="$(mktemp "$TMP/gt-mutset-shim-XXXXXX")"; BACKUP_SHIM="$BACKUP_SHIM.c"
BACKUP_SETTINGS="$(mktemp "$TMP/gt-mutset-settings-XXXXXX")"; BACKUP_SETTINGS="$BACKUP_SETTINGS.swift"
BACKUP_SURF="$(mktemp "$TMP/gt-mutset-surf-XXXXXX")"; BACKUP_SURF="$BACKUP_SURF.swift"
cp "$SHIM" "$BACKUP_SHIM"
cp "$SETTINGS" "$BACKUP_SETTINGS"
cp "$SURF" "$BACKUP_SURF"

run_tests() {
  ( cd "$PKG" && gt_timeout 300 swift test --filter TerminalConfigTests >/dev/null 2>&1 )
}             # 这一组是「设置生效」的断言
run_other() { # 指向性：另一组断言（标签页结构）在同样变异下应仍绿
  ( cd "$PKG" && gt_timeout 300 swift test --filter TabManagerTests >/dev/null 2>&1 )
}

expect() { # $1=期望 red|green  $2=描述  $3=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → $actual"; PASS=$((PASS+1))
  else echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1)); fi
}

rollback() {
  cp "$BACKUP_SHIM" "$SHIM"; cp "$BACKUP_SETTINGS" "$SETTINGS"
  cp "$BACKUP_SURF" "$SURF"
  local a b c
  a="$(shasum -a 256 "$SHIM" | awk '{print $1}')"
  b="$(shasum -a 256 "$SETTINGS" | awk '{print $1}')"
  c="$(shasum -a 256 "$SURF" | awk '{print $1}')"
  if [ "$a" != "$SHA_SHIM" ] || [ "$b" != "$SHA_SETTINGS" ] \
     || [ "$c" != "$SHA_SURF" ]; then
    echo "  ✗ 回滚后 sha256 与起跑前不一致！" >&2; FAIL=$((FAIL+1))
  fi
}

echo "== 基线 =="
expect green "设置断言基线" run_tests
expect green "对照（标签页结构）基线" run_other

# ---- S1：坏配置也放行（不检查诊断） ----
echo "== S1：配置诊断不检查（坏配置被当成有效） =="
python3 - "$SHIM" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    uint32_t n = ghostty_config_diagnostics_count(config);"
new = "    uint32_t n = 0; /* GT_MUTATION_MARK */"
assert old in s, "S1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "S1 无效配置不再显式失败" run_tests
expect green "S1 指向性（标签页结构不受影响）" run_other
rollback

# ---- S2：reload 不真的换配置 ----
echo "== S2：reload_config 不把新配置交给 app =="
python3 - "$SHIM" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "  ghostty_app_update_config(a->app, fresh);"
new = "  /* GT_MUTATION_MARK：设置改了引擎不知道 */"
assert old in s, "S2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "S2 重载配置不再生效" run_tests
rollback

# ---- S3：注释行被当普通行（用户笔记被吃掉） ----
# 变异要**两处一起改**才能造出目标失效状态：
#   (a) 去掉注释行的短路（否则注释行根本走不到解析那一步）；
#   (b) 解析键名时顺手把 `#` 也从两端剥掉（"通用地 normalize 一下" 是
#       现实中真会写出来的代码）。
# 只做 (a) 时 `# font-size` != `font-size`，行被原样保留，断言不会红——
# 第一版变异就栽在这里（记此备忘：造失效状态要真造出来，别停在形似）。
echo "== S3：注释掉的同名键被当成目标键 =="
python3 - "$SETTINGS" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old_skip = '                if trimmed.hasPrefix("#") || trimmed.isEmpty {'
new_skip = '                if trimmed.isEmpty { /* GT_MUTATION_MARK */'
old_name = '                let name = parts[0].trimmingCharacters(in: .whitespaces).lowercased()'
new_name = ('                let name = parts[0].trimmingCharacters('
            'in: CharacterSet(charactersIn: " \\t#")).lowercased() /* GT_MUTATION_MARK */')
assert old_skip in s, "S3(a) 锚点缺失"
assert old_name in s, "S3(b) 锚点缺失"
s = s.replace(old_skip, new_skip, 1).replace(old_name, new_name, 1)
open(p, 'w').write(s)
PY
expect red "S3 注释掉的同名键被改写" run_tests
rollback

# ---- S4：写侧不检测外部修改 ----
echo "== S4：写回不检测外部修改 =="
python3 - "$SETTINGS" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        if isExternallyModified() {\n            throw SettingsStoreError.conflict(\"mtime/size 与加载时不一致\")\n        }"
new = "        if false { /* GT_MUTATION_MARK */\n            throw SettingsStoreError.conflict(\"mtime/size 与加载时不一致\")\n        }"
assert old in s, "S4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "S4 外部编辑被静默覆盖" run_tests
rollback

# ---- S5：主题列表排序方向写反 ----
# 这条的牙在于：`availableThemes()` 是主题选择器的数据面，顺序必须是确定的
# 字典序（每次打开设置看到的排序都一样，也便于用户找）。
# 变异形态取**降序**而不是「删掉 .sorted()」：文件少时 macOS 的目录枚举顺序
# 碰巧就是字典序，删掉排序造不出确定性失效（第一版就栽在这里）；
# 降序同样真实（排序方向写反），且确定会红。
echo "== S5：主题列表按降序返回 =="
python3 - "$SETTINGS" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        return names.filter { !$0.hasPrefix(".") }.sorted()'
new = '        return names.filter { !$0.hasPrefix(".") }.sorted(by: >) /* GT_MUTATION_MARK */'
assert old in s, "S5 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "S5 主题列表顺序反了" run_tests
rollback

# ---- S6：TerminalApp.shared 不再是单例 ----
# 守的是 libghostty 的全局状态语义：`ghostty_init` 只跑一次，而 app 释放会
# 连带停掉全局状态——「建 app -> 释放 -> 再建」会拿到一个没有全局状态的 app。
# 实测症状：崩在 xev 的 mach 通知上（MachMsgFailed），且**只在套件里出现**，
# 单独跑那组测试永远不复现（所以这类问题必须由变异守住，不能靠人记得）。
echo "== S6：TerminalApp.shared 每次新建实例 =="
python3 - "$SURF" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        if let existing = sharedInstance { return existing }
        let made = try TerminalApp(configPath: configPath)
        sharedInstance = made
        return made"""
new = """        return try TerminalApp(configPath: configPath) /* GT_MUTATION_MARK */"""
assert old in s, "S6 anchor missing"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "S6 单例失效（配置重载落到别的实例上）" run_tests
rollback

echo "== 回滚复绿 =="
expect green "回滚后设置断言复绿" run_tests
expect green "回滚后标签页断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-settings: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
