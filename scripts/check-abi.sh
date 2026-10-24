#!/usr/bin/env bash
# C ABI 符号对拍：头文件声明 ↔ 静态库实际导出（双向）。
#
# 注意两处平台差异（本机 macOS 实测）：
#   - Mach-O 的 C 符号带前导下划线（_gt_version），须剥掉再比；
#   - 内部函数用 gt__ 前缀，声明提取须排除双下划线（否则把内部符号当公共符号）；
#   - 函数指针 typedef 形如 `gt_foo_t (*gt_cb)(...)` 会被误当函数名，须排除 `_t` 结尾的类型名。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIB="$ROOT/core/build/libghosterm_core.a"
TMP="$ROOT/tmp"
mkdir -p "$TMP"

[ -f "$LIB" ] || { echo "check-abi: 缺少静态库，先跑 scripts/gate.sh" >&2; exit 2; }

# 头文件里的公共函数声明：gt_ 后紧跟小写字母（排除 gt__ 内部符号）
grep -rhoE '\bgt_[a-z][a-z0-9_]*[[:space:]]*\(' "$ROOT/core/include" \
  | sed -E 's/[[:space:]]*\($//' \
  | grep -vE '_t$' \
  | sort -u > "$TMP/gt_declared.txt"

# 静态库已定义的全局符号；剥掉 Mach-O 前导下划线
nm -g --defined-only "$LIB" 2>/dev/null \
  | awk '$2=="T"{print $3}' \
  | sed -E 's/^_//' | sort -u > "$TMP/gt_exported_all.txt"
grep -E '^gt_[a-z]' "$TMP/gt_exported_all.txt" | sort -u > "$TMP/gt_exported.txt"

missing=$(comm -23 "$TMP/gt_declared.txt" "$TMP/gt_exported.txt")
if [ -n "$missing" ]; then
  echo "check-abi: 声明了但未导出（漏导出）：" >&2
  echo "$missing" >&2
  exit 1
fi

leaked=$(comm -13 "$TMP/gt_declared.txt" "$TMP/gt_exported.txt")
if [ -n "$leaked" ]; then
  echo "check-abi: 导出了但头文件未声明（泄漏/漂移）：" >&2
  echo "$leaked" >&2
  exit 1
fi

echo "   ok ($(wc -l < "$TMP/gt_declared.txt" | tr -d ' ') 个公共符号对拍一致)"
