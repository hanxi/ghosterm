#!/usr/bin/env bash
# 按 superproject 的 gitlink 取 vendor 子模块：匿名 HTTPS + 一次 --depth 1 fetch。
#
# 为什么不用 actions/checkout 的 submodules 输入：浅克隆下它会把子模块落到**远端分支头**
# （CI 实测 engine pin mismatch: 55ebd1bb… ≠ 81598385…），而引擎构建与 patches/ 只认
# .gitmodules 钉死的那个 commit。落错 commit 绝不能静默——取不到就当场红。
#
# 用法：bash scripts/fetch-vendor-pin.sh [vendor/ghostty]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PATH_NAME="${1:-vendor/ghostty}"

pinned="$(git ls-tree HEAD "$PATH_NAME" | awk '{print $3}')"
[ -n "$pinned" ] || { echo "gitlink 里没有 $PATH_NAME 的 commit" >&2; exit 1; }

# 本机常用路径：已经在 pin 上就别再动它（fetch 一次也费时间）。
# .git 必须是 -e 不是 -d：正常初始化的子模块里它是指向 .git/modules/… 的**文件**。
if [ -e "$PATH_NAME/.git" ] && [ "$(git -C "$PATH_NAME" rev-parse HEAD 2>/dev/null || true)" = "$pinned" ]; then
  echo "$PATH_NAME 已在 pin $pinned（未改动）"
  exit 0
fi

git submodule init "$PATH_NAME" >/dev/null
url="$(git config --get "submodule.${PATH_NAME}.url" || true)"
[ -n "$url" ] || { echo ".gitmodules 里没有 $PATH_NAME 的 URL" >&2; exit 1; }

mkdir -p "$PATH_NAME"
[ -e "$PATH_NAME/.git" ] || git -C "$PATH_NAME" init -q
git -C "$PATH_NAME" remote add origin "$url" 2>/dev/null \
  || git -C "$PATH_NAME" remote set-url origin "$url"
git -C "$PATH_NAME" -c protocol.version=2 fetch --quiet --depth 1 origin "$pinned"
git -C "$PATH_NAME" checkout --quiet --detach "$pinned"
# vendor 不许就地改（铁律 #9）：脏了就是前面某步改坏了，别让补丁链对着半个工作树跑。
git -C "$PATH_NAME" diff --quiet HEAD -- \
  || { echo "$PATH_NAME 工作树不干净" >&2; exit 1; }
echo "$PATH_NAME pin = $(git -C "$PATH_NAME" rev-parse HEAD)（$(git -C "$PATH_NAME" ls-files | wc -l | tr -d ' ') 个文件）"
