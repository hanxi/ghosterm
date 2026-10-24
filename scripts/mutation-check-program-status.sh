#!/usr/bin/env bash
# 变异验证：证明 OSC 7501 / OSC 133 相关断言**有牙**。
#
# 三个变异各打一处「我们自己写的规则」；每个都配一条**同变异下仍绿**的
# 对照，证明红有指向性（不是整个闸门崩了）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中即 exit 2）；trap 复原；
# 回滚时比对起跑前记录的 sha256。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
STORE="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/ProgramStatusStore.swift"
BADGE="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/ProgramStatusBadge.swift"
TAB="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/TerminalTab.swift"
STRIP="$ROOT/apple/GhostermApp/Sources/GhostermApp/TabStrip.swift"
MARK="GT_MUTATION_MARK"
FILES=("$STORE" "$BADGE" "$TAB" "$STRIP")
BK="$TMP/gt-mutps/backup"          # 每份文件一个 .1/.2/.3 备份
PASS=0; FAIL=0

mkdir -p "$TMP/gt-mutps"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-program-status: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-program-status: 缺 GhosttyKit（先跑 scripts/build-ghostty.sh）"; exit 0
fi
for f in "${FILES[@]}"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${f}）——拒绝起跑（exit 2）" >&2; exit 2
  fi
done

declare -a SHA
for i in "${!FILES[@]}"; do SHA[$i]="$(shasum -a 256 "${FILES[$i]}" | awk '{print $1}')"; done

dirty() { local i; for i in "${!FILES[@]}"; do grep -q "$MARK" "${FILES[$i]}" 2>/dev/null && return 0; done; return 1; }
restore_all() {
  # 只有现场还在变异态才复原（残留 trap 会拿旧备份盖掉别人的新改动——踩过）。
  dirty || return 0
  local i
  for i in "${!FILES[@]}"; do
    [ -f "$BK.$i" ] && cp "$BK.$i" "${FILES[$i]}"
  done
}
snapshot() { local i; for i in "${!FILES[@]}"; do cp "${FILES[$i]}" "$BK.$i"; done; }
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

ck() { # $1=期望 $2=描述
  if ( cd "$ROOT" && bash scripts/apple-app-gate.sh >/dev/null 2>&1 ); then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → 闸门 $actual"; PASS=$((PASS+1))
  else echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1)); fi
}
marker_scan() { # $1=文件 $2=想看的两条断言（grep 模式）...
  LOG="$TMP/apple-app-test.log"
  if grep -q "$2" "$LOG" 2>/dev/null && grep -q "$3" "$LOG" 2>/dev/null; then
    echo "  ✓ $4"; PASS=$((PASS+1))
  else
    echo "  ✗ $4（见 ${LOG}）"; FAIL=$((FAIL+1))
    grep -E "$2|$3" "$LOG" 2>/dev/null | tail -4 | sed 's/^/      /'
  fi
}

echo "== 基线 =="
ck green "基线"

echo "== M1：clear 不再删子树（只删同名那一条） =="
snapshot
python3 - "$STORE" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old = """        records.removeAll { $0.id == id || $0.id.hasPrefix(prefix) }"""
new = """        records.removeAll { $0.id == id } /* GT_MUTATION_MARK */"""
assert old in s, "M1 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY
ck red "M1 clear 不删子树"
( cd "$ROOT" && bash scripts/apple-app-gate.sh >/dev/null 2>&1 || true )
marker_scan "$STORE" "testClearRemovesRecordAndItsDescendants.*failed" \
                     "testRootReportReplacesWholeRecord.*passed" \
                     "红落在 clear 断言、整条替换用例仍绿（红有指向性）"
restore_all

echo "== M2：程序文本里的不可见格式字符不再剥掉 =="
snapshot
python3 - "$BADGE" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old = """        for u in s.unicodeScalars where !isInvisibleFormat(u) {
            out.append(u)
        }"""
new = """        for u in s.unicodeScalars { out.append(u) } /* GT_MUTATION_MARK */"""
assert old in s, "M2 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY
ck red "M2 不可见字符不被剥掉"
restore_all

echo "== M3：OSC 133 的 kind 检查去掉（任何事件都清 working/blocked） =="
snapshot
python3 - "$TAB" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old = """        if event.kind == .promptStart {
            programStatus.removeEphemeral()
        }"""
new = """        /* GT_MUTATION_MARK */ programStatus.removeEphemeral()"""
assert old in s, "M3 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY
ck red "M3 不再区分 prompt_start"
( cd "$ROOT" && bash scripts/apple-app-gate.sh >/dev/null 2>&1 || true )
marker_scan "$TAB" "testNonPromptStartEventsDoNotClear.*failed" \
                   "testPromptStartClearsEphemeralRecords.*passed" \
                   "红落在「非 prompt_start 不清」断言、prompt_start 用例仍绿（红有指向性）"
restore_all

echo "== M4：摘要恒为空（角标与 blocked 说明都画不出来） =="
# 注意：变异必须**仍可编译**（AGENTS.md）。先前那版在 apply() 顶部 return，
# 触发了「'if' must have an unconditional 'else' to be used as expression」
# 的编译错误——闸门红得没有指向性。改成让 summary 恒为 nil，编译干净。
snapshot
python3 - "$STORE" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old = """    public var summary: Record? {
        func rank"""
new = """    public var summary: Record? {
        /* GT_MUTATION_MARK */ return nil
        func rank"""
assert old in s, "M4 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY
ck red "M4 摘要恒空 → 角标画不出来"
( cd "$ROOT" && bash scripts/apple-app-gate.sh >/dev/null 2>&1 || true )
marker_scan "$STORE" "testBlockedBadgePaintsOrangePixels.*failed" \
                     "testStatesMapToDistinctSymbolsAndProgress.*passed" \
                     "红落在**像素**断言、角标模型用例仍绿（红有指向性）"
restore_all

echo "== M5：标签胶囊的失败记号不再画（条件写死） =="
snapshot
python3 - "$STRIP" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old = """            if let lc = tab.lastCommandSummary, lc.failed {"""
new = """            if let lc = tab.lastCommandSummary, lc.exitCode == 999 { /* GT_MUTATION_MARK */"""
assert old in s, "M5 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY
ck red "M5 失败记号不画 → 像素断言红"
( cd "$ROOT" && bash scripts/apple-app-gate.sh >/dev/null 2>&1 || true )
marker_scan "$STRIP" "testFailedLastCommandPaintsRedDotInTabChip.*failed" \
                     "testUnknownExitCodePaintsNoRedDot.*passed" \
                     "红落在「失败要有红点」、未知退出码用例仍绿（红有指向性）"
restore_all

echo "== 回滚复绿 =="
restore_all
ok=1
for i in "${!FILES[@]}"; do
  [ "$(shasum -a 256 "${FILES[$i]}" | awk '{print $1}')" = "${SHA[$i]}" ] || ok=0
done
if [ "$ok" = 1 ]; then echo "  ✓ 回滚后全部目标文件 sha256 均一致"; PASS=$((PASS+1))
else echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); fi
ck green "回滚后"

trap - INT TERM
echo
echo "mutation-check-program-status: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
