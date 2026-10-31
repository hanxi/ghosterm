# 拿一份 dev 版 macOS 包

`main` 上命中 dev 工作流触发条件、且构建发布成功的提交会生成 macOS 包，放在滚动 tag `dev` 的 prerelease 里。纯文档或仅 `android/` 等被过滤路径的提交不会触发；连续 push 还可能取消上一轮构建，因此 `dev` 不保证与 `main` 最新提交一致。
本页说明**怎么装、装了之后怎么确认它到底是怎么造出来的**，以及**怎么在本地自己出一份**。

## 怎么装：一键脚本、手工解包、Homebrew 或浏览器

dev 轨**没有 Developer ID 证书**，只做 ad-hoc 签名、不公证。首次打开会不会被拦，
由两件事相乘决定：**取包的工具**贴不贴 `com.apple.quarantine`，以及 Gatekeeper 对
代码的判定（ad-hoc 一律 `rejected`）。本机（macOS 26）实测：

| 装法 | 谁贴 quarantine | 首次打开 |
|---|---|---|
| 浏览器 / AirDrop 下 dmg | 下载工具贴（`0081;…;Safari;…`），且会**从 dmg 传染**给拖出来的 `.app`（`0281;…`） | 拦，需放行一次 |
| `brew install --cask` | **Homebrew 自己贴**（`0381;…;;<uuid>`，agent 写 `Homebrew Cask`；Homebrew 7 没有关掉它的开关） | 拦，需放行一次 |
| 一键安装脚本 | 安装时显式递归清除 | 无需手动执行 xattr |
| `curl` + `ditto` | **不贴**（zip 与解出的 `.app` 都只有 `com.apple.provenance`） | 不拦 |

> **拦与不拦的分界是「标记在不在」，不是标记里的 flag 值。** 拿同一个 ad-hoc 签名
> 的探针包分别贴浏览器风格 `0081` 与 Homebrew 风格 `0381`：LaunchServices 两侧都记
> `launched with launchInQuarantine == true, so not starting the application`（进程
> 被 spawn 出来但挂起不执行），syspolicyd 两侧都报 `GatekeeperPolicyScanError -67018
> Code did not match any currently allowed policy` 并弹框；不贴标记的那一份照常执行。
> 所以**换渠道并不能换来「双击就开」**，能换到的只有「不带标记」这一条。

### 一键安装或升级（自动清除 quarantine）

先退出 Ghosterm，然后执行：

```bash
curl -fsSL https://raw.githubusercontent.com/hanxi/ghosterm/main/scripts/install-macos.sh | /bin/bash
```

脚本读取 dev 发布清单，检查 Apple Silicon 和本次构建的最低 macOS 版本，校验 ZIP 的
SHA256、应用签名完整性及包内 commit，随后安装到 `/Applications/Ghosterm.app`，递归
清除 `com.apple.quarantine`。已有应用会先备份，替换失败自动恢复；目录需要管理员权限时
由 `sudo` 请求密码。后续升级重复执行同一命令。

只装到当前用户目录：

```bash
curl -fsSL https://raw.githubusercontent.com/hanxi/ghosterm/main/scripts/install-macos.sh | /bin/bash -s -- --app-dir "$HOME/Applications"
```

这是用户主动运行的安装入口；应用内更新检测仍只给下载链接。检出内运行时临时文件在
工程 `tmp/`，管道执行时在 `~/Library/Caches/Ghosterm/Install`，安装结束自动清理。

### 免弹窗装（实测不贴标记）

```bash
(
  set -eu
  curl -fsSL -o Ghosterm-0.1.0-dev-arm64.zip https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-0.1.0-dev-arm64.zip
  curl -fsSL -o SHA256SUMS https://github.com/hanxi/ghosterm/releases/download/dev/SHA256SUMS
  awk '$2 == "Ghosterm-0.1.0-dev-arm64.zip" && NF == 2 { print; n++ } END { if (n != 1) exit 1 }' SHA256SUMS > Ghosterm-zip.sha256
  shasum -a 256 -c Ghosterm-zip.sha256
  ditto -x -k Ghosterm-0.1.0-dev-arm64.zip /Applications
)
```

