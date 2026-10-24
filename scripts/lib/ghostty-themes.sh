#!/usr/bin/env bash
# Ghostty 配色集（iTerm2 themes）定位与暂存 —— 被 build-ghostty.sh 与
# package-mac.sh 共用。
#
# 为什么单独抽一份：这条逻辑原先在 build-ghostty.sh 里写成了 `find -type d`，
# 而 zig-pkg 里的 `iTerm2 Default` 是**文件**不是目录（配色集是 600 多个
# 平铺文件），于是**永远找不到**、静默 SKIP；又因为 package-mac.sh 在
# xcframework 已缓存时不跑 build-ghostty.sh，连那次 SKIP 都到不了，
# 结果出过一个**没有主题目录**的包——用户界面上「浅色/深色配色」两个
# 下拉全空（`(不设置)`），还找不到原因。
# 两处各写一份必然再次分叉，所以合成一份。
#
# 定位策略（不依赖 zone 解析，也不依赖 zig 是否跑过）：
#   配色集以 `zig-pkg/<hash>/<主题名>` 的形态平铺在 hash 目录下，
#   故找「**文件**名为 `iTerm2 Default`」的目录就是配色集目录。
#   找到后还要过一道**条数下限**（≥ 100），免得某个无关包里恰好有个
#   同名文件就被当成配色集——宁可显式失败，也不拷半份进去。
#
# 本库**不设 shell 选项**（不改调用方的 set -e 行为）。
# `$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。

# 配色集条数下限（上游 2026-08-31 版是 607 套；下限留足余量，
# 只用来区分「整套配色集」与「恰好同名的单个文件」）。
GHOSTTY_THEMES_MIN=100

# 找配色集源目录，成功则把**绝对路径**打到 stdout。
# 参数：<zig-pkg 目录> [期望条数下限]
ghostty_themes_find() {
  local pkgdir="${1:?ghostty_themes_find 需要 zig-pkg 目录}"
  local floor="${2:-$GHOSTTY_THEMES_MIN}"
  [ -d "$pkgdir" ] || return 1
  local cand
  while IFS= read -r cand; do
    [ -n "$cand" ] || continue
    local dir; dir="$(dirname "$cand")"
    local n; n="$(ls -A "$dir" | wc -l | tr -d ' ')"
    if [ "$n" -ge "$floor" ]; then
      printf '%s\n' "$dir"
      return 0
    fi
  done < <(find "$pkgdir" -maxdepth 2 -type f -name "iTerm2 Default" 2>/dev/null)
  return 1
}

# 把配色集拷进 <目标目录>（该目录会被**清空重建**）。
# 参数：<源目录> <目标目录>；成功打印条数到 stdout 的调用方。
ghostty_themes_stage() {
  local src="${1:?ghostty_themes_stage 需要源目录}"
  local dst="${2:?ghostty_themes_stage 需要目标目录}"
  [ -d "$src" ] || return 1
  rm -rf "$dst"
  mkdir -p "$dst"
  cp -R "$src/." "$dst/"
  ls -A "$dst" | wc -l | tr -d ' '
}