#!/usr/bin/env bash
# 变异验证：证明 terminal-gate / apple-app-gate 的**网格断言**有牙。
#
# 关键区别（这是本脚本存在的理由）：这两个闸门的核心断言读的是
# libghostty 网格里的文本（ghostty_surface_read_text），而不是我们自己记的
# 回调账。若把 feed 改成空操作，网格断言必须变红——否则说明我们测的是
# 自己的账本，不是引擎。
#
# 纪律（AGENTS.md）：起跑前扫残留标记；trap 复原；回滚后比对起跑前 sha256。
#
# T3/T4 覆盖 C shim（CGhostty/shim.c）的**浅/深色下发**：T3 砍掉「逐个 surface
# 下发」，T4 砍掉 `reload_config` 动作处理。两者都会让已有 surface 不换色
# （用户报的「切深色终端没刷新、新开的才对」），断言见
# `TerminalSurfaceTests.testColorSchemeSwitchReachesExistingSurface`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
SURF="$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurface.swift"
SHIM="$ROOT/apple/GhostermTerminal/Sources/CGhostty/shim.c"
VIEW="$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurfaceView.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0; BACKUP=""; SHIM_BACKUP=""; VIEW_BACKUP=""

mkdir -p "$TMP"

if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-terminal: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-terminal: 缺 GhosttyKit（先跑 scripts/build-ghostty.sh）"; exit 0
fi

restore_all() {
  # 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  if grep -q "$MARK" "$SURF" 2>/dev/null; then
    [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SURF" && rm -f "$BACKUP"
  fi
  if grep -q "$MARK" "$SHIM" 2>/dev/null; then
    [ -n "$SHIM_BACKUP" ] && [ -f "$SHIM_BACKUP" ] && cp "$SHIM_BACKUP" "$SHIM" && rm -f "$SHIM_BACKUP"
  fi
  if grep -q "$MARK" "$VIEW" 2>/dev/null; then
    [ -n "$VIEW_BACKUP" ] && [ -f "$VIEW_BACKUP" ] && cp "$VIEW_BACKUP" "$VIEW" && rm -f "$VIEW_BACKUP"
  fi
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "$SURF" "$SHIM" "$VIEW"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done

BASE_SHA="$(shasum -a 256 "$SURF" | awk '{print $1}')"
SHIM_SHA="$(shasum -a 256 "$SHIM" | awk '{print $1}')"
VIEW_SHA="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutterm-XXXXXX")"; BACKUP="$BACKUP.swift"
cp "$SURF" "$BACKUP"
SHIM_BACKUP="$(mktemp "$TMP/gt-mutshim-XXXXXX")"; SHIM_BACKUP="$SHIM_BACKUP.c"
cp "$SHIM" "$SHIM_BACKUP"
VIEW_BACKUP="$(mktemp "$TMP/gt-mutview-XXXXXX")"; VIEW_BACKUP="$VIEW_BACKUP.swift"
cp "$VIEW" "$VIEW_BACKUP"

run_terminal() { ( cd "$ROOT" && bash scripts/terminal-gate.sh >/dev/null 2>&1 ); }
run_app()      { ( cd "$ROOT" && bash scripts/apple-app-gate.sh >/dev/null 2>&1 ); }

expect() { # $1=期望 $2=描述 $3=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → 闸门 $actual"; PASS=$((PASS+1))
  else echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1)); fi
}

mutate() { # $1=标签 $2=目标文件；变异脚本本体从 heredoc 走 stdin
  # 锚点没对上 = 实现改了而这里没跟着改。此时**现场是没变异的**，后面所有
  # 判据都毫无意义（实测漂过一次：期望红却拿到别处的红，看着像有牙）。
  # 所以立刻复原并红掉整条套件，而不是继续跑。
  if ! python3 - "$2"; then
    restore_all
    echo "  ✗ $1 的变异锚点没对上——实现改了，判据必须跟着改（不许拿未变异的现场跑判据）" >&2
    echo
    echo "mutation-check-terminal: $PASS passed, $((FAIL + 1)) failed"
    exit 1
  fi
}

echo "== 基线 =="
expect green "terminal-gate 基线" run_terminal

# ---- T1：feed 变空操作（字节永不到达网格） ----
echo "== T1：feed 不再把字节送进网格 =="
mutate "T1" "$SURF" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="""    public func feed(_ data: Data) {
        guard !data.isEmpty else { return }"""
new="""    public func feed(_ data: Data) {
        if true { return } /* GT_MUTATION_MARK */
        guard !data.isEmpty else { return }"""
assert old in s, "T1 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY
expect red "T1 feed 被短路" run_terminal

