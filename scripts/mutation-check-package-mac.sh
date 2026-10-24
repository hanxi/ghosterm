#!/usr/bin/env bash
# 变异验证：证明 macOS 打包的几条断言「有牙」。
#
# 覆盖的断言（都在 scripts/package-mac.sh / scripts/lib/embed-dylibs.py 里）：
#   1. 依赖必须递归内嵌：没内嵌 → check 红（连「改坏脚本里的内嵌调用」也红）；
#   2. 签名后再往 bundle 里加文件 → code seal 失效 → codesign 校验红；
#   3. 谎报最低系统版本（--min-os 低于依赖实际要求）→ 直接拒绝出包。
#
# 手法与仓库其它 mutation 脚本一致：基线绿 → 变异红 → 回滚复绿，
# 并且**每个变异都配一条「应当仍绿」的对照**，证明红有指向性而不是环境坏了。
#
# 与其它 mutation 脚本的差异：本脚本的变异主要作用在 tmp/ 里的**副本**上
# （不改仓库文件），唯一改仓库文件的那条带 sha256 复原校验。
#
# 用法：bash scripts/mutation-check-package-mac.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/tmp"

SCRIPTS=(scripts/package-mac.sh scripts/lib/embed-dylibs.py scripts/build-icons.sh scripts/lib/ghostty-themes.sh)
for f in "${SCRIPTS[@]}"; do [ -f "$f" ] || { echo "缺少 $f" >&2; exit 2; }; done

declare -A SHA_BEFORE
for f in "${SCRIPTS[@]}"; do SHA_BEFORE[$f]="$(shasum -a 256 "$f" | awk '{print $1}')"; done

WORK="$(mktemp -d "$ROOT/tmp/mutation-package-XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT INT TERM

PASS=0
# 判据反了时必须当场看到**为什么**反（CI 上实测踩过：A7 的对照红了，
# 但所有输出被 >/dev/null 吃掉，只剩一行「预期绿，实际红」＝没有证据）。
# 一次 expect 只跑一条命令，所以这份日志永远是「刚那一次」的输出。
LAST_LOG="$WORK/last.log"
expect_red()   { local d=$1; shift; if "$@" >"$LAST_LOG" 2>&1; then echo "MUTATION FAIL: ${d}：预期红，实际绿（尾 15 行）" >&2; tail -15 "$LAST_LOG" >&2; exit 1; fi; echo "   ok  ${d} → 红"; PASS=$((PASS+1)); }
expect_green() { local d=$1; shift; if ! "$@" >"$LAST_LOG" 2>&1; then echo "MUTATION FAIL: ${d}：预期绿，实际红（尾 15 行）" >&2; tail -15 "$LAST_LOG" >&2; exit 1; fi; echo "   ok  ${d} → 绿"; PASS=$((PASS+1)); }

echo "== 基线：出一次包（同时作为变异用的样本） =="
bash scripts/package-mac.sh --out "$WORK/dist" --app-only --no-zip --version 0.0.0-mut >"$WORK/base.log" 2>&1 \
  || { tail -20 "$WORK/base.log" >&2; echo "MUTATION FAIL: 基线出包就失败" >&2; exit 1; }
BASELINE="$WORK/dist/Ghosterm.app"
[ -d "$BASELINE" ] || { echo "MUTATION FAIL: 基线没有产出 .app" >&2; exit 1; }
expect_green "基线：自包含检查" python3 scripts/lib/embed-dylibs.py check "$BASELINE"

echo "== A1：把一条依赖改回绝对路径 → 自包含检查必须红 =="
cp -R "$BASELINE" "$WORK/A1.app"
LIB="$WORK/A1.app/Contents/Frameworks/libssh2.1.dylib"
install_name_tool -change "@executable_path/../Frameworks/libcrypto.3.dylib" \
  "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib" "$LIB"
expect_red "依赖没内嵌（A1）" python3 scripts/lib/embed-dylibs.py check "$WORK/A1.app"
expect_green "同一变异下基线仍绿（A1 指向性）" python3 scripts/lib/embed-dylibs.py check "$BASELINE"

echo "== A2：改坏打包脚本里的内嵌调用 → 端到端出包必须红 =="
cp scripts/package-mac.sh "$WORK/package-mac.sh.orig"
# 把 embed 那一步换成「什么都没做」，模拟「忘了内嵌依赖」
python3 - "$PWD/scripts/package-mac.sh" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
needle = 'python3 "$ROOT/scripts/lib/embed-dylibs.py" embed "$APP"'
assert needle in t, "打包脚本结构变了，变异脚本要跟着改"
t = t.replace(needle, 'python3 -c "pass"  # MUTATION: 跳过内嵌', 1)
open(p, "w", encoding="utf-8").write(t)
PY
expect_red "跳过内嵌依赖（A2）" bash scripts/package-mac.sh --out "$WORK/A2dist" --app-only --no-zip --version 0.0.0-mut
cp "$WORK/package-mac.sh.orig" scripts/package-mac.sh
expect_green "同一变异下图标闸门仍绿（A2 指向性）" bash scripts/build-icons.sh check

