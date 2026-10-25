#!/usr/bin/env bash
# 变异验证：证明「dev 版更新检测」那批断言的牙（UpdateCheckTests + AboutWindowTests）。
#
# 这一条链条上最容易出的错，是**把「查不出来」读成「已是最新」**：本机没记录、包是
# dirty 的、网络挂了、清单字段不对——四种情况全都会画出一句「已是最新」，用户从此
# 再也不会收到新版提示，而代码一行错都没有。所以这里有七组变异，每组都要让对应
# 断言变红、且带一条**同变异下仍绿**的指向性对照。
#
#   U1 本机 commit 取不到时回落「已是最新」（三态被折成两态，铁律 #6）；
#   U2 判据回到比版本号（`CFBundleVersion` = GITHUB_RUN_NUMBER 按 workflow 文件计数，
#      dev 与 release 两轨不同源单调，比它会把更旧的读成更新）；
#   U3 清单解码给默认值（缺 `signature` 就当 `id`）——宽容一次，检测就永远「已是最新」；
#   U4 `installBlocked` 恒 false（ad-hoc 未公证也宣称能装）；
#   U5 「查不出来」的界面文案写回「已是最新」——文案本身就是断言对象，谎报即红；
#   U6 关于窗口里那一行整个不画（入口消失，用户问「检测更新入口在哪个界面」无解）；
#   U7 再给一颗「安装更新」按钮（点了装不上，比不给更糟）。
#
# 纪律（AGENTS.md）：起跑前扫残留变异标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256；判据反了当场 dump 最后一次运行输出（不许
# `>/dev/null` 一丢了事）；超时只走 scripts/lib/gt-timeout.sh 的 gt_timeout。
# `$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-app-update

PKG="$ROOT/apple/GhostermApp"
CHECK="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/UpdateCheck.swift"
ABOUT="$ROOT/apple/GhostermApp/Sources/GhostermApp/AboutWindow.swift"
FILES=("$CHECK" "$ABOUT")
MARK="GT_MUTATION_MARK"
TMPLOG="$ROOT/tmp/mutation-app-update-last.log"
PASS=0; FAIL=0
BACKUPS=()

mkdir -p "$ROOT/tmp"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-app-update: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-app-update: 缺 build/GhosttyKit.xcframework"; exit 0
fi
# 本机的 xcode-select 常指向 CommandLineTools（那里没有 XCTest 模块），完整 Xcode
# 装着就把 DEVELOPER_DIR 指过去——只影响这一次调用，不改系统设置。
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d /Applications/Xcode.app/Contents/Developer ] \
   && [ "$(xcode-select -p 2>/dev/null)" = "/Library/Developer/CommandLineTools" ]; then
  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  echo "（DEVELOPER_DIR → 完整 Xcode：CommandLineTools 里没有 XCTest 模块）"
fi

