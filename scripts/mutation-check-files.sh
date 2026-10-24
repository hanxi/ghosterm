#!/usr/bin/env bash
# 变异验证：证明 **文件浏览器后端 / 面板模型** 的断言有牙（无网可跑）。
#
#   F1 LocalFileBackend.remove 无视 recursive（「非递归删非空目录必须失败」失效）；
#   F2 LocalFileBackend.walk 无视 maxDepth（深度断言失效）；
#   F3 LocalFileBackend.list 过滤掉隐藏文件（条目数断言失效）；
#   F4 FilesPanelModel.visibleEntries 不把目录排前（目录优先断言失效）；
#   F5 工作区装配时只启动本地模型（远端模型启动接线失效）；
#   F6 FilesPanelModel.setPermissions 无视 recursive（递归 chmod 断言失效）；
#   F7 treeChildrenVisible 不过滤非目录（树应只显示目录）；
#   F8 followTick 无视 followsTerminalCwd（关掉跟随仍乱动）；
#   F9 PanelKeymap 把 ⇧⌘N 判成重命名（快捷键映射失效）；
#   F10 FlowLayout.rows 永不折行（「排不下必须折行」失效）；
#   F11 面板顶栏退回一行 HStack（「窄面板顶栏更高」失效 = 用户报的超框）；
#   F12 顶栏去掉 ✕ 关闭按钮（「给了 onClose 就该有关闭按钮」失效）；
#   F13 条目右键菜单把「重命名…」标成一个没人接的键（菜单承诺一个按不动的
#       快捷键——比不标更糟）。
#
# 纪律照 AGENTS.md：起跑扫残留标记；trap 复原；回滚比对 sha256。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-files

TMP="$ROOT/tmp"
mkdir -p "$TMP"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0

FILES=(
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/FileBackend.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/FilesPanelModel.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/TerminalTab.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/PanelKeymap.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermApp/FlowLayout.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermApp/FilesPanelView.swift"
)
declare -a BACKUPS=()