echo "== A3：签名后往 bundle 里塞文件 → code seal 必须红 =="
cp -R "$BASELINE" "$WORK/A3.app"
expect_green "变异前签名有效（A3 前置）" codesign --verify --strict "$WORK/A3.app"
echo "post-sign file" > "$WORK/A3.app/Contents/Resources/extra.txt"
expect_red "签名后被加文件（A3）" codesign --verify --strict "$WORK/A3.app"
expect_green "同一变异下自包含检查仍绿（A3 指向性）" python3 scripts/lib/embed-dylibs.py check "$WORK/A3.app"

echo "== A4：谎报最低系统版本（低于依赖实际要求）→ 出包必须拒绝 =="
expect_red "谎报 --min-os 13.0（A4）" bash scripts/package-mac.sh --out "$WORK/A4dist" --app-only --no-zip --version 0.0.0-mut --min-os 13.0
expect_green "同一次跑里基线 .app 不受影响（A4 指向性）" python3 scripts/lib/embed-dylibs.py check "$BASELINE"

echo "== A6：包内配置检查（随包主题被引擎接受）必须有牙 =="
# 这条断言的牙在于：它验的是「打包布局（资源目录）→ 推导 → 主题解析」
# 这条链，而链上任一环断了都该红。造一个**主题文件缺失**的包来验：
# 把刚出的基线包里的主题挪走，再用同一份配置跑一次检查，必须失败。
A6_APP="$WORK/A6.app"
ditto "$BASELINE" "$A6_APP"
A6_EXE="$A6_APP/Contents/MacOS/GhostermApp"
printf 'theme = "iTerm2 Default"\n' > "$WORK/a6.conf"
# 跑检查时**必须把主题的两个搜索目录都钉死**，否则这条断言会假绿（实测踩过）：
# 上游 theme.zig 先找「用户主题目录」（$XDG_CONFIG_HOME/ghostty/themes），再找
# 「资源目录」，而资源目录可被 GHOSTTY_RESOURCES_DIR 覆盖——开发机上装着
# Ghostty.app 并把这个变量指过去（本机实测确实如此），于是把**包内**主题挪走
# 后引擎照样在别处找到它，A6 期望红却绿。所以：资源目录显式指向被测包自己，
# HOME 换成空目录（挡掉 ~/.config/ghostty/themes 那条）。
A6_HOME="$WORK/a6-home"
mkdir -p "$A6_HOME"
# $0=被测二进制 $1=配置 $2=被测包 $3=空 HOME
A6_RUN='env GHOSTTY_RESOURCES_DIR="$2/Contents/Resources/ghostty" HOME="$3" "$0" --config-check "$1" 2>/dev/null | grep -q "CONFIG_OK diagnostics=0"'
if [ -f "$A6_APP/Contents/Resources/ghostty/themes/iTerm2 Default" ]; then
  mv "$A6_APP/Contents/Resources/ghostty/themes/iTerm2 Default" "$WORK/theme.bak"
  expect_red "主题文件缺失时包内配置检查红（A6）" \
    bash -c "$A6_RUN" "$A6_EXE" "$WORK/a6.conf" "$A6_APP" "$A6_HOME"
  mv "$WORK/theme.bak" "$A6_APP/Contents/Resources/ghostty/themes/iTerm2 Default"
  expect_green "主题放回后同一检查复绿（A6 指向性）" \
    bash -c "$A6_RUN" "$A6_EXE" "$WORK/a6.conf" "$A6_APP" "$A6_HOME"
else
  echo "   SKIP A6：基线包里没有随包主题（先跑 build-ghostty.sh 生成资源）"
fi
rm -rf "$A6_APP"

echo "== A5：公证的前置守卫（ad-hoc 签名 + --notarize 必须拒绝） =="
# 本机没有 Developer ID 证书（security find-identity 0 项），所以公证这条路
# 走不到真提交那一步。能验的、也该验的是**守卫**：拿 ad-hoc 签名的包去
# notarytool 提交是错的（Apple 侧也会拒），脚本必须自己先拦住，
# 而不是把包提上去、等 Apple 回一个看不懂的错。
# 顺带证明这条守卫不是「随便什么都拒」：带真证书身份时不走这个分支
# （--sign 传的是不存在的身份 → 失败点变成签名，而不是这条守卫）。
expect_red "ad-hoc + --notarize 必须被守卫拦下（A5）" \
  bash scripts/package-mac.sh --out "$WORK/A5dist" --app-only --no-zip \
    --version 0.0.0-mut --adhoc --notarize ghosterm-notary
expect_green "同一次跑里基线 .app 不受影响（A5 指向性）" python3 scripts/lib/embed-dylibs.py check "$BASELINE"