# 双断言：同一变异下 app 闸门里**不依赖网格**的用例（主机列表）仍绿，
# 证明「红」落在网格断言上，不是整个闸门崩了。
# 注意：app 闸门失败时会退出，故直接看它落盘的测试日志，不看退出码。
( cd "$ROOT" && bash scripts/apple-app-gate.sh >/dev/null 2>&1 || true )
APP_LOG="$TMP/apple-app-test.log"
if grep -q "HostListTests.*passed" "$APP_LOG" 2>/dev/null; then
  echo "  ✓ 同一变异下主机列表用例仍绿（红落在网格断言上）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${APP_LOG}）"; FAIL=$((FAIL+1))
fi

# ---- T2：C→Swift 抄写改坏（app 丢掉） ----
# 这是我们**自己**的边界逻辑（0002 的 C 报告 → Swift 值）。它坏了，OSC 7501
# 的断言必须变红；而只查 title/msg 的 base64 用例应仍绿 —— 双断言。
echo "== T2：ProgramStatusReport 抄写时丢掉 app =="
# T1 的变异必须先复原，否则 T2 的现场会带着 T1（feed 被短路）——那样报告
# 根本到不了回调，红就没指向性了（实测踩过）。复原后重新备份再变异。
restore_all
BACKUP="$(mktemp "$TMP/gt-mutterm-XXXXXX")"; BACKUP="$BACKUP.swift"
cp "$SURF" "$BACKUP"
mutate "T2" "$SURF" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old = """        self.message = str(c.message)
    }
}"""
new = """        self.message = str(c.message)
        self.app = "" /* GT_MUTATION_MARK */
    }
}"""
assert old in s, "T2 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY
expect red "T2 app 被丢" run_terminal
# expect 的 run_terminal 已把**变异态**的日志写进 terminal-test.log，直接读它。
T2LOG="$TMP/terminal-test.log"
if grep -q "testProgramStatusReportReachesCallback.*failed" "$T2LOG" 2>/dev/null \
   && grep -q "testReportDecodesBase64TitleAndMessage.*passed" "$T2LOG" 2>/dev/null; then
  echo "  ✓ 红落在 app 断言、base64 用例仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${T2LOG}）"; FAIL=$((FAIL+1))
  grep -E "ProgramStatus|Base64" "$T2LOG" 2>/dev/null | tail -6 | sed 's/^/      /'
fi

# ---- T3：砍掉「逐个 surface 下发」（只剩 app 级） ----
# 这是用户实测症状的**原始形态**：app 级事件只改 app 自己的条件状态，已有
# surface 各自那份没变 → 不换色；新开的 surface 创建时才拷到新状态 → 看着
# 像「只有新开终端对」。断言用 mode 2031 的配色上报，必须变红。
echo "== T3：砍掉逐个 surface 下发（只剩 app 级） =="
mutate "T3" "$SHIM" <<'PY_T3'
import sys
p=sys.argv[1]; s=open(p).read()
old = """  for (size_t i = 0; i < a->surface_count; i++) {
    struct gt_ghostty_surface *s = a->surfaces[i];
    if (s && s->surface && !s->replica) ghostty_surface_set_color_scheme(s->surface, scheme);
  }"""
new = "  /* GT_MUTATION_MARK：砍掉逐个 surface 下发 */"
assert old in s, "T3 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY_T3
expect red "T3 已有 surface 不换色" run_terminal
T3LOG="$TMP/terminal-test.log"
if grep -q "testColorSchemeSwitchReachesExistingSurface.*failed" "$T3LOG" 2>/dev/null \
   && grep -q "testOutputReachesGhosttyGrid.*passed" "$T3LOG" 2>/dev/null; then
  echo "  ✓ 红落在配色断言、网格用例仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${T3LOG}）"; FAIL=$((FAIL+1))
fi
# T3 的变异必须复原（否则 T4 现场带着 T3，红就没指向性），且复原会**删掉备份**，
# 故这里重新备份 shim 再变异——与 T2 同一手法。
restore_all
SHIM_BACKUP="$(mktemp "$TMP/gt-mutshim-XXXXXX")"; SHIM_BACKUP="$SHIM_BACKUP.c"
cp "$SHIM" "$SHIM_BACKUP"

