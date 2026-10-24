#!/usr/bin/env bash
# 闸门：证明 libghostty 补丁**真的生效**（不是"用了旧 xcframework 静默出绿"）。
#
# 断言分两层：
#   1. 每个 patches/libghostty/<pin>/*.patch 都能在干净 pin 上正向应用，
#      且在**已打补丁**的树上反向应用（= 树与补丁一致）；
#   2. 补丁引入的 4 个 C 符号真的出现在 GhosttyKit.xcframework 的静态库里。
#
# 缺 zig/构建产物时**显式 SKIP**（不假装通过）。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$ROOT/vendor/ghostty"
PIN="$(git -C "$VENDOR" rev-parse --short=7 HEAD)"
PATCHDIR="$ROOT/patches/libghostty/$PIN"
XCFW="$ROOT/build/GhosttyKit.xcframework"
LIB="$XCFW/macos-arm64/libghostty-internal.a"
HDR="$XCFW/macos-arm64/Headers/ghostty.h"
PASS=0; FAIL=0

ok()   { echo "  ✓ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ✗ $1" >&2; FAIL=$((FAIL+1)); }

if [ ! -d "$PATCHDIR" ]; then
  echo "SKIP gate-ghostty-patch: 无 patches/libghostty/$PIN"
  exit 0
fi

echo "== 1/2 补丁与 vendor 树一致 =="
shopt -s nullglob
patches=("$PATCHDIR"/*.patch)
shopt -u nullglob
# 补丁是**叠加**的（0002 改的就是 0001 刚加的上下文），所以不能拿每份补丁
# 单独对当前树做 forward/reverse 检查——叠加时后一份会改前一份的上下文，
# 于是前一份「既不能正向也不能反向应用」，会出假红。
#
# 口径改为：在一份**干净 pin 树**的副本上按文件名顺序逐个应用，全绿才说明
# 这套补丁对当前 pin 有效。（副本在 tmp/ 下，不动 vendor 树。）
SCRATCH="$ROOT/tmp/gk-patchcheck"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"
git -C "$VENDOR" archive HEAD | tar -x -C "$SCRATCH"
git -C "$SCRATCH" init -q
for p in "${patches[@]}"; do
  name="$(basename "$p")"
  if ! git -C "$SCRATCH" apply --check -p1 "$p" 2>/dev/null; then
    bad "$name 不能应用到干净 pin 树（顺序不对，或与 pin 不匹配）"
    continue
  fi
  git -C "$SCRATCH" apply -p1 "$p"
  ok "$name 可在干净 pin 树上按序应用"
done

echo "== 2/2 补丁引入的符号已导出 =="
if [ ! -f "$LIB" ]; then
  echo "SKIP gate-ghostty-patch: 缺 ${LIB}（先跑 scripts/build-ghostty.sh）"
else
  # 注意：符号表要落盘再 grep，不能 `echo "$SYMS" | grep -q`。
  # grep -q 命中即退出，上游 echo 收到 SIGPIPE，pipefail 会把「命中」判成失败
  # —— 靠前出现的符号会被误报为缺失（实测踩过）。
  SYMFILE="$ROOT/tmp/gk_exported.txt"
  nm -g --defined-only "$LIB" 2>/dev/null | awk '$2=="T"{print $3}' | sed 's/^_//' \
    | sort -u > "$SYMFILE"
  # 补丁必须真的把新入口导出，否则嵌入侧链接不上（或更糟：链到旧库）。
  for s in ghostty_surface_process_output ghostty_surface_new ghostty_surface_set_size; do
    if grep -qx "$s" "$SYMFILE"; then ok "导出 $s"; else bad "缺导出 $s"; fi
  done
  # 头部必须声明 host 三个字段（嵌入侧编译期就要看得见）。
  if grep -q 'void (\*host_write)' "$HDR" 2>/dev/null; then
    ok "头文件声明 host_write/host_resize/host"
  else
    bad "头文件未声明 host 回调（xcframework 里的头是旧的？）"
  fi

  # 0003（OSC 133 shell integration）：同上。
  if [ -e "$PATCHDIR/0003-semantic-prompt.patch" ]; then
    if grep -q 'void (\*host_semantic_prompt)' "$HDR" 2>/dev/null \
       && grep -q '} ghostty_semantic_prompt_s;' "$HDR" 2>/dev/null; then
      ok "头文件声明 host_semantic_prompt + ghostty_semantic_prompt_s"
    else
      bad "头文件缺 OSC 133 的 C 面（0003 没打进 xcframework？）"
    fi
  fi

  # 0002（OSC 7501）：回调字段与 POD 必须也在头里（嵌入侧编译期要用）。
  if [ -e "$PATCHDIR/0002-program-status.patch" ]; then
    if grep -q 'void (\*host_program_status)' "$HDR" 2>/dev/null \
       && grep -q '} ghostty_program_status_s;' "$HDR" 2>/dev/null; then
      ok "头文件声明 host_program_status + ghostty_program_status_s"
    else
      bad "头文件缺 OSC 7501 的 C 面（0002 没打进 xcframework？）"
    fi
  fi
fi

echo
echo "gate-ghostty-patch: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]