下载文件名须与清单一致。这里只提取 ZIP 的唯一一条记录；缺失、重复、下载失败或摘要不符时停止，不会继续解包。示例资产名对应当前脚本默认版本，版本变化时以 Release 实际名称为准；一键安装脚本会从 `version.json` 读取名称。手动解包不提供一键脚本的运行中检查和替换回滚。

`curl` 不属于会贴标记的「下载代理」，所以解出来的 `.app` 没有 `com.apple.quarantine`。
实测到的是**「标记不在」这一层**（外加一个不带标记的 ad-hoc 探针包被 LaunchServices
照常放行执行）；「所以双击直接开」是从「Gatekeeper 只在有标记时才评估」推出来的，
本仓库没在真机上量过双击这件事。代价：**这条路绕过的只是 Gatekeeper 的首次弹窗，
不是校验**——字节对不对全看上面对 `SHA256SUMS` 的那一步，跳过它就等于什么都没验。

### 用 Homebrew 装（一条命令装 + 一条命令升级）

```bash
brew tap hanxi/ghosterm
brew trust hanxi/homebrew-ghosterm        # Homebrew 7 起，第三方 tap 要先信任
brew install --cask ghosterm-dev
brew upgrade --cask --greedy ghosterm-dev # 升级：:latest cask 默认不进 brew outdated
```