# ---- T4：砍掉 reload_config 动作处理（条件状态变了却不重下发） ----
# 另一半：即使逐个 surface 报了，libghostty 也只是把「该重读配置」当动作丢回
# 嵌入侧；不消费它，条件状态改了也不重派生 → 同样不换色。
echo "== T4：砍掉 reload_config 动作处理 =="
mutate "T4" "$SHIM" <<'PY_T4'
import sys
p=sys.argv[1]; s=open(p).read()
old = """  if (action.tag == GHOSTTY_ACTION_RELOAD_CONFIG) {
    struct gt_ghostty_app *a = ghostty_app_userdata(app);
    if (a && a->app && a->config) {
      if (target.tag == GHOSTTY_TARGET_APP) {
        ghostty_app_update_config(a->app, a->config);
        return true;
      }
      if (target.tag == GHOSTTY_TARGET_SURFACE && target.target.surface) {
        ghostty_surface_update_config(target.target.surface, a->config);
        return true;
      }
    }
    return false;
  }"""
new = "  /* GT_MUTATION_MARK：不消费 reload_config */"
assert old in s, "T4 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY_T4
expect red "T4 条件状态变了不重下发" run_terminal
T4LOG="$TMP/terminal-test.log"
if grep -q "testColorSchemeSwitchReachesExistingSurface.*failed" "$T4LOG" 2>/dev/null \
   && grep -q "testVisibilityIsReportedToProgramThroughHostWrite.*passed" "$T4LOG" 2>/dev/null; then
  echo "  ✓ 红落在配色断言、可见性用例仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${T4LOG}）"; FAIL=$((FAIL+1))
fi
restore_all

# ---- T5：输入法预编辑不下发（用户看不到正在打什么） ----
# 组合串必须经 preedit 进引擎做内联显示。观测口径是 libghostty 自己的
# `ime_point` 宽度（按预编辑串算）；不下发就恒 0。
echo "== T5：setPreedit 不下发（组合串看不到） =="
# T4 的 restore_all 已删掉 SURF 备份；重新备份再变异（否则没得复原，实测踩过）。
restore_all
BACKUP="$(mktemp "$TMP/gt-mutterm-XXXXXX")"; BACKUP="$BACKUP.swift"
cp "$SURF" "$BACKUP"
mutate "T5" "$SURF" <<'PY_T5'
import sys
p=sys.argv[1]; s=open(p).read()
old = """    public func setPreedit(_ text: String) {
        if text.isEmpty {
            gt_ghostty_surface_preedit(raw, nil, 0)
        } else {
            text.withCString { gt_ghostty_surface_preedit(raw, $0, strlen($0)) }
        }
    }"""
new = """    public func setPreedit(_ text: String) { /* GT_MUTATION_MARK：不下发预编辑 */ }"""
assert old in s, "T5 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY_T5
expect red "T5 预编辑不下发" run_terminal
T5LOG="$TMP/terminal-test.log"
if grep -q "testMarkedTextShowsAsEnginePreedit.*failed" "$T5LOG" 2>/dev/null \
   && grep -q "testCommittedChineseReachesTransportAsTypedInput.*passed" "$T5LOG" 2>/dev/null; then
  echo "  ✓ 红落在预编辑断言、提交通道仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${T5LOG}）"; FAIL=$((FAIL+1))
  grep -E "MarkedText|CommittedChinese" "$T5LOG" 2>/dev/null | tail -6 | sed 's/^/      /'
fi

# ---- T6：输入法提交被当成**粘贴**（中文被 bracketed paste 包裹） ----
# 提交必须走键入语义；错走粘贴语义时，开了 bracketed paste 的程序会把中文
# 当成一次粘贴（`ESC[200~ … ESC[201~`），断言因此变红。
echo "== T6：提交走成粘贴语义 =="
restore_all
BACKUP="$(mktemp "$TMP/gt-mutterm-XXXXXX")"; BACKUP="$BACKUP.swift"
cp "$SURF" "$BACKUP"
mutate "T6" "$SURF" <<'PY_T6'
import sys
p=sys.argv[1]; s=open(p).read()
old = """        text.withCString {
            gt_ghostty_surface_committed_text(raw, $0, strlen($0))
        }"""
new = """        text.withCString {
            gt_ghostty_surface_text(raw, $0, strlen($0)) /* GT_MUTATION_MARK：误当粘贴 */
        }"""
assert old in s, "T6 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY_T6
expect red "T6 提交被当粘贴" run_terminal
T6LOG="$TMP/terminal-test.log"
if grep -q "testCommittedChineseReachesTransportAsTypedInput.*failed" "$T6LOG" 2>/dev/null \
   && grep -q "testMarkedTextShowsAsEnginePreedit.*passed" "$T6LOG" 2>/dev/null; then
  echo "  ✓ 红落在提交断言、预编辑用例仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${T6LOG}）"; FAIL=$((FAIL+1))
  grep -E "MarkedText|CommittedChinese" "$T6LOG" 2>/dev/null | tail -6 | sed 's/^/      /'