restore_all() {
  # 只有现场**还在变异态**才复原：脚本被 kill -9 后残留的 EXIT trap 会拿旧备份
  # 把源文件盖回去，连别人的新改动一起抹掉（`mutation-check-pixel.sh` 踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  [ "${#BACKUPS[@]}" -eq 0 ] && return 0
  local dirty=0 f
  for f in "${FILES[@]}"; do
    grep -q "$MARK" "$f" 2>/dev/null && dirty=1
  done
  [ "$dirty" -eq 1 ] || return 0
  local i=0
  for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f" 2>/dev/null || true; i=$((i+1)); done
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
  b="$(mktemp "$ROOT/tmp/gt-mutapp-XXXXXX")"; b="$b.swift"
  cp "$f" "$b"; BACKUPS+=("$b")
done

# 三层判据各一份：模型层（判据本身）、UI 层（入口画得出）、对照（谁都没碰的结构）。
run_model()  { ( cd "$PKG" && gt_timeout 420 swift test --filter UpdateCheckTests ) >"$TMPLOG" 2>&1; }
run_ui()     { ( cd "$PKG" && gt_timeout 420 swift test --filter AboutWindowTests ) >"$TMPLOG" 2>&1; }
run_other()  { ( cd "$PKG" && gt_timeout 420 swift test --filter TabManagerTests )  >"$TMPLOG" 2>&1; }
expect() {
  local want="$1" label="$2" fn="$3" actual
  if "$fn"; then actual=green; else actual=red; fi
  if [ "$actual" = "$want" ]; then echo "  ✓ ${label} → ${actual}"; PASS=$((PASS+1)); return 0; fi
  echo "  ✗ ${label} → 期望 ${want}，实际 ${actual}"; FAIL=$((FAIL+1))
  echo "---- dump ${TMPLOG}（尾部） ----"
  tail -n 60 "${TMPLOG}"
  echo "---------------------------------"
}
rollback() {
  local i=0 now
  for f in "${FILES[@]}"; do cp "${BACKUPS[$i]}" "$f"; i=$((i+1)); done
  i=0
  for f in "${FILES[@]}"; do
    now="$(shasum -a 256 "$f" | awk '{print $1}')"
    [ "$now" = "${SHAS[$i]}" ] || { echo "  ✗ 回滚后 ${f} 的 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
    i=$((i+1))
  done
}
# 锚点必须**在文件里恰好出现一次**：漂了就是变异根本没落地，不能拿「期望 red 实际
# green」冒充「断言没牙」。标记另起一行写成注释——贴在语句尾巴上会把 Swift 打坏，
# 于是「红」红得没有指向性。
mutate() {
  python3 - "$1" "$2" "$3" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path, encoding='utf-8').read()
if text.count(old) != 1:
    raise SystemExit(f"锚点漂移（出现 {text.count(old)} 次，要 1 次）：{old[:60]}…")
open(path, 'w', encoding='utf-8').write(text.replace(old, new + "\n// GT_MUTATION_MARK", 1))
PY
}

echo "== 基线 =="
expect green "模型层判据（UpdateCheckTests）" run_model
expect green "入口几何（AboutWindowTests）" run_ui
expect green "对照（标签页结构 TabManagerTests）" run_other

echo "== U1：本机 commit 取不到 → 谎报「已是最新」 =="
mutate "$CHECK" \
  'guard let raw = local.commit, raw != "unknown" else { return .cannotTell(.localCommitUnknown) }' \
  'guard let raw = local.commit, raw != "unknown" else { return .upToDate }'
expect red "U1 三态被折成两态" run_model
expect green "U1 指向性（入口几何不受影响）" run_ui
rollback

echo "== U2：主锚从 commit 换回版本号 =="
mutate "$CHECK" \
  'return theirs.hasPrefix(mine) ? .upToDate : .newer(remote)' \
  'return local.version == remote.version ? .upToDate : .newer(remote)'
expect red "U2 不比 commit、改比版本（跨轨不可比）" run_model
expect green "U2 指向性（对照：标签页结构）" run_other
rollback

echo "== U3：清单解码给默认值（缺 signature 就当 id） =="
mutate "$CHECK" \
  'signature = try text(.signature, "signature")' \
  'signature = (raw["signature"] as? String) ?? "id"'
expect red "U3 解码宽容一次，检测就永远可信" run_model
expect green "U3 指向性（入口几何）" run_ui
rollback

echo "== U4：installBlocked 恒 false =="
mutate "$CHECK" \
  'public var installBlocked: Bool { signature != "id" || !notarized }' \
  'public var installBlocked: Bool { false }'
expect red "U4 ad-hoc 未公证也宣称装得上" run_model
expect green "U4 指向性（对照：标签页结构）" run_other
rollback

echo "== U5：「查不出来」的文案写回「已是最新」 =="
mutate "$CHECK" \
  'case .cannotTell(let reason): return "查不出来：\(reason.text)"' \
  'case .cannotTell: return "已是最新"'
expect red "U5 文案撒谎（铁律 #6 在 UI 上的落点）" run_model
expect green "U5 指向性（入口几何仍绿）" run_ui
rollback

echo "== U6：关于窗口里那一行整个不画 =="
mutate "$ABOUT" \
  'UpdateRow(updater: updater)' \
  'EmptyView()'
expect red "U6 入口消失（按钮 / 本机身份行都数不到）" run_ui
expect green "U6 指向性（模型层不受影响）" run_model
rollback

echo "== U7：多给一颗「安装更新」按钮 =="
mutate "$ABOUT" \
  'Button(updater.isChecking ? "检查中…" : "检查更新") { updater.checkNow() }' \
  'Button("安装更新") { updater.checkNow() }
                    Button(updater.isChecking ? "检查中…" : "检查更新") { updater.checkNow() }'
expect red "U7 给了点了装不上的按钮" run_ui
expect green "U7 指向性（模型层仍绿）" run_model
rollback

echo "== 回滚复绿 =="
expect green "回滚后模型层" run_model
expect green "回滚后入口几何" run_ui

echo
echo "通过 ${PASS} 项，失败 ${FAIL} 项"
if [ "$FAIL" -ne 0 ]; then echo "mutation-check-app-update: FAIL"; exit 1; fi
echo "mutation-check-app-update: PASS（5 组判据变异 + 2 组入口变异，各带指向性对照，SHA256 回滚）"