tap 在 [`hanxi/homebrew-ghosterm`](https://github.com/hanxi/homebrew-ghosterm)，
cask 指向 `dev` 滚动直链、写的是 `sha256 :no_check`——**因为 dev 资产每轮强删重建，
字节钉不住，所以这里故意不校验**，不是漏了。要验字节就照上面 `shasum -a 256 -c
SHA256SUMS` 那一步手工对（包内 `BUILD-INFO.txt` 与 release 上那份同源）。
cask 里的 `depends_on arch: :arm64` / `depends_on macos: :tahoe` 是显式安装下限，
不满足会当场报错。该下限保留此前本机打包时内嵌 Homebrew `libcrypto` 要求 macOS 26.0
的限制；`010c251` 的 CI dev 包机读清单实际记录 `min_macos=15.0`，两者须分开看。
打包脚本按每轮实际依赖核算二进制下限，cask 尚未降低系统要求；15.0 的包元数据
不表示 macOS 15 全量 UI 与发行矩阵已验收。**首次打开照样会被 Gatekeeper 拦**（见上表），brew 换来的是安装与
升级省事，不是免弹窗。

### 被拦之后怎么放行

- `xattr -dr com.apple.quarantine /Applications/Ghosterm.app`——**实测**：去掉标记之后
  LaunchServices 不再走 quarantine 那条支路（日志里 `(quarantined)` /
  `so not starting the application` 两套行同时消失）。
- 系统设置 → 隐私与安全性 → 底部「仍要打开」——这条是**用户实测报上来的现象**
  （被拦时系统把人指到这里），本仓库没有第二次独立量过它。

> 旧文档这里写过「右键图标 → 打开 → 确认」。**那条没在本仓库的实测里验证过**
> （macOS 26 + ad-hoc 未公证），别再依赖它；上面两条是各自的证据档。

## 从 GitHub Releases 装（dmg）

1. 下载 [`dev` release](https://github.com/hanxi/ghosterm/releases/tag/dev) 里的
   `Ghosterm-0.1.0-dev-arm64.dmg`（连同 `SHA256SUMS`）。
2. 校验和比对——`SHA256SUMS` 是打包脚本对**最终字节**算的：

   ```bash
   (
     set -eu
     awk '$2 == "Ghosterm-0.1.0-dev-arm64.dmg" && NF == 2 { print; n++ } END { if (n != 1) exit 1 }' SHA256SUMS > Ghosterm-dmg.sha256
     shasum -a 256 -c Ghosterm-dmg.sha256
   )
   ```

   必须看到该 DMG 的 `OK` 后再继续；无需下载清单里的其他资产。

3. 挂载拖进「应用程序」。浏览器下载的 dmg 会把 quarantine **传染**给拖出来的 `.app`，
   首次打开按上面 [被拦之后怎么放行](#被拦之后怎么放行) 处理。

## 确认这个包是怎么来的：读 `BUILD-INFO.txt`

随包与 release 正文都带同一份 `BUILD-INFO.txt`，它是**实际值**而不是配置值：

| 字段 | 含义 |
|---|---|
| `版本: 0.1.0-dev (build N)` | `N` = 那次 CI 运行的 `GITHUB_RUN_NUMBER`。**没有 tag 时版本号就是 `0.1.0-dev`** |
| `构建时刻` | 打包那一刻的 UTC 时间（ISO-8601）。`version.json` 里的 `build_time` 就是同一行、同一语义（两侧都是打包时刻），所以两者**可以**比；但客户端当前不拿它下结论，见下面 [在 app 里看](#在-app-里看关于窗口的那一行) |
| `架构` | 目前只有 `arm64`（`GhosttyKit.xcframework` 只切了 arm64） |
| `最低系统` | 二进制自身的 `minos` 与**内嵌依赖**里最高者的较大值——内嵌的 Homebrew 库会把最低系统抬高，所以这一行往往大于 `Info.plist` 里的原始声明 |
| `签名` | 记账档位：`adhoc` / `dev`（免费个人账号）/ `id`（Developer ID）/ `none`。**dev 轨恒为 `adhoc`**（CI 当场验这一条），取值口径见 [签名档位](#签名档位adhoc--dev--id--none) |

`CFBundleVersion` 用的是 dev workflow 自己的运行编号。拆成 dev / release 两个 workflow
文件之后，两条轨的 build 号**不再同源单调**（`GITHUB_RUN_NUMBER` 按文件计数）；
比较先后请以 commit 为准，别拿 build 号跨轨比大小。

## 「有没有新版」的机读入口：`version.json`

dev Release 上挂了一份固定 URL 的清单，给客户端做**更新检测**用：

```bash
curl -fsSL https://github.com/hanxi/ghosterm/releases/download/dev/version.json
```

为什么是它而不是 GitHub API：`releases/download/dev/<name>` 是**固定直链**，不占
未认证 API 的 60 次/小时额度，也不需要 token；渠道判据因此全部落在文件里。

| 字段 | 口径 |
|---|---|
| `git_commit` | **dev 渠道新旧的唯一主锚**。整条 40 位，比对不等即「有新版」 |
| `build_time` | 打包时刻，ISO-8601 UTC，取自随包 `BUILD-INFO.txt` 的 `构建时刻`——**与本机那份同一个语义**，空串或格式不对一律判红。它只做展示与对账：判据不拿时间比「谁新」（rolling 的 `dev` tag 会被重打，晚打包的可能是更旧的 commit） |
| `version` / `build` | `0.1.0-dev` 与该 workflow 文件的运行编号，**不可跨轨比大小** |
| `min_macos` / `arch` | 装机下限；`arch` 目前恒为 `arm64` |
| `signature` / `notarized` | `adhoc` / `false`——这两个字段就是「不许自动安装」的机器可读声明 |
| `assets` + `sha256` + `download_base` | 拼出 dmg/zip 直链并自行校验，不走 API 拿 `browser_download_url` |

清单由 `.github/workflows/dev-build.yml` 的「生成 version.json」step 从**随包
`BUILD-INFO.txt` 与 `SHA256SUMS`** 解析而来（不是第二份真相），解析不到任何一项
就当场红——写 `unknown` 蒙过去等于把检测做成永远「已是最新」。同一 job 的「复验」
step 再从固定 URL 把它取回来对答案（commit、渠道、架构、sha256 形状），取不到或
对不上就判失败。

## 在 app 里看：关于窗口的那一行

清单是给机器读的，机器读它的是 app。**入口在「关于 Ghosterm」窗口**（菜单栏
`Ghosterm → 关于 Ghosterm`），版本号下面那一行：

```
更新    有新版（9ef21070d320…）        [检查更新]
本机    0.1.0-dev (23) · d3e9fde742bd · 2026-10-25T03:17:24Z
```

口径（实现在 `apple/GhostermApp/Sources/GhostermAppModel/UpdateCheck.swift`，
UI 在 `AboutWindow.swift` 的 `UpdateRow`）：

- **只在点「检查更新」时联网**：没有启动时检查、没有后台轮询。dev 包 ad-hoc 且未
  公证，当前产品只提供手动下载与用户主动执行的安装入口。
- **结论三态，不折叠**：`有新版` / `已是最新` / `查不出来（+原因）`。「本机这份包
  里没有 commit 记录」「带未提交改动（`+dirty`）」「清单渠道不是 dev」「取不到
  version.json」「version.json 读不懂」全都如实报，**绝不报成「已是最新」**。
- **主锚是 commit**：本机记的是短 sha（`--short=12`），清单里是整条 40 位，比前缀。
  不比 `version`/`build`——那两条跨轨不可比（见上面 [`BUILD-INFO.txt`](#确认这个包是怎么来的读-build-infotxt)）。
- **没有「安装」按钮**：清单里 `signature=adhoc` + `notarized=false` 表示当前渠道不允许自动安装，
  不表示用户无法手动安装。界面只给「打开下载页」这一条直链；有新版时另外写明
  首次打开会被 Gatekeeper 拦、要怎么放行（口径同上面 [怎么装](#怎么装一键脚本手工解包homebrew-或浏览器)）。
- 「本机」那行是**可复制**的：要用户自己核对 commit，报「查不出来」时才有的放矢。

**当前校验边界**：发布脚本生成/复验清单、安装脚本验包和 App 解码是三条不同路径。`RemoteBuild.decode` 会拒绝缺字段、空字符串、不可解析时间及非 64 位摘要，但尚未检查远端 `git_commit` 为 40 位十六进制、摘要的十六进制字符集，也没有约束 `download_base` 的来源。`compare` 按 commit 前缀相同/不同作判断，不验证提交祖先关系；因此“有新版”实际表示“与本机不同的 dev 提交”。这些校验缺口列入[路线图](../reference/roadmap.md#macos-下一步与共享缺口)，不能把已有缺字段测试扩大为全部畸形输入都返回“查不出来”。

闸门：`bash scripts/mutation-check-app-update.sh`——七组变异（把「查不出来」折成
「已是最新」、拿版本号当判据、解码给默认值、`installBlocked` 恒 false、文案撒谎、
那一行整个不画、多给一颗「安装」按钮），每组都要让对应断言变红并带一条指向性对照。

## dev 包的边界（不是缺陷，是现状）

- **此 dev 发布渠道只有 macOS**。Android 已有[本地 debug APK 构建入口](../../android/README.md)，但尚无 Android CI/发布渠道；iOS 仍无可用构建入口。
- **`dev` 不能当自动更新通道**：每轮 ad-hoc 签名的 cdhash 都不同，而 `dev` tag 会被
  强删重建；做自动更新要等 Developer ID 签名 + 公证（见
  [roadmap](../reference/roadmap.md) 的遗留项）。「有没有新版」的检测不受此限制，
  见上面 [version.json](#有没有新版的机读入口versionjson)——它只给直链，装不装由人。
- 普通终端构建细节，见 [CI 出包口径](#ci-是怎么串起来的)。

## 签名档位：`adhoc` / `dev` / `id` / `none`

`scripts/package-mac.sh` 按**证书本身的类**记账，记账结果原样进 `BUILD-INFO.txt` 的
`签名:` 那一行与 `version.json` 的 `signature`：

| 档位 | 怎么来 | 能公证吗 | 用户下载以后 |
|---|---|---|---|
| `adhoc` | `--adhoc`（默认；CI 的 dev 轨走这条） | ❌ | Gatekeeper 拦：`xattr -dr com.apple.quarantine` 或系统设置「仍要打开」（见 [怎么装](#怎么装一键脚本手工解包homebrew-或浏览器)） |
| `dev` | `--sign "Apple Development: …"`（免费个人账号，不花 $99） | ❌ Apple 不给这一档公证，脚本自己先拦 | 同上，仍要放行 |
| `id` | `--sign "Developer ID Application: …"`（付费 Apple Developer Program） | ✅ `--notarize <profile>` | 双击即装 |
| `none` | `--no-sign`（排障用） | ❌ | 未签名，macOS 直接拒开 |

- **决定「首次打开要不要放行」的是下载工具贴不贴 `com.apple.quarantine`，不是换渠道**：
  `brew install --cask` 也贴（Homebrew 自己调 LSQuarantine，agent 写 `Homebrew Cask`），
  所以 `adhoc` + brew **照样拦**，仍然不等于能自动安装（`signature=adhoc` 的判断不变）。
  实测细节与免弹窗的 `curl` + `ditto` 装法见上面 [怎么装](#怎么装一键脚本手工解包homebrew-或浏览器)。

- **免费账号买到的只有「签名身份稳定」这一件事**：同一 Team ID、同一 designated
  requirement，覆盖安装不再因每轮 cdhash 变化被判成不同来源。它**不解决**「下载来能不能
  双击」——那要公证，公证要付费的 Developer ID。
- **因此「每 7 天重签一次」不是第三条路**：7 天是免费账号 *provisioning profile* 的有效期
  （iOS 装机才吃这套），macOS 直发 dmg 不吃 profile；而带 `--timestamp` 的签名到期后依然
  有效（校验用的是签名时刻）。真正的拦路石是公证，重打多少次都不动它。每轮重打反倒会换
  cdhash、强删重建 `dev` tag，让用户机器上那份与清单里那份不是同一个二进制——检测照报
  「已是最新」（`git_commit` 相同），实际内容已经换了，这是最坏的一种假绿。
- 身份串不属于这两类（Enterprise、`Developer ID Installer`、自签）→ **当场拒**：宁可红，
  不拿别的证书冒充 `id`。那个字段是 app 侧判断「能不能自动装」的唯一依据
  （`RemoteBuild.installBlocked`，见
  [关于窗口的那一行](#在-app-里看关于窗口的那一行)）。
  牙见 `scripts/mutation-check-package-mac.sh` 的 A8。A9 现验证普通引擎收据和包内无旧 runtime。

## 本地怎么出同样的包

前提：macOS arm64、Xcode、Zig 0.16.0 与普通核心构建依赖。

```bash
bash scripts/build-ghostty.sh
bash scripts/gate.sh
bash scripts/package-mac.sh --adhoc --out dist
```

普通 full libghostty 仅应用 host IO、程序状态与语义提示三份补丁；构建器在工程 `tmp/`
内使用隔离源码，保持 vendor pin 不变。SDK 的 `terminal-engine.json` 记录实际 archive
与补丁哈希。打包校验拒绝旧 session helper、快照 API、旧收据与随包 runtime。
应用不携带 Linux 安装包，不要求远端编译器，也不自动安装 tmux。

## CI 是怎么串起来的

共享打包工作流运行 `build-host-ssh2.py --fetch`，Core/SwiftPM 消费仓库自建补丁 libssh2，OpenSSL 仍由宿主提供。`010c251` 显式将 Homebrew OpenSSL 3 的 pkg-config 路径放在前面，修复 runner 默认选中 OpenSSL 4 后准备依赖失败的问题。[dev CI 41235795612](https://github.com/hanxi/ghosterm/actions/runs/41235795612) 已通过主机依赖准备、47 CTest / 91 ABI、普通引擎打包与滚动 dev 发布；发布后的 `version.json` 已核对提交号 `010c251d246b42f582322f4c5d7af95bead7ec92`、arm64、最低系统 15.0、ad-hoc 与未公证状态。这是轻闸门结果，不替代完整 macOS UI 验收，既有设置页像素失败仍待修。构建准备与证据见[主机依赖说明](../reference/host-ssh2-build.md)。

```text
push main（通过路径过滤）或手动 → dev-build.yml
              ├─ macos-package.yml → package-mac.sh → macOS artifact
              └─ release-dev → 复验字节与版本清单 → 滚动 dev prerelease
```

触发条件以仓库的 [dev 工作流](../../.github/workflows/dev-build.yml)和 [release 工作流](../../.github/workflows/macos-release.yml)为准：

- dev 的 push 忽略 `docs/**`、`**.md`、`android/**`、`LICENSE`、`.gitignore`；只有全部改动都在忽略范围内时不触发。`terminal/android/**` 不在这个忽略范围内，不能笼统称“Android 改动都不触发”。fork 的 dev 出包/发布 job 被显式禁用。
- release 接收 `v*` tag、手动触发和命中路径白名单的 PR。PR 白名单为 `apple/**`、`terminal/**`、`core/**`、`assets/icons/**`、`scripts/**`、`patches/**` 及 release/package 两份 workflow；仅 `docs/**`、根 README 或 `android/**` 的 PR 不触发。它没有全局 Markdown 排除规则，例如仅改 `apple/README.md` 的 PR 仍会触发 full 档，不能笼统说“文档 PR 都不构建”。
- dev 按分支取消尚未完成的旧运行；发布 job 另行串行化。发布失败或运行取消不能记为该提交已出包。安装后用随包 `BUILD-INFO.txt`，检测时用 `version.json` 的 commit 确认实际版本。

两条轨共用 `macos-package.yml` 与同一份打包脚本；这些触发过滤不改变 [AGENTS.md](../../AGENTS.md) 的合并验收要求，也不提供 Android APK 的 CI 验收。
release 的 full 档运行 Apple 原生与变异闸门；有证书时才走签名/公证。
macOS 包只依赖普通引擎 SDK、共享 C 核心、原生 UI、图标和主题，不再依赖 Linux bundle job。

当前图标脚本缺依赖直接失败；workflow 对打包/认证变异日志中的 `SKIP` 有显式拦截，并在 Apple 步骤起跑前检查 Swift；没有对所有宿主闸门输出统一拒绝跳过。`apple-app-gate.sh` 在 sshd 启动失败时仍会跳过 SSH e2e、运行其余用例后返回成功；CI 已接线不等于该条 SSH 证据已取得。统一覆盖审计仍是[路线图待办](../reference/roadmap.md#macos-下一步与共享缺口)。

宿主脚本还未显式传播 `check-abi-apple.sh` 的失败返回值（未启用 `set -e`，该调用没有失败分支），后续成功可能覆盖 ABI 检查失败。复核构建记录时应单独核验 ABI 步骤和真实 SSH 覆盖，不能只取最后的 `APPLE APP GATE PASS`。这是源码核对发现的独立待修项，不代表已复现某次发布包或 CI 失败；详见 [Apple 构建与验证](../../apple/README.md#构建与验证)。

## 相关文档

- [能力矩阵](../reference/capability-matrix.md)——构建与交付一行的状态口径
- [路线图](../reference/roadmap.md)——签名 / 公证 / 降最低系统版本的遗留项
