#!/usr/bin/env bash
# 变异验证：证明 macOS 打包的几条断言「有牙」。
#
# 覆盖的断言（都在 scripts/package-mac.sh / scripts/lib/embed-dylibs.py 里）：
#   1. 依赖必须递归内嵌：没内嵌 → check 红（连「改坏脚本里的内嵌调用」也红）；
#   2. 签名后再往 bundle 里加文件 → code seal 失效 → codesign 校验红；
#   3. 谎报最低系统版本（--min-os 低于依赖实际要求）→ 直接拒绝出包；
#   4. 签名档位必须按证书**真实的类**记账：免费个人账号的 Apple Development 不能
#      冒充 Developer ID（A8 打 package-mac.sh 的分类器，A9 打 sessiond 的 runtime.json）；
#   5. 对外声明的下载入口必须跟着本轮打包改名：Homebrew cask 的 URL 在另一个仓库手写，
#      改名/版本 bump 时 dev 轨自己全绿、用户侧 404（A10 打 package-mac.sh 的资产命名，
#      牙落在 scripts/check-dev-cask.sh 上）。
#
# 手法与仓库其它 mutation 脚本一致：基线绿 → 变异红 → 回滚复绿，
# 并且**每个变异都配一条「应当仍绿」的对照**，证明红有指向性而不是环境坏了。
#
# 与其它 mutation 脚本的差异：部分变异作用在 tmp/ 里的**副本**上（不改仓库文件），
# 改仓库文件的那几条（A2/A7/A8/A9/A10）统一由末尾的 sha256 对拍兜底，且清理钩子先把
# 备份拷回去再删工作目录。
#
# 用法：bash scripts/mutation-check-package-mac.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/tmp"

SCRIPTS=(scripts/package-mac.sh scripts/lib/embed-dylibs.py scripts/build-icons.sh scripts/lib/ghostty-themes.sh scripts/build-sessiond-mac.py scripts/check-dev-cask.sh)
for f in "${SCRIPTS[@]}"; do [ -f "$f" ] || { echo "缺少 $f" >&2; exit 2; }; done

# 起跑前扫变异标记：上一轮被 kill -9 打断可能把 `# MUTATION` 留在仓库文件里，
# 那种状态下每条断言都红得没有意义（AGENTS.md：命中即 exit 2，不许接着跑）。
for f in "${SCRIPTS[@]}"; do
  if grep -qF '# MUTATION' "$f"; then
    echo "MUTATION FAIL: $f 里残留变异标记（# MUTATION）——上一轮没复原，先 git checkout 该文件再跑。" >&2
    exit 2
  fi
done

# 超时只走 gt_timeout：macOS 没有裸 `timeout`（brew coreutils 装的是 gtimeout），
# 而「命令不存在」与「断言变红」都是非零退出码，混在一起就是假牙。解析不到则
# exit 2 拒绝起跑。
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-package-mac

declare -A SHA_BEFORE
for f in "${SCRIPTS[@]}"; do SHA_BEFORE[$f]="$(shasum -a 256 "$f" | awk '{print $1}')"; done