restore_all() {
  local i=0
  for f in "${FILES[@]}"; do
    if [ -n "${BACKUPS[$i]:-}" ] && [ -f "${BACKUPS[$i]}" ]; then cp "${BACKUPS[$i]}" "$f"; fi
    i=$((i+1))
  done
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "${FILES[@]}"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（$MARK in ${f}）——拒绝起跑（exit 2）" >&2; exit 2
  fi
done

i=0
for f in "${FILES[@]}"; do
  b="$(mktemp "$TMP/gt-mutfiles-XXXXXX")"; cp "$f" "$b"; BACKUPS[$i]="$b"; i=$((i+1))
done
sha_of() { shasum -a 256 "$1" | awk '{print $1}'; }
declare -a BASE_SHAS=()
i=0
for f in "${FILES[@]}"; do BASE_SHAS[$i]="$(sha_of "$f")"; i=$((i+1)); done

run_test() {
  ( cd "$ROOT/apple/GhostermApp" && gt_timeout 900 swift test --filter \
      "FilesPanelTests|PanelChromeTests" >/dev/null 2>&1 )
}
expect_green() {
  if run_test; then echo "  ✓ $1 → 测试 green"; PASS=$((PASS+1));
  else echo "  ✗ $1 → 期望 green，实际 red"; FAIL=$((FAIL+1)); fi
}
expect_red() {
  if run_test; then echo "  ✗ $1 → 期望 red，实际 green（断言无牙）"; FAIL=$((FAIL+1));
  else echo "  ✓ $1 → 测试 red"; PASS=$((PASS+1)); fi
}
mutate() { python3 "$1" "$2"; }
rollback() {
  local i=0
  for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f"; i=$((i+1)); done
  i=0
  for f in "${FILES[@]}"; do
    [ "$(sha_of "$f")" = "${BASE_SHAS[$i]}" ] || { echo "  ✗ 回滚后 sha256 不一致：$f" >&2; FAIL=$((FAIL+1)); }
    i=$((i+1))
  done
}
mk() { cat > "$TMP/$1.py"; }

echo "== 基线 =="
expect_green "基线"

echo "== F1：remove 无视 recursive =="
mk gtmut_f1 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="        if recursive {\n            try fm.removeItem(atPath: path)\n            return\n        }"
new="        if true { /* GT_MUTATION_MARK */\n            try fm.removeItem(atPath: path)\n            return\n        }"
assert old in s, "F1 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f1.py" "${FILES[0]}"; expect_red "F1 非递归删非空目录断言失效"; rollback

echo "== F2：walk 无视 maxDepth =="
mk gtmut_f2 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="            if maxDepth >= 0 && depth > maxDepth { en.skipDescendants(); continue }"
new="            if false && depth > maxDepth { en.skipDescendants(); continue } /* GT_MUTATION_MARK */"
assert old in s, "F2 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f2.py" "${FILES[0]}"; expect_red "F2 深度断言失效"; rollback

echo "== F3：list 过滤隐藏文件 =="
mk gtmut_f3 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="        let names = try fm.contentsOfDirectory(atPath: path)"
new="        let names = try fm.contentsOfDirectory(atPath: path).filter { !$0.hasPrefix(\".\") } /* GT_MUTATION_MARK */"
assert old in s, "F3 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f3.py" "${FILES[0]}"; expect_red "F3 条目数断言失效"; rollback

echo "== F4：visibleEntries 不把目录排前 =="
mk gtmut_f4 <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="            if a.isDirectory != b.isDirectory { return a.isDirectory }"
new="            if a.isDirectory != b.isDirectory { return a.isDirectory && false } /* GT_MUTATION_MARK */"
assert old in s, "F4 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_f4.py" "${FILES[1]}"; expect_red "F4 目录优先断言失效"; rollback

echo "== F5：工作区只启动本地文件模型 =="
mk gtmut_f5 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="        local.start()\n        remote.start()"
new="        local.start() /* GT_MUTATION_MARK */"
assert old in s, "F5 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f5.py" "${FILES[2]}"; expect_red "F5 双栏接线断言失效"; rollback

echo "== F6：setPermissions 无视 recursive =="
mk gtmut_f6b <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="            guard recursive, entry.isDirectory else { return }"
new="            guard recursive && false, entry.isDirectory else { return } /* GT_MUTATION_MARK */"
assert old in s, "F6 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f6b.py" "${FILES[1]}"; expect_red "F6 递归 chmod 断言失效"; rollback

echo "== F7：treeChildrenVisible 不过滤非目录 =="
mk gtmut_f7b <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="            let dirs = subs.filter(\.isDirectory).sorted {"
new="            let dirs = subs.sorted { /* GT_MUTATION_MARK */"
assert old in s, "F7 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f7b.py" "${FILES[1]}"; expect_red "F7 树只显示目录断言失效"; rollback

echo "== F8：followTick 无视跟随开关 =="
mk gtmut_f8b <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="        guard followsTerminalCwd, let cwd = cwdProvider?(), !cwd.isEmpty,\n              cwd != currentPath else { return }"
new="        guard let cwd = cwdProvider?(), !cwd.isEmpty, cwd != currentPath else { return } /* GT_MUTATION_MARK */"
assert old in s, "F8 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f8b.py" "${FILES[1]}"; expect_red "F8 跟随开关断言失效"; rollback

echo "== F9：PanelKeymap ⇧⌘N 判错 =="
mk gtmut_f9 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old='        case ("n", true, true): return .newFolder      // ⇧⌘N'
new='        case ("n", true, true): return .rename        /* GT_MUTATION_MARK */'
assert old in s, "F9 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f9.py" \
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/PanelKeymap.swift"
expect_red "F9 快捷键映射断言失效"; rollback

echo "== F10：FlowLayout.rows 永不折行 =="
# 用户报的「顶部按钮超框了」就是这个失效态：排不下也硬塞一行。
mk gtmut_f10 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="            } else if used + spacing + w <= available {"
new="            } else if true { /* GT_MUTATION_MARK */"
assert old in s, "F10 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f10.py" "${FILES[4]}"; expect_red "F10 折行规则失效"; rollback

echo "== F11：面板顶栏退回一行 HStack =="
mk gtmut_f11 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="                FlowLayout(spacing: DS.Padding.xs) {"
new="                HStack(spacing: DS.Padding.xs) { /* GT_MUTATION_MARK */"
assert old in s, "F11 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f11.py" "${FILES[5]}"; expect_red "F11 窄面板顶栏不折行（超框）"; rollback

echo "== F12：顶栏去掉 ✕ 关闭按钮 =="
mk gtmut_f12 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old="""                if let onClose {
                    GlassIconButton(systemName: "xmark", help: "关闭文件面板（⇧⌘E）",
                                    action: onClose)
                }
"""
new="                // GT_MUTATION_MARK：去掉关闭按钮\n"
assert old in s, "F12 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f12.py" "${FILES[5]}"; expect_red "F12 没有关闭按钮"; rollback

echo "== F13：右键菜单把「重命名…」标成没人接的键 =="
mk gtmut_f13 <<'PYX'
import sys
p=sys.argv[1]; s=open(p).read()
old='            .modifier(PanelMenuShortcut("r", modifiers: .command))'
new='            .modifier(PanelMenuShortcut("k", modifiers: .command)) /* GT_MUTATION_MARK */'
assert old in s, "F13 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PYX
mutate "$TMP/gtmut_f13.py" "${FILES[5]}"
expect_red "F13 菜单标的键倒推不回 PanelKeymap"; rollback

echo "== 回滚复绿 =="
expect_green "回滚后"

restore_all
trap - INT TERM
echo
echo "mutation-check-files: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
