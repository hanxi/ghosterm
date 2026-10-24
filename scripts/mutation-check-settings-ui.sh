#!/usr/bin/env bash
# 变异验证：证明**设置弹窗**那批断言的牙（SettingsUITests）。
#
# 为什么单独一份：设置页这次重做（页签形态 / 下拉 / 配色 / 快捷键表）
# 最容易出的错是「界面好看、数据面悄悄坏掉」——比如把 `theme` 拼成引擎
# 读不懂的形态（用户一保存就报错）、把 `/etc/shells` 的注释当成 shell、
# 或让快捷键表跟真菜单对不上（用户照着按没反应）。
#
#   U1 配色两档序列化成非法形态（`light:X` 不带 dark → 引擎报 InvalidValue）；
#   U2 `/etc/shells` 里的相对路径（不是合法 shell）被收进下拉；
#   U3 快捷键表说 ⌥⌘S，菜单里其实是 ⌘S（表与实装脱钩）；
#   U4 设置窗口的内容被**垂直居中**（不贴顶）——用户报的「切页时页签
#      图标位置一直变」就是它，而断言只看「贴顶差 = 0」就能抓住。
#   U5 配色档位取「实际生效名」时不看参数（`light ?? dark`）——两个下拉
#      都报浅色那个名字，用户报的「选了能生效、但下拉还显示原来那个」。
#   U6 下拉控件的宽度回到 `.frame(maxWidth:)`——同一个控件换值就换宽度
#      （实测 93 / 109 / 190 / 240 四种），用户报的「下拉宽度/对齐不稳」。
#   U7 配色色块被塞回 `Menu` 的 **label**——label 会被 AppKit 收编成按钮内容、
#      只认一个视图，多给的色块整个被吃掉，用户报的「主题预览没正常显示」。
#   U8 改外观偏好不再落到 AppKit（`NSApp.appearance`）——用户报的「切深/浅色
#      界面不动、要重启才变」（`applyToApplication` 只剩启动那一次）。
#   U9 设置窗口底色回到「viewDidLoad 里把动态色求成 CGColor」——`.cgColor` 按
#      `NSAppearance.current`（系统外观）解、不是窗口的 effectiveAppearance，
#      系统浅色 + 用户选深色会解出**白底** + 浅字，用户报的「看不清」。
#   U10 preferences 不是 @Published——改默认 shell 只写 UserDefaults、不发
#      objectWillChange，用户报的「命令行解释器切换后下拉列表没更新」。
#   U11 外观页定高不对表（640）——用户报的「外观设置下面空很多」，底部 145pt
#      留白，打上界。**锚点写的是当前那个数**（现在是 470），所以每次增删外观
#      页的行都要把锚点跟着改：锚点失配时这段会打 traceback 并以「期望 red 实际
#      green」收场，那不是断言没牙，是变异根本没落地。
#   U12 键盘页定高回到 420（装不下内容）——底部 91pt 藏在滚动里，打下界。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。`$var` 紧跟全角字符会被 bash 吃进变量名，
# 故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-settings-ui

TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
THEME="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/ThemeSelection.swift"
SHELLCAT="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/ShellCatalog.swift"
SHORTCUTS="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/Shortcuts.swift"
APPMODEL="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift"
MENU="$ROOT/apple/GhostermApp/Sources/GhostermApp/AppMenu.swift"
WIN="$ROOT/apple/GhostermApp/Sources/GhostermApp/SettingsWindow.swift"
SETTINGS="$ROOT/apple/GhostermApp/Sources/GhostermApp/SettingsView.swift"
FILES=("$THEME" "$SHELLCAT" "$SHORTCUTS" "$APPMODEL" "$MENU" "$WIN" "$SETTINGS")
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUPS=()

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-settings-ui: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-settings-ui: 缺 GhosttyKit"; exit 0
fi