WORK="$(mktemp -d "$ROOT/tmp/mutation-package-XXXXXX")"
PKG_BAK="$WORK/package-mac.sh.orig"
SESSIOND_BAK="$WORK/build-sessiond-mac.py.orig"
cleanup() {
  # 先复原仓库文件、再删工作目录：备份就放在工作目录里，顺序反了会把证据一起删掉。
  [ -f "$PKG_BAK" ] && cp "$PKG_BAK" "$ROOT/scripts/package-mac.sh" 2>/dev/null || true
  [ -f "$SESSIOND_BAK" ] && cp "$SESSIOND_BAK" "$ROOT/scripts/build-sessiond-mac.py" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

PASS=0
# 判据反了时必须当场看到**为什么**反（CI 上实测踩过：A7 的对照红了，
# 但所有输出被 >/dev/null 吃掉，只剩一行「预期绿，实际红」＝没有证据）。
# 一次 expect 只跑一条命令，所以这份日志永远是「刚那一次」的输出。
LAST_LOG="$WORK/last.log"
expect_red()   { local d=$1; shift; if "$@" >"$LAST_LOG" 2>&1; then echo "MUTATION FAIL: ${d}：预期红，实际绿（尾 15 行）" >&2; tail -15 "$LAST_LOG" >&2; exit 1; fi; echo "   ok  ${d} → 红"; PASS=$((PASS+1)); }
expect_green() { local d=$1; shift; if ! "$@" >"$LAST_LOG" 2>&1; then echo "MUTATION FAIL: ${d}：预期绿，实际红（尾 15 行）" >&2; tail -15 "$LAST_LOG" >&2; exit 1; fi; echo "   ok  ${d} → 绿"; PASS=$((PASS+1)); }
# expect_red_saying：不只看退出码，还看**红在哪一句**。A8 里「分类器拒了」与
# 「钥匙串里没这身份」都是非零退出码，光看状态码分不出拦对还是拦错——只有文案能
# 证明是分类器在做判断（这也是「假绿/假红」唯一的现场证据，故失败时照例 dump）。
expect_red_saying() {
  local d=$1 needle=$2; shift 2
  if "$@" >"$LAST_LOG" 2>&1; then
    echo "MUTATION FAIL: ${d}：预期红，实际绿（尾 15 行）" >&2; tail -15 "$LAST_LOG" >&2; exit 1
  fi
  if ! grep -qF -- "$needle" "$LAST_LOG"; then
    echo "MUTATION FAIL: ${d}：是红了，但输出里没有「${needle}」＝红得没有指向性（尾 15 行）" >&2
    tail -15 "$LAST_LOG" >&2; exit 1
  fi
  echo "   ok  ${d} → 红（文案含「${needle}」）"; PASS=$((PASS+1))
}

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

echo "== A8：签名档位必须按证书**真实的类**记账（免费账号不能冒充 Developer ID） =="
# 为什么打这一条：`--sign` 原先把**任何**身份都记成 SIGN_MODE=id，于是随包
# BUILD-INFO 与 version.json 的 `signature` 会谎报「Developer ID」——而那个字段正是
# app 侧 `RemoteBuild.installBlocked` 判断「能不能自动装」的依据（铁律 #6：宁可红，
# 不拿别的证书冒充）。本机没有真证书（security find-identity 0 项），所以验的是
# 分类器本身：三类假身份都走不到签名那一步，但**红必须红在不同的地方**——
# 「随便什么都拒」和「按证书类分档」用退出码分不出来，只有文案分得出来。
DEV_ID="Apple Development: 测试 (TEAMID)"
DEVID="Developer ID Application: 测试 (TEAMID)"
OTHER_ID="Developer ID Installer: 测试 (TEAMID)"
pkg_sign() {
  # 两条分支而不是 ${2:+…}：身份串里有空格，嵌套引号在展开里最容易出错。
  if [ "$#" -gt 1 ]; then
    gt_timeout 180 bash scripts/package-mac.sh --out "$WORK/A8dist" --app-only --no-zip \
      --version 0.0.0-mut --sign "$1" --notarize "$2"
  else
    gt_timeout 180 bash scripts/package-mac.sh --out "$WORK/A8dist" --app-only --no-zip \
      --version 0.0.0-mut --sign "$1"
  fi
}
expect_red_saying "Apple Development 记成 dev 档（A8 基线）" "签名档：dev" pkg_sign "$DEV_ID"
expect_red_saying "dev 档 + --notarize 被守卫拦下（A8 基线）" "不给公证" pkg_sign "$DEV_ID" ghosterm-notary
expect_red_saying "Developer ID 假身份红在钥匙串那一层（A8 基线）" "钥匙串里没有" pkg_sign "$DEVID"
expect_red_saying "不认识的身份直接拒（A8 基线）" "不在可记账的两档内" pkg_sign "$OTHER_ID"
cp scripts/package-mac.sh "$PKG_BAK"
python3 - scripts/package-mac.sh <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
needle = '''    "Developer ID Application: "*) SIGN_MODE="id" ;;
    "Apple Development: "*)        SIGN_MODE="dev" ;;'''
assert needle in t, "签名档分类实现变了，A8 的变异要跟着改"
t = t.replace(needle,
              '    *) SIGN_MODE="id" ;;  # MUTATION: 回到「凡是真证书都算 Developer ID」', 1)
open(p, "w", encoding="utf-8").write(t)
PY
expect_red_saying "变异后：开发证书被记成 id（A8 有牙）" "签名档：id" pkg_sign "$DEV_ID"
expect_red_saying "变异后：未知身份也不再被分类器拒（A8 第二处有牙）" "钥匙串里没有" pkg_sign "$OTHER_ID"
expect_green "同一变异下基线 .app 不受影响（A8 指向性）" python3 scripts/lib/embed-dylibs.py check "$BASELINE"
cp "$PKG_BAK" scripts/package-mac.sh
expect_red_saying "复原后同一调用重新记成 dev（A8 复绿口径）" "签名档：dev" pkg_sign "$DEV_ID"

echo "== A9：会话 helper 的 runtime.json 同一条记账口径 =="
# build-sessiond-mac.py 原先写 `'ad-hoc' if sign=='-' else 'developer-id'`：拿免费账号
# 的证书签 sessiond，runtime.json 里就成了 developer-id——同一个谎的第二处落点。
# 这一档不进 release id（id 只由 contract 与文件哈希算），所以改它不动 mac↔Linux
# 的契约对拍；也因此这里只能**现取现调**该函数（整脚本要真构建，代价不成比例）。
A9_PROBE='import runpy
f = runpy.run_path("scripts/build-sessiond-mac.py", run_name="gt_probe")["sign_label"]
for got, exp in [("-", "ad-hoc"),
                 ("Developer ID Application: X (T)", "developer-id"),
                 ("Apple Development: X (T)", "apple-development")]:
    if f(got) != exp:
        raise SystemExit("记账错：%s → %s，应为 %s" % (got, f(got), exp))
try:
    f("Developer ID Installer: X (T)")
except RuntimeError:
    pass
else:
    raise SystemExit("未识别的身份没报错")'
A9_CTRL='import runpy,sys
f = runpy.run_path("scripts/build-sessiond-mac.py", run_name="gt_probe")["sign_label"]
sys.exit(0 if f("-") == "ad-hoc" and f("Developer ID Application: X (T)") == "developer-id" else 1)'
expect_green "三档记账 + 未知身份报错（A9 基线）" python3 -c "$A9_PROBE"
cp scripts/build-sessiond-mac.py "$SESSIOND_BAK"
python3 - scripts/build-sessiond-mac.py <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
needle = "        return 'apple-development'"
assert needle in t, "sign_label 实现变了，A9 的变异要跟着改"
t = t.replace(needle, "        return 'developer-id'  # MUTATION: 真证书一律算 developer-id", 1)
open(p, "w", encoding="utf-8").write(t)
PY
expect_red "变异后：开发证书被记成 developer-id（A9 有牙）" python3 -c "$A9_PROBE"
expect_green "同一变异下 ad-hoc 与 Developer ID 两档不受影响（A9 指向性）" python3 -c "$A9_CTRL"
cp "$SESSIOND_BAK" scripts/build-sessiond-mac.py
expect_green "复原后三档记账复绿（A9 复绿）" python3 -c "$A9_PROBE"

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
expect_green "变异前就找得到配色集（A7 前置，源 ${A7_ROOT}）" \
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

echo "== A10：Homebrew cask 声明的资产名必须跟着本轮打包改名 =="
# 为什么打这一条：tap 是**另一个仓库**（hanxi/homebrew-ghosterm），cask 里的 URL 是手写的，
# 而 dev 资产名由 package-mac.sh 的 BASE 现算（带版本号）。版本一 bump、命名一改，那条 URL
# 就指到不存在的东西——**dev 轨自己却是全绿的**：SHA256SUMS 与 version.json 都从本轮产物
# 现算（它们跟着改名），用户侧的表现才是 `brew install --cask` 当场 404。
# 分工要说清：这里验的是「check-dev-cask.sh 的比较逻辑有没有牙」（拿 fixture 当输入）；
# 与**真实** cask 的一致性由 dev-build.yml 那一步直接取 raw 文件来对（它跑的是真 URL、真资产）。
A10_CASK="$WORK/ghosterm-dev.rb"
cat > "$A10_CASK" <<'RB'
cask "ghosterm-dev" do
  version :latest
  sha256 :no_check

  url "https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-0.0.0-mut-arm64.zip"
  name "Ghosterm (dev channel)"
  desc "Libghostty 驱动的多端终端（dev 滚动渠道，未公证）"
  homepage "https://github.com/hanxi/ghosterm"

  depends_on arch: :arm64
  depends_on macos: :tahoe

  app "Ghosterm.app"

  uninstall quit: "dev.ghosterm.mac"
end
RB
# fixture 只把资产名换成 mutation 用的版本串 0.0.0-mut，其余口径与真 cask 逐条一致。
# **先断言源在、基线绿**，再谈变异（否则红得没有指向性）。
check_cask() { bash scripts/check-dev-cask.sh --dist "$1" --cask-file "$2"; }
expect_green "变异前：出一次带 zip 的包作为对拍输入（A10 前置）" \
  bash scripts/package-mac.sh --out "$WORK/A10base" --app-only --version 0.0.0-mut
expect_green "变异前：对拍绿（A10 基线）" check_cask "$WORK/A10base" "$A10_CASK"
cp scripts/package-mac.sh "$PKG_BAK"
python3 - scripts/package-mac.sh <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
needle = 'BASE="Ghosterm-${VERSION}-${ARCH}"'
assert needle in t, "资产命名实现变了，A10 的变异要跟着改"
t = t.replace(needle, 'BASE="ghosterm-${VERSION}-${ARCH}"  # MUTATION: 改名（模拟版本/命名漂移）', 1)
open(p, "w", encoding="utf-8").write(t)
PY
expect_green "改名后打包本身仍然成功（A10 前置：红不该来自打包）" \
  bash scripts/package-mac.sh --out "$WORK/A10mut" --app-only --version 0.0.0-mut
expect_red_saying "变异后：cask 的 URL 追不上新资产名（A10 有牙）" "≠ 本轮资产" \
  check_cask "$WORK/A10mut" "$A10_CASK"
# 指向性对照：同一轮改名产物里，旧的自洽断言**照旧绿**（SHA256SUMS 是现算的，跟着改名）——
# 证明红来自新加的那条下界，而不是环境坏了；这正是 dev 轨原先看不见这次漂移的原因。
expect_green "同一变异下 SHA256SUMS 仍自洽（A10 指向性）" \
  bash -c 'cd "$1" && shasum -a 256 -c SHA256SUMS' _ "$WORK/A10mut"
cp "$PKG_BAK" scripts/package-mac.sh
expect_green "复原后同一调用复绿（A10 复绿口径）" check_cask "$WORK/A10base" "$A10_CASK"

# 第二处有牙：口径本身不许漂。把 fixture 的 `sha256 :no_check` 换成一个**真 sha**——
# brew 会开始校验一个滚动渠道根本钉不住的字节串，等于谎报「这条路在校验完整性」。
sed 's|sha256 :no_check|sha256 arm64: "8c2885e9fd4e24f60fa7166f03a912632d8abd232474354501c8299a96c803f3"|' \
  "$A10_CASK" > "$WORK/ghosterm-dev.sha-pinned.rb"
expect_red_saying "cask 被改成钉死 sha256（A10 第二处有牙）" "sha256 :no_check" \
  check_cask "$WORK/A10base" "$WORK/ghosterm-dev.sha-pinned.rb"
# 第三处有牙：能力缺失要显式报错。包只有 arm64 切片，删掉 arch 声明就是让 Intel
# 机器装到一个**装得上、跑不起来**的包——Gatekeeper 不会替这一步兜底（它只看
# quarantine 标记与代码判定，不看架构）。
grep -v 'depends_on arch: :arm64' "$A10_CASK" > "$WORK/ghosterm-dev.no-arch.rb"
expect_red_saying "cask 丢了 arch 下限（A10 第三处有牙）" "depends_on arch: :arm64" \
  check_cask "$WORK/A10base" "$WORK/ghosterm-dev.no-arch.rb"
expect_green "同一变异下未篡改的 fixture 仍绿（A10 三处对照）" check_cask "$WORK/A10base" "$A10_CASK"

echo "== 复原：仓库文件必须与起跑前逐字节一致 =="
for f in "${SCRIPTS[@]}"; do
  NOW="$(shasum -a 256 "$f" | awk '{print $1}')"
  [ "$NOW" = "${SHA_BEFORE[$f]}" ] || { echo "MUTATION FAIL: $f 未复原（${SHA_BEFORE[$f]} → ${NOW}）" >&2; exit 1; }
done
echo "   ok  ${#SCRIPTS[@]} 个仓库文件 sha256 与起跑前一致"
expect_green "复原后基线自包含检查复绿" python3 scripts/lib/embed-dylibs.py check "$BASELINE"

echo "MUTATION PASS（$PASS 项）"