fi

# ---- T7：keyDown 不再把按键交给输入法/终端（键盘整条断掉） ----
# 这条钉住「keyDown 必须走 NSTextInputClient 链路」：把 keyDown 整个短路，
# 普通按键就到不了 transport——正是用户报的「敲了没反应」的终态。
echo "== T7：keyDown 不路由按键 =="
mutate "T7" "$VIEW" <<'PY_T7'
import sys
p=sys.argv[1]; s=open(p).read()
old = """    public override func keyDown(with event: NSEvent) {
        keyTextAccumulator = []"""
new = """    public override func keyDown(with event: NSEvent) {
        if true { return } /* GT_MUTATION_MARK：不路由按键 */
        keyTextAccumulator = []"""
assert old in s, "T7 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY_T7
expect red "T7 keyDown 不路由" run_terminal
T7LOG="$TMP/terminal-test.log"
if grep -q "testKeyDownRoutesThroughTextInputClient.*failed" "$T7LOG" 2>/dev/null \
   && grep -q "testMarkedTextShowsAsEnginePreedit.*passed" "$T7LOG" 2>/dev/null; then
  echo "  ✓ 红落在 keyDown 断言、预编辑用例仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${T7LOG}）"; FAIL=$((FAIL+1))
  grep -E "MarkedText|KeyDown" "$T7LOG" 2>/dev/null | tail -6 | sed 's/^/      /'
fi
restore_all

# ---- T8：sendCommittedText 丢掉控制字符（#14451 吞键事故的形态） ----
# 上游 Ghostty discussion #14451：切输入法后的第一个键被 keyDown 里
# 「键盘布局 ID 变了 ⇒ 假定被输入法吃掉」的 guard 整键丢弃（tmux 前缀
# Ctrl+B 到不了 pty）。Ghosterm 的对应链路是「输入法提交控制字符 →
# sendCommittedText 原样下发」；若哪天给提交文本加上「过滤控制字符」的
# 清洗，tmux 前缀 / Ctrl+组合键就会重新被静默吞掉。断言
# （testCommittedControlCharReachesTransport）必须变红，且中文提交用例
# 仍绿（中文字符 ≥ 0x20，不受影响——红有指向性）。
echo "== T8：提交文本丢掉控制字符 =="
restore_all
BACKUP="$(mktemp "$TMP/gt-mutterm-XXXXXX")"; BACKUP="$BACKUP.swift"
cp "$SURF" "$BACKUP"
mutate "T8" "$SURF" <<'PY_T8'
import sys
p=sys.argv[1]; s=open(p).read()
old = """    public func sendCommittedText(_ text: String) {
        guard !text.isEmpty else { return }"""
new = """    public func sendCommittedText(_ text: String) {
        guard !text.isEmpty else { return }
        guard text.unicodeScalars.contains(where: { $0.value >= 0x20 }) else { return } /* GT_MUTATION_MARK：丢控制字符 */"""
assert old in s, "T8 锚点缺失"
open(p,'w').write(s.replace(old,new,1))
PY_T8
expect red "T8 提交的控制字符被丢" run_terminal
T8LOG="$TMP/terminal-test.log"
if grep -q "testCommittedControlCharReachesTransport.*failed" "$T8LOG" 2>/dev/null \
   && grep -q "testCommittedChineseReachesTransportAsTypedInput.*passed" "$T8LOG" 2>/dev/null; then
  echo "  ✓ 红落在控制字符断言、中文提交用例仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 无法确认红有指向性（见 ${T8LOG}）"; FAIL=$((FAIL+1))
  grep -E "CommittedControl|CommittedChinese" "$T8LOG" 2>/dev/null | tail -6 | sed 's/^/      /'
fi
restore_all

echo "== 回滚复绿 =="
restore_all
now="$(shasum -a 256 "$SURF" | awk '{print $1}')"
shim_now="$(shasum -a 256 "$SHIM" | awk '{print $1}')"
view_now="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
if [ "$now" = "$BASE_SHA" ] && [ "$shim_now" = "$SHIM_SHA" ] && [ "$view_now" = "$VIEW_SHA" ]; then
  echo "  ✓ 回滚后三处 sha256 一致"; PASS=$((PASS+1))
else
  echo "  ✗ 回滚后 sha256 不一致！（surf ${now} vs ${BASE_SHA}；shim ${shim_now} vs ${SHIM_SHA}；view ${view_now} vs ${VIEW_SHA}）" >&2
  FAIL=$((FAIL+1))
fi
expect green "回滚后 terminal-gate" run_terminal

trap - INT TERM
echo
echo "mutation-check-terminal: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]