restore_all() {
  [ "${#BACKUPS[@]}" -eq 0 ] && return 0
  local i=0
  for f in "${FILES[@]}"; do
    cp "${BACKUPS[$i]}" "$f" 2>/dev/null || true
    i=$((i+1))
  done
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "${FILES[@]}"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done
SHAS=()
for f in "${FILES[@]}"; do
  SHAS+=("$(shasum -a 256 "$f" | awk '{print $1}')")
  b="$(mktemp "$TMP/gt-mutsetui-XXXXXX")"; b="$b.swift"
  cp "$f" "$b"; BACKUPS+=("$b")
done

run_ui()    { ( cd "$PKG" && gt_timeout 300 swift test --filter SettingsUITests >/dev/null 2>&1 ); }
run_other() { ( cd "$PKG" && gt_timeout 300 swift test --filter TabManagerTests >/dev/null 2>&1 ); }
expect() {
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; FAIL=$((FAIL+1)); fi
}
rollback() {
  local i=0
  for f in "${FILES[@]}"; do
    cp "${BACKUPS[$i]}" "$f"; i=$((i+1))
  done
  i=0
  for f in "${FILES[@]}"; do
    local now; now="$(shasum -a 256 "$f" | awk '{print $1}')"
    [ "$now" = "${SHAS[$i]}" ] || { echo "  ✗ 回滚后 ${f} 的 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
    i=$((i+1))
  done
}

echo "== 基线 =="
expect green "设置弹窗断言基线" run_ui
expect green "对照（标签页结构）基线" run_other

# ---- U1：配色对序列化成引擎读不懂的形态 ----
echo "== U1：两档配色只写一半（light:X，缺 dark） =="
python3 - "$THEME" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '            return "light:\\(quote(l)),dark:\\(quote(d))"'
new = '            return "light:\\(quote(l))" /* GT_MUTATION_MARK */'
assert old in s, "U1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "U1 配对形态缺一半（往返不再逐字相同）" run_ui
expect green "U1 指向性（标签页结构不受影响）" run_other
rollback

# ---- U2：非绝对路径被当成 shell ----
# 为什么打这一条而不是「注释行」：`/etc/shells` 里真正有牙齿的是**绝对路径
# 校验**——`# /bin/foo` 这种注释行即使漏过 `hasPrefix("#")` 短路，解析出的
# 名字也带 `#`，与断言里的期望值对不上（第一版变异就栽在这里：改了短路，
# 断言照样绿——因为两处同时「错得一致」）。相对路径则一路畅通，只有
# 绝对路径那道闸门拦得住它。
echo "== U2：非绝对路径进了 shell 下拉 =="
python3 - "$SHELLCAT" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '            guard line.hasPrefix("/") else { continue }   // 只认绝对路径'
new = '            /* GT_MUTATION_MARK：不校验绝对路径 */'
assert old in s, "U2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "U2 相对路径被当成 shell" run_ui
rollback

# ---- U3：快捷键表与真菜单脱钩 ----
# 这一条打的是「说明书式过期」：表里写 ⌥⌘S，菜单里改回 ⌘S ——
# 用户照着按不会发生任何事，而两边各自都「看着对」。
echo "== U3：快捷键表与菜单脱钩（表 ⌥⌘S，菜单 ⌘S） =="
python3 - "$MENU" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        sidebar.keyEquivalentModifierMask = [.command, .option]'
new = '        /* GT_MUTATION_MARK：菜单改回 ⌘S */'
assert old in s, "U3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "U3 表里的键在菜单里按不出来" run_ui
rollback

# ---- U4：窗口高度不跟着内容走（停在开窗时的固定高度） ----
# 这一条正是用户报的症状的**原始形态**：窗口高度是固定的 480，不随当前页
# 变化；内容贴顶之后，短页会在底下留一大片空白、长页会被裁掉。
# （第一版变异拿掉了「钉顶」那条约束，结果断言照样绿——因为去掉位置约束
# 后 AppKit 把子视图放在 y=0，而容器高度就等于子视图高度，贴顶差还是 0。
# 造不出失效状态，记此备忘。）
echo "== U4：窗口高度不跟着当前页走 =="
python3 - "$WIN" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    private func applyFit() {\n        guard let window, let height = container.paneHeight, height > 0 else { return }"
new = "    private func applyFit() {\n        if true { return } /* GT_MUTATION_MARK：窗口不跟内容走 */\n        guard let window, let height = container.paneHeight, height > 0 else { return }"
assert old in s, "U4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "U4 各页窗口一样高（内容会被裁 / 留白）" run_ui
rollback

# ---- U5：配色档位取「实际生效名」时不看参数 ----
# 这一条是用户实测的第二个症状的**原始形态**：两档各选各的，但两个下拉都
# 报浅色那个名字——改深色那档写回文件是真的、终端也真的换了色，只有标题
# 在骗人（看起来像没选上）。断言要的是「两档报不同的名字」。
echo "== U5：两档配色都报浅色那个名字 =="
python3 - "$THEME" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        switch slot {
        case .light: return light ?? dark
        case .dark: return dark ?? light
        }"""
new = """        /* GT_MUTATION_MARK：不看 slot，两档都报浅色那个（回到用户报的 bug） */
        return light ?? dark"""
assert old in s, "U5 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "U5 两档报同一个名字（下拉标题骗人）" run_ui
expect green "U5 指向性（标签页结构不受影响）" run_other
rollback

# ---- U6：下拉控件回到会伸缩的 maxWidth ----
# 这一条是用户实测的第三个症状的**原始形态**：`PopupLabel` 用
# `.frame(maxWidth:)` 时，同一个控件换值就换宽度（短值缩、长值撑），
# 旁边那一列色块跟着左右挪、两个下拉之间也左右错开。
# 注意变异要落在**控件自己**身上（`PopupLabel.body` 里那一行），
# 不是调用点的 `.frame(...)`——调用点早就删干净了。
echo "== U6：下拉宽度随选中值伸缩（回到 maxWidth） =="
python3 - "$SETTINGS" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        .frame(width: width, alignment: .leading)"
new = "        /* GT_MUTATION_MARK：回到会伸缩的 maxWidth */\n        .frame(maxWidth: width, alignment: .leading)"
assert old in s, "U6 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "U6 同一个下拉换值就换宽度" run_ui
expect green "U6 指向性（标签页结构不受影响）" run_other
rollback

# ---- U7：配色色块被塞回 Menu 的 label（AppKit 把色块吃掉） ----
# 用户实测「主题预览没正常显示」的原始形态：色块当 `Menu` 的**label 里的兄弟
# 视图**——label 会被 AppKit 收编成按钮内容、只认一个视图，色块整个被吃掉
# （数据面读得到、`ThemeSwatch` 也构造了，就是不画）。断言数色块背景色的像素，
# 所以这条变异下必红。
echo "== U7：配色色块被塞回 Menu 的 label（色块不画） =="
python3 - "$SETTINGS" <<'PYU7'
import sys
p = sys.argv[1]; s = open(p).read()
old = """                PopupLabel(title: selected ?? "（不设置）")
            }
            .disabled(model.availableThemes.isEmpty)
            .accessibilityLabel(slot.title)
            .accessibilityValue(selected ?? "不设置")

            if let preview = preview(of: selected) {
                ThemeSwatch(preview: preview)
            }
"""
new = """                /* GT_MUTATION_MARK：色块塞回 label（被 AppKit 吃掉） */
                HStack { PopupLabel(title: selected ?? "（不设置）"); if let pv = preview(of: selected) { ThemeSwatch(preview: pv) } }
            }
            .disabled(model.availableThemes.isEmpty)
            .accessibilityLabel(slot.title)
            .accessibilityValue(selected ?? "不设置")
"""
assert old in s, "U7 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYU7
expect red "U7 色块进了 Menu label，画不出来" run_ui
expect green "U7 指向性（标签页结构不受影响）" run_other
rollback

# ---- U8：改外观偏好不再落到 AppKit（界面不实时刷新） ----
# 用户实测「切深/浅色界面不动，重启才变」的原始形态：`didSet` 只喂终端
# （libghostty），把 AppKit 那半截删掉——界面要等下次启动才跟上。
echo "== U8：外观偏好不落到 NSApp.appearance（要重启才变） =="
python3 - "$APPMODEL" <<'PYU8'
import sys
p = sys.argv[1]; s = open(p).read()
old = """            // 界面：AppKit 的外观一改，所有窗口（含设置窗口）随重画。
            appearance.applyToApplication()
            applyColorSchemeToTerminal()"""
new = """            /* GT_MUTATION_MARK：界面那半截不喂了（回到「要重启才变」） */
            applyColorSchemeToTerminal()"""
assert old in s, "U8 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYU8
expect red "U8 切偏好后 NSApp.appearance 不动" run_ui
expect green "U8 指向性（标签页结构不受影响）" run_other
rollback

# ---- U9：设置窗口底色回到会吃的 baked CGColor ----
# 用户实测「重启后设置界面看不清了」的原始形态：在 viewDidLoad（不在绘制
# 上下文里）把动态 `NSColor.windowBackgroundColor.cgColor` 求死。系统浅色 +
# 用户选深色时求出来是**白**底，SwiftUI 文字按深色画成浅色 → 白底浅字。
echo "== U9：设置窗口底色 bake 成 CGColor（深色下解成白底） =="
python3 - "$WIN" <<'PYU9'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    override func loadView() { view = SettingsBackgroundView() }

    override func viewDidLoad() {
        super.viewDidLoad()"""
new = """    override func loadView() { view = NSView() } /* GT_MUTATION_MARK：回到 baked cgColor */

    override func viewDidLoad() {
        super.viewDidLoad()
        view.wantsLayer = true
        view.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor /* GT_MUTATION_MARK */"""
assert old in s, "U9 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYU9
expect red "U9 深色下设置窗口底解成白（看不清）" run_ui
expect green "U9 指向性（标签页结构不受影响）" run_other
rollback

echo "== U10：preferences 不再 @Published（改偏好不发广播 → 下拉不刷新） =="
# 用户实测「命令行解释器切换后，下拉列表没有更新」的原始形态：`preferences`
# 是普通存储属性，`AppPreferences` 的 setter 写完 UserDefaults 不重赋 struct、
# 不发 `objectWillChange` → SwiftUI 不重画，下拉标题停在旧值。变异就是**把
# @Published 拿掉**（setter 仍是变异的，但没 @Published 兜底，赋值不广播）。
python3 - "$APPMODEL" <<'PYU10'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    @Published public var preferences: AppPreferences"
new = "    public var preferences: AppPreferences /* GT_MUTATION_MARK：不发广播 */"
assert old in s, "U10 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYU10
expect red "U10 改默认 shell 后 objectWillChange 不发（下拉不刷新）" run_ui
expect green "U10 指向性（标签页结构不受影响）" run_other
rollback

echo "== U11：外观页定高不收（回 640 → 底部空一大截） =="
python3 - "$SETTINGS" <<'PYU11'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            case .appearance: return 470"
new = "            case .appearance: return 640 /* GT_MUTATION_MARK：定高不对表 */"
assert old in s, "U11 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYU11
expect red "U11 外观页底部留白 ~190pt（定高远大于内容）" run_ui
rollback

echo "== U12：键盘页定高装不下内容（回 420 → 底部藏滚动里） =="
python3 - "$SETTINGS" <<'PYU12'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            case .keyboard: return 535"
new = "            case .keyboard: return 420 /* GT_MUTATION_MARK：定高不对表 */"
assert old in s, "U12 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYU12
expect red "U12 键盘页内容顶到窗口底（留白 < 2pt）" run_ui
rollback

echo "== 回滚复绿 =="
expect green "回滚后设置弹窗断言复绿" run_ui
expect green "回滚后标签页断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-settings-ui: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]