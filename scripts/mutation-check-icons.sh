#!/usr/bin/env bash
# 变异验证：证明图标闸门（build-icons.sh check + 各端几何断言）真的「有牙」。
#
# 每个变异都要造出**目标失效状态**，并证明：
#   · 基线（未变异）→ 绿；
#   · 变异 → **目标那条**红，且另一条不相关路径仍绿（红有指向性，不是工具链坏）；
#   · 回滚 → 复绿，且文件与起跑前 sha256 一致。
# 样板见 scripts/mutation-check-sshconfig.sh。
#
# 用法: bash scripts/mutation-check-icons.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/tmp"

MASTER="assets/icons/ghosterm.svg"
BUILDER="scripts/build-icons.sh"
SQUIRCLE="scripts/lib/icons_squircle.py"
MARK="MUTATION-CHECK-ICONS"

fail() { echo "MUTATION FAIL: $*" >&2; exit 1; }

# ---- 起跑前扫变异标记：命中即退出，别带着上次的痕迹开跑 ----
if grep -Rqs "$MARK" "$MASTER" "$BUILDER" "$SQUIRCLE" 2>/dev/null; then
  echo "检测到未复原的变异标记 ${MARK}，请先恢复现场" >&2
  exit 2
fi

# 起跑前记录 sha256（回滚时比对）
declare -A SHA_BEFORE
for f in "$MASTER" "$BUILDER" "$SQUIRCLE"; do
  SHA_BEFORE[$f]=$(shasum -a 256 "$f" | awk '{print $1}')
done

restore() {
  python3 - "$MASTER" "$BUILDER" "$SQUIRCLE" <<'PY'
import sys
# 这三个文件是本 PR 新增的（git 未跟踪），不能靠 git checkout 复原，必须就地改回。
master, builder, squircle = sys.argv[1:4]

t = open(master, encoding="utf-8").read()
t = t.replace('stop-color="#C6D1F3"', 'stop-color="#C6D1F2"')
# 标记是一整行 XML 注释：整行剔除，避免空白残留导致 sha256 对不上。
t = "".join(line for line in t.splitlines(True)
            if "MUTATION-CHECK-ICONS" not in line)
open(master, "w", encoding="utf-8").write(t)

b = open(builder, encoding="utf-8").read()
b = b.replace('"$bbox" 108 80', '"$bbox" 108 66')
open(builder, "w", encoding="utf-8").write(b)

s = open(squircle, encoding="utf-8").read()
s = s.replace("ring += list(reversed(ur))", "ring += ur", 1)
open(squircle, "w", encoding="utf-8").write(s)
PY
  local f
  for f in "$MASTER" "$BUILDER" "$SQUIRCLE"; do
    [ -f "$f" ] || { echo "回滚后 $f 不见了" >&2; return 1; }
    local now
    now=$(shasum -a 256 "$f" | awk '{print $1}')
    if [ "$now" != "${SHA_BEFORE[$f]}" ]; then
      echo "回滚后 $f 与起跑前 sha256 不一致" >&2
      echo "  起跑前: ${SHA_BEFORE[$f]}" >&2
      echo "  回滚后: $now" >&2
      return 1
    fi
  done
  return 0
}
on_exit() {
  local st=$?
  restore || echo "MUTATION FAIL: 现场未完全复原" >&2
  exit "$st"
}
trap on_exit EXIT
trap 'echo "中断，正在复原" >&2; exit 130' INT TERM

PASS=0
expect_red() {
  local desc=$1; shift
  if "$@" >/dev/null 2>&1; then fail "${desc}：预期红，实际绿（断言没牙）"; fi
  echo "   ok  ${desc} 红"
  PASS=$((PASS + 1))
}
expect_green() {
  local desc=$1; shift
  if ! "$@" >/dev/null 2>&1; then fail "${desc}：预期绿，实际红"; fi
  echo "   ok  ${desc} 绿"
  PASS=$((PASS + 1))
}

echo "== 基线：产物与母版一致 =="
bash scripts/build-icons.sh >/dev/null 2>&1
expect_green "基线 check" bash scripts/build-icons.sh check

echo "== A1：母版配色改 1/255 → 产物哈希对拍必须红 =="
python3 - "$MASTER" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
assert 'stop-color="#C6D1F2"' in t, "母版结构变了，变异脚本要跟着改"
# 只动渐变色、不动形状：验的是「母版改了没重生成」这条漂移检测。
# 变异必须是**合法 SVG**：只动颜色数值，标记用一行 XML 注释（放进属性里会成非法 XML）。
# 锚在母版本体渐变的末档（幽灵底部，白→浅蓝），改 1/255 即足以让全端产物哈希漂移。
t = t.replace('stop-color="#C6D1F2"', 'stop-color="#C6D1F3"', 1)
t = t.replace("</svg>", "  <!--MUTATION-CHECK-ICONS-->\n</svg>", 1)
open(p, "w", encoding="utf-8").write(t)
PY
expect_red "母版改了没重生成" bash scripts/build-icons.sh check
expect_green "同一变异下 apple 仍可生成（红有指向性）" bash scripts/build-icons.sh apple
restore

echo "== A2：Android 前景放大到 80dp → 安全区断言必须红 =="
python3 - "$BUILDER" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
assert '"$bbox" 108 66' in t, "生成脚本结构变了，变异脚本要跟着改"
open(p, "w", encoding="utf-8").write(t.replace('"$bbox" 108 66', '"$bbox" 108 80', 1))
PY
expect_red "Android 前景越出 72dp 安全区" bash scripts/build-icons.sh android
expect_green "同一变异下 harmonyos 不受影响（红有指向性）" bash scripts/build-icons.sh harmonyos
restore

echo "== A3：圆角外环拼接顺序错（自交）→ 生成端必须拒绝 =="
python3 - "$SQUIRCLE" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
assert "ring += ur" in t, "圆角脚本结构变了，变异脚本要跟着改"
open(p, "w", encoding="utf-8").write(
    t.replace("ring += ur", "ring += list(reversed(ur))", 1))
PY
expect_red "圆角外环自交被拒" python3 "$SQUIRCLE" 0.2237 /dev/null
restore

echo "== 回滚：全部复原 =="
restore
bash scripts/build-icons.sh >/dev/null 2>&1
expect_green "回滚后 check 复绿" bash scripts/build-icons.sh check

echo "MUTATION PASS（$PASS 项）"
