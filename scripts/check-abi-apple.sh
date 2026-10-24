#!/usr/bin/env bash
# C ABI 对拍（Apple 绑定侧）：core/include 声明的符号 ↔ Apple 绑定真正引用的符号。
#
# 为什么需要它（AGENTS.md「C ABI 符号表」一节）：
#   scripts/check-abi.sh 只对拍「core 实现 ↔ 头文件」。头改了而**某一端没改**，
#   那个闸门看不见。本脚本把 Apple 端的引用面并进来。
#
# 两层断言：
#   1. **漂移**：Apple 在 core 命名空间里引用的 gt_* 必须都在头文件里声明
#      （引用了不存在的符号 = 绑定与头不同步）；
#   2. **覆盖登记**：头文件声明但 Apple 未引用的符号，必须出现在下面的
#      ALLOW_UNBOUND 里并写明理由——**新增遗漏会红**，逼着你显式登记。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
mkdir -p "$TMP"

# 故意未绑定到 Apple 的 core 符号（每行 "符号 理由"）。
# 口径：未实现的子系统（M4 的 SFTP）与宿主尚未使用的基础设施。
ALLOW_UNBOUND=$(cat <<'EOF'
gt_config_effective      通用视图（ssh -G 对拍用）：UI 暂只需要 typed 视图
gt_config_supported_keys 支持集枚举：同上，UI 未做「生效配置」面板
EOF
)
# 只取符号名（每行第一列；空行与注释行丢弃），落盘供 comm 使用。
echo "$ALLOW_UNBOUND" | awk 'NF && $1 !~ /^#/ {print $1}' | sort -u > "$TMP/apple_allow.txt"

APPLE_SRC=(
  "$ROOT/apple/GhostermCore/Sources/CGhosterm/shim.c"
  "$ROOT/apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift"
  "$ROOT/apple/GhostermCore/Sources/GhostermCore/RecoverableSession.swift"
  "$ROOT/apple/GhostermCore/Sources/GhostermCore/SessionSetup.swift"
  "$ROOT/apple/GhostermCore/Tests/GhostermCoreTests/GhostermCoreTests.swift"
  "$ROOT/apple/GhostermTerminal/Sources/CGhostty/shim.c"
  "$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurface.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/SessionController.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/HostListModel.swift"
  "$ROOT/apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift"
  "$ROOT/apple/GhostermApp/Tests/GhostermAppTests/GhostermAppTests.swift"
)

missing_src=0
for f in "${APPLE_SRC[@]}"; do
  [ -f "$f" ] || { echo "check-abi-apple: 缺文件 $f" >&2; missing_src=1; }
done
[ "$missing_src" -eq 0 ] || exit 2

# 头文件里的公共符号（与 check-abi.sh 同口径：排除 gt__ 内部符号与 _t 类型名）
grep -rhoE '\bgt_[a-z][a-z0-9_]*[[:space:]]*\(' "$ROOT/core/include" \
  | sed -E 's/[[:space:]]*\($//' | grep -vE '_t$' | sort -u > "$TMP/apple_declared.txt"

# Apple 侧引用：只取 core 命名空间（排除 CGhostty 自己的 gt_ghostty_/gt_host_）。
# 用 perl 而不是 grep：要排除「紧跟在双引号后的 gt_*」——那多半是字符串里的
# 消息文本（如 "gt_status 4: ..."）而不是符号引用。
perl -ne 'while (/[^"\w](gt_[a-z][a-z0-9_]*)/g) { print "$1\n" }' "${APPLE_SRC[@]}" \
  | grep -vE '^gt_(ghostty_|host_)' | grep -vE '_t$' | sort -u > "$TMP/apple_referenced.txt"

drift=$(comm -13 "$TMP/apple_declared.txt" "$TMP/apple_referenced.txt")
if [ -n "$drift" ]; then
  echo "check-abi-apple: Apple 引用了头文件未声明的符号（绑定漂移）：" >&2
  echo "$drift" >&2
  exit 1
fi

comm -23 "$TMP/apple_declared.txt" "$TMP/apple_referenced.txt" > "$TMP/apple_unbound.txt"
comm -23 "$TMP/apple_allow.txt" "$TMP/apple_unbound.txt" > "$TMP/apple_stale_allow.txt"
if [ -s "$TMP/apple_stale_allow.txt" ]; then
  echo "check-abi-apple: ALLOW_UNBOUND 里列了其实**已经绑定**的符号，请删掉：" >&2
  cat "$TMP/apple_stale_allow.txt" >&2
  exit 1
fi

comm -13 "$TMP/apple_allow.txt" "$TMP/apple_unbound.txt" > "$TMP/apple_unregistered.txt"
if [ -s "$TMP/apple_unregistered.txt" ]; then
  echo "check-abi-apple: 有符号既未绑定 Apple、也未登记理由：" >&2
  cat "$TMP/apple_unregistered.txt" >&2
  echo "（要么在 Apple 侧接上，要么加进本脚本的 ALLOW_UNBOUND 并写明理由）" >&2
  exit 1
fi

comm -12 "$TMP/apple_declared.txt" "$TMP/apple_referenced.txt" > "$TMP/apple_bound.txt"
bound=$(wc -l < "$TMP/apple_bound.txt" | tr -d ' ')
unbound_n=$(wc -l < "$TMP/apple_unbound.txt" | tr -d ' ')
echo "   ok（Apple 绑定 $bound 项；未绑定 $unbound_n 项，均已登记理由）"