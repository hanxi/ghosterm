#!/usr/bin/env bash
# 对拍：Homebrew cask（dev 轨）声明的下载入口 ↔ 本轮 macOS 打包的真实资产。
#
# 为什么需要这一道：tap 是**另一个仓库**，cask 里的 URL 是手写的。dev 轨的资产名带版本号
# （Ghosterm-<version>-<arch>.zip），源码 bump 版本号就会让那条 URL 指到不存在的东西——
# 用户侧的表现是 `brew install --cask` 当场 404，而 dev 轨自己**全绿**（SHA256SUMS 与
# version.json 都是从本轮产物现算的，它们跟着改名，只有 cask 跟不上）。这条断言就是把
# 「跟不上」变成红。
#
# cask 里刻意写 `version :latest` + `sha256 :no_check`：滚动 tag `dev` 每轮强删重建、
# 每轮 ad-hoc 签名的 cdhash 都不同，钉不住校验和。所以这里反过来守——**不许有人把它改成
# 一个钉死的 sha256**（那会造出「装到一半校验失败」的假象，或更糟：钉住一个旧 sha 让新包
# 永远装不上）。brew 这条路不校验字节，口径与自查口子见 docs/how-to/dev-build.md。
#
# 用法：
#   bash scripts/check-dev-cask.sh --dist dist --cask-url <raw URL> --live   # CI
#   bash scripts/check-dev-cask.sh --dist dist --cask-file Casks/x.rb        # 变异验证
set -euo pipefail

DIST=""
CASK_URL=""
CASK_FILE=""
LIVE=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dist)       DIST="${2:?--dist 需要目录}"; shift 2 ;;
    --cask-url)   CASK_URL="${2:?--cask-url 需要 URL}"; shift 2 ;;
    --cask-file)  CASK_FILE="${2:?--cask-file 需要路径}"; shift 2 ;;
    --live)       LIVE=1; shift ;;
    -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "未知参数：$1（见 --help）" >&2; exit 2 ;;
  esac
done

fail() { echo "CASK FAIL: $*" >&2; exit 1; }
note() { echo "   cask  $*"; }

[ -n "$DIST" ] || fail "必须给 --dist"
[ -n "$CASK_URL" ] || [ -n "$CASK_FILE" ] || fail "必须给 --cask-url 或 --cask-file 之一"
[ -z "$CASK_URL" ] || [ -z "$CASK_FILE" ] || fail "--cask-url 与 --cask-file 只能给一个"

# 取输入这一步要和被测代码取同一个源：本轮资产名只认 dist 里实际产出的 zip，
# 名字从 SHA256SUMS 的兄弟文件里数出来，不拿常量去比。
ZIP_COUNT=0
ZIP=""
for f in "$DIST"/*.zip; do
  [ -e "$f" ] || continue
  ZIP_COUNT=$((ZIP_COUNT + 1))
  ZIP="$f"
done
[ "$ZIP_COUNT" = "1" ] || fail "本轮产物里有 ${ZIP_COUNT} 个 zip（预期恰好 1 个），无法对拍：$DIST"
ZIP_NAME="$(basename "$ZIP")"

CASK_TEXT=""
if [ -n "$CASK_FILE" ]; then
  [ -f "$CASK_FILE" ] || fail "cask 文件不在：$CASK_FILE"
  CASK_TEXT="$(cat "$CASK_FILE")"
  note "cask 来源：$CASK_FILE"
else
  # 取不到就直接红：拿「网络不可达」冒充「断言通过」就是假绿。
  CASK_TEXT="$(curl -fsS --max-time 30 "$CASK_URL")" \
    || fail "取不到 cask 文件（重试无果）：$CASK_URL"
  note "cask 来源：$CASK_URL"
fi

# 解析 url 那一行（不是 homepage）。多行/缺失都算读不懂，不许默默跳过。
CASK_URL_LINE="$(printf '%s\n' "$CASK_TEXT" | grep -E '^[[:space:]]*url[[:space:]]+"' || true)"
[ -n "$CASK_URL_LINE" ] || fail "cask 里找不到 url 那一行"
CASK_URL_VALUE="$(printf '%s\n' "$CASK_URL_LINE" | sed -E 's/^[[:space:]]*url[[:space:]]+"([^"]+)".*/\1/')"
CASK_ASSET="$(basename "${CASK_URL_VALUE%%\?*}")"

# 1) 资产名必须就是本轮打出来的那一个（版本 bump 忘改 tap → 这里红）。
[ "$CASK_ASSET" = "$ZIP_NAME" ] \
  || fail "cask 的 url 文件名是「${CASK_ASSET}」≠ 本轮资产「${ZIP_NAME}」：tap 里的 URL 没跟着版本走"
# 2) 渠道判据：必须落在滚动 dev 直链上（指到别的 tag 就等于把 dev 用户领去别的历史版本）。
case "$CASK_URL_VALUE" in
  */releases/download/dev/*) ;;
  *) fail "cask 的 url 必须走滚动 dev 直链（…/releases/download/dev/…），实际是：$CASK_URL_VALUE" ;;
esac
# 3) 校验和口径不许漂（改了就等于谎报「brew 在校验字节」）。
printf '%s\n' "$CASK_TEXT" | grep -qE '^[[:space:]]*sha256[[:space:]]+:no_check' \
  || fail 'cask 必须保持 sha256 :no_check——滚动 dev 包钉不住校验和，写死一个 sha 就是谎报'
# `version :latest` 是 Homebrew 的规范写法（`brew audit --strict` 拒字符串形式）。
printf '%s\n' "$CASK_TEXT" | grep -qE "^[[:space:]]*version[[:space:]]+:latest" \
  || fail 'cask 必须保持 version :latest——钉一个版本号会让 brew 永远认为已是最新'
# 4) 能力缺失要显式报错：包只有 arm64 切片（GhosttyKit.xcframework 没有 x86_64），
#    且内嵌的 Homebrew libcrypto 把最低系统抬到 26.0（随包 BUILD-INFO.txt 的「最低系统」行）。
printf '%s\n' "$CASK_TEXT" | grep -qE '^[[:space:]]*depends_on[[:space:]]+arch:[[:space:]]+:arm64' \
  || fail 'cask 必须声明 depends_on arch: :arm64——Intel 机器不该拿到一个跑不起来的包'
printf '%s\n' "$CASK_TEXT" | grep -qE '^[[:space:]]*depends_on[[:space:]]+macos:[[:space:]]+:tahoe' \
  || fail 'cask 必须声明 macOS 下限（depends_on macos: :tahoe）——包的 minos 由内嵌库抬到 26.0，不声明就是让老系统装了打不开'
note "url 文件名与本轮资产一致：$ZIP_NAME"
note "渠道 / sha256 :no_check / version :latest / arch 与 macOS 下限都在"

if [ "$LIVE" = "1" ]; then
  # 客户端用的是固定直链，所以只有真取一次才算证明这条路是活的。刚替换完的 release
  # 有秒级传播延迟，重试有界；耗尽必须红。
  try=0
  until curl -fsSIL --max-time 30 "$CASK_URL_VALUE" >/dev/null 2>&1; do
    try=$((try + 1))
    [ "$try" -lt 3 ] || fail "cask 指向的直链取不到（重试 ${try} 次后仍不可达）：$CASK_URL_VALUE"
    note "直链还没就绪（第 ${try} 次），$((try * 5)) 秒后重试"
    sleep "$((try * 5))"
  done
  note "直链可达：$CASK_URL_VALUE"
fi

echo "CASK PASS"