echo "== A7：配色集定位（A7 必须红——这条正是「出过没主题的包」的根因） =="
# 为什么打这一条：配色集定位原先写成 `find -type d`，而 zig-pkg 里的
# `iTerm2 Default` 是**文件**，于是永远找不到、静默 SKIP，出过一个主题
# 下拉全空的包（用户实测报的「浅色/深色配色（不设置）」）。这里把定位
# 函数改回按目录找 —— 必须返回「找不到」，而不是碰巧拿到别的东西。
# 定位函数是 lib/ghostty-themes.sh 里的 ghostty_themes_find；回滚由上面的
# sha256 那道统一验（它已并入 SCRIPTS）。
A7_LIB="$PWD/scripts/lib/ghostty-themes.sh"
cp "$A7_LIB" "$WORK/ghostty-themes.sh.orig"
# **先定源，再谈变异**：CI 的 runner 上 `vendor/ghostty/zig-pkg` 常常不存在
# （缓存命中时不跑 zig build，zig-pkg 是构建期产物、不入库），配色集来自
# 缓存的 `tmp/ghostty-resources` 暂存目录——package-mac.sh 也正是这两条路。
# 原先这里写死 zig-pkg，于是 CI 上「变异→红」是**因为目录不在**（假红＝
# 红得没有指向性），而「复原→应绿」也红，闸门整个判失败。现在跟打包脚本
# 取同一个源；两条路都没有就当场判失败（不 SKIP、不拿不存在的路径蒙）。
# 两条都**在子 shell 里现源现调**：本函数正是 A7 的变异目标，若在脚本自己
# 的 shell 里 `. ` 一次，内存里留下的永远是未变异的实现，A7 就失去牙
# （本地实测：直接调用时函数没定义，两条分支同时落空）。
if bash -c '. scripts/lib/ghostty-themes.sh; ghostty_themes_find "$1" >/dev/null 2>&1' _ "$ROOT/vendor/ghostty/zig-pkg"; then
  A7_ROOT="$ROOT/vendor/ghostty/zig-pkg"
elif bash -c '. scripts/lib/ghostty-themes.sh; ghostty_themes_find "$1" >/dev/null 2>&1' _ "$ROOT/tmp/ghostty-resources/ghostty"; then
  A7_ROOT="$ROOT/tmp/ghostty-resources/ghostty"
else
  echo "MUTATION FAIL: 两处都没有配色集（vendor/ghostty/zig-pkg、tmp/ghostty-resources 暂存）——A7 无法验到定位分支。跑一次 bash scripts/build-ghostty.sh。" >&2
  exit 1
fi
expect_green "变异前就找得到配色集（A7 前置，源 $A7_ROOT）" \
  bash -c '. scripts/lib/ghostty-themes.sh; ghostty_themes_find "$1"' _ "$A7_ROOT"
python3 - "$A7_LIB" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
needle = 'find "$pkgdir" -maxdepth 2 -type f -name "iTerm2 Default"'
assert needle in t, "定位实现变了，变异脚本要跟着改"
t = t.replace(needle,
              'find "$pkgdir" -maxdepth 2 -type d -name "iTerm2 Default"  # MUTATION', 1)
open(p, "w", encoding="utf-8").write(t)
PY
expect_red "配色集按目录找（A7，回到出过 bug 的实现）" \
  bash -c '. scripts/lib/ghostty-themes.sh; ghostty_themes_find "$1"' _ "$A7_ROOT"
# 端到端那半：定位失败时**必须拒绝出包**，不许像原先那样 note 一句 SKIP
# 把「没主题的包」放出去。为了确定性地验到这条分支（而不是被 tmp 暂存
# 目录的兜底救回来），把兜底目录临时挪开。
STASH="$WORK/ghostty-resources.stash"
STASHED=0
if [ -d tmp/ghostty-resources ]; then mv tmp/ghostty-resources "$STASH"; STASHED=1; fi
expect_red "定位失败时拒绝出包（A7 端到端）" \
  bash scripts/package-mac.sh --out "$WORK/A7dist" --app-only --no-zip --version 0.0.0-mut
if [ "$STASHED" -eq 1 ]; then mv "$STASH" tmp/ghostty-resources; fi
cp "$WORK/ghostty-themes.sh.orig" "$A7_LIB"
expect_green "改回按文件找后同一调用复绿（A7 指向性）" \
  bash -c '. scripts/lib/ghostty-themes.sh; ghostty_themes_find "$1"' _ "$A7_ROOT"

echo "== 复原：仓库文件必须与起跑前逐字节一致 =="
for f in "${SCRIPTS[@]}"; do
  NOW="$(shasum -a 256 "$f" | awk '{print $1}')"
  [ "$NOW" = "${SHA_BEFORE[$f]}" ] || { echo "MUTATION FAIL: $f 未复原（${SHA_BEFORE[$f]} → $NOW）" >&2; exit 1; }
done
echo "   ok  ${#SCRIPTS[@]} 个仓库文件 sha256 与起跑前一致"
expect_green "复原后基线自包含检查复绿" python3 scripts/lib/embed-dylibs.py check "$BASELINE"

echo "MUTATION PASS（$PASS 项）"
