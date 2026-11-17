# AGENTS.md — Ghosterm 开发铁律

> 本文件是**验收闸门的唯一事实来源**。与代码冲突时以本文件为准，改规则须走 PR 并说明理由。

## 项目一句话

Ghosterm = **libghostty 驱动的多端终端** × **VS Code 式 `~/.ssh/config` 图形管理** × **WinSCP 式 SFTP 文件管理器**。

技术栈：**各端原生 UI + 共享 C 核心（libghostty / libssh2）+ Transport 接缝**。
**不用跨端 UI 框架，不用 Rust。**

## AI 协作的测试语境

本仓库的变异验证、故障注入和 OOM 扫描用于发现并修复 Ghosterm 及锁定依赖的缺陷，
核对断言能否发现回归。执行前按具体脚本确认测试对象：工程内隔离副本、一次性夹具、
明确指定的测试设备或已获授权的测试服务；仓库访问权不自动授权其它主机或用户数据。
「目标红」指编译成功后预期的测试断言失败；编译失败、崩溃和 sanitizer 报错须单独记账，
不得当作正常断言红。已有各闸门的具体判据继续适用。

协作时说明具体缺陷、文件、测试对象和修复后的预期行为。遇到 `This content can’t be shown`
或 Daybreak 提示，按[AI 辅助开发与安全提示排查](docs/how-to/ai-development.md)记录并反馈；
本地文档不能调整服务端安全检查或授予 Daybreak 访问权。

## 当前重构决定

2026-10-28 起按 `docs/reference/plan-optional-tmux.md` 实施：普通本地 PTY/SSH 为默认终端方式，
后台会话由用户显式选择 tmux；整体退役自有 sessiond、私有恢复协议、安装与随包 runtime，
不保留旧 ABI/记录的兼容层。2026-11-06 经用户确认，远端主机入口改为统一连接面板：
打开面板可自动探测/列举 tmux，普通终端无需等待；创建/attach 仍显式选择。
本地默认与普通终端快捷入口不探测，能力未知不得当作不存在。规则调整理由与证据见上述计划末节。
依赖闭包、命令决策与验收进度见
`docs/reference/optional-tmux-implementation.md`。旧会话系统已退役；通用能力对应闸门仍须执行；
退役提交同步修改源码、调用方、构建、测试、打包、CI 和下文覆盖范围，不能先删断言换绿。

普通默认入口另以 `bash scripts/mutation-check-default-entry.sh` 手工验证：隔离副本不提供
runtime catalog，四组变异护本地创建、启动偏好、分屏目录与关闭确认；每组目标红、
同变异的主机配置读取对照绿，复原比对 SHA256。已纳入 CI 的 full 档；轻闸门不运行此变异。

## 目录边界（禁止越界）

| 目录 | 内容 | 依赖规则 |
|---|---|---|
| `core/` | C 共享库：ssh / sftp / ssh_config / known_hosts / forward / pty | **纯 C11 + POSIX**，禁止依赖任何端代码、禁止依赖 UI |
| `terminal/` | 各端 TerminalSurface（full libghostty 或 libghostty-vt+自绘） | 只依赖 `core/include` 与 libghostty |
| `apple/ android/`（现有）· `windows/ linux/ harmony/ web/`（规划） | 各端宿主 + UI | 通过 `core/include/ghosterm.h` 消费核心；**禁止**直接引用其他端 |
| `vendor/` | 上游 submodule（ghostty、ish-arm64、quickjs-ng），**锁 pin** | 不允许就地改；改动一律走 `patches/` |
| `runtime-payload/` | 环境包（Android Termux/proot、iOS iSH） | 只放清单与下载/校验逻辑，二进制不入库 |
| `patches/` | 上游补丁 | 每个补丁必须带 `WHY.md` |
| `scripts/` | 构建/闸门脚本 | — |
| `packages/` | 插件 SDK、schema 与开发 testkit；当前为未冻结候选 | 可移植契约不依赖端 UI；portable SDK 不引 Node / DOM。C 核心不依赖 TypeScript 或开发工具运行时 |
| `docs/` | Diátaxis 文档 | — |
| `assets/` | 品牌资源：应用图标**母版 SVG** + 各端派生产物 | 无代码依赖；产物一律由 `scripts/build-icons.sh` 从母版派生，禁手改产物 |

## 铁律

1. **三层接缝**：`UI → TerminalSurface → Transport → Core`。UI **禁止**直接调 Core；TerminalSurface **禁止**知道背后是 SSH 还是本地 shell。违反＝架构错误，不是风格问题。
2. **字节流不过桥**：终端输出走 `transport → surface` 直连，**不进事件队列、不过 UI 线程**。事件队列只承载低频事件（状态/进度/退出）。高频过桥是性能红线。
3. **C ABI 是唯一跨语言边界**：所有平台绑定只经 `core/include/ghosterm.h`。句柄一律 opaque；跨语言只传 POD 与 NUL 结尾字符串。**改 `ghosterm.h` 必须同步各端绑定并跑 `scripts/gate.sh`。**
4. **`ssh_config` 文件即真相**：解析→编辑→写回必须**逐字节无损往返**（保留注释/未知指令/块顺序/缩进）。写回前强制备份。**「无损」分两档验收**：未编辑往返＝逐字节相同；编辑后往返＝「除目标行外必须逐字保持不变」（M1 补闸门）。语义以真 `ssh -G` 对拍为准，锚点是 `core/tests/fixtures/golden.{conf,effective.tsv,json}`（由 `core/tests/scripts/regen-golden.sh` 同源再生，**勿手改**）。**再生必须在钉版本环境执行、且环境干净**（`ssh -G` 也读系统级 `/etc/ssh/ssh_config`）：当前锚 `OpenSSH_10.3p1`；换环境再生＝换基线，须标 `BREAKING`。**默认 IdentityFile 候选链是版本相关行为，不是常量**（9.6p1=7 条含 xmss/dsa、10.3p1=5 条、master=6 条含抗量子混合），故实现为 `gt_identity_policy_t` 策略，内核提供 9_6/10_3/MASTER/NONE/CUSTOM 五档，见 core/tests/README.md。**锚点不得带再生机器的身份**：环境相关的默认值（回落登录名、`~` 展开出的路径）由 `regen-golden.sh` 映射成中性 token，脚本落盘后跑泄漏守卫，搜到真实家目录/登录名即失败。
5. **密钥红线**：私钥只进平台安全存储（Keychain/Keystore/HUKS）。核心只接受**已解锁**的材料，**永不**落盘明文、不进日志、不进测试快照。
6. **三态语义不得折叠**：`未知 ≠ 拒绝`（hostkey / job 结果 / 能力探测）。**能力缺失必须显式报错**（`GT_ERR_UNSUPPORTED`），禁止静默降级、禁止假装成功。
7. **终端引擎按平台分档，不得混用**（见下表）。full libghostty 在 Android/HarmonyOS **没有渲染后端**；Web 端上游 wasm 产物只有 libghostty-vt（`renderer/WebGL.zig` 在 pin 版本是空 stub）——这三端**当前**必须走 libghostty-vt + 自绘。Web 的长期目标仍是 full libghostty（WebGL, wasm），等上游后端落地再评估切换。
8. **平台差异显式化**：能力矩阵维护在 `docs/reference/capability-matrix.md`，状态口径 ✅/🟡/⏳/❌ 必须诚实，🟡 必须写明「在哪档证据下成立」。
9. **上游补丁入库**：对 libghostty / libssh2 / iSH 的任何本地修改放 `patches/<upstream>/` 并附 `WHY.md`，随代码提交。
10. **环境包按需下载**：运行时环境（Termux bootstrap / iSH rootfs）**不进 App 包**；下载须校验 sha256、原子激活、可回滚、可删除。
11. **许可**：仓库整体 **MIT License**（`LICENSE`）。随包组件按各自许可声明；iSH / Termux 这类 GPL 组件若日后随二进制分发，仍须履行**其自身的** source offer；安排见 `docs/explanation/licensing.md`。

## 终端引擎分档（铁律 #7 的执行体）

| 平台 | 引擎 | 渲染 | 依据 |
|---|---|---|---|
| macOS | **full libghostty** | Metal | Ghostty 本体 / [Termini](https://github.com/hudsonkit/Termini) / [iGhostVT](https://github.com/owngoal-dev/iGhostVT) |
| iOS | **full libghostty** | Metal | [Termini](https://github.com/hudsonkit/Termini) / [iGhostVT](https://github.com/owngoal-dev/iGhostVT) |
| Linux | **full libghostty** | OpenGL 4.3 core | Ghostty GTK |
| Web | **libghostty-vt + 自绘**（目标 full libghostty，待上游 WebGL 后端） | Canvas/WebGL（wasm） | [ghostty-web](https://github.com/coder/ghostty-web) 即此路线；上游 wasm 只产 libghostty-vt，`renderer/WebGL.zig` 为空 stub |
| Android | **libghostty-vt + 自绘** | GLES/Canvas | 无 GL4.3 core 后端 |
| HarmonyOS | **libghostty-vt + 自绘** | ArkUI Canvas | [libghostty-ohos](https://github.com/wiedymi/libghostty-ohos) |
| Windows | **待定** | — | OpenGL on Windows 需 WGL/ANGLE，先验证 |

> 两档挂在**同一个 transport 接缝**后面，故 UI 与业务代码零分叉。

## 临时文件与执行（环境硬约束）

- **临时文件一律放工程根目录下的 `tmp/`**（已 git 忽略）；**不写、不依赖系统 `/tmp`**。
- **禁止在系统 `/tmp` 下执行**任何二进制 / 脚本 / 探针——本环境不允许，从 `/tmp` 执行的进程会被判死（SIGKILL，表现为 `exit 137`）。临时探针、一次性脚本、二进制都在 `tmp/` 里生成并在 `tmp/` 下执行。
- `scripts/` 与闸门产生的临时文件同样落在 `tmp/`（构建产物 `core/build/` 已忽略，不在此列）。
- `tmp/` 内容不入库；用完自行清理：`rm -rf tmp/*`（保留目录本身）。

## Shell 写法（跨 runner 的硬约束）

- **`$var` 紧跟非 ASCII 字符（中文标点、全角括号、破折号）时必须写 `${var}`**：某些
  bash×locale 组合会把多字节的首字节**吃进变量名**，`set -u` 下当场报「unbound
  variable」——于是**该打日志说明失败原因的那一行，反而把脚本打死**。CI 实测两例：
  打包变异 A7 的 `源 $A7_ROOT）` 报 `A7_ROOT?: unbound variable`（其余 15 项全绿却整步
  判红），本地 `LC_ALL=en_US.UTF-8 /bin/bash` 可稳定复现同样的旧写法红、`${A7_ROOT}）`
  绿。仓库文案与注释里中文很多，这条不是风格而是验收口径；自查用正则
  `\$\{?[A-Za-z_][A-Za-z0-9_]*\}?[^\x00-\x7F]`（对 `git ls-files '*.sh'` 全量跑，
  命中且未带花括号的就是雷）。

## 验收闸门（合并前必须全绿）

项目自有 C 代码另按 `docs/reference/c-coding-style.md` 排版，使用 clang-format 19.1.7：
`python3 scripts/format-c.py --check` 必须通过。独立 `c-format.yml` 在 push main / PR /
手动触发时执行同一只读检查；不改离线 `gate.sh`，不计入下文二十份变异验证。
新增原因：统一 C 代码可读性并防止后续提交产生排版漂移。

```bash
bash scripts/gate.sh        # 离线闸门：C 核心构建 + ssh_config 对拍 + ABI 冒烟
bash scripts/build-core.sh  # 只构建核心（快速）
```

闸门内容（`scripts/gate.sh`）：

1. `core` 以 `-Wall -Wextra -Werror` 构建通过；
2. `ctest` 全绿，其中 **`sshconfig_golden` 必须做全表语义对拍（42 键 × 6 host）**，并**双向校验支持集**（C 表 ↔ 夹具元数据，防漂移）；
3. `ghosterm.h` 导出符号集合无漂移（符号表对拍，见下）；
4. 未实现入口**必须返回错误**（`api_smoke` 断言，防「静默成功」）。

`public_headers` 随 `ctest` 验证全部公开头可独立以 C11 编译、依赖无环、
子头不反向包含聚合头 `ghosterm.h`。平台绑定仍只经聚合头进入；
共享错误类型位于 `ghosterm/types.h`，不在各绑定复制。

**改了 `apple/ android/ windows/ linux/ harmony/ web/` 必须另跑对应平台构建**；JS/宿主闸门不覆盖原生编译。

**CI 双轨（按工作流路径过滤触发）**：未被 `paths-ignore` 过滤的 push `main` → `dev-build.yml`（轻闸门 + ad-hoc macOS 包 +
滚动 `dev` prerelease）；tag `v*` / PR / 手动 → `macos-release.yml`（全量闸门 + 签名/公证分支）。
两条轨共用 `.github/workflows/macos-package.yml`；仅文档、Android 等被忽略路径的 dev push 不触发，取消或发布失败也可能没有新包，不能承诺每次提交均出包。这些触发条件不改变上文合并闸门要求。普通终端包不携带 helper 或 Linux 安装包，
只构建普通 full libghostty、共享核心和原生 UI。装机口径见 `docs/how-to/dev-build.md`
（**首次打开拦不拦，取决于取包工具贴不贴 `com.apple.quarantine`**：浏览器与 `brew --cask` 都贴，
`curl` + `ditto` 不贴——所以「走 Homebrew」换不到免弹窗，只换到一条命令装/升级）。
dev 轨另有一条 Homebrew 分发链：公开 tap `hanxi/homebrew-ghosterm` 的 `ghosterm-dev` cask
指向 `dev` 滚动直链、写 `sha256 :no_check`（滚动资产字节钉不住，如实标注不校验）；
`dev-build.yml` 在「复验 dev Release」之后多一道 **cask 对拍**（`scripts/check-dev-cask.sh`
从 raw 取回 cask，比资产名 / `:latest` / `:no_check` / 下限），它红只红这一步，不回滚 release。
dev Release 另挂一份机读清单 `version.json`（固定 URL，不走 API）：**新旧以 `git_commit` 为主锚**
——`build` 号按 workflow 文件计数，跨轨不可比；清单由随包 `BUILD-INFO.txt`/`SHA256SUMS` 解析而来
（不是第二份真相），解析不到即当场红，「复验」step 再从固定 URL 取回对答案。
**2026-11-17 按用户要求调整应用内更新**：没有 Apple 证书的 ad-hoc 包也须支持用户选择的
下载与重启更新；不再把 `signature=adhoc` / `notarized=false` 当作更新能力禁止条件。
采用锁定 Sparkle 引擎与独立 Ed25519 更新签名：本机公钥固定，appcast 与 ZIP 都须验签；
密钥缺失或验证失败显式报错，禁止跳过校验。Apple 签名 / 公证状态仍如实记账，首次安装的
系统放行规则保持。dev 比随包 commit 与远端 dev 清单；正式版比 GitHub 最新正式发布的
版本号，不混用渠道或 workflow 运行号。`CFBundleVersion` / `update_version` 改为同源打包
Unix 秒数，供 Sparkle 安装校验；workflow build 仍在随包信息中用于追溯。
入口为关于窗口与应用菜单，仍仅主动检查联网；下载、校验及重启提示在右下角，允许收起、
取消下载和稍后重启，重启保留原退出确认。规则调整原因：用户明确要求无证书包也能正常
应用内更新；Ed25519 信任链独立于 Apple 证书。发布闭包、密钥配置和本地证据口径见
`docs/how-to/macos-updates.md`，变异入口仍为 `scripts/mutation-check-app-update.sh`。
这组规则变更应与实现一起经 PR 审阅，不据本地验证声称已经发行。

> **CI 覆盖范围（诚实标注）**：`gates=full` 只自动跑这 **20 份**变异验证——
> Apple 端 `terminal / transport / tabs / chrome / settings / pixel / focus / split-dim / occlusion / app-update`、
> `default-entry / ssh-auth / ssh-credentials / auth / tmux / tmux-ui`、`tab-chrome / sidebar-hover`、`install-macos`（dev 也执行）、以及 `package-mac`（在**干净检出**上验打包链，输出里
> 出现 `SKIP` 一律判红，缺随包主题不许蒙过去）。**其余闸门仍是「手工跑」**：`core/` 侧的
> sshconfig / include / ssh / sftp / exec / files / transfers / dragdrop / pty-inheritance /
> login-shell / known-hosts / public-headers 等 CI 看不见。
> 所以「CI 绿」≠「闸门全绿」——改到上面那些面，合并前仍要本地 `bash scripts/gate.sh` +
> 对应变异脚本各自跑绿。
> 另：dev 轨比 release 轨多**一道对拍**（`check-dev-cask.sh` 比 Homebrew cask ↔ 本轮资产，
> 它依赖公开 tap 里那份手写的 `.rb`，所以只能在 dev 轨跑）——tap 改了没改、版本号 bump 忘
> 跟着改，都由这一步红。

**Android 设置与正式图标（2026-11-01，已提交 29d3f3d）**：五类设置、16 项已提交偏好的生产消费者、恢复保护与关于已接。功能代码完整 576 项在 API 36 / arm64 / 4 KB 页通过（1,587.573 秒），零失败/错误/跳过，785 份输入及两安装 APK 摘要一致；之后的正式 SVG 派生图标接线由独立 PackageManager/位图断言和误绑默认图标变异验收，不能称完整 577 项重跑。新增 settings 16＋6＋2、terminal-style 29、terminal-controls 18、file-preferences 16、settings-protection 7、about 12、APK build-info 7、icon-binding 1 组分轮手工证据见 Android 实施计划 §9.83–§9.91；不计入上述 macOS CI 20 份。主工程 Core 47/91、Android JNI 51/40、平台 release lint 与带图标的本地固定证书 release 构建通过；16 KB 页已验旧云端包覆盖升级、五文件保留、设置重启及两条 ASCII 系统键盘命令。该本地包保留 dirty 来源；随后 `29d3f3d` 的 release CI 41282597767 成功，实际云端包为 6,129,125 字节，提交/clean 收据、固定证书、压缩/对齐及实际 R8/资源裁剪步骤已核验。同一云端包的 16 KB 页升级、七文件保留、设置重启、关于 clean 提交及两条 ASCII 命令专项通过；完整设备/输入法/性能/功能矩阵仍未完成，证据见 §9.91。

**Android 历史位置核对（§9.104）**：新增显式只读目标/候选位置观察，结果与持久任务结果分开；不因存在而升级成功或授予删除。远端重开独立目标 owner，不连接已失效来源。API 29 / API 36 各 33 项组合、签名 release / 四模块 lint 和 `mutation-check-android-history-inspection.py` 十组设备变异通过（目标红、同 APK Core 绿、复原绿与 SHA256）；不增加 macOS CI 20 份。内容/归属核验、残留清理、历史重新发起与完整客户端仍未完成。

**Android 历史重新发起（§9.105）**：历史检查点经确认转换为全新请求，关联原记录并重新预检/认证，不恢复旧冲突授权；SQLite 结构仍为 1，普通头/逐项格式 1 与关联头格式 2 共存，旧记录不重写。API 29 / API 36 最终各 62 项通过，十五个新保护组分轮通过（完整十四组＋初始跳过增量一组），既有记录二十组与重试控制两组复验通过；目标红、同 APK Core 绿、复原绿与 SHA256。修正后签名 release / 四模块 lint 通过；云端发行另记。不增加 macOS CI 20 份，残留归属/清理和完整客户端仍未完成。

**Android 跨请求目标暂存预算（§9.106）**：保留候选与完整写入预约统一计费，不按清除历史而漏账，不据此删除文件。API 29 / API 36 各 137 项组合、十七个新保护组分轮（完整十六＋借用父 fd 增量一组）、既有本机发布 25 组/记录 20 组复验通过；目标红、同 APK Core 绿、复原绿与 SHA256。Core 47/93、签名 release 与四模块 lint 通过。只读 fd 的匿名发布探针在 API 29 遭 SELinux link 拒绝，已撤回；没有降低策略或加入静默回退。预算已随 `5a0618d` 发布，来源/签名/包内容核验通过；已发行历史重试 `106f66b` 的 16 KB 页实际 UI 范围见 §9.106。本机显式清理的新增证据见 §9.107，持久归属证明与远端/SAF 残留仍待完成，不增加 macOS CI 20 份。

**Android 显式本机暂存维护（§9.107）**：`ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-retained-files.py` 的二十组 API 29 手工设备变异通过，护旧列表、文件变化、活动预约、列表/预览限额、不完整/二进制预览、fd 释放、原生只读/不跟链接/CLOEXEC/普通文件/借用父 fd、实际删除与确认/保留按钮、错误可见性和确认前重新打开根目录。每组真实编译后单条断言红、同 APK Core 配置对照绿、恢复绿与 SHA256；不增加 macOS CI 20 份。API 29 / API 36 各 158 项最终组合、Core 47/93、固定证书 release 与四模块 lint 通过；本地签名包的 16 KB 页实际 UI 范围见实现计划，不以专项代替客户端全量验收。

**Android 空请求目录维护（§9.108）**：`ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-empty-requests.py` 首二十一组完整与取消交接/晚到释放两组增量通过，共二十三个不同保护组分轮证据；每组真实编译后单条目标红、同 APK Core 配置对照绿、复原绿与 SHA256。不递归删除文件，任务从来源阶段到资源退出期间阻止维护。原文件清理二十组复验通过；两设备各 178 项前置组合、交接收紧后各 68 项相关复验、Core 47/93、release 与四模块 lint 通过。本地签名包 16 KB 页实际 UI 与一次 QEMU 离线后恢复记录在实现计划单列，不冒充云端/真机/全客户端验收，不增加 macOS CI 20 份。



**Android 擦除背景（§9.115）**：完整 848 项发现两处相关背景像素失败，不能记绿。修复后 API 36 的 75 项终端/Kitty/字体/性能组合、Core 47/93、release 与四模块 lint 通过。API 29 以 `ANDROID_KITTY_MUTATE=1 ANDROID_KITTY_CASES=background-layer,erased-background` 配合 `scripts/test-android-ssh.sh` 选跑两组：默认层不透明与旧 style-only 空白背景判据均编译后单条正常断言红、同 APK Core 绿、恢复绿与 SHA256。为手工设备证据，不增加 macOS CI 20 份；不能把定向通过当新完整回归通过。

**Android DNS 探测兼容（§9.116）**：真实模拟器仅限制测试 UID 到 AOSP 两个可达性探测地址，保留本机 SSH/DNS可达，验证旧接口 errno 64 与零 DNS 请求、修复后系统 DNS→真实 SSH/原域名验证名称。API 29 / 36 各 50 项、签名 release/四模块 lint 通过；`mutation-check-android-dns.py` 十三个新增与原平台 answer/rcode 两组同轮通过，每组编译后单条正常断言红、同 APK Core 绿、恢复绿及 SHA256，不增加 macOS CI 20 份。独立取消、双地址族/上限、响应码、错误范围、快照与晚到回调、启动释放和文案均有保护；模拟器规则精确恢复，不将其记作用户真机或完整 VPN/私人 DNS 验收。

**Android 结果不明来源重新选择（§9.121）**：普通安全重试不放开未知项；独立入口默认全不选，确认后新请求重新扫描/认证/ASK 冲突，旧记录/目标/候选保留。API 29 / 36 各 47 项组合（含真实 SFTP/SAF）、Core 47/93、签名 release 与四模块 lint 通过。`mutation-check-android-history-recovery.py` 十五个不同组分轮取证（11＋4）：正常断言红、同 APK Core 绿、恢复绿和 SHA256；最终生产源与两轮原件一致。初轮第十二组参数异常和后续 SAF 用例缺导入的编译失败均未计通过。手工证据，不增加 macOS CI 20 份；完整恢复/残留清理与设备矩阵未完成。

**受控远端删除后端（§9.122；后续已随应用发行）**：当前 Core 49 CTest / 94 ABI，Apple 90 引用/4 登记未用，JNI 56/38。`mutation-check-sftp-remove.py` 的 Core 21＋Swift 7 组与 `ANDROID_REMOVE_MUTATE=1` 配合 `test-android-ssh.sh` 的 Android 19 组为分轮手工证据：编译后正常断言红、独立配置绿、复原绿及 SHA256；旧保护 13＋4 组复验通过。API 29/36 各 22 项与签名 release / 四模块 lint 通过；不增加 macOS CI 20 份。该后端切片当时未接页面/持久意图，后续历史文件清理见 §9.123；不据此记 A6 完成。

**历史位置显式清理（§9.123，已随 98d845e 发行）**：`ANDROID_FILE_CLEANUP_MUTATE=1` 配合 `test-android-ssh.sh` 的十三个不同保护分轮（12＋1）通过，后续三组 UI 和一组 write-ahead 复验通过：编译后正常断言红、同 APK 配置绿、复原绿及 SHA256。API 29/36 各 76 项、Core 49/94、签名 release/四模块 lint 通过，885 份最终输入一致。不增加 macOS CI 20 份；实际签名包在 16 KiB 页设备已验 SAF 删除回包前结束进程、UNKNOWN 意图保留和无重放；其它端点死亡、完整提供方/服务器及物理设备矩阵未完成，不记 A6/客户端完成。

**Android 触屏输入与快捷键连发（§9.201）**：鼠标模式下触屏轻点同时请求 IME 与发送原鼠标操作；长按仍选字。快捷键按系统延迟/间隔连发，松手、滑出/取消、失焦、禁用及移除停止。API 36 / arm64 / 4 KiB 页的终端 53＋控制 9＋连发 5 项为分轮证据；`ANDROID_MOUSE_MUTATIONS=tap-route,keyboard,long-press,key-repeat,key-repeat-release` 五组（初始 4＋1，最终完整五组复验）真实编译后目标正常断言红、同 APK 配置绿、恢复绿与 SHA256。探针输入源、停止会话旧断言、取消变异编译失败及虚拟时钟不足的失败均保留，不计通过。不增加 CI 20 份；未发行，不代表真实设备/输入法/全客户端验收。

**Android 文件连接复用与连接提示（§9.202）**：导航保留浏览器独立 SFTP owner，输入/目标匹配且 native ready 时复用；显式断开和 FilesViewModel 清理仍关闭，晚到认证仍受页面代次隔离。被动认证/打开改为同一页内进度，交互认证和错误保留弹窗。API 36 的初始 84 项组合有一条新增测试步骤失败，不记整轮绿；修正后 11 项及后续 9 项定向通过。`ANDROID_FILES_PRODUCT_MUTATIONS=reconnect-error,leave,stale-handoff,retain-navigation,reuse-terminal,clear-lifetime,progress-dialog` 七组同轮目标正常断言红、同 APK 配置绿、恢复绿及 SHA256；关闭保护迁到显式关闭/模型清理，另护导航不得关闭。Core 51/95、Debug/测试 APK、未签名 Release/R8 和四模块 lint 通过。手工模拟器证据，不增加 CI 20 份；未发行，非长时换网/真实设备/完整客户端验收。

**Android 会话入口与分组面板（§9.207）**：辅助键行固定会话按钮，隐藏辅助键仍保留入口，菜单去重；按主机分组、稳定编号、当前 UUID 高亮、进程内上一会话、停止项折叠/搜索及固定新建动作已接。少量会话收紧高度，大字号纵排；关闭确认捕获目标 UUID，切换/取消恢复此前实际 IME 状态。`mutation-check-android-sessions.py` 的二十个不同组为分轮手工证据（当时通用默认十九组，API 29 专属 `--case keyboard-platform-state` 另跑），另有布局五组、session-management 两组及 terminal T10 复验：正常单条断言红、同 APK Core 配置绿、恢复绿与 SHA256。未杀死的旧短窗口展开/纯墙钟负向键盘判据、测试重复构造/编译与 PID 字段误用、API 29 旧 Compose 快照及文件往返 IME 同步失败均保留且未计绿。最终 API 29 / 36 同源组合各 85 / 93 项，680 份指定输入及安装 APK 摘要一致；Core 52/95、Debug/测试 APK、未签名 Release/R8 与四模块 lint 通过。手工模拟器 / 4 KiB 页证据，不增加 macOS CI 二十份；未发行，非真机/厂商输入法/完整客户端验收。

**Android 会话切换保持输入法（§9.208）**：面板改为同窗口覆盖层，复用原生终端输入视图并按 UUID 重绑 Surface，旧输入上下文/本地选区模式/修饰键隔离保留；搜索移除前直接交还焦点，关闭回调防重，API 29 前置返回与 API 33+ overlay 返回回调仅在面板期间接管。打开/取消逐帧验实际 IME、窗口焦点与高度，切换/搜索允许候选栏高度变化但 IME 不消失。十四个不同会话保护分轮（新增十一＋迁移三）与既有布局/菜单四组通过：编译后正常断言红、同 APK Core 配置绿、恢复绿和源码 SHA256；未杀死的冗余选区清除探针与 nullable 编译失败未计红。API 29 / 36 最终同源组合 87 / 95 项、680 份输入及安装 APK 摘要一致；Core 52/95、Debug/测试 APK、未签名 Release/R8 和四模块 lint 通过。真实截图见 `docs/design/android-session-ime/index.html`。手工 arm64 / 4 KiB 页模拟器证据，不增加 CI 二十份；未发行，非真机/厂商输入法/完整客户端验收。

**Android Nerd 图标单元格裁切（§9.209）**：私用区字形按浮点轮廓等比适配原格子并保留栅格边缘，普通层与块状光标共用；字体/字号/行高变化清缓存。`mutation-check-android-fonts.py` 六组分轮（5＋1）正常断言红、同 APK 配置绿、恢复绿及 SHA256 通过。专用 API 36 / arm64 / 4 KiB 页模拟器的本地 53 项与 SSH 图形 1 项分轮通过；Debug/测试 APK、未签名 Release/R8、四模块 lint、C 格式检查及隔离检出 Core 470/169 通过。最初边缘像素失败、未配置 SSH 夹具的 54 项组合失败及原工作区并行插件摘要失败均单列，不计绿。手工证据，不增加 CI 二十份；未发行，非用户真机或完整字体/客户端矩阵验收。

**远端统一入口（2026-11-06）**：远端主机面板自动探测/列举 tmux，普通终端可直接打开；创建/attach 仍显式选择，直达偏好按端点隔离且可撤销。macOS 24 项与 Release 构建、Android 专用 API 29 arm64 / 4 KiB 页 8＋38 项分轮、未签名 Release/R8 与四模块 lint 通过；Swift 新四组、默认入口旧四组、Android 新三＋旧五组均正常断言红、独立配置绿、恢复绿与 SHA256。C 格式 208 文件、隔离 HEAD Core 470/169 通过；原工作区并行插件 SDK 摘要失败单列，不记整仓绿。不增加 CI 二十份脚本，未发行，非 API 36/真机/完整客户端验收；原件与前置失败见 `docs/reference/optional-tmux-implementation.md` 及 Android 实施计划 §9.211。

**Android Unicode 符号与 emoji 裁切（§9.212）**：箭头/分支/警告等普通符号与彩色组合 emoji 共用完整字形适配，补位图边界与 API 29 斜体剪切范围；保留 VT 一/两列分配、正文基线及框线接缝。API 29 字体 8 项、API 36 字体/本地终端/本地 Kitty/字体切换 45 项通过，两设备 APK 与构建产物摘要一致。字体变异原六＋新三组在 API 36、专属 `--case 10` 在 API 29，共十组分轮正常断言红/同 APK 配置绿/恢复绿/SHA256。Debug/测试 APK、未签名 Release/R8、四模块 lint、C 格式 208 文件及 ABI 169 通过；原工作区 Core 469/470 的既有插件 SDK 摘要失败单列，未记整仓绿。旧实现裁切、遗漏标点、PTY 列宽假设与 API 29 斜体/测试网格异常保留。手工 arm64 / 4 KiB 页模拟器证据，不增加 CI 二十份；未发行，非真机/厂商字体或完整客户端验收。

### C ABI 符号表（改 `core/include/ghosterm.h` 或 `core/src/*` 时另加）

`ghosterm.h` 是**单一事实来源**。新增/删除/改签名后必须：

1. 同步实现与各端绑定；
2. 跑 `scripts/check-abi.sh`：`nm` 静态库实际导出名 ↔ 头文件声明，**双向对拍**（漏导出与多泄漏都拦）。

> **当前覆盖范围（诚实标注）**：`check-abi.sh` 对拍「`core/` 内部实现 ↔ 头文件」；
> `check-abi-apple.sh` 另对拍 Swift 的引用名集合；`check-abi-android.py` 对拍 Android JNI 引用。
> A1 验收时为 Apple 71/2、JNI 11/62（引用/显式登记未用）；A2 本地终端首片 JNI 为 19/54，整个 A2 阶段尚未完成。连接检查接口加入后公共符号为 74 项，Apple 72/2；JNI 配置检查切片 20/54，SSH 后端切片为 32/42，身份容器首片加入后为 33/41；IdentityFile 来源查询切片为 75 个公共符号、Apple 73/2、JNI 34/41；主机信任记录快照切片为 79 个公共符号、Apple 77/2、JNI 38/41；共享信任写入与移除绑定为 80 个公共符号、Apple 78/2、JNI 39/41；已验通道取消切片为 81 个公共符号、Apple 79/2、JNI 39/42（新入口替换旧 shell 引用）；受控 SFTP 切片为 82 个公共符号、Apple 80/2、JNI 40/42（§9.47），只有绑定/helper，无文件产品接线。§9.45 Android 依赖补丁已验六场景及完整 214 项；§9.47 主机补丁依赖已接默认构建，17 项协议、十组接口变异、十一组依赖守卫与既有打包变异通过，详见 `docs/reference/host-ssh2-build.md`。§9.56 为 87 个公共符号、Apple 85/2、JNI 47/40，专项和最终完整 Android 260 项通过；首轮失败及测试同步修正仍保留记录。§9.57 为 88 个公共符号、Apple 86/2、JNI 48/40，远端非覆盖发布与绑定专项及最终完整 Android 266 项通过（API 36 arm64 / 4 KB 页模拟器）。§9.65 新增受控远端单目录创建，为 89 个公共符号、Apple 87/2、JNI 49/40；本片为专项/变异与原生构建证据，不沿用旧完整设备结果。实际数量以当前检出的脚本输出为准；符号对拍不替代平台行为验收。
> 各次接口与平台验收分别记在 `docs/reference/roadmap.md`，不得借旧结果记新接口通过。
> Android 已验收组件/配置绑定；A2 首片已接本地 Transport、Canvas/InputConnection 与会话生命周期，整个终端阶段未完成。连接配置检查已接两端；Android SSH native owner/认证桥及远端终端后端已接，产品 DNS/认证页面与 Keystore 容器首片已接（实现计划 §9.27），整个 A3 仍未完成；SFTP 文件专用 owner/JNI/Kotlin 后端已由 §9.51 接入，远端只读页面已由 §9.52 接入，§9.69 已接复制执行器数据层，§9.70 已接任务认证与粘贴页面；§9.71 已接进程共享任务、复制清单和认证，§9.72 已接 FIFO 队列，§9.73 已验持久观察记录与只读历史，§9.74 已验前台服务/通知的 API 36 模拟器档；§9.77 已验当前文件阶段进度；§9.98 已接整体数据 ETA、协作暂停/继续与当前进程内安全重试（API 29/36 专项与分轮变异）；持久历史位置/内容核对、显式重新发起、本机创建身份、残留清理与归档已按 §9.104–§9.124 分片接入；完整客户端及外部端点/真实设备矩阵仍待完成；NAPI 尚未实现。
> §9.98 切片为 93 个公共符号、Apple 89/4、JNI 49/44；新增可暂停收发由 Android 专属文件 owner 消费。Core/Swift 输入摘要与通过记录一致；新增十三组与受影响旧七组均为手工证据，不增加 CI 20 份。Android Kitty 图形/动画与 Nerd Fonts 已接并发行，设备与真实签名包范围见 §9.100–§9.102；不代表整个 A2 完成。
> §9.103 新接 Android 远端 tmux 管理/独立 attach，ABI 仍为 93，Apple 89/4、JNI 55/38；API 29/36 各 45 项组合与十二组 Android 变异通过，云端签名包的 16 KB 页真实 SSH/tmux 操作范围见 §9.103，不增加上述 macOS CI 20 份。
> 新增平台绑定必须把引用名集合纳入对拍；签名兼容仍须各端真实编译验证。

`libssh2-sftp` 用 `python3 scripts/test-libssh2-sftp.py --fetch --mutate` 验锁定依赖的九场景、七组重新编译的 native 变异及两组隔离收据变异，目标红、独立对照绿、恢复绿与 SHA256；`--android 设备` 用实际 NDK archive 验同九场景。Android 与主机默认构建均应用补丁；`mutation-check-sftp-open.py` 另验 7 C + 2 Swift + 1 无库组，`mutation-check-host-ssh2.py --app 本轮.app` 验十一组依赖/包/CMake 守卫，目标红、独立对照绿、恢复绿与 SHA256。已交付证据见 Android 实现计划 §9.45/§9.47/§9.49，范围见 `docs/reference/host-ssh2-build.md`。手工证据，不增加 CI 20 份，此处仅登记范围。

`sftp-progress` 以 `python3 scripts/mutation-check-sftp-progress.py` 手工验 13 组隔离 Core 变异，`mutation-check-sftp-progress-bindings.py` 另验 2 组真实无库 archive 与 4 组 Swift 转换。Android 以 `ANDROID_SERIAL=设备 ANDROID_FILES_PROGRESS_MUTATE=1 bash scripts/test-android-ssh.sh` 验 4 组；每组须真实编译后目标断言红、同变异独立配置对照绿、恢复绿与 SHA256。当前为 91 公共符号、Apple 89/2、JNI 51/40；§9.76 接口验收只证明每次收发的数字观察及绑定。§9.77 已接 Android 执行器和任务页，以 `ANDROID_COPY_PROGRESS_MUTATE=1 bash scripts/test-android-ssh.sh` 手工验 25 组实际字节、读取/写入/核验、部分读写、定时补发、阶段平均/剩余时间与页面；目标红、同 APK 独立配置绿、恢复绿及 SHA256。相关旧变异累计 114 组分轮通过，完整 491 项通过，仅 API 36 arm64 / 4 KB 页模拟器；不增加 macOS CI 20 份覆盖，不代表整体 A6 或发行矩阵完成。

`sftp-mkdir` 以 `python3 scripts/mutation-check-sftp-mkdir.py` 手工执行 26 Core＋1 无库 archive＋5 Swift 组；真实 SSH 与协议矩阵为 81 组，护单次预算、取消、规范路径/名称、冲突/错误/副作用、空目录核验与连接失效。Android 另以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteDirectoryTest ANDROID_MKDIR_MUTATE=1 bash scripts/test-android-ssh.sh` 执行 20 组，护结果保留、一次性请求、排队/迟到取消、请求与连接代次隔离、失败连接释放及完成管道。目标须真实编译后红、独立配置对照绿、恢复绿与 SHA256；12 项设备基线通过，仅 API 36 arm64 / 4 KB 页模拟器。范围及失败记录见 Android 计划 §9.65，不增加 CI 20 份，不将原语记为任务/页面或 macOS 产品迁移完成。

`terminal-backing-scale` 以 `python3 scripts/mutation-check-terminal-backing-scale.py` 手工执行六组，护引擎 DPI、Metal 比例、backing 回调、换屏通知、像素换算与未上屏同步；真实编译后目标红、同变异字节入网格对照绿、恢复绿及 SHA256。`4cbfcf4` 的三项原生回归与六组变异已有记录，自动注入比例不等于真实双屏视觉验收；完整宿主仍保留一项既有设置页像素失败，不记全绿。范围见 `docs/reference/optional-tmux-implementation.md` 的跨显示器缩放记录，不增加 CI 20 份覆盖。

**插件桥接 P0 首批隔离探针（2026-11-05）**：`python3 scripts/test-plugin-bridge.py --mutate --tmux`
在 `scripts/probes/plugin-carrier.c` 验 C11 / ASan / UBSan 八类检查与六组重新编译变异，
正常目标断言红、同构件独立完整帧绿、恢复绿与 SHA256；仅隔离副本，不接生产 ABI / Transport / App。
tmux 3.6a 两个真实附着 PTY 客户端验证候选输出完整，同时复现另一客户端切 pane 后回包进入错误 pane。
该结果禁止据普通 passthrough 的输出连通性宣称双向输入安全；后续引擎与 SSH 探针分批登记，不倒填首批范围。
实验载荷 / 编码 / 前缀均未冻结，不表示插件能力已实现；范围与首轮失败见
`docs/reference/plan-plugin-bridge.md` 的 P0 第一批记录。手工证据，不增加 macOS CI 二十份。

**插件 JS 双候选 P0 第二批（2026-11-05）**：`python3 scripts/test-plugin-js.py --fetch --mutate --android-build`
显式下载 / 校验归档，`--android 序号` 才记设备执行；源码为 `scripts/probes/plugin-js.c`，不接生产依赖。
QuickJS 2026-06-04 / QuickJS-NG 0.18.0 的 macOS arm64 各十一项与各四组重新编译变异通过，
正常目标断言红、同构件 portable 对照绿、恢复绿与 SHA256；两套 NDK 28.2 / API 29 arm64
构件在专属 API 29 / 4 KiB 页模拟器各十一项通过。主机用 ASan / UBSan 并拒绝恢复后假绿。
宿主实验均 C11 / Werror；QuickJS 上游 arm64 需 GNU11，不能记为纯 C11；NG 上游原样 C11 通过。
本轮首选候选收敛至 NG，尚未新增 vendor gitlink、冻结 SDK 或接 App；编译失败与行为边界见计划
P0 第二批。仅主机变异 / 原生模拟器专项，不是 Android 变异或客户端全量验收，不增加 CI 二十份。

**插件嵌套 SSH P0 第三批（2026-11-05）**：`python3 scripts/test-plugin-ssh.py` 在同一主机的两套
隔离 loopback sshd / 不同钉公钥别名上跑三条 SSH / tmux 链路，768 字节载荷由同一 C decoder
验完整；真实第二客户端切 pane / window、detach / attach、后台输出及外层 pane 负向均取证。
原 TTY 写回仍不安全；显式认证的独立 SSH exec 管道保持接收者，关闭后拒发、已收请求丢回包记 unknown。
一次性测试私钥只经内存管道交给专属系统 agent，落盘仅公钥；无用户凭据、无自动重连或发现。
`python3 scripts/test-plugin-ssh-cleanup.py` 护中断不得被 PTY / pipe IO 吞掉，并在 live SSH 进程树上
验 SIGINT / SIGTERM 退出与清理；修正后第一批六组变异以不可恢复 sanitizer 复验通过。
源码、失败和当前摘要见计划 P0 第三批；无生产 ABI / App 接线，不增加 CI 二十份，不代替 Android TTY / WAN 验收。

**插件无 Client / schema P0 第四批（2026-11-05）**：`python3 scripts/test-plugin-no-client.py --android 序号`
以同一十一份字节夹具验真实 macOS full surface 与 API 29 / arm64 / 4 KiB 页 VT，DSR 作处理完成屏障，
完整候选帧静默、Kitty 查询回包正常；截断 DCS 吞后续字节直至 ST 的限制单独记账。无 Android UI / 渲染或完整 App 结论。
`packages/plugin-sdk/schema/` 仅放未冻结的 JSON Schema 与样例；`test-plugin-schema.py --fetch --mutate`
显式取锁定开发工具，默认离线，五十七份结构样例与四组 schema 变异通过，不代替 C / channel 状态机验证。
`test-plugin-protocols.py --fetch` 取锁定 trzsz-go 1.2.0 作上游比较：十五项及两档各 256 KiB 真实 PTY 文件互通通过，
不表示 Ghosterm 兼容 trzsz / Kitty 文件协议。首次继承宿主 tmux 的失败与无法证明原选项继承完全恢复的限制保留；
最终清除环境并以阻断器禁止访问宿主 tmux 后重跑通过。全部为手工探针，不增加 CI 二十份；详见计划 P0 第四批。

**插件包元数据 P0 第五批（2026-11-05）**：`python3 scripts/test-plugin-package.py --fetch --mutate`
首次显式取锁定开发依赖，后续默认离线。manifest / integrity 候选与 testkit 元数据检查器有 96 项具名检查、
六组模块重新加载后的变异：正常目标断言红、同模块 Client 兼容性对照绿、恢复绿及源字节一致。
只比较调用方提供的成员统计 / 摘要和能力事实，返回兼容不授予权限；不是实际解包、hash 读取、无链接打开、
签名、安装或插件激活证据。`process.pipes` 可用性与 `process.spawn` 的程序权限分开，计划原样示例纳入两侧检查。
包结构与本地版本 / 命令预检不替代仍待选择的授权启动流程。失败、摘要与合成输入范围见计划 P0 第五批；
无新增 Core ABI / 平台接线，不增加 macOS CI 二十份。

**插件 TailPocket 参考 P0 第六批（2026-11-05）**：`python3 scripts/test-plugin-tailpocket.py --fetch`
显式取锁定上游 / Go 工具链，缓存就绪后默认离线构建；上游 Go 仅作独立参考进程，正式底层仍须 C，
不接 gomobile / SDK / App helper。C11 两端在同机两套 loopback SSH / tmux 与本机 DERP / STUN 上完成
配对、四轮双向候选消息、未配对公钥、第二客户端切 pane / window、外层切 pane、执行端退出 23 后
EOF / unknown / 晚到拒发、撤销前网络仍可达的独立对照，以及撤销后的旧 Link 新客户端不可达，共九项观察。
固定源归档 / 模块锁 / `go mod verify`，私钥与邀请只在内存中；C 用 ASan / UBSan，macOS 未运行 LSan。
编译、工具链、启动和本机探测超时的初轮失败保留，不计通过；无 C 版 Tailcat / 网络底层、GUI 批准、
移动设备或 WAN 结论。正常结束清理专属进程与 socket，详见插件计划 P0 第六批；手工证据，不增加 CI 二十份。

**插件 Go→C ABI P0 第七批研究（2026-11-05）**：`python3 scripts/test-plugin-go-abi.py --apple --android`
仅隔离研究库与 C caller，不修改纯 C Core 口径或公共 ABI，不接入 App。macOS arm64 的 C main 调用同进程 Go
archive，实跑真实配对 / 本机 DERP 往返、缓冲区与失效句柄、C 线程取消 / 活动关闭及无网络 owner 清理；
iOS device / simulator arm64 为 archive / C 链接证据，Android API 29 / NDK 28.2 为 shared / C 链接与四个
LOAD 的 16 KiB 对齐证据，均未记移动端运行。Android c-archive 实际不支持。研究库含测试中继，包体 / RSS
不外推产品增量；Go runtime 在首次 ABI 前已启动，关闭实例不卸载 runtime。首轮把构建 telemetry 文件误判为
运行 HOME 文件而失败，拆分两种 HOME 后完整入口通过；不计首轮绿。源摘要、样本与封闭内网 SSH 优先建议见
插件计划 P0 第七批；不增加 CI 二十份，不替代信号 / PTY / 原生宿主 / 设备 / 发行验收。

**插件 SSH 优先接缝 P0 第八批研究（2026-11-05）**：用户已选独立 SSH 优先、预留网关 provider。
仅静态审计与文档设计：当前 exec 无 stdin write、terminal command 会申请 PTY、read 0 / 默认退出码 0
不能升级成可靠结束事实。计划收敛 Route owner / 私有 byte stream / 共用 BridgeSession；首片专属 SSH owner，
后续多跳 / 前台 gterm IPC 附着 / 网关分片分别验收，不用互斥锁共用终端 SSH 或自动换路假装已隔离。
具体契约、源码依据和 S1–S5 次序见插件计划第八批；未改生产 Core / ABI / App，未新增运行或 CI 证据。

**插件 S1 私有字节流 / 分帧第九批（2026-11-05）**：`core/src/extension/` 新增 C11 stream / fd-stream /
frame，已纳入 Core 构建，无公共 ABI / 平台接线。真实 pipe / socketpair 共用 conformance，九模式护
部分 IO、背压 WAIT、尾字节 EOF、停止唤醒 / 拒发、EINTR 截止、分帧 / 截断 / 上限与精确 allocator。
`python3 scripts/mutation-check-plugin-stream.py` 在本轮隔离副本用 ASan / UBSan 真编译十三组：
目标正常 FAIL 断言红、同库独立 ssh_config 对照绿、恢复九模式绿及 SHA256；gt_timeout / 标记预扫，
不修改生产源，未运行 LSan。三份新 C 源码与测试另以严格 C11 / pedantic-errors 编译并跑九模式通过，
`build-core.sh` / `gate.sh` 通过，当前 Core 61 CTest / 95 ABI。变异为手工证据，不增加 CI 二十份。
codec 仅长度 / 帧边界，不是 JSON / schema / 权限 / 业务结果验证；未新增 SSH / App / Android / WAN
证据，不记 P0 / P1 完成。关闭后的 owner / wake 资源计费至 destroy；后续专属 SSH、多跳、会话与前台附着
仍按插件计划 S2–S5 推进，网关不在当前实现范围。

**插件 S2a 直连 SSH 第十批（2026-11-05）**：私有 `extension/backends/ssh_exec.h/.c` 复用认证 connector，
claim 拒绝已有 channel / borrow 的连接，被认领的连接拒绝普通 exec / shell / SFTP；不申请 PTY。
固定 16 KiB 收发、分离 / 有界 stderr、finite budget / wake / 分阶段失败、未知退出不猜 0，close 先 abort
专属 socket 再按真实 libssh2 释放结果计费。`test-plugin-ssh-stream.py` 的 OpenSSH 十项与 Paramiko 六项
共十六场景通过；reset 以客户端接收确认作屏障，初轮缺屏障的恢复失败与测试编译错误不计通过。
`mutation-check-plugin-ssh-stream.py` 十四组同轮 ASan / UBSan 编译后正常 C 断言红、同库配置绿、恢复全套
十六场景绿与 SHA256；未运行 LSan，不宣称外部库全部无泄漏。Core 62 CTest / 95 ABI、Apple 91/4、JNI
57/38、严格 C11 新源 / 测试编译和实际无 SSH archive / stub 链接通过。CTest / macOS CI 工具链准备已接，
只有本地证据，十四组变异不增加 CI 二十份。旧 exec helper 独立负向 / 修复及更完整取消 / 重协商矩阵仍待完成，
不记整个 S2 / P0 / P1 完成。无公共 ABI / App / Android / 多跳 / WAN 证据，网关继续暂缓；详见插件计划第十批。

**插件 S2b 旧 exec 事实第十一批（2026-11-05）**：先在真实协议端复现未知→0及查询关闭活动通道，
已知 0/23 对照绿后修复。`0004-channel-exit-status.patch` 新增 C 内依赖 getter 与存在位，首个完整状态
生效，截断/重复/超范围不冒充 0；主机/Android 四补丁清单和 SHA 收据同步并实际重建。Core getter
被动查询、未知输出保持；read 非阻塞，不把 0 记 EOF，后续读取可推进迟到状态，恢复会话 IO 设置。
`bridge_exec_e2e` 十个真实协议场景与 `ssh_exec_boundary` 加入 CTest；`mutation-check-exec-state.py`
十六组（Core 十＋依赖五＋实际无库一）ASan/UBSan 正常 C 断言红、同库配置绿、恢复绿及 SHA256；未跑 LSan。
原 exec 三组及 SSH 后端十四组分轮复验通过。原 0/3 断言保留并按期限推进迟到状态；stderr 驱动超时曾被拒绝
计红，补总期限后正常断言红；缓存/Zig路径和 CLT XCTest 失败也不计通过。最终 Core 64/95、Apple 91/4、
JNI 57/38，严格 C11 新源/边界编译、SFTP 九场景、Android NDK 四补丁/实际 bridge Debug链接、局部 Xcode
下 Apple 注入 sshd 的同轮 34 项零失败/跳过通过。Swift 仅同步查询语义文案，无公共 ABI 签名变化。
新 BridgeStream 可选进程诊断仍保持未知；无 Android 设备/整 App/多跳/WAN/发行结论，不增加 CI 二十份。
S3/S4/S5及完整取消/重协商/长时组合仍继续，P0/P1/插件计划未完成，网关不实现；详见插件计划第十一批。

**插件 S3 受管多跳第十二批（2026-11-05）**：私有 `extension/route.h/.c` / `ssh/wire.h` 用单 owner
和 libssh2 回调逐跳 direct-tcpip，最终无 PTY exec；逐跳身份 / 认证、总期限 / wake / DRAINING，
不共享终端 SSH、不猜手工链路、不直拨内层或自动回退。八台为配置上限，本轮仅两 / 三台 loopback。
`bridge_route_e2e` 十八场景在不同内存 hostkey、跳板禁 shell / 限下一跳转发下通过，客户端实际 connect
仅准第一跳且逐项断言一次；未对远端设 WAN 防火墙，不记真实封闭内网。内层错 pin / 密码、未知决定 /
补凭据、第一 / 中间跳拒绝、实际内层 banner 屏障后的半开取消 / 总期限和读取消、allocator / fd 归零均验。
`mutation-check-plugin-route.py` 十三组 ASan / UBSan 真编译后正常 C 断言红、同库配置绿、恢复十八场景绿
及 SHA256；原直连十四组另完整复验，未跑 LSan。Core 65/95、Apple 91/4、JNI 57/38、严格 C11 四源编译、
实际无库 route / ssh stub、Apple 注入 sshd 同轮 34 项零失败 / 跳过、Android NDK bridge Debug 实际链接通过。
无公共 ABI / App 接线、Android 设备多跳 / 八层 / WAN / 发行结论；十三组手工，不增加 CI 二十份。
P0/P1/整个插件目标未完成，S4 共用会话 / 原生授权与 S5 前台 IPC 继续，网关不实现；详见插件计划第十二批。

**插件 S4a wire 第十三批（2026-11-05）**：私有 `extension/wire.h/.c` 实现有界原始 JSON / UTF-8，
`generate-plugin-wire.py` 从 SDK 候选生成 `wire-schema.inc` 并以 CTest `--check` 对拍。重复 decoded key /
非法编码、surrogate、污染、精确数值与 64 KiB / 32 层 / 4096 node 策略单独检查；不是通用 schema 引擎。
固定 workspace、parse 无分配、失败清空输出 / 撤销 view、释放擦除。`bridge_wire` 的 575 项通过，其中
246 个实例与锁定 Ajv 独立对拍；329 项 lexical / owner 策略不冒充 Ajv 证据。十六组隔离 ASan / UBSan
编译后正常 C 断言红、同库配置绿、恢复完整 575 项 / 生成物绿及 SHA256；首轮 unused-variable 编译失败
未计红，修正变异后完整十六组同轮通过，未跑 LSan。原 schema 57 项 / 四组 JS 变异另复验通过。
Core 67/95、严格 C11 新源 / 测试、实际无 SSH archive 同套 wire、Android NDK 新 object 编译和 bridge Debug
构建通过；wire 没有 JNI 消费者，不将 object 符号当页面接线。无公共 ABI / App / 新 Swift 绑定、Android
设备 / 真实 SSH wire 会话 / WAN / 发行结论；十六组手工，不增加 CI 二十份。schema 只改消费者说明，
协议仍是候选。握手 / nonce / 实例归属 / 连续 seq / 授权 / 请求 / 资源发布与 S5 继续，P0/P1/S4/总目标
未完成；外部网关不实现，详见插件计划第十三批。

**插件 S4b 会话内核第十四批（2026-11-05）**：私有 `extension/session.h/.c` 在单一已选实例上复用
stream / frame / wire，HELLO / OFFER → native ticket 决定 → nonce PROBE / ACCEPT → 双方 READY，
才发放本端 scoped lease；UNKNOWN / DENIED / REVOKED / UNSUPPORTED 不折叠，control seq / ID / epoch
绑定，原生 PeerEvidence 不受远端覆盖。用户等待另计期限，IO 后须重检；实际 read / write 越期激活及批准后
过期 effect none 曾以正常断言复现、同库配置绿，修复后恢复绿。27 socket / 4 真实两三台 SSH 场景通过，
后者仅准 native 直拨第一跳，不是远端 WAN 防火墙 / 真实内网证据。21 组隔离 ASan / UBSan 编译后正常 C
断言红、配置绿、恢复完整 27＋4 绿与 SHA256；旧 wire 十六组另完整复验，未跑 LSan。wire 候选随控制消息
更新为 585 C / 256 Ajv 实例及 329 lexical / owner；schema 67 / 四组、包元数据 96 / 六组另复验通过。
Core 95 CTest / 95 ABI、严格 C11 三源、实际无库 27 Session＋wire / 生成物共 29 项、Android NDK session
object / bridge Debug 构建通过，Apple 91/4、JNI 57/38 对拍保持；无新 JNI /
Swift 消费者、原生批准页面或设备 / WAN / 发行证据。没有实际插件激活，包摘要仅匹配 caller 的已核验快照，
不冒充本片文件核验。21 组手工，不增加 CI 二十份；S4c 请求 / 多流 credit / ResourceStore、原生 ABI / 页面
与 S5 仍继续，P0/P1/S4/整个目标未完成，网关不实现；证据与边界见插件计划第十四批。

**插件 S4c1 请求第十五批（2026-11-05）**：私有 `extension/rpc.h/.c` 接实际 Session，显式 requests
协商、最多 16 method 的 receiver / capability / 双向校验，所有原生请求操作核对 scoped lease。64 项
outgoing / incoming-receipt、双向独立 request / response credit / seq、control 保留；wire ID 在有窗口且
交 writer 时分配，缓存外旧 ID 不执行。排队取消可 none，可能已发出的取消 / 断流保留 UNKNOWN，完整确认
不可改写；相对期限不冒充远端硬取消。31 socket / 两三跳四 SSH 场景通过，允许项各 80 echo 后撤销；
23 新请求＋21 Session＋16 wire 组分套最终复验，真实编译后正常 C 断言红、同 archive 配置绿、恢复完整
套件绿及 SHA256，ASan / UBSan，无 LSan。初始变异编译失败和一次恢复测试对象未重编导致的断言红均未
计通过；三个 runner 每轮删除隔离 Core / test 对象并核对实际编译。Core 127/95、实际无库 60 项、严格
C11 三源与 NDK bridge Debug 通过，rpc object 15 私有定义，无 JNI caller。wire 607 C / 260 Ajv /
347 lexical-owner、schema 71 / 四组、元数据 96 / 六组通过；Apple 91/4、JNI 57/38 对拍保持。无公共 ABI /
UI / 实际插件 / 设备 / WAN / 发行结论，手工变异不增加 CI 二十份。S4c2 ResourceStore、原生门面 / 页面、
runtime / SDK、多实例和 S5 继续；整个目标未完成，网关不实现，证据与限制见插件计划第十五批。

**插件 S4c2a ResourceStore 第十六批（2026-11-05）**：私有 `resource.h/.c` 使用真实私有目录 / 匿名
普通文件，固定 workspace、完整已知预约 / unknown 全额度预约、实例与连接共享预算 / 数量，单实例
generation 高水位、native lease / ref token、逐 offset 部分写和 SHA-256 全量核验后关闭 writer 才发布。
未完成资源拒读，已发布快照引用和 seal 后读取继续计费；未完成清理保留占用，替换 inode / symlink 拒删。
目录 ACL / 挂载策略由可信 native caller 提供，Core 核对 mode / uid，不把 mode 当完整目录证明。22 native
mode、68 独立 hashlib 用例及 32 组隔离 ASan / UBSan 通过：正常编译后 C 断言红、同 archive 配置绿、
恢复全套 / SHA256，未跑 LSan。真实 256 KiB 分块逐字节及 begin 后子进程退出通过，不外推任意 syscall
中途崩溃。初次 Darwin feature 宏缺声明的编译失败未计红，修复保留 NOFOLLOW。Core 150/95、严格 C11
三源、无 SSH archive 的资源 23 项与 NDK bridge Debug 通过；actual object 13 ResourceStore / 3 SHA 函数，
无 JNI caller / UI / 设备 / WAN / 发行证据。32 组手工不增加 CI 二十份；Session 尚未派发 resource，源文件
范围检查 / resource credit / 两三跳组合、原生 ABI / view / SDK / runtime / S5 继续，目标未完成、网关不实现。
完整收据、清理 / destroy 语义与后续切片见插件计划第十六批。

**插件 S4c2b 资源消息第十七批（2026-11-05）**：私有 `transfer.h/.c` 接 Session / RPC / Store，精确
resources feature 与 native lease / resource.receive，串行资源流独立 credit / seq，写完块才归还窗口、
核验发布后才 resource.result；取消 / 回执走保留 control，RPC / resource 同档轮换。47 native 和真实两 /
三跳允许 / 拒绝四 SSH 场景通过；允许项双向各 256 KiB、逐字节 / 回执和 80 确认 RPC，native 只直拨首跳。
已发无回执 UNKNOWN、未发取消、缓存旧 generation 不再发布、冲突不改确认结果；Store retire 让已交接
snapshot 在 Session 销毁后继续计费，最后 ref / 清理成功才解绑。两份快照释放提前销毁、last-close IO
无法重试先以真实 close 后 EIO 边界复现正常断言红、同 archive 配置绿，修复后恢复；清理失败仍保留
owner / 未处理引用和可重试 scope。新 36 组与旧 Session 21 / RPC 23 / Store 32 / wire 16 分套完整复验：
编译后正常 C 断言红、配置绿、恢复全套 / SHA256，ASan / UBSan，无 LSan。Core 198/95、无 SSH 依赖
130 项、严格 C11 六源、NDK bridge Debug 和 Apple 91/4 / JNI 57/38 对拍通过；wire 618 C / 271 Ajv /
347 lexical-owner，schema 82/四组、元数据 96/六组通过。原 size / counter / 错误码、缺测试头 / 旧锚点及
中断轮未计绿；两 runner 改 KeyboardInterrupt，独立真实 SIGINT / selector 探针通过，不声称任意中断
完整清理。36 组手工，不增加 CI 二十份；无源文件 scoped reader、公开 ABI、App / SDK / runtime / IPC
消费者或设备 / WAN / 发行结论，目标未完成、网关不实现；完整收据 / 限制见插件计划第十七批。

**插件 S4c2c 源文件第十八批（2026-11-06）**：私有 `source.h/.c` 接显式授权根 fd、native lease 的
file.readScoped / resource.receive，逐层 NOFOLLOW / CLOEXEC / NONBLOCK / 只读 open，普通文件和祖先
身份核对；同 fd 两遍有界 pread / SHA256 / 实际 EOF，遵守 Transfer 背压，发布回执才成功。64 native
场景和两 / 三跳允许 / 拒绝四 SSH 场景通过；允许项真实双向各 256 KiB 源文件 / 快照逐字节与摘要对拍、
80 确认 RPC，客户端只直拨首跳。文件变化、IO、取消、已发无回执 UNKNOWN 分开，关闭不覆盖原失败；
close EIO 不重试已消费 fd，待取消保留 owner。固定 workspace / 每步 16 KiB，不承诺磁盘硬期限、
原子文件系统快照、同 UID 沙箱或目录 ACL / 挂载策略；对象总数由可信 native caller 管理。Core 263/95、
实际无 SSH archive 的 64 native、严格 C11 两源、NDK bridge Debug 的实际 7 个 Source object 定义符号、
Apple 91/4 / JNI 57/38 对拍通过；无新 JNI / Swift caller。`mutation-check-plugin-source.py` 三十四组
ASan / UBSan 正常编译后 C 断言红、同 archive 配置绿、目标重编恢复绿 / SHA256，首尾完整 64 native /
四 SSH 通过，未跑 LSan；收据见插件计划第十八批。首轮 main 宏编译失败、四项不当边界判据及三轮
未杀死的 stat / read 后授权与排队失败变异不计绿，保留失败并补文件 IO 次数和精确结果状态断言。
手工证据，不增加 macOS CI 二十份；没有 App 根授权 / ResourceRef 视图、公共 ABI / SDK / runtime /
gterm IPC 或设备 / WAN / 发行结论，目标未完成；未来网关暂不实现。

**插件 S4d1 公共通道第十九批（2026-11-06）**：`ghosterm/extension.h` / `extension/channel.c` 新增十一项
公共 fd / SSH route / 逐跳 challenge / 有限 IO / 生命周期入口，既有 95 签名保留；当前 Core **106 ABI**、
Apple **102 引用 / 4 未用**、JNI **68 / 38**。平台仅从聚合头进入，Swift 非 UI queue 与 JNI 创建 worker
独占 IO；代次注册表 / stop pin 防旧 ID 和跨线程释放，决定三态分开。C 原生 16 KiB 稳定写缓冲按实际
部分接受推进；WAIT / stop / DRAINING / close IO 保留借用直到 provider 释放，实际已观察字节不因 stop
丢账，原失败与 cleanup_error 分开，实际 destroy 才归还 16-owner 预算。初始决定 UNKNOWN、旧 challenge
拒绝、错误凭据新 challenge 不重置期限。公共通道 ON/OFF 与无 libssh2 两档维持 106 导出；缺能力显式
UNSUPPORTED，不回退网关 / TTY。不是整个插件档 / OS 沙箱 / 插件权限或业务完成接口。
10 native / 8 边界与十个真实两三跳 C 场景、Swift 9、API 36 arm64 / 4 KiB 页 JNI 12 项通过；模拟器只
reverse 首跳和内存夹具，C native dial 边界另只准首跳。21 C / 3 Swift / 4 JNI 手工变异正常目标断言红、
独立同 archive / 同 APK 配置对照绿、重编恢复目标 / SHA256，首尾完整相关场景；Core ASan / UBSan，
未跑 LSan。282 CTest / 106 ABI、严格 C11 三源、macOS App native build、NDK Debug / 未签名 Release /
R8 和四模块 lint 通过。五项提前擦除 / IO 后取消漏账先正常断言红再修；初轮 JNI 原 lookup 判据、
NDK / disabled 缺条件编译、unused 参数和未杀死的额度变异均未计绿；补未进入分配器断言，原因未定
的历史 PASS 不外推解释。28 组不增加 macOS CI 二十份；未接公共 Session / ResourceRef、原生批准 /
视图、SDK / runtime / gterm IPC / 产品 ProxyJump 或真实设备 / 封闭内网 / WAN / 发行。目标未完成，
网关仍不实现；收据 / 生命周期 / pending 清理语义见插件计划第十九批。

**插件 S4d2 公开会话第二十批（2026-11-06）**：`extension_session.h` / `session-api.c` 新增十项
Session 握手 / ticket 决定 / READY 后 lease / access / 撤销 / 生命周期入口；当前 Core **116 ABI**、
Apple **112 / 4**、JNI **78 / 38**。只接 READY / 无 raw 读写 / 未停止通道，成功才消费旧句柄；
构造失败保留可用 owner，容量跨转换 / close 到 Session 实际 destroy 才归还。UNKNOWN 不等于批准，
票据 / 实例 / epoch 隔离，失败 end / error 与 cleanup_error 分开。销毁失败后重复 close 抹去错误曾以
正常断言红 / 同库配置绿复现，修复后恢复通过。Swift 原 queue / 生命周期锁与 JNI 同 slot / 新代次 /
stop pin、Kotlin 原 worker 已接；deinit 重新占满 Core 额度和 Android 两轮 16 Session 验实际释放。
14 native / 5 失败边界、真实两 / 三跳允许 / 拒绝四场景、Swift 8 与 API 36 arm64 / 4 KiB 页 Android
10 项通过。13 C / 2 Swift / 4 JNI 手工变异正常断言红、同 archive / APK 独立配置绿、重编恢复 /
SHA256，首尾完整相关套件；Core ASan / UBSan，无 LSan，不增加 macOS CI 二十份。冗余外层 guard
变异被内层检查拦住的初轮未计绿，提前 destroy 拒绝断言保留。302 CTest / 116 ABI、严格 C11 三源、
关闭 provider / 无 SSH 档、macOS App native build、NDK Debug / 未签名 Release / R8 与四模块 lint
通过；原通道 10 C SSH / Swift 9 / JNI 12 及 JNI 四变异另复验。features 非零明确 UNSUPPORTED；
公开请求 / 资源 / source、App 批准 / 视图、SDK / runtime / gterm IPC / 多实例继续，目标未完成。
仅隔离环回 / 模拟器，不借给物理设备 / WAN / 真实封闭内网 / 完整客户端 / 签名发行；网关不实现。
两份新变异 runner 的残留预扫实际入口均验返回 2 / 源不变，修正后整套重跑；详细所有权、失败历史、收据见插件计划第二十批。

**插件 S4d3 公开请求第二十一批（2026-11-06）**：`extension_rpc.h` 新增八项公共入口，当前 Core
**124 ABI**、Apple **120 / 4**、JNI **86 / 38**。显式 requests、复制方法表 / capability / receiver /
七种 JSON 根类型，共用严格 wire；不是完整业务 schema，执行前由后续 SDK/runtime 校验并 check ticket。
各操作核对 scoped lease，incoming / result 全量复制到 POD / Swift Data / Java 数组，失败清零；64-slot
背压、未发取消 effect none、已发无回执 UNKNOWN、已确认结果不随 stop 改写。JNI 先分配返回数组再消费
incoming，异常停止；原 worker / queue / stop pin 与容量生命周期保留。19 个本机场景、两三跳允许 / 拒绝
四 SSH、Swift 7、API 36 arm64 / 4 KiB 页 Android 8 项通过；允许链路双向各八十个确认请求，执行端等待
最终 done 确认才写本轮完成记录，runner 独立核对。只准 native 直拨首跳，不称真实封闭内网或 WAN。
初轮 receiver 变异因 archive 更新却未链接 executable 而 PASS，已拒收；十二份 C runner 强制删除相关
对象 / archive / 目标和配置对照，再核对实际编译及链接，三份 JNI 同样强制重链 shared library。
修正后新 **18 C / 3 Swift / 4 JNI** 与旧依赖 **236 C / 5 Swift / 8 JNI** 分套通过，共 **274 组**；
正常目标断言红、同 archive / APK 独立配置绿、恢复重链绿及 SHA256，Core ASan/UBSan、无 LSan。
十五份 runner 实际 main 的隔离残留预扫均退出 2 / 源不变 / 无子命令。以上均手工，不增加 CI 二十份。
322 CTest / 124 ABI、严格 C11 两源、关闭 provider / 无 SSH 变体、macOS App native build、Android
Debug / 未签名 Release / R8 和四模块 lint 通过。重复 respond 的错误预期、初次修复脚本缩进失败、被拒收
旧 binary 轮次不计通过；不倒填第十九批历史容量 PASS 的原因。公开资源 / source、App 批准 / 视图、
SDK / runtime / gterm IPC / 插件 / 多实例仍待接；无签名发行、物理设备或完整客户端结论，网关不实现。
完整链接取证、闭包范围及本地收据路径见插件计划第二十一批。

**插件 S4d4 公开资源第二十二批（2026-11-06）**：`extension_resource.h` / `resource-api.c` 新增十六项
公共 Store / 资源 Session / producer / 回执 / 快照入口，原 124 签名保留；当前 Core **140 ABI**、
Apple **136 / 4**、JNI **102 / 38**。精确 RESOURCES / 可选 REQUESTS；Store 借用从构造成功到实际
Session destroy 持续计费，快照 pin 跨 Session 保留，scope budget 在构造前核对。固定字段先查 NUL，
真实发布 / 回执才能确认；排队取消、已发无回执 UNKNOWN、确认后 stop 分开。Store WAIT 保留，目录
close 后 IO 消费 owner；最后 ref 真实 close 后 EIO 不重试数值 fd，全部 Session 释放后才收集退休空
scope。Swift queue / owning Snapshot / 原队列析构 ledger、JNI Store 代次表 / 原 worker、take 前预分配
及 Kotlin 解包失败的无分配返回包 discard 已接；无 source 文件访问 / App 根授权或资源视图。
22 本机＋3 边界、两三跳允许 / 拒绝四 SSH、Swift 7、API 36 arm64 / 4 KiB 页 Android 7 项通过；允许
链路双向各 256 KiB 与 80 确认 RPC，执行端独立资源 / RPC 完成记录各六份，数量和内容严格核对。
新 **26 C / 4 Swift / 6 JNI**、旧 Store 32 / Transfer 36 C 与公开 Session 13 C / 2 Swift、RPC 18 C /
3 Swift 分套复验通过，共 **125 C / 9 Swift / 6 JNI = 140 组手工**，不增加 macOS CI 二十份。
实际编译 / archive / executable / JNI shared 重链后正常目标断言红、独立同库 / APK 配置绿、恢复 /
SHA256；Core ASan/UBSan，无 LSan。两份新 runner 的实际 main 隔离预扫退出 2 / 源不变 / 无子命令。
348 CTest / 140 ABI、严格 C11 五源、关闭 provider / 无 SSH、强制实际链接的 macOS App、Android
Debug / 测试 APK / 未签名 Release / R8 和四模块 lint 通过。metadata-boundary 先正常断言红 / 配置绿
再修复；错误额度和 finish 预期、fixture 编译失败、未触及状态映射的变异 PASS、清理覆盖断言、JUnit
ComparisonFailure / 数组断言拒收以及改输入触发 SHA 拒收轮次均保留，不计最终通过。测试现在保留原
异常及 suppressed 清理错误；JUnit 分类另有三份真实输出 / 五类拒收验证。完整多实例、公开 scoped
source、App / SDK / runtime / gterm IPC / 实际插件、物理设备 / WAN / 封闭内网 / 签名发行未完成；
整个目标继续，网关不实现。完整语义、限制和当前收据见插件计划第二十二批。

**插件 S4d5 公开文件来源第二十三批（2026-11-06）**：`extension_source.h` / `source-api.c` 七项
公开 Source 入口，显式已授权根 fd / 原 lease / file.readScoped / resource.receive，同 fd 两遍有界
读取 / SHA256 / 实际 EOF，复用私有 scoped reader，不隐式展开路径。Source 自管 transfer result /
forget，无 transfer handle 外泄；最多每 Session 16 个，成功 destroy 前持续 pin，父 transport 可释放
但 façade close 保留 DRAINING / destroy 拒绝。WAIT / IO 不消费 Source，真实 close 后 EIO 不重试 fd，
原失败与 cleanup_error 分开。Swift 原 queue / 无强父引用环，存活 Session step 回收已丢弃 Source，
IO 只显式重试，异步父 reaper 防递归销毁；Kotlin 原 worker / 显式关闭、构造前分配 owner，严格 UTF-8
拒绝坏 UTF-16，不以 `?` 替换误选文件，JNI 独立代次槽已接。无 App 根授权 / 资源视图或 CLI 消费。
26 本机＋3 真实关闭边界、两三跳四 SSH、Swift 8、API 36 arm64 / 4 KiB 页 Android 8 项通过；双向各
256 KiB / 80 确认 RPC，Source / 资源 / RPC 独立完成记录各六份。新 24 C / 5 Swift / 6 JNI / 1 Kotlin，
旧 Source 34 C / 公开 Session 13 C＋2 Swift / 资源 26 C＋4 Swift 分套复验，共 115 组手工，真实编译 /
链接后正常目标断言红、同库 / APK 配置绿、恢复绿与 SHA256；Core ASan/UBSan，无 LSan，不增加 CI
二十份。新 runner 实际 main 隔离残留预扫退出 2 / 源不变 / 无子命令。378 CTest / 147 ABI、Apple
143 / 4、JNI 109 / 38、严格 C11 五源、关闭 provider / 无 SSH、强制实际链接的 macOS App、Android
Debug / 测试 APK / 未签名 Release / 实际 R8 / 四模块 lint 通过。错误 capability 预期、fixture 编译
失败、已释放 Session 的重复 closeStep 三项失败与无 SSH 夹具的跳过均保留，不计最终通过。根 ACL /
挂载策略由 native caller 授权，不承诺同 UID 沙箱 / 原子快照 / 磁盘硬期限。App / SDK / runtime /
gterm IPC / 实际插件 / 多实例、物理设备 / WAN / 封闭内网 / 签名发行仍待验；目标继续，网关不实现。
本批语义、限制及收据见插件计划第二十三批。

**插件 Portable SDK 第二十四批（2026-11-06）**：`packages/plugin-sdk/portable/` 首片 strict TS /
ES2020 类型化请求、对象 / 文本 / 整数 codec 与 schema、取消 / Outcome / step / dispose 已接，错误
常量从既有 wire schema 生成并 `--check`。native port 只由受信 adapter 绑定原 Session / lease /
owner；私有字段不泄露 port，Core 仍再验权限 / 实例 / 期限。没有生产 JS adapter / 完整 runtime。
16 场景基线 / 恢复通过：13 项真实 Core RPC / socketpair，另为纯值、入队前拒绝 / native 零请求、
以及不一致确认 adapter 故障各一项；故障 adapter 不算真实对端回执。QuickJS-NG 0.18.0 校验归档
原样 C11 构建，无 quickjs-libc / Node / DOM。TS 5.9.3 / Ajv 8.17.1 仅锁定开发工具，四类静态负向 /
36 schema 对拍通过。九组隔离 TS 变异真实编译 / C 重链后 SDK 正常断言红、同构件独立对照绿、
恢复绿 / SHA256；实际 main 残留预扫退出 2 / 源不变 / 无子命令。两处 TS 编译失败拒收后重跑，
request / 聚合导出后完整复验；ASan/UBSan，无 LSan，手工覆盖不增加 CI 二十份。Core 本轮仍为
378 CTest / 147 ABI，未改 Core / 公开头 / native 绑定；没有本轮 Android / NDK / App / SSH / 设备 /
发行结论。原生能力在每次副作用前仍须检查；SDK 无法终止任意 JS / 阻塞原生调用，不是恶意代码
沙箱。完整 SDK / runtime / 页面 / gterm IPC / 插件 / 多实例继续，网关不实现；见插件计划第二十四批。

**插件可选 Core JS runtime 第二十五批（2026-11-06）**：私有 `runtime.h/.c` / `runtime-quickjs.inc`
接复制模块、受限 bootstrap / SDK port、真实公开 Session RPC / 激活 / 预算 / 取消与关闭。
QuickJS-NG 0.18.0 submodule pin `d7215b0ad3fc2c562e8317fd9bca058fd4863804`，四编译源 / 顶层头 /
LICENSE SHA256 另验，无上游修改或 quickjs-libc。CMake 显式可选、默认 OFF；缺 provider / 测试产物 /
pin / 内容即拒配，关闭档入口 UNSUPPORTED，无系统或网络回落。成功才接管 ACTIVE / 同角色 / 原
owner Session，无 Source / 旧请求 owner；trusted caller 校验包 / 模块 snapshot，摘要不是授权。
每实例独立 engine/context；只 stop 可跨线程，宿主仍须调度 owner。模块 64 / 每个 128 KiB / 总
1 MiB，ASCII 规范 ID；JS heap 默认 16 MiB / 栈 256 KiB / step CPU 25 ms / 32 jobs / 激活 5 s，
不等于 RSS / 原生 IO 限额；显式禁止 Atomics.wait 阻塞 owner。只 bootstrap 可取 native port，POD 用 null prototype，原 lease 每次
再查；已入队请求在 JS POD 交接 OOM 即 revoke，不能遗留发送。close 先撤销，再受中断 dispose /
强制释放 engine，原错误 / dispose 失败 / native cleanup 分开；非 OS / 同 UID 沙箱。
27 项实际 Core / QuickJS / strict TS SDK 场景通过，含双向请求、TLA、拒绝处理、预算、导入、并存
实例隔离、对端死亡及实际 queued request / QuickJS OOM 边界；具名注册 / 执行集合一致。十三组
隔离 C 真实 obj / archive / executable 重链后正常目标断言红、同库配置绿、恢复绿及 SHA256；另
四项配置拒绝不计作编译后变异。实际 main 残留预扫退出 2 / 源不变 / 无子命令。ASan/UBSan，无
LSan，严格 C11 四次通过，均为手工，不增加 CI 二十份。顶层 CPU 误记 SCRIPT、边界缺实现宏的编译
失败 / Not Run 保留且未计绿。默认 build-core / gate 为 379 CTest / 147 ABI，公开头及 native
绑定未改。可选 NDK 28.2 / arm64 / API 29 真编译并链接含 runtime 和引擎定义的 ELF，未设备执行，
无 App / APK / 签名 / 云端 / WAN 结论。公共 runtime ABI、两端启用、完整 SDK / 原生授权页 / gterm /
插件 / 多实例仍待接；网关不实现，范围 / 许可 / 收据见插件计划第二十五批。

**插件公共 runtime 第二十六批（2026-11-06）**：`extension_runtime.h` / `runtime-api.c` 七项 opaque /
POD 入口及 Swift / Kotlin / JNI worker 已接。Core **154 ABI**、Apple **150/4**、JNI **116/38**；
默认 JS OFF，缺能力明确 UNSUPPORTED。成功才消费 ACTIVE / 同角色 requests Session；旧 outgoing /
已取 unfinished ticket / resource producer / Source owner 拒绝，未取 peer 请求可继续处理。模块复制 /
预算 / 原错误和 dispose / cleanup 分开；stop 单独跨线程，destroy 失败保留。Swift 原 queue 清理
ledger / Store 强引用、JNI 同 16-slot 独占 pin / retiring / 新代次 ID、Kotlin 先分配后接管已验。
开启档公共 31 C / 私有 27 C、Swift 8、API 36 arm64 / 4 KiB 页模拟器 Android 8 项通过；真实 JS /
Session RPC、owner 准入、队列清理与 OOM 交接。新 19 C / 6 Swift / 7 JNI-Kotlin 手工变异：真实
obj / archive / executable 或 shared / APK 重建后正常单条断言红、同库或同 APK 配置绿、重编恢复 /
SHA256；旧私有 13 组 / 27 项 / 四配置拒绝另复验，Core ASan/UBSan，无 LSan。两新 runner 实际
main 残留预扫退出 2 / 零子命令 / 源不变，不称任意信号边界已验。普通 gate **380 CTest / 154 ABI**，
无 SSH / provider OFF 仍 154 导出，四源严格 C11、macOS App 实际重链和关闭档 Swift 单项通过。
Android 显式测试 profile ON 与默认 false 同步重置 CMake，enabled-only 测试不在普通发现集；
启用 APK 原样附 QuickJS MIT 许可，SDK 只入测试 APK。JNI 锚点歧义、strip 前后误比及停用变异被 ABI
拦截的失败均未计目标红，修正后七组完整重跑。手工证据不增加 CI 二十份；无 App 激活 / 完整 SDK /
gterm IPC / 实际插件 / 多实例、物理设备 / WAN / 签名发行结论。下一片接受控资源服务，目标继续；
网关不实现，完整收据和默认原生构建范围见插件计划第二十六批。
Android 默认 Debug / 测试 APK、未签名 Release / 实际 R8 与四模块 lint 通过；同设备关闭档单项 /
独立 Core 三项通过，默认 APK 无 QuickJS / 测试 SDK / 引擎许可及 enabled-only 测试类。

**插件 runtime 资源第二十七批（2026-11-06）**：`runtime-resource.h/.c` / `runtime-resource-js.inc`
与 Portable `resources.ts` 复用原 Source / Session / Store，接真实文件发布、回执、本地 owning ref /
有界顺序流。Config 新增 Root POD / 数组，**结构布局 BREAKING、消费者重编译**；仍 154 ABI、
Apple 150/4、JNI 116/38。两端 root ID / fd / byte limit 同步，成功复制至多八根 CLOEXEC fd / 标识，
原 fd 不消费；不取终端 cwd / 自报路径，不暴露 native lease / fd / snapshot token。JS 复制的
metadata 不能 mint 读取权限；Source / Read 继续查原 scope/grant，缺 Store 不装 resources，有
Store 无根只收。十六 Source、六十四 pin、每 step 一次 Source 16 KiB / 读取另共享 16 KiB，
native 期限 / 时钟 IO 分开；仅真实确认成功，UNKNOWN 保留，读错误不能转 EOF。关闭保留 IO /
WAIT、先释放资源后 Session destroy，已消费 fd 不重关，匹配 scope 的已不存在 ref 清 slot。
32 个 C / QuickJS 场景（17 owner / 边界、15 JS，含三 adapter 故障及 native pin 后 OOM 异常注入）、
Swift 11 / API36 arm64 4KiB 页模拟器 Android 11 通过；实际 256 KiB / 空文件 / 读回逐字节，
不把 adapter 观测算真实回执 / 磁盘失败。新 21 C / 5 SDK / 2 Swift / 3 JNI-Kotlin 分套共 31
变异通过，连同原公开门面 / private runtime / SDK 复验共 85 组本批分轮手工证据：真实编译 /
archive / executable / shared / APK 重建后正常目标断言红、同库或 APK 配置绿、重建恢复 /
SHA256，ASan/UBSan，无 LSan，不增加 CI 二十份。最终资源专项 26 组 / 32 场景另完整通过。
默认 Core 397 CTest / 154 ABI、无 SSH 17 owner / provider OFF、四次严格 C11、macOS App 实际
重链、Android Debug / 测试 APK / 未签名 Release / 实际 R8 与四 lint 通过；关闭档 Swift 1 /
Android 1+3 Core 通过，普通包不含引擎 / 测试 SDK。编译失败、TS narrowing、未证明对端收到
BEGIN 的未杀死变异均拒收；修正预约 / 部分发送屏障及已不存在 pin 清理后重跑。没有本轮 SSH /
tmux / SAF 根 / 真机 / WAN / 签名发行，App 批准 / 视图 / 激活、包校验 / gterm / 完整 SDK /
实际插件与多实例继续；网关不实现。收据与失败范围见插件计划第二十七批。

**插件物理包快照第二十八批（2026-11-06）**：私有 `package-snapshot.h/.c` 从真实目录 fd
采集固定字节 / SHA256，非 UI owner 分步读取；最多 1024 文件、单文件 16 MiB、内容 64 MiB，
另限 4096 文件 / 目录条目，integrity / signature 计物理预算。逐组件 no-follow、独立目录
游标、只读 NONBLOCK / CLOEXEC、打开前后身份、EOF / 目录复核，拒绝链接 / 特殊文件、
非法路径 / UTF-8 / ASCII 大小写碰撞；完成前和失败后不暴露内容，错误保持、析构擦除。
32 本机 / 配置＋9 syscall 边界、28 组真实编译变异、四份残留预扫通过；目标正常断言红、
同 archive 配置绿、重编译恢复 / SHA256，ASan/UBSan、无 LSan，不增加 CI 二十份。
默认 Core 438 CTest / 154 ABI、Apple 150/4、JNI 116/38；provider / SSH / JS OFF 同 41 项，
macOS / NDK 严格 C11 与 Android Debug bridge AAR 编译通过。原元数据 96 项 / 6 变异另验。
unused 编译错误与仍 PASS 的身份变异均拒收，补读取前判据后完整重跑。采集不是原子目录 /
元数据可信证明，不授予执行；原生 manifest / integrity / Unicode / 来源策略 / 模块加载、
批准 / 视图 / gterm / 完整插件继续；无设备 / App / 发行结论，网关不实现。见插件计划第二十八批。

**插件包元数据第二十九批历史未验收记录（2026-11-06；最终见第三十四批）**：当前私有固定字节校验候选不接 App / 不增
公共 ABI。正常路径 35 组编译后变异 / 17 配置拒绝及 42 本机 / 41 Android C 场景通过，
但 QuickJS 在逐分配失败扫描中仍有字节码清理崩溃：Debug ordinal 1057、优化 ASan/UBSan
与实际 NDK ELF 的 Android ordinal 680。候选 patch 位于 `patches/quickjs-ng/0.18.0`，
vendor 未改；崩溃不算正常断言红，前面的单点 OOM / 正常路径通过不代表整批完成。
默认 JS OFF gate 为 457 CTest / 154 ABI，Apple 150/4、JNI 116/38；均为分范围证据，
不增加 CI 二十份。按用户约定待人工决定引擎加固或隔离 / 替代评估，当前不提交本批；
详细故障 / 收据见插件计划第二十九批，网关未实现。

**插件内存第三十批历史专项（2026-11-06；独立提交见第三十二批）**：在现有未提交元数据候选上，包 JSON 的节点 /
字符串 / 重键表按需增长并逐检查擦除释放；插件自持 heap 申请统一计入默认 128 MiB 进程级
requested / reserved budget，含 native owner / 分帧 / RPC / 包快照 / 源码 / JS、失败归还与
realloc 新旧峰值。该值为候选政策，不等于 RSS / PSS / footprint 或线程栈、外部库、Swift /
JVM / UI / 磁盘配额。每实例仍独立 runtime、heap / stack / CPU / jobs 限额；OOM sticky 撤权。
私有 `memory.h/.c` / 动态 JSON 新 14 组、117 项基线 / 恢复及既有 runtime 13 组通过；小文档
峰值 3,008 bytes，1 / 4 / 10 runtime 真实 RPC、私有 / 公共各 100 次开关后 heap 计账 / fd 归零。
默认 Core 467 / 154 ABI、JS ON / SSH ON 完整 574 项、24 次严格 C11 编译及实际 NDK arm64 /
API29 ELF 编译链接通过，未设备执行。旧释放保护分族完整复验及 snapshot 擦除专项、公共
channel / resource / Source 的 C / Swift 变异另记在插件计划；统一收据
`tmp/plugin-memory-work/result.json` 核对 17 份 accepted 原件 / 输入 SHA256。初版编译 / CTest
发现 / Debug+ASan 栈 / 未重编、冗余变异未杀死及一次 Source SSH 清理超时均未计绿，修正
探针后复验；未放宽生产栈限额或原清理断言。不增加公共 ABI / 平台绑定、macOS CI 二十份或
App 启用。第二十九批引擎编译 OOM 崩溃仍未解决，不能借本片专项记整批绿，候选保持未提交；
App 按需激活 / 回收、包快照复用与实际设备进程内存仍待验。范围 / 证据见插件计划第三十批。

**插件编译 OOM 第三十一批（2026-11-06）**：七份 QuickJS-NG pin 补丁由私有构建副本应用，
新增 DynBuf sticky 与 switch default 回填检查。独立编译器 / 缓冲八组真实编译后正常断言红、
同 archive 配置绿、重建恢复与 SHA256；switch 另用确定性发射失败与危险写前观察，不将
无钩子崩溃计红。macOS Debug 164、NDK arm64 ELF 在 API36 / 4 KiB 页模拟器 167 分配点
及释放归零通过；独立提交 Core 默认 438 CTest / 154 ABI、JS ON / SSH ON 511 项通过。
包候选 Debug 1892 / Android 1554 点通过，但优化 ASan 的正则 OOM ordinal 1427 另有越界，
故第二十九批仍未验收 / 未提交。本片不接 App、不增公共 ABI、平台产品或 CI 二十份；
失败 / 范围 / 收据见插件计划第三十一批，后续按独立切片继续。

**插件共享内存 / 按需 JSON 第三十二批（2026-11-06）**：从未验收包校验器拆出独立提交；
私有进程 requested / reserved heap、native owner / 队列 / 字节快照 / 源码 / JS 共用预算，
动态 JSON 保持严格词法 / 精确整数 / 重键与旧 wire 预算，不含 metadata 校验 / 执行授权。
独立源码默认 465 CTest / 154 ABI、JS ON / SSH ON 550 项、内存十四组 / 91 项、runtime
十三组 / 33 场景 / 四配置拒绝通过；目标为编译后正常断言红、同 archive 配置绿、重建恢复
与 SHA256，ASan/UBSan，无 LSan。NDK arm64 API29 实际 ELF 只编译链接，未设备执行。
既有三十批定量 / 逐族释放证据不冒充本轮全重跑。无新 ABI / 平台产品 / App / CI 二十份；
包 metadata 仍因正则 OOM 未验收，范围 / 失败 / 收据见插件计划第三十二批。

**插件正则 OOM 第三十三批（2026-11-06）**：第八份 QuickJS-NG pin 补丁在 atom 发射后 /
量词扫描前传播 bytecode OOM，并拒绝捕获组名称缓冲分配失败后继续解析。七种独立原生
语料 208 分配点在 macOS 优化 ASan/UBSan 与 NDK API29 ELF / API36 arm64 4KiB 页模拟器
通过；两组隔离变异为编译后正常断言红、同 archive literal 配置绿、重建恢复及 SHA256。
无观察钩子的包候选另通过 Debug 72＋1892、优化 ASan 与 Android 各 72＋1554 夹具 / 分配点。
独立默认 Core 465 / 154 ABI、JS ON / SSH ON 551 项全绿。初版捕获组断言红、变异锚点 /
带前缀诊断拒收与隔离路径设置错误均单列保留。只提交引擎修复；包校验继续单独验收，不增
公共 ABI / 平台产品文件或 CI 二十份，非 App / 真机 / 发行结论。收据见插件计划第三十三批。

**插件包元数据第三十四批（2026-11-06）**：第二十九批私有固定字节检查经八份引擎补丁后
独立验收并提交。trusted validator 只检查 manifest / integrity / native host / registry，
不执行插件；JS OFF 显式不支持，仅兼容结果消费快照，失败保留，始终不授予执行 / 发布者
身份。默认 467 CTest / 154 ABI、JS ON / SSH ON 完整 577 项、Apple 150/4 / JNI 116/38，
35 编译后正常断言变异及 23 配置拒绝通过；目标红、同 archive 配置绿、重建恢复与 SHA256。
当前 Debug 72＋1892、优化 ASan 与 Android ELF 各 72＋1554 夹具 / OOM 分配点，设备另
49 原生专项、全 OFF 67 项及八次 macOS / NDK 严格 C11 编译通过。Android 为 API36 / arm64 /
4KiB 页私有模拟器，ASan/UBSan 无 LSan；漏构建边界 executable 的 Not Run 初轮保留且
未计绿，补齐后重跑。不增公共 ABI / 平台产品源码或 CI 二十份，非 App / 真机 / 发行证据。
后续模块闭包 / 信任 / 安装、App 授权 / 视图、gterm / 完整 SDK 和真实插件仍待接；原件和
输入摘要见插件计划第三十四批，不以 metadata 兼容当作执行授权。

`android-foreground` 以 `ANDROID_SERIAL=设备 python3 scripts/test-android-foreground.py` 验八项真实服务/页面定向，`mutation-check-android-foreground.py` 手工验 23 组；护真实 promotion、调度阻断、权限/类型、代次与请求取消、通知隐私/路由和生命周期。`test-android-foreground-system.py` 另验真实系统超时、stop-app 后新 PID 只读历史、通知拒绝及系统设置渠道关闭；`mutation-check-android-foreground-system.py` 有九组相应场景变异，含开启通知后补发及渠道恢复的状态检查。每组须编译后目标单条 JUnit 断言红、同 APK 配置对照绿、恢复绿与 SHA256，崩溃/编译失败不算目标红。系统设置及权限须恢复，stop-app 被中断的 staging 不计通过。§9.74 的 30 组与 §9.75 追加的两组均为分轮证据，不称一轮全量；完整 483 项仅 API 36 arm64 / 4 KB 页模拟器。以上均属手工覆盖，不增加 macOS CI 20 份，也不代表整个 A6 或发行矩阵完成。

`android-file-journal` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-file-journal.py` 手工执行 20 组，护事务回滚、损坏/版本/容量、严格 JSON 与 UTF-8、检查点顺序、已知结果保留、历史加载/删除和队列阻断。`mutation-check-android-file-journal-crash.py` 在同样的设备环境下另验四组真实 SIGKILL/新 PID 恢复变异（含创建身份与阻止普通失败冒充死亡）；`test-android-file-journal-crash.py` 提供本机/SAF 六个中断点，新增创建标记已落盘、内容未写入边界。API 29 的 run-as 信号被 SELinux 拒绝，测试使用请求/PID/边界绑定的命令与 ack，由应用仅 SIGKILL 自身，宿主确认旧 PID 消失；不使用 root 或修改策略。远端以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteFileJournalTest ANDROID_FILE_JOURNAL_REMOTE_MUTATE=1 bash scripts/test-android-ssh.sh` 执行四组变异及四个真实中断点，单独复验场景将 MUTATE 换为 CRASH。新进程只加载只读记录，核对实际文件/文档与清除记录不删文件，不自动重放任务、不恢复凭据。每组须真实编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；数量、失败与已运行范围见 Android 实现计划 §9.73，不将专项通过记为整个 A6 或断电恢复完成。

`android-file-journal-discovery` 以 `ANDROID_SERIAL=设备 python3 scripts/test-android-file-journal-discovery.py --mutate` 手工执行一组，护默认 JUnit 发现不会把禁用的宿主中断探针算作一项空测试；目标红、同 APK 实际配置对照绿、恢复后具名清单一致与 SHA256。`ANDROID_SSH_ALL=1` 的完整入口另对拍默认发现与实际执行的具名集合、开始/结束及零跳过；发现数量本身不是实际执行通过。以上任务记录/中断/发现共 29 组均为手工覆盖，不增加 macOS CI 20 份。

`android-external-preview` 用 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-external-preview.py` 手工执行 24 组，护已知/未知限额与原始字节、总量/零字节数量、默认大小/期限、过期与未命名仍使用的 pin、只读/URI/临时授权、provider、页间 seek、清理失败、单次启动、旧副本、按钮、后台与实际 worker 取消及远端父目录拒绝。当前 API 29 / arm64 / 4 KB 页每组编译后目标单条断言红、同 APK Core 配置对照绿、恢复绿与源码 SHA256；其他设备与完整客户端另验。`ANDROID_SERIAL=设备 python3 scripts/test-android-preview-restart.py --mutate` 另护真实 producer 进程死亡：独立 UID 保留 unstable client 取得的同一个 proxy FD，8 MiB 未读尾段须报 IO 错误，重启清理该次过期 token；改为原始 inode FD 后目标红、同 APK Core 对照绿、复原绿与 SHA256。不能拿被系统联动杀掉的接收者或已经缓存的旧页当成目标红/通过；前三轮探针与变异不足的失败及修正见 Android 实施计划 §9.92。两入口均为手工证据，不增加上述 macOS CI 20 份；完整矩阵仍待执行。

**Android 外部预览整合（2026-11-01，§9.92，已提交 ab2f008）**：API 29 最终 30 项、24 组编译后变异与一组真实进程中断反例通过；Core 77 份源文件仍与先前 47 CTest / 91 符号闸门相同，未修改 Apple 源码。主工程四模块 release lint、本地固定证书 release 构建与 API 36 / arm64 / 4 KB 页完整 609 项通过（1,551.598 秒），零失败/错误/跳过，具名发现/开始结束、两安装 APK 摘要及 798 份输入前后与最终审计一致。带正式图标的本地 R8 release 为 6,146,497 字节，验签/压缩/16 KB 对齐通过；包内是 a0328da + dirty，不冒充云端包。随后 ab2f008 的 release CI 41288109410 成功，实际云端包 6,146,193 字节，提交/clean、固定证书、压缩/对齐与实际 R8/资源优化已核验；同包 16 KB 页升级/九文件/设置/ASCII 与跨 UID 外部只读打开专项通过。SAF 文本修复仍在隔离工作树，不在此完整 APK；完整设备、输入法、发行矩阵及整个 Android 目标仍未完成。

Android Release 资产以 `python3 scripts/check-android-release-assets.py 目录 --expect-commit 提交` 检查 APK/Android 专用摘要/构建清单及内置源码收据一致；`mutation-check-android-release-assets.py` 对实际包做十组输入损坏反例，同包代码/源码独立对照绿、原件和 SHA256 不变。只证明发布完整性，不代替真实签名、优化或原生对齐验收；Android workflow main 发布前/回读后复用检查，PR 不发布，不增加 macOS CI 20 份。新 workflow 的实际执行进度见 Android 实施计划 §9.94。

共享 dev Release 最终命名为 Ghosterm-macos-arm64-dev.dmg / .zip 与 Ghosterm-android-arm64-dev.apk；版本在清单里，dev 不代表 Android debug 构建。两端发布 job 共用 dev-release 互斥组；Mac 原位更新并仅删除明确属于 Mac 的旧归档，严禁删整个共享 Release。check-dev-release-assets.py 检查各自附件字节、完整另一平台集合及 Release/另一平台资产 ID 保留；mutation-check-dev-release-assets.py 的二十二组反例护丢失、替换、错字节/大小与陈旧归档。独立 android-dev Release/tag 已按用户要求删除；记录见 Android 实施计划 §9.96。

`android-file-queue` 以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FileQueueProductTest ANDROID_FILE_QUEUE_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 14 组，护 FIFO、请求容量、取消等待项、实际释放前不推进、各终态推进、队列位置/页面和请求内决策。另启动独立 OpenSSH 服务验两个端点的排队认证与真实内容；同宿主双服务不等于公网或物理双机矩阵。每组真实编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256；实际通过范围见 Android 实现计划 §9.72。属于手工覆盖，不增加 CI 20 份。

`android-process-files` 以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.ProcessFileWorkTest ANDROID_PROCESS_FILES_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 15 组，护进程任务/清单唯一持有、页面结束/后台、全局名额、结果订阅、认证接管及系统选择器/旧确认归属。编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；设备与实际运行范围见 Android 实现计划 §9.71/§9.72。不增加 CI 20 份，不将进程内存活记为前台服务或进程恢复完成；排队另见 §9.72。

`android-file-task-product` 以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FileTasksProductTest,com.hanxi.ghosterm.RemoteFileTasksProductTest,com.hanxi.ghosterm.SafFileTasksProductTest ANDROID_FILE_TASK_PRODUCT_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 24 组，覆盖粘贴/确认/任务入口、固定目标、名额与取消、清单与页面保护、实际结果、配置/认证归属以及 SAF 读取结束等待。每组须编译后目标红、同 APK 独立配置对照绿、恢复绿与 SHA256；设备档位、失败和最终结果见 Android 实现计划 §9.70。此处登记覆盖范围，不增加 CI 20 份，§9.70 的 Activity 内任务后来由 §9.71 改为进程共享单活动请求，仍不表示完整 A6 排队或后台恢复。

`android-copy-executor` 以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FileCopyExecutorTest,com.hanxi.ghosterm.SafCopyExecutorTest ANDROID_COPY_EXECUTOR_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 33 组。覆盖真实 HOME/SAF/SSH 编排、来源复核、名称/快照预算、跳过、逐项副作用/错误码、取消与暂存释放；每组要求编译后目标红、同 APK 独立配置对照绿、恢复绿与 SHA256。数量、失败及最终运行证据见 Android 实现计划的复制执行器切片，不增加 macOS CI 20 份，也不将 data 层编排记为粘贴页面或完整客户端交付。

`android-file-tasks` 以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteFileTaskTest ANDROID_FILE_TASK_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 23 组，护每连接完成通知、目录通知顺序、CLOEXEC/构造失败回收、两角色独立连接、固定身份、作用域/一次性使用与迟到取消。六组先编译 native 延迟清理并验基线，再验目标红、同 APK 配置对照绿、恢复绿及 SHA256；最终移除延迟复验。设备档位与记录见 Android 计划 §9.67，不增加 macOS CI 20 份，不将数据层作用域记为粘贴执行器或任务认证页面完成。

`android-mkdir-data` 以 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteDirectoryDataTest ANDROID_MKDIR_DATA_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 23 组，护实际连接身份、完整端点、冻结绑定、预算、六态/原始错误码、实际位置与取消释放。两组另编译 native 延迟清理/完成管道，先验延迟基线绿，再验目标红、同 APK 配置对照绿、恢复绿及 SHA256；最后移除延迟并复验九项真实 SSH/协议用例。范围见 Android 计划 §9.66，不增加 CI 20 份，不将数据层适配记为应用级任务或 macOS 产品迁移完成。

`android-directories` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-directories.py` 手工执行 26 组，护本机非覆盖/路径/权限与 SAF 授权、创建结果、父子归属、列表预算、一次性请求及取消晚到结果。真实编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256；16 项目录基线及完整 358 项通过，仅 API 36 arm64 / 4 KB 页模拟器。初轮测试盲区及完整重跑见 Android 计划 §9.64；不增加 CI 20 份，不将单目录创建记为任务/页面或远端已完成。

`android-decisions` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-decisions.py` 手工执行 42 组，护请求/修订隔离、确认/取消、本次全部、不支持项/目录跳过、不可变清单及名称/元数据预算。真实编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256；15 项定向与最终完整 342 项通过，仅 API 36 arm64 / 4 KB 页模拟器。首轮选择顺序盲区与重跑范围见 Android 计划 §9.63；不增加 CI 20 份，不将逻辑清单记为名称预留、任务或页面完成。

`android-preflight` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-preflight.py` 手工执行 34 组，护目标创建三态、目录关系、来源完整性、目标身份/列表/预算、只读冲突和取消。真实编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256；13 项定向及完整 327 项通过，仅 API 36 arm64 / 4 KB 页模拟器。范围见 Android 计划 §9.62；不同远端身份关系仍未知，不把预检记作写入授权/任务完成，不增加 CI 20 份。

`android-scan` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-scan.py` 手工执行 34 组，护粘贴请求身份/只读快照、来源属性、目录/路径/名称、空目录、未知与溢出、扫描预算、错误、复核与取消。真实编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256；13 项定向与完整 314 项通过，仅 API 36 arm64 / 4 KB 页模拟器。范围见 Android 计划 §9.61；不增加 CI 20 份，不把来源枚举记作目标授权或粘贴任务交付。

`android-clipboard` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-clipboard.py` 手工执行 16 组，护清单独立/只读、UUID、来源、选择、离页、确认、卡片及完整详情。编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；9 项设备定向与完整 301 项通过，仅 API 36 arm64 / 4 KB 页模拟器。既有浏览 18 / 远端产品 14 组复验。范围和中断/编译失败见 Android 计划 §9.60；仅登记覆盖，不增加 CI 20 份，不将来源清单记作粘贴执行或任务已交付。

`android-saf-publication` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-saf-publication.py` 手工执行 30 组：护持久读写授权、树/子目录、列表和冲突、新文档身份/能力、创建未知、实际名称、内容/源变化、关闭/回读/可靠管道、背压取消、进度挂起、结果保留和 fd 释放。编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256；16 项设备定向及最终完整 292 项通过，仅 API 36 arm64 / 4 KB 页模拟器。范围及早期失败见 Android 实现计划 §9.59；不增加 CI 20 份，不将其记为复制任务、目录或进程恢复已完成。

`android-publication` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-publication.py` 手工执行，当前脚本共 25 组。§9.58 的 24 组护独占/非覆盖、路径/链接、权限/CLOEXEC、候选身份/长度、复制内容/源变化、取消、清理和 fd 释放，另有既有暂存 23 组及该片完整 276 项通过。§9.60 修正取消回包测试的同步前提，新增晚取消保留已发布文件的一组变异，十项发布基线及新增组通过；本轮没有重跑全部旧 24 组。各变异均要求编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256。仅 API 36 arm64 / 4 KB 页模拟器，详细失败与最终完整结果见对应实施记录；仅登记覆盖，不增加 CI 20 份，不将私有文件提交记为 SAF/复制任务或进程恢复已完成。

`android-staging` 以 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-staging.py` 手工验证 23 组：匿名创建/权限/CLOEXEC/目录关闭、源变化与片段、容量/数量、读租约、取消和未交付结果回收；目标编译后断言红、同 APK 配置对照绿、恢复绿与 SHA256。范围见 Android 计划 §9.55；不增加 CI 20 份，也不将暂存快照记作复制产品或最终目标发布。

`sftp-publish` 以 `python3 scripts/mutation-check-sftp-publish.py` 手工执行 20 Core＋1 无库＋5 Swift 组，护查询三态、非覆盖、源复核、总预算/取消、结果与连接所有权；编译后目标红、独立配置对照绿、恢复绿与 SHA256。11 个真实 OpenSSH 场景纳入 Core gate（§9.57 时 46 项 / 88 ABI），另有 45 项协议场景。Android `ANDROID_FILES_PUBLISH_MUTATE=1` 验十组，同 APK 配置对照；既有读取 13、创建九、owner 18 组复验。详见 Android 实现计划 §9.57；只登记已有覆盖，不增加 CI 20 份，不改变验收规则或将接口记为复制产品交付。

`sftp-receive` 以 `python3 scripts/mutation-check-sftp-receive.py` 手工执行 21 Core＋1 无库＋4 Swift 组，护零字节上限空源、大小/类型/源变化、偏移、写入和关闭、取消/总预算、失败发布与连接失效；编译后目标红、独立配置对照绿、恢复绿与 SHA256。11 个真实 OpenSSH/本地故障场景纳入 Core gate（45 项 / 87 ABI），另有 63 项模拟协议场景。Android 以 `ANDROID_FILES_RECEIVE_MUTATE=1` 执行 13 组，含同一延迟关闭条件下的上传/下载 fd 租约验证；既有创建九组、owner 18 组独立复验。范围见 Android 实现计划 §9.56；不增加 CI 20 份，不将匿名暂存当最终发布或复制页面已交付。

`sftp-create` 用 `python3 scripts/mutation-check-sftp-create.py` 手工执行 17 Core＋1 无库＋4 Swift 组，护独占/权限/源/偏移、拒绝与未知、取消/预算/关闭确认及不可用入口；目标红、独立配置对照绿、恢复绿与 SHA256。六个真实 OpenSSH 场景纳入 Core gate（§9.54 时 44 项 / 86 ABI），另有 14 项协议场景。Android 以 `ANDROID_FILES_CREATE_MUTATE=1` 配合 `RemoteFilesTest` 执行九组：结果结构、偏移、fd 释放/CLOEXEC、尺寸、未知、状态/字节与取消，同 APK 配置对照。既有 owner 18 组独立复验；范围与失败见 Android 实现计划 §9.54，不新增 CI 20 份，不将暂存创建当作最终目标发布。
`sftp-transfer` 用 `python3 scripts/mutation-check-sftp-transfer.py` 手工验证 13 组：两侧关闭、两方向排队取消、覆盖截断、下载打开顺序、目标打开失败释放、源定位与合法零写后继续。每组隔离编译后目标红、同变异配置对照绿、恢复绿与 SHA256；基线 14 个真实 OpenSSH/本地故障场景＋5 个协议拒绝场景。正常/本地故障回归纳入 Core CTest（§9.53 时 43 项），协议故障与变异仍手工执行，不增加 CI 20 份。范围与失败记录见 Android 实现计划 §9.53；不将关闭检查当作非覆盖或原子提交。
`sftp-browse` 用 `python3 scripts/mutation-check-sftp-browse.py` 手工验证 13 Core + 2 依赖 + 3 Swift + 1 无库组，覆盖受控目录、实际分配器释放、NAME 边界和绑定；36 项协议矩阵独立执行。该切片为 83 ABI、Apple 81/2、JNI 40/43，产品当时尚未消费。`sftp-submit` 用真实 worker 提前释放与 ASan 验任务返回 ID 所有权，一组变异目标 UAF 红、独立 API 绿、恢复绿与 SHA；`exec` 增加会话超时恢复组，共三组。正常目录/提交回归进入 Core gate，手工脚本不增加 CI 20 份。范围与失败记录见 Android 实现计划 §9.49；此处登记现有覆盖，不改变既有验收规则。

`sftp-preview` 用 `python3 scripts/mutation-check-sftp-preview.py` 手工验证 15 Core + 1 无库 + 4 Swift 组，护预算/取消、只读打开、打开前后类型/大小、限额额外字节、EOF 完整性、关闭结果/释放、连接失效、发布与绑定；独立协议矩阵 46 项。该切片为 84 ABI、Apple 82/2、JNI 40/44，Android 预览当时登记待接。正常真实 SSH 预览进入 Core gate；20 组变异属手工证据，不增加 CI 20 份。首轮验证脚本文案失败及最终结果见 Android 实现计划 §9.50。

`android-files-owner` 用 `python3 scripts/mutation-check-android-files-owner.py` 手工执行 18 组，护专用连接中止/取消/预算、目录/预览限额和内容、未知元数据/编码、认证作用域/凭据缓冲及请求/句柄代次；编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256。`python3 scripts/mutation-check-ssh-abort.py` 另验 1 Core + 1 无库 + 1 Swift 组，独立配置/API 对照。§9.51 当时为 42 CTest / 85 ABI、Apple 83/2、JNI 45/40；13 项新增设备与最终 52 项定向回归通过，页面尚未接，不称完整客户端回归。此处登记已验证范围，不更改既有验收要求或增加 CI 20 份。

`android-files-product` 用 `ANDROID_SERIAL=设备 ANDROID_FILES_PRODUCT_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteFilesProductTest bash scripts/test-android-ssh.sh` 手工执行 14 组，护独立文件连接、实际用户名快照、规范路径/排序、未知与无符号大小/类型文案、预览、离页释放、迟到认证与错误可见性；目标红、同 APK 配置对照绿、恢复绿与 SHA256。9 项产品设备用例及最终回归范围见 Android 实现计划 §9.52。只登记现有验证范围，不增加 CI 20 份或改变验收规则。

### 变异验证（新增闸门必须附）

`android-tmux` 用 `ANDROID_SERIAL=设备 ANDROID_TMUX_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.RemoteTmuxTest bash scripts/test-android-ssh.sh` 手工执行十三组：实际执行端命名空间、固定地址、native/Kotlin 代次、认证 scope/材料清理、取消释放、真正 attach、未知结果/文案/禁止重试与普通入口/选择器隔离。每组真实编译后单条目标断言红、同 APK Core 配置对照绿、复原绿并核对输入 SHA256；入队次数与服务器 create 日志共同约束未知重试。`ee2a83f` 的 API 29/36 各 45 项组合、新选择器补丁的两端三项与入口/选择器两组变异及失败/复验记录见 Android 计划 §9.103，不计入 macOS CI 20 份。


新增任何断言/闸门，都要证明它**有牙**：故意改坏 → 断言变红 → 回滚复绿。
样板见 `scripts/mutation-check-sshconfig.sh`（17 组变异：A1~A9 + B1~B8，另验基线与回滚）。A9 用真实目录触发 fread 错误，断言必须返回失败；同变异下合法配置 golden 对照仍须通过，不能把读取错误当成空配置。B7/B8 护非连续重复指令替换/删除，构建通过后逐字节写回目标红、同变异求值对照绿、恢复绿与 SHA256 一致。
`config-metadata` 以 `python3 scripts/mutation-check-config-metadata.py --apple` 手工执行：8 组 C 变异护来源/块归属、包含与歧义块拒写和重解析路径，1 组 Swift 变异护来源转换。编译成功后目标断言红、独立 API/配置往返对照绿、恢复绿与 SHA256 一致；不计入 CI 20 份。Android 来源、只读页面、显式覆盖/恢复继承、别名重命名及配置错误恢复由 `mutation-check-android-config.py` 的 59 组设备变异覆盖（同 APK 独立对照）；错误恢复另护快照失效、错误可见与跨页保留、重载放弃确认/动作路由、编辑目标刷新及无快照入口禁用。SAF 通过真实系统选择器与测试 APK 专有 provider 验证预览/确认/取消、完整字节导入、错误/重载、外部文档不写入，另验别名/地址/端口校验。`mutation-check-config-rename.py --apple` 另验 12 组 C / 1 组 Swift：目标 token 保真、名称/冲突、归属与重复别名拒绝、重解析错误与对象保留、来源路径、大小写和无操作保真；均编译成功后目标红、独立对照绿、复原绿与 SHA256 一致。均属手工证据，不计入 CI 20 份。
`connection-config` 用 `python3 scripts/mutation-check-connection-config.py` 手工执行 14 组隔离 C 变异，护连接准入、范围/首值/累积规则、条件、值与来源；`mutation-check-android-connection-config.py` 用五组设备变异护 JNI/Kotlin/页面接线。均先编译，再目标红、独立对照绿、恢复绿与 SHA256，未加入 CI 20 份；auth 当前 11 组包括 macOS 准入与来源接线，证据见 Android 实现计划 §9.24。此处登记现有验证范围，不改变原验收要求。
`android-ssh` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 14 组：真实隔离 sshd 经 adb reverse 到设备，编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256，护认证请求/来源、信任/凭据结果、取消与远端 feed/resize。`ANDROID_SSH_ALL=1` 运行包含真实 SSH 的完整设备套件，缺夹具不跳过；历史后端证据见实现计划 §9.25，后续通道取消见 §9.41，未新增 CI 覆盖。
`android-content` 以 `ANDROID_FILE_CONTENT_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FileContentTest,com.hanxi.ghosterm.FileJournalProductTest,com.hanxi.ghosterm.RemoteFileTasksProductTest,com.hanxi.ghosterm.LocalRetainedContentTest,com.hanxi.ghosterm.FileExecutionPlanTest,com.hanxi.ghosterm.RetainedFilesProductTest bash scripts/test-android-ssh.sh` 手工执行；当前十七个不同组分轮通过（8＋4＋3＋2），护摘要字节/跨块/取消检查、写前参考保存与严格格式、历史比较/无参考/元数据变化/错误分类、按钮动作与新目标专用认证，以及全局单读取/排队、外来释放、VM 释放与历史保留。每组真实编译后单条断言红、同 APK Core 配置绿、恢复绿与 SHA256；后台断言导致进程崩溃、Compose 超时未计正常断言红。摘要一致不提升旧未知结果、不证明归属或赋删除权；API 29 / 36 各 113 项前置组合通过；全局读取占用调整后各 50 项相关复验、原队列保护与最终 release/四模块 lint 通过，旧账本二十组另有前置证据；§9.112 已接私有 file-tasks 绝对候选只读核验，八组新增及共享摘要三组原保护同轮通过，两端各 69 项与签名 release/四模块 lint 通过；不证明持久归属，不增加 macOS CI 20 份。§9.113 另有十一组执行计划引用保护，覆盖事务登记/版本 3、重复引用与阶段约束、实际复制和暂存标签；计划关联不是当前文件归属或删除权。设备与发行范围见 Android 实现计划 §9.111–§9.113。 §9.114 新增本机创建标记、inode/UID/父目录联合只读身份核验；二十七组新保护同轮通过，四组受影响旧摘要保护分轮通过，既有本机发布二十五组和账本二十组完整复验通过。目标红须为编译后的正常断言，同 APK Core 绿、复原绿与 SHA256；记录无参考时不从当前文件回填。身份与内容结果分开，不自动修改原任务、删除或重放。最终设备和发行范围见 §9.114，不增加 CI 20 份。
`android-namespace` 用 `ANDROID_FILE_NAMESPACE_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.FilePastePreflightTest,com.hanxi.ghosterm.RemoteFileTasksProductTest bash scripts/test-android-ssh.sh` 手工执行六组，护未知关系默认阻断、本次请求确认归属、已知子目录仍拒绝、执行前重验传参、旧页面确认失效与按钮路由。每组真实编译后目标断言红、同 APK 独立 Core 配置对照绿、恢复绿与 SHA256；不同远端身份仅经用户本次明确核对继续，不把 UNKNOWN 改成 SEPARATE、不继承到重试或历史。受影响的旧预检父/子 UNKNOWN 两组另复验；不增加 CI 20 份，设备与发行范围见 Android 实现计划 §9.110。
`android-dns` 以 `ANDROID_SSH_DNS_MUTATE=1 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHDnsResolverTest,com.hanxi.ghosterm.SSHDnsWireTest bash scripts/test-android-ssh.sh` 手工执行；§9.109 的 23 个不同组分轮取证（19＋2＋2，最后另复跑旧两组）；当前目录共 36 组，后增十三组及原平台两组的同轮复验见 §9.116。旧轮护系统 DNS 交付/错误分档、数值地址/scope、UTS46 非过渡转换与校验、快照/上限、取消/迟到应答和产品文案。每组编译成功后单条目标断言红、同 APK Core 配置对照绿、恢复绿与 SHA256。真实 DNS 用随机 qname 的本轮标记 UDP NAT 规则，只在可 `su 0` 的模拟器运行，finally 及 runner 清理；`mutation-check-android-dns-cleanup.py` 另验跳过删除的隔离内存变异、独立标记保留与生产 SHA 不变。最终 API 29 / 36 相关 34 项通过，不含真实 VPN/Private DNS、网络切换、硬件或原用户域名复现；不增加 CI 20 份，分轮范围见 Android 实现计划 §9.109。
`android-ssh-network` 用 `ANDROID_SERIAL=模拟器 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHNetworkTest ANDROID_SSH_NETWORK_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 14 组，护候选推进/去重、TCP 宽限/总预算、协商及提示等待边界、快照、地址/scope、产品传参和取消；目标红、同 APK 配置对照绿、恢复绿与 SHA256。12 项定向使用真实 SSH 和设备回环双栈代理；TCP 丢包要求模拟器可 `su 0`，仅向测试地址/端口添加本轮独立标记的规则，测试及 runner 的 finally 清理（不承诺 SIGKILL 后仍能执行清理）。当前 `ANDROID_SSH_ALL=1` 包含此组，也要求该模拟器条件。`mutation-check-android-network-cleanup.py` 另在真实规则上验隔离内存变异、独立标记对照及生产文件 SHA 不变；这些是手工证据，不增加 CI 20 份。该候选切片不覆盖真实域名、外部 IPv6、通道打开取消及整套 A3 网络矩阵，见 Android 实现计划 §9.40；后续通道工作区状态单列于 §9.41。
`android-files` 用 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-files.py` 手工验证 18 组：真实本机文件/SAF 树选择器、选择/过滤/失败/代次、持久授权/撤权、预览限额/解码/链接、会话保持及流/fd 释放。编译后目标红、同 APK Core 配置对照绿、恢复绿与 SHA256；新增 14 项设备定向包含于完整 214 项回归（API 36 arm64 / 4 KB 页模拟器），见 Android 实现计划 §9.43。远端文件页面后续由 §9.52 接入；本片当时未接复制任务，后续执行器与产品接线见 §9.69–§9.70。不增加 CI 20 份覆盖，此处仅登记本片已执行范围。
`android-ssh-channel` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHChannelTest ANDROID_SSH_CHANNEL_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行十三组：channel/PTY/shell 取消、剩余预算、超时/拒绝/断线映射和产品取消。先用 `bootstrap-ssh-channel-tests.sh` 准备仅测试使用的隔离依赖；协议故障端明确观测请求阶段，真实 PTY/PID 仍由独立 OpenSSH 夹具验证。`mutation-check-ssh-shell.py` 另验 1 C + 3 Swift，`mutation-check-ssh-shell-unavailable.py` 验无 libssh2 的显式错误；均编译后目标红、独立对照绿、恢复绿与 SHA256。证据见实现计划 §9.41，未增加 CI 20 份覆盖；这里只登记新增验证范围，不改变原验收要求。
`android-ssh-product` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_PRODUCT_MUTATE=1 bash scripts/test-android-ssh.sh` 手工验证 15 组产品/容器变异：默认工厂、密文/AAD/清理/提交状态、最终目标准入、用户名与预算、首页/拒绝/指纹和多会话路由。编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；不新增 CI 覆盖。共享 SSH 零超时修复及五组 SSH 变异见 Android 实现计划 §9.27；这些结果不替代该节剩余 A3 产品验收。
`android-identity-import` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHIdentityImportTest ANDROID_SSH_IMPORT_MUTATE=1 bash scripts/test-android-ssh.sh` 手工验证七组：真实系统选择器的旋转请求保留、旧结果归属、连接/管道取消、只读打开、大小限制与取消后等待状态。编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；五项定向用例经真实 SAF 管道、Keystore 和隔离 sshd，不新增 CI 覆盖。失败与复验、事务已提交和第三方 provider 的取消边界见 Android 实现计划 §9.28–§9.29。
`android-credentials` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHCredentialFlowTest ANDROID_SSH_CREDENTIAL_MUTATE=1 bash scripts/test-android-ssh.sh` 手工验证七组：加密密钥重试、口令提示与传递、密码传递、旋转/方式切换及取消后秘密清理。编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；密码夹具使用一次性 Docker 账户，不改宿主密码，完整 `ANDROID_SSH_ALL=1` 同样需要 Docker。共享错误分类另用 `python3 scripts/mutation-check-ssh-core.py credentials` 验一组真实 sshd 变异，配置对照绿；原连接器 18 组独立回归。各次失败/恢复与设备范围见 Android 实现计划 §9.30–§9.31，不新增 CI 覆盖。
`android-identities` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.IdentitySettingsTest ANDROID_SSH_IDENTITIES_MUTATE=1 bash scripts/test-android-ssh.sh` 手工验证十组：OpenSSH 指纹/RSA/EC、过期删除、Keystore 删除、损坏条目、设置入口、旧选择器结果、管道取消与移除确认。编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；测试清理必须独立于被变异的解析器，不能将残留数据造成的失败冒充目标红。八项定向用真实 DocumentsUI/Keystore/SSH 与 ssh-keygen 指纹对拍；仅元数据验证不等于全部密钥格式的 SSH 认证已验。证据与当时余项见 Android 实现计划 §9.32–§9.33，后续主机引用接线另见 §9.35；主机验证管理单独见 §9.39，不新增 CI 覆盖。
`config-identities` 以 `python3 scripts/mutation-check-config-identities.py --apple` 手工执行：8 组隔离 C 变异护 IdentityFile 值/来源、累积范围、显式与继承、Include 只读和 Match/非法参数拒绝；1 组 Swift 变异护来源转换。`mutation-check-android-config-identities.py` 在指定设备另验 3 组 JNI/Kotlin 变异，目标红、同 APK 配置保存对照绿、恢复绿与 SHA256。查询和绑定证据不替代主机身份选择/引用页面的独立验收；两脚本不增加 CI 20 份覆盖。
`android-host-identities` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.HostIdentityTest ANDROID_SSH_HOST_IDENTITIES_MUTATE=1 bash scripts/test-android-ssh.sh` 手工验证十组：身份值引用、非目标行保护、请求值校验、身份脏状态、配置候选隔离、仅本次选择、引用清单/读取失败、来源失败及活动会话保留。编译后目标红、同 APK 配置对照绿、恢复绿与 SHA256；完整产品路径使用真实 Keystore/SSH，按 UUID 和远端 PID 验改名/移除不关闭已建立连接。配置引用仅对本 Android 安装的容器有效，不等于跨端密钥迁移。范围与各次复验见 Android 实现计划 §9.35；不增加 CI 20 份覆盖。
`known-hosts-snapshot` 以 `python3 scripts/mutation-check-known-hosts-snapshot.py --apple` 手工执行：十组 C 变异护原始行/字节范围、marker、形态/哈希标记、畸形行可见、NUL/限额与错误输出清理；一组 Swift 变异护字节范围转换。`mutation-check-android-known-hosts.py` 在指定设备另验八组 JNI/Kotlin 变异：原有四组护 marker、原始范围、未识别状态及 UTF-8 拒绝，新增四组护删除索引、状态、已提交与冲突结果；目标红、独立 API/配置对照绿、恢复绿与 SHA256。读取形态不等于信任，绑定不等于管理页面；范围见 Android 实现计划 §9.36–§9.37，不新增 CI 覆盖。
`known-hosts-store` 以 `python3 scripts/mutation-check-known-hosts-store.py --apple` 手工执行：17 组 C、2 组 Swift 变异护精确删除、旧快照冲突、只读规则、备份/写入/同步/关闭/替换结果、提交状态、链接及线程/进程协调。基线包含 20 种独立文件故障与真实锁竞争；每组编译后目标红、同变异配置对照绿、恢复绿与 SHA256。生产 TOFU 与移除共用事务；同步/异步真实 SSH 另验确认期间规则变化、同/不同身份同时接受、TOFU 与移除相撞及限额。仍为手工证据，不新增 CI 覆盖，不宣称非协作写入或断电恢复已验证。
`android-known-hosts-settings` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.KnownHostsSettingsTest ANDROID_SSH_KNOWN_HOSTS_SETTINGS_MUTATE=1 bash scripts/test-android-ssh.sh` 手工验证 17 组：入口/确认、删除索引、快照复制、公钥指纹/算法、只读规则、移除资格、文件类型/父目录链接、可见转义、读取失败、晚到读取、提交/冲突/未知结果和离页期间写入。编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256；空指针或编译失败不算目标红。11 项定向含真实 SSH 指纹对拍、移除后新连接重验及旧会话 PID/IO 保留；故障结果映射的 fake repository 与共享 C 真实文件故障分别记账。设备不允许创建硬链接，本片不声称已验 Android 硬链接读取路径。范围见 Android 实现计划 §9.39，仍为手工证据，不增加 CI 20 份覆盖。
**Android 远端入口与自动密钥（§9.154）**：`ANDROID_REMOTE_ENTRY_MUTATE=1` 配合 `scripts/test-android-ssh.sh` 与实施计划中的目标选择，运行 `mutation-check-android-remote-entry.py` 十组编译变异。远端/本机路由、不可变主机快照、配置身份自动认证及安全候选策略均目标红、同 APK Core 绿、恢复绿与 SHA256；API 29 30 项、API 36 / 16 KiB 页最终 13 项同源专项通过。Core 49/94、签名 release 与四模块 lint 通过；不借旧915项记新全量，不增加 macOS CI 20 份。

**Android 记住成功密钥（§9.172）**：`ANDROID_REMEMBER_IDENTITY_MUTATE=1` 配合 `scripts/test-android-ssh.sh`，并在 `ANDROID_SSH_TEST_CLASS` 选择 `RememberedSSHIdentityTest,RemoteFilesProductTest`，运行 `mutation-check-android-remember-identity.py` 四组。保存、连接范围、成功条件与取消勾选均须编译后目标红、同 APK Core 对照绿、恢复绿与 SHA256；夹具直接恢复原偏好快照，不调用被变异的存储方法清理。仍为手工证据，不增加 macOS CI 20 份，不借旧完整套件记新全量。

**Android 最终快照全量（§9.152）**：干净 `6deb04b` 在 API 29 / arm64 / 4 KiB 页及 API 36 / arm64 / 16 KiB 页各默认未筛选 915 项通过，零失败/错误/跳过；两端同一 903 份输入前后不变，具名发现/执行集合与实际安装 APK 摘要匹配。为模拟器 debug instrumentation 全量，不等于实际 release 全量、物理手机、TalkBack 或全部外部提供方/服务器矩阵；新增十五组无障碍变异按 §9.149 单独记账，不增加 macOS CI 20 份。

`android-terminal`（`python3 scripts/mutation-check-android-terminal.py`）当前有 25 组，按各轮记录在指定 arm64 设备运行：VT/PTY/输入法/Canvas/生命周期目标红，同 APK 独立 Core 配置对照绿，恢复绿与 SHA256。静止画面闪烁另有 T22–T24：真实窗口像素护重绘、闪烁相位与 native 标记，§9.19 与 T8/T19 共选跑五组；不称该轮全量 24 组重跑。新增 T25 护同帧真实 PID 捕获：编译无效 PID 后，必须在指定检查点普通断言红、同 APK Core 对照绿、恢复绿与 SHA256；本轮只选跑 T25，不称全 25 组重跑（§9.146）。`android-text` 当前共 87 组，各轮按同设备/同 APK 对照执行，护 VT 粘贴、选区格式化/坐标/高亮、复制/确认/拖动与本地清屏（含上游隔离补丁），以及按下/重复/释放、失焦/换 surface 归属与辅助键成对点击，以及手柄边缘持续滚动/停止、跨越锚点/重新抓取、最近手柄命中与 Ctrl 单次/锁定/失焦/会话隔离；修饰键/锁定位、数字键盘导航身份和组合音符/设备/IME 隔离由 §9.18 的新增 16 组验证；其他新增组护 Ctrl 标点/Unicode、键盘协议模式与布局字符传递、非法字符显式拒绝、辅助键路由与输入法单独 LF 的 Enter 语义。新增 X73–X87 的十五组仅本轮选跑通过，护终端系统无障碍节点/焦点、隐藏文字和有界视口正文（§9.149）；不称整个 87 组重跑。`android-vt-libc` 用 NDK 真实 archive 验两组分配器替身守卫，再以缓存头损坏验第三组头文件收据守卫，archive 校验为独立对照。均是手工证据，未纳入上述 macOS CI 20 份。各轮完整或选组执行的数量与范围见 Android 实现计划 §9.13–§9.19，不能把切片通过当作整个 A2 完成。
`android-performance` 用 `python3 scripts/mutation-check-android-performance.py` 在指定设备跑四组：延迟输入取帧、破坏实际背景像素、丢弃帧指标和丢弃 IME 提交均须目标红，同 APK Core 配置对照绿，恢复绿并核验 SHA256。第三组验证测量缺失不会被报告为零成本；它不替代 renderer 功能断言。输入像素超时按同负载 PixelCopy/轮询标定，保留绝对下限；吞吐/帧耗时/CPU/内存为设备档位数据，不设跨设备的固定 P95 承诺。均属手工证据，未纳入 macOS CI 20 份；执行范围见 Android 实现计划 §9.21。
Android API 29 兼容入口为 `bash scripts/check-android-api.sh`：app/bridge/data/terminal
分别执行 release lint，避免 app 单模块漏掉依赖库的高版本 API。`mutation-check-android-api.py`
在真实 APK 编译成功后，验证 data 的 API 30 调用被该入口精确拦下，同变异 terminal lint
对照绿、恢复绿与 SHA256。`mutation-check-android-fd-flags.py` 的九组分轮手工证据护
非阻塞/恢复、追加、访问模式、原槽位 CLOEXEC、临时 fd 释放与 errno；设备档为 API 29 / arm64。
`mutation-check-android-ime-finish.py` 当前七组手工证据护接受组合、关闭取消、Enter/编辑器动作、
失效连接与键入语义，第七组护单 LF 回车前接受组合；七组完整复验已通过，raw PTY 证据属于 API 36 / 4 KB 页，不能借给 API 29。
这三份变异不计入 macOS CI 20 份；新增 API lint 已在 ca6f104 的 Android release CI 实际运行通过；
本轮旧终端 24 组已完整复验通过；文本回归第 30 组恢复态失败已归档，已修正选区观测阶段并用新 X72 证明行断言有牙；相关六组（含原失败 X30、新 X72）已复验通过，其余 41 组文本也已通过（共 72 个不同变异分轮通过），暂存 23 组、本机发布 25 组、SAF 发布 30 组及实时复制进度 25 组已复验；身份导入 7 组和四模块 release lint 也通过。新完整设备回归 498 项全部通过、零失败/错误/跳过，默认发现与实际具名开始/结束集合一致，两份安装 APK 摘要匹配，753 份输入集合/SHA256 不变；仅 API 36 arm64 / 4 KB 页，不借给 API 29 输入法或整个发行矩阵。范围和未通过的历史轮次见 Android 实现计划 §9.79。

`android-ime-window`（`python3 scripts/mutation-check-android-ime-window.py`）当前 18 组手工证据护全局 UTF-16 输入坐标、局部替换/光标、finish 选区/标记、已发送字节唯一性、Unicode 拒绝、尾部交付与按键恢复（§9.81）。`android-preferences`（`python3 scripts/mutation-check-android-preferences.py`）当前 32 组分两轮手工证据护 16 项偏好存储的格式/类型/版本、拒绝与未知三态、发布回读、原型 ID、串行更新、失败保留与文件边界（§9.82）；原第 17 组并发测试未变红被拒收，改成实际 IO 执行轮次屏障后完成目标断言红/同 APK Core 绿/恢复绿与回滚。最终存储 15 项在 API 29 arm64 / 4 KB 页通过，仅证明存储，不代表设置页面、已有 surface 应用或完整 A7。六份源码已随 `272d140` 提交，组合四模块 lint 通过；该组合完整 523 项已通过、零失败/错误/跳过（1526.785 秒），761 份输入集合/SHA256 与实际安装两 APK 摘要一致，范围仅 API 36 arm64 / 4 KB 页。本地签名 release 的 16 KB 页覆盖安装/原文件预览及连续两条 ASCII 系统键盘命令专项另通过；不替代完整输入法/发行矩阵。这两份仍未纳入 macOS CI 20 份。`272d140` 的签名 release CI 41269551404 已成功，实际下载包 5,906,513 字节及固定证书/摘要/16 KB 对齐均已核验；同云端包 16 KB 页的升级保留文件预览与连续两条 ASCII 系统键盘命令专项通过。旧完整 498 项属于 ca6f104；A7 页面及消费者仍在隔离验证，未进入该 release。

**基础与产品变异验证**（每份都必须能独立跑绿）：sshconfig / include / settings /
settings-ui / tab-chrome / workspace / tabs / chrome / pane-chrome /
auth / login-shell / transport / ssh / sftp / exec / files / transfers /
dragdrop / terminal / pixel / focus / split-dim / occlusion / known-hosts /
package-mac / icons / ghostty-patch / program-status / split-divider / public-headers / pty-inheritance / app-update / default-entry / ssh-auth / ssh-credentials / tmux / tmux-ui / tmux-ssh / install-macos / sidebar-hover。
`tab-chrome` 验证单个原生窗口中的顶部 / 侧边导航、真实红绿灯避让、无顶部空带、唯一加号（顶部固定右端 / 侧栏紧随最后一个标签）、
标签 UUID 选择 / 关闭确认 / 重排 / 右键、唯一 sheet 与窗口关闭路由；用两个真实 shell
验证布局切换保留宿主、surface、PID、窗格焦点状态与 AppKit firstResponder。旧 `native-*`
五份脚本随多窗口标签组整体退役，等价业务断言迁入本脚本，不再读取 AppKit 私有 ivar。
CI 在 macOS 15 执行，本机 macOS 26 也须验证；未运行的系统不能记为通过。
（`package-mac` 另护「**签名档位必须按证书真实的类记账**」：`--sign` 原先把任何身份都记
成 `id`，于是随包 `BUILD-INFO.txt` 与 `version.json` 的 `signature` 会谎报 Developer ID——
而那个字段正是 app 侧 `RemoteBuild.installBlocked` 判断「能不能自动装」的唯一依据。
A8 打 `package-mac.sh` 的分类器（`Developer ID Application:` → `id`、`Apple Development:`
→ `dev`、其余身份当场拒；`dev` 档配 `--notarize` 必须先拦下来）。背景：免费个人账号的
Apple Development **能签但不能公证**，与 ad-hoc 一样被 Gatekeeper 拦，只多买到「签名身份
稳定」这一件事，所以它不构成第三条分发路（口径写在 `docs/how-to/dev-build.md`）。
签名分类的判据是「**红在哪句文案**」而不是退出码——分类器拒的、钥匙串缺的、公证守卫拦的
三类红用退出码分不出来，故新增 `expect_red_saying`；同一轮补齐了起跑前的变异标记扫描
与 `gt_timeout`（原先这份脚本既没扫标记也没用超时）。
A10 护**另一条分发链的下界**：Homebrew cask（独立公开 tap `hanxi/homebrew-ghosterm`）声明的
资产名必须追得上 `package-mac.sh` 的改名——平台/架构/渠道命名变动而 tap 忘改，用户拿到的是 404，
不报错、只在装机时才发现。判据落在 `scripts/check-dev-cask.sh`（对拍 zip 名 / `:latest` /
`sha256 :no_check` / dev 直链 / arm64 与 macOS 26 下限），变异打的是打包脚本里的资产名常量；
指向性对照是「同一轮 `SHA256SUMS` 仍自洽」（那份清单是现算的，自适应改名——红的必须是新
下界而不是旧断言）。**cask 不解决 Gatekeeper**：实测 Homebrew 自己给下载物贴
`com.apple.quarantine`（agent `Homebrew Cask`），首次打开与浏览器下载同样要放行一次；
决定拦不拦的是「标记在不在」，不是标记里的 flag 值。）
（`package-mac` 的 A9 现护普通引擎与旧 runtime 清理：重新签名后塞入旧 helper 目录，
新包校验仍红，而既有依赖校验仍绿；旧 profile 收据、旧 p0 头也分别目标红、对照绿、复原绿。
`check-terminal-package.py` 复用普通引擎构建器的验证，核验实际 archive 哈希与三份补丁收据。
打包变异起跑前检查 Pillow，不能拿图标对照缺依赖冒充有牙；EXIT/INT/TERM 都恢复并比对 SHA256。
另调用 `mutation-check-package-dmg.py` 跑三组真实只读 DMG 变异：普通卸载忙时强制卸载、
卸载失败保留临时目录、命令返回成功后仍核验挂载状态。任何递归清理之前必须确认本次验包
挂载已卸载；清理钩子保留原始失败状态，不得吞掉卸载错误再谎报成功。）
（`install-macos` 用 `python3 scripts/mutation-check-install-macos.py --archive 本轮.zip` 执行，
在工程 `tmp/` 的隔离目标里安装真实签名包，不触碰 `/Applications`。六组变异护 SHA256、
应用标识、签名完整性、递归清除 quarantine、替换失败恢复与平台/运行中保护；每组目标红、
独立对照绿、SHA 回滚。dev/full 打包后都用本轮 ZIP 运行，验证当前脚本与本轮产物配套。）
（`tmux` 以 `python3 scripts/mutation-check-tmux.py` 跑 27 组隔离变异（当前脚本数量）：缺失判定、
重复/UTF-8 解析、未知策略/结果、明确拒绝、版本、format/分号/shell 引用、身份、禁止 attach 起新 server、
尺寸接线、丢失本地退出状态、关闭标准描述符、子命令信号隔离、取消及探测错误分类。目标断言红、同变异下独立对照绿、SHA 回滚；make 整秒时间戳下强制重链，
不让旧可执行文件冒充变异结果。普通离线 gate 只跑命令与恶意输出夹具；真实本地/SSH
八模式（含已有空 server 与 owner 被 SIGKILL）另以 `test-tmux.py` / `test-tmux-ssh.py` 执行，缺 tmux 时明确失败，不算通过。已与 `tmux-ui` 纳入 CI full 档；Linux 与真实 SSH/断网仍是手工证据。）
（`tmux-ui` 用 `python3 scripts/mutation-check-tmux-ui.py` 验新格子不覆盖任务、detach 保留
断开格子、关闭只断开客户端、未知文案、探测错误分类、路径变更清选择、鼠标输入及拒绝覆盖运行格子等共十三组（以现有脚本枚举为准），含 runner 长路径下 Unix socket 的真实绑定。使用真 tmux +
full surface，断言内层任务 PID 不变；旧绑定不提供结束入口。`auth` 现复用同一异步认证，
当前共十一组隔离变异：原六组在真 sshd 上护 tilde、没口令的钥匙、服务器方法表、撤销提示、Swift 撤销枚举映射与保存失败提示路由；新增五组护连接配置准入、目标/主机/独立 SFTP 传值与来源。拒绝配置在 DNS 前停止，SFTP 产品入口另验撤销/不支持规则不请求信任或凭据，不再变异已经删除的认证副本。）
（`tmux-ssh` 用 `python3 scripts/mutation-check-tmux-ssh.py` 配合隔离真 sshd 运行，六组变异护
PTY、退出未知、变更结果未知、总 deadline、取消与背压尾队列；目标红、同变异下 lifecycle
仍绿、回滚后目标绿并核对 SHA。TCP 代理以非阻塞双向缓冲保留密文，静默断网与恢复不丢
中间字节；3 MiB 负载必须由远端任务回报完整 SHA256。属于手工证据，未计入上述 CI 20 份。）
（`known-hosts` 通过 shell 入口调用隔离 Python runner 跑 25 组：23 组经真实 ed25519/ECDSA sshd，护算法过滤、多记录、裸名、标记、撤销与顺序、CA/未知/哈希拒绝、通配/否定、读取错误、同步/异步消费及保存结果；另含确认期间重新验证、去重、撤销/冲突状态与记录限额。K17/K20 随旧追加实现退役改用真实文件事务的关闭/打开故障注入，保留独立配置对照；不把 RLIMIT 写失败当作关闭错误证据。每组清洁构建，目标断言红、同变异配置对照绿、复原绿与 SHA256；真实 SSH 项遇 SKIP 或缺结束标记拒绝通过。仍为手工证据，未纳入 CI 20 份；共享事务边界见 `core/tests/README.md`，不宣称任意外部写者安全或已验证断电持久性。）
（`ssh-auth` 保留公开非阻塞认证的八组翻译变异；`ssh-credentials` 保留方法表、取消、请求归属、
拒绝凭据清理、服务端方法掩码与未知格式六组变异。它们独立于旧安装器和 daemon 探针运行。）
（`app-update` 护 dev 版**更新检测**这条链——七组变异，每组都要让对应断言变红并带一条
同变异下仍绿的指向性对照：U1 把「本机没有 commit 记录」折成「已是最新」（铁律 #6）、
U2 把主锚从 commit 换回版本号（`build` 按 workflow 文件计数，跨轨不可比）、U3 解码时给
`signature` 补默认值（缺了就当能自动装）、U4 `installBlocked` 恒 false、U5「查不出来」的
界面文案写回「已是最新」（**文案本身就是断言对象**，谎报即红）、U6 关于窗口那一行整个
不画、U7 多给一颗「安装更新」按钮。入口的观测口径是**几何**：实测 SwiftUI 的 `Button`
在这棵树里不落 `NSButton`，`accessibilityLabel()` / `accessibilityIdentifier()` 也不下发到
NSView（SwiftUI 自绘，a11y 元素只走真 AX API），本机 macOS 26 上的句柄是 `KeyViewProxy`（一控件一个）
与 `_FocusRingView`；测试也接受原生 `NSButton` 并直接量其边界，兼容其他系统的控件实现。于是「有几颗按钮 / 画在不在框内」看得见，「按钮上写的什么」看不见，
后者由 `UpdateCheckTests` 的 `statusText` 断言兜住（U5 正是打在那里）。）
（`public-headers` 用 `python3 scripts/mutation-check-public-headers.py` 执行，
护子头→聚合头、隐藏聚合类型依赖与子头环路；变异目标红、独立 config 头对照绿。
旧私有恢复/P0 探针已随其能力整体退役；本闸门继续验证实际公开头。）
（`pty-inheritance` 用 `python3 scripts/mutation-check-pty-inheritance.py` 执行，
护 master/唤醒管道的 CLOEXEC 以及 exec 前隔离调用方描述符：后开的 shell 若继承
前一条会话的 master，父进程关 master 也无法触发 hangup，close 的 waiter join
会卡死。回归用真 exec 子进程检查描述符，且第二个 PTY 活着时第一个必须秒关；
三组变异在独立 core 副本中分别破坏 CLOEXEC、子进程 close 与信号屏蔽隔离，API 对照仍绿。
信号回归验证调用线程屏蔽 SIGWINCH/SIGUSR1 时，exec 子进程收到两次尺寸变化通知，
调用线程屏蔽状态不变；`--apple` 配隔离 loopback sshd 另验真实窗口 → 本地 shell →
系统 ssh → tmux 的无输入缩放，原生目标红、主机列表对照绿、恢复绿与 SHA256。
此项仍为手工证据，不增加 CI 20 份覆盖。）
（`sftp` 护文件操作原语（stat 尺寸 / 列目录名 / 下载内容 / job 三态 /
`remove_all` 递归 / `chmod` 模式位 / `walk` 深度 / `pause`）；`exec` 护远端
命令通道（stdout-stderr 分离 / 退出码）——两者都跑真 sshd 的 `ssh_e2e`。
`files` 护文件浏览器后端与面板模型（非递归删非空必须失败 / walk 深度 /
list 不过滤隐藏 / 目录优先排序 / 双栏接线）**与面板顶栏那两条用户实测
反馈**（F10~F12：顶栏工具条排不下必须折行——退回一行 `HStack` 就是用户报的
「顶部按钮超框了」，按钮被画到框外、不报错；给了 `onClose` 就得画得出 ✕
关闭按钮——没有它面板只能靠 ⌘⇧E 开合。后两条的判据是**像素**；F13 护**条目
右键菜单上的快捷键**：菜单标的键必须倒推回 `PanelKeymap`——标错就等于菜单
承诺了一个按不动的键，比不标更糟；只在 macOS 14+ 标，13 上没人接键），
`transfers` 护全局传输队列
（进度总量 / 失败不折叠 / 冲突 skip / 暂停路由），`dragdrop` 护拖放路由
（远端→本地=下载的方向 / 转义往返 / 完成回调），三者跑 `swift test`。）
（旧 `band-controls` 与 `sidebar-chrome` 已并入 `tab-chrome` 的 navigation / menu / sidebar 组。
无切换按钮、唯一加号取代旧按钮顺序规则；配置错误必须可见，设置 / 传输 / 关于和分屏菜单必须可达。
仍保留实际标签高亮、红绿灯避让、清除系统标题和普通鼠标横向滚轮的回归证据。）
（`focus` 护「分屏只有当前窗格的光标才闪」——焦点从 SwiftUI 下发到 libghostty
的 surface；漏下发时**屏幕上不报错**，每个格子都停在引擎「默认已聚焦」上
一起闪。断言用 DEC 1004 焦点上报的 `CSI I`/`CSI O` 字节做观测，变异 F1~F3 有牙。
F4/F5 另护**键盘落点**（AppKit firstResponder）：它与「报给引擎的焦点位」是
两件事，只做后者就是用户实测的「开终端 / 分屏后光标在新格子闪、敲字却进了上
一个终端」——后台标签页的 surface 保持挂载，没人把 firstResponder 撤下来。
新窗格是「先置当前、后进视图树」，故 `paneFocused` 翻转与进树时都要要一次键盘。）
（`split-dim` 护「非聚焦窗格变暗」——盖的是 `unfocused-split-fill`（未设回落
`background`）、alpha = 1 - `unfocused-split-opacity`（上游口径，≥1 即关掉）；
纯规则穷举 + 端到端抓像素，变异 D1~D3 有牙。）
（`occlusion` 护「后台标签页停画」——surface 保持挂载但不显示时，把可见性报给
引擎让它停掉渲染线程（窗口 isKeyWindow/遮挡状态一变就重新同步）；断言用 DEC 2033
可见性上报的 `CSI ?999;1n / ;2n` 做观测，变异 O1~O3 有牙。）
（`transport` 护「本地 PTY 的子进程真的拿到控制终端」与「前台作业判定
（空闲=IDLE / 有作业=JOB / 查不出=UNKNOWN 不折叠）」——它连着那个潜伏 bug：
父进程抢跑 setpgid 会让子进程 setsid 憋成 EPERM、整个会话拿不到控制终端。）
（`transport` 还护「**写绝不无限等**」：pty 输入队列满（前台程序不读 stdin）
时写要在有界时间内返回、积压字节随后补送——旧实现的 `poll(...,-1)` 会把会话
线程钉死，读循环出不来（终端冻住）；旧 `stop()` 的 `queue.sync` 还会把主线程钉死
＝整个 app 无响应。现在 stop 只取消，IO owner 有界退出后异步释放；Swift 另断言 owner 真正释放，不能把无限写藏进后台。变异 P4 有牙：回到 `poll(-1)` 即整条套件卡到 timeout 变红。）
（`ssh` 护「**SSH shell 写也不无限等 + 不丢字节**」：与本地 PTY 同结构——
`gt__ssh_shell_write` 在 `GT_SSH_WRITE_GRACE_MS` 内尽量送，剩下的进 `wtail`
队列由后续 read/write 补送；timeout 由写路径自己设、自己还（`get_timeout`
保存、`set_timeout` 还原），不打扰共用同一 session 的 SFTP。读路径每轮也
flush 一次 wtail，当尾队列的泵。契约见 `transport.h`：「输入不丢，只是晚到」。
已知 e2e 限制：`sshd` 排空速率 ≈ 40 MiB/s（环回实测），3 MiB payload 写完时
队列**根本没形成**——flush 机制无法被真 sshd e2e 触发。test_ssh §5b 只断言
「写有界（elapsed<3s）+ 不丢字节（rc=GT_OK）」；队列机制本身由代码审阅 +
与 PTY `wtail` 的结构对称性保证（见 `core/src/io/pty_posix.c` 的 wtail + P4）。无独立变异
脚本——与 PTY P4 是同构的 wtail 实现，PTY 的 12/12 变异覆盖已验证同一套机制。）
（`terminal` 护「字节真的进了网格」（feed 被短路 → 网格断言红）、「浅/深色切换
下发到**已有** surface」——后者是用户实测「切深色终端没刷新、新开的才对」：
每个 surface 各自持有条件状态，app 级事件改不到它们，必须逐个下发 + 消费
`reload_config` 动作，断言用 DEC 2031 配色上报观测——以及「macOS 输入法真的接上
了终端」（用户实测「敲不出中文、输入法不弹」）：视图必须实现 `NSTextInputClient`
且 `keyDown` 走 `interpretKeyEvents`，组合串经 preedit 内联显示、提交走**键入**
语义（`TerminalIMETests`，观测口径是 libghostty 的 `ime_point` 宽度与 transport
字节）。变异 T1~T8 有牙，T8 另护输入法提交的控制字符原样到达 transport，避免吞掉 tmux 前缀。）
`terminal-keyboard` 以 `python3 scripts/mutation-check-terminal-keyboard.py` 手工执行三组隔离变异：原功能键组护 AppKit 功能键占位符不得作为可输入文本，真实 NSEvent 经 full libghostty 验普通、应用光标及 Kitty 六种 flags，共 90 个功能键场景；新增两组护输入法 ASCII 提交保留 Control、空提交不吞控制键，以重放组字/删除/提交回调验证最终 `02 6e` 字节，不等同于真实系统输入法切换或 tmux 窗口切换验收。编译后目标断言红、同变异中文提交对照绿、恢复绿与 SHA256；终端测试纳入既有 terminal gate，变异不增加 CI 20 份。真实 SSH/Codex 输入框与本轮回调验证范围见 `docs/reference/optional-tmux-implementation.md`。

`kitty-macos` 以 `python3 scripts/test-kitty-macos.py` 手工验证八项协议/像素检查：
查询、分块 PNG、实际图像像素、删除、损坏 PNG 拒绝、动画帧接收/切换与自动播放。
隔离生产 TerminalSurface 丢弃图形序列后须重新编译，八项目标红、同变异文本入网格
对照绿、恢复编译后绿与 SHA256。仅原生窗口/full libghostty 证据，不代表完整 App、
PTY、SSH/tmux 或全协议验收，不增加 CI 20 份；用法见 `terminal/apple/README.md`。

`shell-mouse-recovery` 以 `python3 scripts/test-shell-mouse-recovery.py --mutate --codex --ssh`
手工验证：真实 full libghostty / PTY / tmux 复现窗格内程序异常退出后的鼠标模式残留，
Bash/Zsh 提示符集成自动清理，重连与新 surface 不再产生序列文本输入，正常 TUI 仍实际
收到鼠标事件。八组隔离变异护清理、重复加载、原提示符钩子/退出码和启动文件备份/保真；
目标红、同变异独立对照绿、恢复绿与 SHA256。系统 SSH 经隔离 loopback sshd 跑同组场景；
Codex 使用本机真实 CLI 输入框，不提交测试草稿，未复现不得记为修复。
通知注入及进程暂停不等于整机休眠；远端真实主机及硬件休眠另列证据。
这是实际 shell 的集成（远端须在远端安装），不在唤醒时强制重置终端；不新增 CI 20 份覆盖。
2026-11-05 普通本地 zsh 已接随包自动恢复，`.direct` / 自定义启动参数（仅 `-i` 除外）/ 其它 shell
及远端不走自动接线；Bash/远端保留手工安装。`--ssh-hops 3 --mouse-mode clicks`
补验真实按下/松开及三层 loopback SSH；`--scenario disconnect --automatic --mutate`
验无手工钩子的生产本地入口，在服务连接中断后自动清理，原本地 PID/键盘保持，正常
TUI 真正收到点击字节。五项启动组合护 ZDOTDIR、顺序、退出码、历史路径、旧钩子去重
与用户文件不变；四组新自动变异及既有八组分轮通过。`test-shell-mouse-package.py --app`
另验实际包四组资源/加载/清理变异：正常目标红、同包配置绿、恢复绿与 SHA256。
Core 52/95、Apple 91/4、31 项宿主定向回归、默认入口四组及打包 63 项/DMG 三组通过；
未重跑完整宿主。初版历史路径、开发目录回落、Bash 3.2 启动与类型推断失败见
`docs/how-to/mouse-report-recovery.md`，未计作通过。仅手工隔离/本地 ad-hoc 包证据，
真实硬件休眠和用户远端仍待验，不增加 CI 20 份覆盖。
（`tab-chrome` 保留当前标签高亮与标签条普通鼠标横滚；`auth` 护「没口令的钥匙不被问口令」。
`tabs` 另护**标签右键菜单的落点**（T17/T18）：菜单里的分屏 / 文件面板开关
必须作用在**被右键的那条**标签上，不是「当前标签页」——右键的未必是当前的，
写错就「右键 A 却动了 B」，不报错、只在屏幕上不对。T20 再护**菜单上的快捷键
显示**：`.keyboardShortcut` 要真的落到 NSMenuItem 的 keyEquivalent 上（不然
菜单右侧什么都不画），且与 `ShortcutCatalog`（已与主菜单对拍）逐条一致。
`program-status` 护 OSC 7501/133 的五处**我们自己写的规则**：clear 的子树
删除、程序文本里不可见格式字符的剥离、OSC 133 的 kind 判别（只有
`prompt_start` 才清 working/blocked）、**角标的像素渲染**（M4 让摘要恒空 →
侧栏画不出橙色/红色）、以及**上一条命令的失败记号**（M5 把条件写死 →
标签胶囊画不出红点）——各自配一条同变异下仍绿的对照。
（`pane-chrome` 护「窗格外观 + 缩放 + 只有一个加号」这批纯外观接缝：P1 间距
偏好读了却没留白（padding 加在错的层，两格仍贴着）、P2 标题栏开关没接上、
P3 侧栏展开时顶部标签条不隐（同一份标签清单画两处）、P4 顶栏与侧栏同时画
`+`（用户实测：「现在出现 2 个加号了，我只要一个加号」）、P5 缩放时把被缩放
那格的位置塌掉——兄弟窗格跟着重排，终端那边就是一轮 SIGWINCH、回卷被揉烂
（所以实现是「盖一层」：原位留一张空卡片，`layout.viewID` 不变）、P6 关掉被
缩放那格后不清状态（覆盖层盖出一片空白）。P2 的判据是**像素**；P1 量的是真视图树里
两块终端面之间空出多少点——按颜色比会假绿（浅色外观下窗口底色与窗格底色都是
近白，变异验证当场抓到）；P3/P4 已迁至 `tab-chrome`：直接量生产窗口的几何并数原生新建按钮；P5 量的是兄弟 `TerminalHostView` 的宽度。P10/P11 验证贯穿窗格顶边的焦点线：分屏当前格显示，单格不显示；有间距时恢复当前格淡蓝细框、其他格中性细框，贴边不叠整圈描边。默认间距为 0，已有用户间距偏好仍有效。）
`split-divider` 以 10pt 留白验证双轴透明手柄、命中与调比例；贴边细线及间距切换的像素由 `tab-chrome` 的 appearance 组覆盖。
`tab-chrome` 用 `python3 scripts/mutation-check-tab-chrome.py` 执行。按 `--group` 可分开验证
preferences（存储与发布）、connections（选择器、配置错误与主机管理）、navigation（导航几何与目标）、
lifecycle（终端宿主与键盘落点、关闭、sheet 与右键）、menu（删按钮后的主菜单退路）、sidebar（高亮与滚轮）、search（实时标题、搜索目标和保留终端清屏）、
panes（窗格 UUID 命令、悬停 / 键盘按钮、分屏预览的像素 / 命中 / 方向 / 尺寸 / 失焦 / 拖动互斥、真实 Shift 按键）。
appearance（默认贴边、分隔线与留白、导航与终端同色、标题去底线、贴边不叠框、留白细框、上下终端与文件混排的焦点顶边（隐藏标题仍可见）、条件主题已解析颜色、原生按钮前景色与新建标签页文字对比）也包含在 `tab-chrome` 中。
每组必须目标断言红、同变异独立对照绿、复原绿与 SHA256 一致；不能把编译失败当目标红。
测试注入独立 UserDefaults，不改用户偏好。已接入 full CI；本机与 CI 的运行结果在实施计划分别记账。
窗格预览测试在锁屏 / CI 中注入可控制 `isKeyWindow` 的 NSWindow，内容仍由生产控制器创建，
证明的是焦点接线和原生控件行为；真实系统激活、全屏与最终视觉验收须另列证据，不得混称已验证。
`workspace` 以 `python3 scripts/mutation-check-workspace.py` 手工执行 21 组隔离变异，
护布局与宿主身份、文件目标 / 焦点 / 关闭、拖动和双击接线。标题栏带的命中采样必须
转换到父视图坐标，并先证明命中终端与文件拖动把手；局部坐标直接传 hitTest 会测到底部而假绿。
文件双击要经生产窗口安装的 action / target 真正打开目录；仅判断不等于空 selector 既漏 nil，
也发现不了 NSObject.perform 的重载选错。安装缺失与错误 selector 分别变异，独立焦点对照须绿。
本脚本仍属手工证据，未计入上述 CI 20 份。
`settings-ui` 护设置弹窗的数据面：配色两档的序列化（拼错整份配置就废）、
`/etc/shells` 的解析、**快捷键表与真菜单的双向对拍**、**下拉控件的定宽**
（U6：回到 `maxWidth` 就会随选中值伸缩，短值 93pt / 长值 190pt）、
**外观页下拉右边的配色色块真画得出来**（U7：色块塞回 `Menu` 的 label 就被
AppKit 收编吃掉——用户报的「主题预览没显示」；断言数的是色块背景色的像素）、
**改外观偏好要立刻落到 `NSApp.appearance`**（U8：删掉 `didSet` 里那半截，
界面就要重启才变——用户报的「切深/浅色界面不动」）、以及**设置窗口底色要
跟着外观走**（U9：底色回到 `viewDidLoad` 里 `.cgColor` 求死，动态色按
`NSAppearance.current`（系统外观）解，系统浅色 + 选深色会解成白底 + 浅字——
用户报的「设置界面看不清」）。）
手法要造出**目标失效状态**：

- 验「无损往返」必须改被插值的**期望值**，不能改两侧一起变的常量；
- 验「agent 不产生假绿」要用**双断言**（同一变异下闸门红 + 另一条仍绿，证明红有指向性）；
- 变异脚本必须：**起跑前扫变异标记**（命中即 `exit 2`）、`trap … INT TERM` 复原、回滚时比对**起跑前记录的 sha256**；
- 变异脚本要超时**只能走 `scripts/lib/gt-timeout.sh` 的 `gt_timeout`**，**禁止**裸用 `timeout`：
  macOS（含 CI 的 macos-15 runner）没有 GNU `timeout`，brew 装的是 `gtimeout`。
  「命令不存在」和「断言变红」都是非零退出码，于是**基线红、变异也红**，`expect red`
  整片假通过（CI 实测：`mutation-check-transport` 报「4 passed / 8 failed」，每项只花
  2–4 秒，而同一 job 里 ctest 是绿的）。`gt_timeout_require` 解析不到 `timeout`/`gtimeout`
  就 `exit 2` 拒绝起跑——**宁可红，不拿「命令不存在」冒充有牙**；也绝不降级成
  「不设超时」（P4 那类「无限等」的变异靠超时才有界，拿掉超时就是拿掉判据）。
- 变异脚本**取输入的那一步要和被测代码取同一个源**，并且先有一条「变异前就找得到」的
  前置断言：CI 的 runner 与开发机看到的文件系统不一样。实测两例——`vendor/ghostty/zig-pkg`
  是构建期产物、不入库，缓存命中时不跑 zig，于是写死 zig-pkg 的 A7「变异→红」是**因为目录不在**
  （红得没有指向性），「复原→应绿」也假红；缺 Pillow 的开发机上图标对照直接红。
  两条都靠「先断言源在，再谈变异」挡在前面。
- 判据反了（期望红实际绿 / 期望绿实际红）时必须**当场 dump 最后一次运行的输出**，
  不许 `>/dev/null 2>&1` 一丢了事——只剩一行结论的失败没有证据，归因只能靠猜（CI 上真猜过一轮）。
- **wall-clock 类判据要和「同一负载下的标定」比**，不能只钉一个绝对秒数：共享 runner 会把
  调度抖动整倍放大（`test_transport` 的 close-speed 实测连续三次 1.07/1.53/1.09 秒，同一
  commit 在空机器上 0.2 秒）。做法：被测那段等待是 `20×10ms` 轮询，就再量一遍一模一样的
  轮询当标定，预算取 `max(原来的绝对下限, 1.5×标定 + 余量)`——**放大对两侧同向**，真回归
  （把宽限写长）只有 close 一侧变慢照样红；绝对下限**不放宽**，否则历史那次 2.3 秒的回归
  就没人管了。
- **wall-clock 的下界判据，时间戳必须在被测动作「之前」取**，而且松弛量要显式钉住：
  `test_ssh_connector` 的 real-deadline 原先在 `open()` 返回**之后**才取 `started`，而被测的
  到期时刻记的是连接器**自己**那一刻——两者之间被 runner 挤掉 1ms，`elapsed` 就成 29，
  而 `>=30` 的下界零余量（CI 实测 0.04 秒内即红；本机空载与 8 路忙循环下都是 30/31，
  所以**不是忙循环问题，是取点问题**；注入 3ms 调度间隙即可复现 26/27）。取早了 `elapsed`
  只会变大，代价是下界白捡一段准备成本，于是那段成本要**实测出来并断言在预算一半以内**
  （`open_cost<budget/2`），否则「松弛自己长大到把回归整段吞掉」没人看得见。
  另一条同类坑：**标定必须在取点之前跑完**——把标定循环放在 `started` 之后，等于把标定
  自己的 43ms 算进 `elapsed`，上界从此不再量轮询。

## 提交纪律

- **直接提交到 `main` 分支**：不新建功能分支、不走 PR 流程（本仓库约定）。
- 每切片必提交；一个提交只做一件事；闸门绿才提交。
- 提交信息遵循 **Conventional Commits**：`type(scope): description`（feat/fix/docs/test/chore/refactor），description 与 body **尽量用中文**。
- 破坏性变更（`ghosterm.h` / fixture）在提交信息里显式标注 `BREAKING`。
- **禁止**在提交信息里添加任何署名类 trailer：`Co-Authored-By`、`Generated-with`、`Assisted-by` 一律不写（无论 AI 助手还是替他人代签），作者字段只填本人。发现历史里混入即清理（`git filter-branch --msg-filter` 剥掉该行）并 `--force-with-lease` 推回。
- 关联 GitHub issue 的提交信息**必须**带 issue 引用，引用规则：
  - 短写 `#123` 永远指向**当前 commit 所在仓库**的 issue；
  - 只要引用的不是当前仓库的 issue，就必须写完整 `owner/repo#123`。

**插件包加载第三十五批（2026-11-06）**：私有固定包 / SDK 模块加载与 Session 身份、
能力、关联核对；共享 checked 快照引用，独立 JS 引擎。默认 469 CTest / 154 ABI，
JS ON / SSH ON 617 项；39 包加载专项及十一组优化 ASan/UBSan 变异通过，旧元数据
三组复验通过，目标正常断言红、独立配置绿、恢复绿及 SHA256。NDK arm64 / API29
archive / ELF 编译链接通过，本批无在线设备、未设备执行。无公共 ABI / 平台产品改动，
不增加 CI 二十份；App / 安装信任 / 完整 SDK / gterm / 业务插件继续，失败与收据见
插件计划第三十五批，不以私有加载首片称整个模块 / 插件计划完成。

**插件公开包门面第三十六批（2026-11-06）**：新增 opaque 目录采集 / 固定字节校验 /
元数据快照与包 runtime 接管，共 162 ABI；Apple 158/4、JNI 124/38。默认 470 CTest、
enabled Core 626 项；公开包六 Core、四 Swift、四 Android 变异通过，目标正常断言红、
独立配置绿、恢复绿与 SHA256。Swift enabled 两项含真实包 RPC；Android API36 / arm64 /
4 KiB 页独立模拟器 runtime 十一项，变异最终加 availability 十二项；默认 bridge 与
JS enabled 两 APK 真构建通过。元数据兼容不授予来源信任或执行批准，App 安装 / 激活 /
原生视图仍待接。均为手工专项，不增加 CI 二十份，不代表真机 / App 产品 / 发行；首次
失败、最终输入差异和收据见插件计划第三十六批。

**插件私有命令注册第三十七批（2026-11-06）**：checked 包按实际命令数保留固定贡献，
32 槽单 worker 注册表显式来源三态、前缀冲突、id＋side＋digest 选择 / 替换 / 移除，
不执行插件、不等于来源认证或运行批准。默认 470 CTest / 162 ABI、enabled Core 627 项；
十二组优化 ASan/UBSan 变异及旧 metadata 三组复验通过，目标正常断言红、独立配置绿、
恢复绿与 SHA256。72 夹具＋1554 分配失败点通过，无 LSan；NDK arm64 / API29 registry
ELF 在独立 API36 / 4 KiB 页模拟器通过。私有 Core 首片，不增加 CI 二十份；公共注册 /
安装、App 与业务插件仍待接，原件和范围见插件计划第三十七批。

**插件公共命令注册第三十八批（2026-11-06）**：七个新 ABI 入口与 Swift / JNI / Kotlin
worker 同步，登记显式来源三态，解析产生独立 CHECKED 包及命令贡献；原包取消、目录
移除和注册表销毁后，选中包仍保留固定字节，但不获执行授权。默认 470 CTest / 169 ABI，
Apple 165/4、JNI 131/38；Core 七组、Swift 三组、JNI 三组新增变异正常单条断言红、独立
配置绿、恢复绿与 SHA256。两端真实包 RPC 与 API36 / arm64 / 4 KiB 页专用模拟器十五项
组合通过。enabled 首轮 628 项有一条 tmux SSH PATH 探测超时，单独复验及随后串行
完整 628 项全部通过，首轮失败保留；原件详见插件计划第三十八批。手工后端证据，不增加 CI 二十份，不代表 App 激活 / 授权 /
视图、持久安装、完整 SDK、gterm IPC、业务插件或真实设备 / 发行完成。

**插件独立 CLI 第三十九批（2026-11-06）**：`cli/gterm` 只经公共 C ABI 提供 doctor 与
固定包兼容性检查，调用方 host 事实不等于授权；缺失保留 UNKNOWN，未接业务激活报错。
主机 ON/OFF 构建、十二项启用与两项禁用检查、七组隔离真编译变异通过（正常断言红、
同 archive 配置绿、复原绿及 SHA256）；默认 470 CTest / 169 ABI。NDK API29 arm64
CLI 在专用 API36 / 4 KiB 页模拟器五项通过，设备临时目录清理。首次编译/配置失败与
测试解码异常均保留且不计红；原件见插件计划第三十九批。未进 App 包、不安装远端环境，
手工证据不增加 CI 二十份；IPC、业务激活、App 与完整插件计划仍未完成。

**插件运行时能力第四十批（2026-11-06）**：冻结的 `capabilities.query` 观察当前 Session
授权五态，不缓存 allowed，不暴露 lease，也不把授权等同于服务已实现；SDK context 类型
同步。修复 bootstrap 吞 dispose 异常，原生 dispose_error 现在可见且继续释放资源。
默认 470 CTest / 169 ABI、enabled JS/SSH 完整 632 项、六组隔离优化 ASan/UBSan 变异
正常断言红/同 archive 配置绿/复原绿/SHA256、NDK 两运行时在 API36 / 4 KiB 页模拟器
四项通过。旧 test_config 的 512 字节 realpath 缓冲区触发 NDK 编译检查，单列未修复，不
计 Android 全套 Core 通过；原件见插件计划第四十批。手工证据，不增加 CI 二十份，CLI
激活、IPC、App 和业务插件仍待继续。

**插件配置对照第四十一批（2026-11-06）**：修复第四十批 NDK 暴露的 test_config 路径
缓冲区缺陷，检查所有拼接/realpath/临时创建失败，Include 临时树默认工程 tmp；ssh -G
改走 argv，避免 shell 路径拼接。322 条配置断言保持，长于 600 字节夹具/含分号工作目录
及创建失败检查通过；默认 470 CTest / 169 ABI、NDK arm64 test_config 编译通过。模拟器
缺 ssh，不记设备完整配置对拍。SSH auth 八组和编译器八组既有变异复验通过（正常断言
红、同 archive 配置绿、复原与 SHA256）；系统 libssh2 初次链接失败单列，锁定库后才绿。
本批测试对照修复，不增加产品能力或 CI 二十份；原件与限制见插件计划第四十一批。

**插件调用信息第四十二批（2026-11-06）**：runtime config 与 Swift/JNI/Kotlin 同步可选
Invocation，原始命令/argv 在接管前复制；规范 token、固定包完整声明、UTF-8 与 32 个/
4096 单项/16384 总字节限额，失败保留 Session。JS 通过 native 字符串构造并冻结对象，
不执行 shell，不冒充 schema 解码。默认 470 CTest / 169 ABI、enabled 634 项、Core 七组
＋Swift 两组＋JNI 两组正常断言红/独立配置绿/复原/SHA256通过；两端实际包 RPC，API36
4 KiB 页模拟器十五项组合及真实构建通过。JNI 初次编译失败和 Swift 收据文案识别失败
保留，最终完整复验见插件计划第四十二批。手工证据不增加 CI 二十份；CLI 激活/IPC、
来源授权、参数 schema、App 和业务插件仍未完成。

**插件前台 stdio 第四十三批（2026-11-06）**：独立 gterm relay --stdio 明确开发包摘要 /
Client 摘要 / 包目录 / 完整命令，固定检查后等待 Host Session 批准才激活；原始 argv 经
现有调用接缝传入，业务仍由插件/codec负责。两向 TTY 拒绝，stdout 仅协议，未知/拒绝/
取消/关闭错误分开。主机真实 child 七项、TTY 两向、旧 CLI 十二项/OFF 两项、默认
470 CTest / 169 ABI通过；七组优化 ASan/UBSan 变异正常断言红/同 archive 配置绿/复原/
SHA256。NDK arm64 CLI在专用 API36 / 4 KiB 页模拟器同源七项通过；不是 APK/真实设备/
SSH 拓扑验收。原件见插件计划第四十三批；不进 App 包、不增加 CI 二十份。跨包分发、
schema 解码、本机 IPC、App授权/视图和官方业务插件仍待完成。

**插件前台断流第四十四批（2026-11-06）**：Core Session 的阻塞发送不再跳过接收，
修复 macOS 真实 child 断流不退出；阶段转换仍等待发送完成。主机与专用 API36 arm64 /
4 KiB 页模拟器各十二项进程矩阵通过，护双管道、激活后取消、断流 UNKNOWN 和握手超时。
隔离 Core 旧行为变异正常断言红、同 archive 配置绿、复原十二项绿及 SHA256；默认
470 CTest / 169 ABI 与 JS ON / SSH ON 634 CTest 全绿。初始失败、原件与范围见插件
计划第四十四批；非 APK/真机/SSH CLI 拓扑或完整插件验收，不增加 CI 二十份。

**插件 CLI 解析第四十五批（2026-11-06）**：显式开发目录/摘要经固定检查后进入公共
注册器，完整 Executor 命令不按输入顺序抢占；冲突需 ID＋精确摘要选择，过期/缺失/
未知分开，全部校验/关闭成功后才输出，不授予运行权限。主机与专用 API36 arm64 /
4 KiB 页模拟器各十五项通过；六组新增与既有七组诊断变异正常断言红、同 archive 配置
绿、复原及 SHA256，默认 gate 470/169 通过。首轮选择变异编译失败未计红；原件见插件
计划第四十五批。非安装、参数解码、APK/真机或完整插件验收，不增加 CI 二十份。


**ProxyJump 两端入口扩展（2026-11-08，§9.214）**：macOS 与 Android 普通终端、SFTP、
tmux 管理/attach 均接受管跳板链。Android 同一 AST 展开、只解析首跳、逐跳身份/信任与
缺用户名交互、整链预算及释放；tmux attach 固定管理路线/首跳候选，仍重新认证。
API 29/36 专用 arm64/4 KiB 模拟器各十项两/三跳专项与配置对照通过；新增九组 Android
变异分轮、两组 macOS/Core 变异与三组迁移旧保护分轮正常断言红/独立配置绿/恢复/SHA256。
macOS 二十项真实场景、Android 既有 49 项组合、两端 Release/Android 四模块 lint、
C 格式 212 文件及隔离 Core 474/176 通过；原工作区插件 SDK 摘要失败单列。
首轮编译、测试反射参数、变异锚点/异常判据及隔离夹具路径失败见实施计划，不计通过。
手工证据，不增加 CI 二十份，未发布，非真机/WAN/完整服务器或客户端验收。

**插件 S5 公共本机进程通道（2026-11-15）**：`gt_ext_channel_open_process` 与 Channel /
Session / Runtime 的独立进程观察已接，181 ABI、Apple 178/3、JNI 144/37。显式 Native
执行批准、绝对路径 / raw argv / 目录 fd / 环境，复用既有 provider；不是插件 grant、shell /
PTY 或 OS 沙箱。16-channel 预约跨交接保持到实际销毁；Runtime 在内部 Session 销毁前存
最终快照，退出 / 信号 / reap / 组信号诊断与业务结果分开。Swift / Kotlin / JNI 已接且关闭
后保留观察副本，fd / SSH 查询 UNSUPPORTED，不伪造进程成功。`test-plugin-process-channel.py
--mutate --apple --full` 的 13 C / 关闭档、九 C / 两 Swift 变异、六 Swift；
`test-plugin-process-channel-android.py --headless-api 29 --also-api36 --mutate` 的两 JNI /
一 Kotlin 变异、两档各六项 / 配置一项 / 旧通道与 Session 四项通过。实际编译后正常断言
红、同 archive / 同 APK 独立配置绿、恢复及 SHA256；C 为 O1 ASan/UBSan、无 LSan。
API29 / 36 使用同一对 APK、arm64 / 4 KiB / 自有无窗口 AVD；不增加 macOS CI 二十份。
默认 Core 577/181、独立 Debug JS/SSH-on 803、macOS Release、Android JS-enabled 未签名
Release/R8 和四模块 lint、C 格式 251 文件通过。前置编译、夹具退出竞态、Swift 对象取证、
JUnit 分类及完整档 802/803 的长 socket 路径失败均保留在插件计划 S5 第五批，不计通过。
本机 App / IPC 准入与发现、生产本机 Node/Exec 组合及完整插件 / 客户端验收仍未完成，未发行。

**插件 S5 macOS 本机入口（2026-11-15）**：实验 JS-enabled App 已接本机 gterm 摘要 /
cwd / TMPDIR / 独立执行确认、raw argv、现有前台实例显式附着与关闭后进程观察。
真实 Native → gterm → Node 26.4.0 / arm64 Exec 的本机启动 64 项、IPC 附着 24 项、旧
SSH 直连 / ProxyJump 回归 20 项，以及配置 16 项 / 十六组隔离变异通过：实际编译后单条
正常断言红、独立配置绿、复原及 SHA256。Core 577/181、Apple 178/3、完整 App gate
493 项含十四项显式跳过 / 零失败、真实 SSH / WebKit 绘制、arm64 Release、C 格式 251
文件通过。无本片 Android / 公共 C ABI 改动，不增加 CI 二十份。首轮 SIGKILL 清理判据
未达与次级诊断错误单记，最终中断矩阵明确为自有宿主 SIGTERM，不冒充崩溃恢复；相同 UID
路径替换、组信号 EPERM、未知业务结果仍显式保留。原件与前置失败见插件计划 S5 第六批。
默认 JS-off；安装 / 发现 / Android 本机产品、崩溃残留归属与清理及完整 P1–P6 未完成，未发行。

**插件 S5 Native 临时归属（2026-11-15）**：macOS 本机执行在明确选择的根下新建私有
TMPDIR，Native 保留 fd / inode；Core owner 释放后分步清理该 namespace，根 / 工作目录 /
兄弟不删除，链接不跟随、文件不 chmod，替换 / 限额 / IO 错误保留 owner，阻止新启动并可
显式重试。26 项 / 二十四组原生变异、真实 App 重试一组变异、本机 66 / 附着 24 / 旧 SSH
20 项及 Node SIGKILL 三项通过；编译后单条正常红、同构件配置绿、复原 / SHA256。
Core 577/181、Apple 178/3、完整 App 503 项含十四跳过 / 零失败、真实 SSH / WebKit、
arm64 Release、C 格式 251 文件通过，不增加 CI 二十份。APFS 拒绝无效 UTF-8 夹具不是
Native 遍历证据，真实挂载点专项未执行。Exec WAIT 强杀仍有活 PID，严格检查保持失败，
不因临时目录移除升级业务 / 任意后代成功；本轮夹具按身份回收该 PID，不算生产 owner
修复。Native App 崩溃后的 ledger / 持久预算、Exec 父死亡生命周期及完整 P1–P6 未完成；
原件见插件计划 S5 第七批，默认 JS-off，未发行。


**插件 S5 父死亡监护（2026-11-15）**：共享 process owner 增加 fork-local POSIX 监护，
父方 liveness EOF 后 TERM / 100 ms KILL 本次独占组；保留实际 leader 的 WNOWAIT 身份到
组处理后再 reap，父方交回 lease 后不再发组信号。真实退出 / 监护最终释放 / 业务结果独立，
数据仍走原管道，无随包 helper / 服务或新 ABI。Channel、Node/Exec、jobs 与 Portable JS
清理适配异步退休，EOF 可重复观察。默认 Core 588/181、JS/SSH-on 完整 814、公共通道
原九 C / 两 Swift 变异与六 Swift、两档 Android 各六项 / 配置一项 / 旧通道及 Session 四项
同轮通过；Android 为同 APK / 专用 no-window arm64 / 4 KiB，未重跑旧 JNI/Kotlin 变异。
macOS 本机 66 / IPC 附着 24 / SIGKILL 六 / SSH-ProxyJump 二十项最终同源通过。
正常退出不替代未知业务；第七批 Exec WAIT 孤儿已在上述严格对象范围修复，不扩成任意
后代保证。监护本身外部强杀、逃离组、权限变化、Native App 重启 ledger / 持久预算及完整
P1–P6 仍未完成。新父死亡 / 升级 KILL / 同组后代 / 真实退出码及信号 / 报文拒绝 / 重复 EOF 七组 O1
ASan/UBSan 变异（无 LSan）编译后单条正常断言红、同 archive 配置绿、复原 / SHA256。
Native 26 项无跳过，完整 App 503 项含十五跳过 / 零失败、WebKit、macOS arm64 Release、
Android JS-enabled 未签名 Release / 四模块 lint、C 格式 255 文件通过；R8 复用缓存。
前置失败、runner INT/TERM 收尾与最终构建收据见插件计划 S5 第八批；默认 JS-off，
未发行，不增加 CI 二十份。


**插件 S5 Native 持久预约（2026-11-15）**：macOS 本机 namespace 创建前先持久化预约，
目录创建后按 dev / inode / uid / birth time 提交身份；记录与全局操作分别持有非阻塞
flock，原子 metadata 更新交接 lease。每条固定 256 MiB、同记录根最多 1 GiB，残留与
创建未决跨重启计入；不是物理磁盘扫描或 OS 配额。正常 rmdir 完成后才退休预约，记录
路径替换或同 inode / 等长内容变化均保留 owner；rmdir 后失败可重试，不丢账或假完成。
原生面板显式只读核对已记录位置，仅在无 live lease、父目录身份匹配且子目录 ENOENT
时允许按准确 SHA 快照二次确认释放预约；不删除旧目录，不恢复进程、权限或请求。
`test-plugin-local-journal.py --core-build 已启用JS的Core目录 --mutate` 的四次真实 Native
owner TERM/KILL / 新进程检查、42 项无跳过 Native / AppModel、十二组实际重编 Swift
对象变异通过：单条正常断言红、同库主机配置绿、复原与 SHA256。前置编译及非预期异常
不计通过，原件与前置失败见插件计划 S5 第九批。本机 66 / 附着 24 / SIGKILL 六项最终同源通过；
默认 Core 588/181、Apple 178/3、JNI 144/37、完整 App 519 项含十八跳过 / 零失败、
WebKit、macOS arm64 Release 与 C 格式 255 文件通过。无新 C ABI / Android 代码或设备
证据；损坏记录维护、跨重启残留删除、实际用量治理及完整 P1–P6 未完成，不增加 CI 二十份。

**插件 P5 表格首片（2026-11-15）**：Portable descriptor / page codec 与官方 `table`
Executor 已接，至多八列 / 十六行 / 2048 JSON 字节；百万行按页计算，立方保留精确十进制。
56 项编译后值 / adapter 检查、六组纯值与两组实际 Core 包业务变异通过：正常断言红、
同 archive 独立配置绿、复原及 SHA256。实际公开 package / Session / QuickJS 的首尾页、
空表、过期 UNKNOWN 与缺 Native 服务 UNSUPPORTED，在 macOS 及同 probe / 包的自有
API29 / API36 arm64 / 4 KiB 页 / no-window AVD 通过。旧 SDK O1 ASan/UBSan 十七场景 /
36 项 Ajv 对拍通过，未重跑旧变异。Android 初轮重复复制 / 编码触发 CPU 失败单列，
移除 handler 冗余处理后通过；默认 25ms CPU / 1s RPC / 2000 观察步与 64 模块上限保持。
SDK 增至七模块并同步生成身份 / 输入 / 测试闭包，无新 ABI。原生 table 服务与分页控件
未接，当前 Client 明确不支持；非 APK / Compose / Native UI、SSH gterm 整链、真机或发行，
P5 与完整 P1–P6 仍未完成，不增加 macOS CI 二十份。原件、前置失败和收据见插件计划 P5 第一批。
默认 Core 588/181、JS/SSH-on 815 CTest、Apple 178/3、JNI 144/37、C 格式 256 文件、
Android JS-enabled 未签名 Release / 四模块 lint 通过；native 实际重构建，R8 复用缓存。

**插件 P5 macOS 原生表格（2026-11-15）**：按已批准命令的 views 声明复用 Native 模型 /
Grid / Text / Button，不增加业务专属 C ABI 或窗格类型。十五项含准确分页 / 迟到隔离、
浅深主题、13 / 28 pt、进程内 AX 标签 / 值和真实下一页 action；同一官方包经本机 / 直连 /
ProxyJump 各百万行与空表六项，九组实际重编 Native 变异通过：单条正常断言红、同构件
配置绿、复原及 SHA256。三组 Core RelWithDebInfo ASan/UBSan 变异及其 53 项包运行时
基线、两组 CLI O1 ASan/UBSan 行为变异通过，均正常断言红 / 同 archive 配置绿 / 恢复 /
SHA256、无 LSan；CLI 的 linked Core 为单列 enabled archive，不冒充整库 sanitizer。
gterm 显式 Client 服务要求纳入精确协商 / Native consent，不宣称 Executor 自有 UI；
Core 仅允许 Host 的已授权 ui.table，其它服务与 Executor UI 仍不支持。未提高 CPU /
握手 / RPC 期限。旧 CLI 12 / stdio 17 / IPC 36、enabled Core 819 项通过。
Android table、OS VoiceOver / 键盘焦点、table IPC 附着、完整选包 / 授权用户点击链、
设备 / 生命周期与 P5 / P1–P6 仍未完成；默认 App JS-off，不增加 CI 二十份，未发行。
前置失败、独立收据与 Native / C / CLI 证据边界见插件计划 P5 第二批。
默认 Core 588/181、Apple 178/3、JNI 144/37、完整 App 535 项含十九跳过 / 零失败、真实
SSH / WebKit、macOS arm64 Release、Android JS-enabled bridge Release 原生编译与 C
格式 256 文件通过。Android 本片无 APK / 设备 / table UI 或四模块 lint 复跑结论。

**插件 P5 Android 原生表格（2026-11-15）**：按已批准准确命令的 views 声明复用 Native
文本 / 选择 / 分页组件，单页、精确修订与 Runtime 代次隔离；显式 ui.table Client 服务
与两条 RPC 纳入原生 consent，不增加业务 C ABI、SDK 身份或专属窗格。API29 / 36 自有
arm64、4 KiB、无窗口 AVD 各 23 项模型 / 值 / UI、每端直连 / 两跳百万行与空表四场景
分轮通过，同一官方 Executor 包 / gterm / Core。十二组隔离重编变异为同轮证据：编译后
单条正常断言红、同 APK Core 配置绿、恢复目标绿、实际 APK 变化与源码 SHA256。
原双轴共 semantics 节点的滚动动作丢失已修复，修正变异另护独立寻址；首版错误嵌套变异
的布局异常、前置 timeout 与测试标签 / 帧同步失败不计红或通过。
551 份输入与工作区一致；最终 API29 / 36 模型 / UI 复验的恢复 APK 摘要相同，但 API29
首轮四 SSH 使用初始包，不称全矩阵一次同 APK，分轮摘要见插件计划 P5 第三批。
默认 Core 588/181、Apple 178/3、JNI 144/37、C 格式 19.1.7 / 256 文件、SDK 七模块
生成 / 输入守卫、Android JS-enabled Debug / 测试 APK、未签名 Release / R8 与四模块
release lint 通过。手工模拟器证据，不增加 macOS CI 二十份；TalkBack / 键盘焦点、IME、
16 KiB / 真机、table IPC、多视图 / ctx.views、完整选包启动 UI 与 P5 / P1–P6 未完成。
默认 App 保持 JS-off，未推送 / 发行。
既有 Preview / Controller / Claude 启动与观察 / Core 配置组合最终 25 项通过；首轮
Controller 的 CPU_BUDGET 与相同 APK / 预算的独立复验通过分账，不据未复现称已修复。

**插件 P5 Portable diff（2026-11-15）**：只读 JSON 文本比较、精确 Unicode / LF / CRLF /
CR / 无末尾换行，标准描述 / 页 / 双行号与游标契约；有界 LCS 经既有 worker hook 分步
准备，不提高 25 ms CPU / 1000 ms RPC / 2000 次观测上限。2107 项编译后检查、十二组
隔离变异通过：七组值、三组真实 Core 业务、worker 推进及 dispose 撤权；正常断言红、
同 archive 配置绿、恢复目标 / JS 产物 / SHA256。相同 NDK ELF 与官方包在自有 API29 /
36 arm64、4 KiB、无窗口 AVD 各七种 standalone Core 场景通过，含最大交错和宽 Unicode。
聚合 SDK 八模块，64 / 65 边界保持；既有 module-count 一组选跑优化 ASan/UBSan、53 项
基线 / 最终通过，无 LSan，不称默认十四组已重跑。旧 SDK 17 场景 / 36 schema 与表格
56 项基线通过；默认 Core 588/181、enabled Core 819、Apple 178/3、JNI 144/37、C 格式
19.1.7 / 257 文件、macOS arm64 Release、Android JS-enabled Debug / 测试 APK / 未签名
Release 与四模块 lint 通过，R8 为 UP-TO-DATE。两端 ui.diff / 实际 SSH、原生主题 / 大字号 /
无障碍、完整交互与设备矩阵仍待接；默认 JS-off，不增加 CI 二十份，P5 / P1–P6 未完成，
未推送 / 发行。前置编译 / 夹具崩溃 / activation CPU 与失败变异的编译错误见计划第四批，
均未计普通断言红或通过。

**插件 P5 macOS 原生 diff（2026-11-15）**：准确批准命令 views 驱动通用只读 Text /
ScrollView / Button；精确修订、双行号 / 行尾、所选页与有界连续游标，迟到 / 停止隔离。
十五项新原生测试与既有 table 十五项通过，含浅深主题、13 / 28 pt 实际位图、本进程 AX
值与真实下一页 action。同一官方包经本机 / 直连 / 两跳 SSH 的变化 / 空 / Unicode 九项，
加本机宽 Unicode 与交错矩阵两项，共十一场景通过；全页 UTF-8 / 行尾重建、生产面板和
停止 / 本机 reap / TMPDIR 移除已核对。十一组隔离 Native 变异通过：编译后单条正常
断言红、同构件配置绿、恢复 / 对象 / SHA256。最初 OCR / AX 属性错误、请求字段编译
失败和自赋值变异编译失败均单列，未计普通断言红或通过。ui.diff 纳入 Client 精确协商，
Core 仅允许 Host 且仍检查 lease；无新 ABI / SDK 身份，不提高原 CPU / RPC / 握手预算。
Android 原生 diff、diff IPC、通用 ctx.views / 多视图、系统辅助技术 / 键盘焦点、完整
用户点击 / 生命周期 / 设备与 P5 / P1–P6 仍未完成。默认 JS-off，未推送 / 发行，不增加
macOS CI 二十份；分批原件、收据与其它闸门见插件计划 P5 第五批。
本批另复验 table Native 九组 / 六种实际路线，Core diff 新三组与 table 旧三组分套优化
ASan / UBSan 各 56 项基线 / 最终，以及 CLI 两服务各两组；正常断言红、独立配置绿、
恢复 / SHA256，无 LSan。默认 Core 588/181、enabled 822、CLI 12 / stdio 17 / IPC 36、
完整 App 551 项含二十跳过 / 零失败、真实 SSH / WebKit、macOS 新 scratch arm64 Release、
Android JS-enabled bridge Release 原生重编、C 格式 19.1.7 / 257 文件、Apple 178/3 与
JNI 144/37 通过。旧 stdio Preview 一次 owner cleanup 失败、同构件原预算定向及完整
复验通过分账，原因未确认，不宣称已修复；本片无 Android APK / 设备 / lint 或 R8 复跑。

**插件 P5 Android 原生 diff（2026-11-15）**：准确批准命令 views / ui.diff 接通用只读
Compose 原文、双行号 / 行尾、连续游标与所选页；停止 / 替换 / 迟到隔离。与 table
共用严格 JSON 边界，不增加 C ABI、SDK 身份或业务专属窗格，不提高既有预算。
自有 API29 / 36 arm64、4 KiB、无窗口档各 27 项 diff 与旧 table 23 项通过，浅深主题 ×
1× / 2× 字号、实际字形像素 / semantics / 真正前后按钮；每端直连 / 两跳各变化 / 空 /
Unicode / 相同文本八场景，通过原始 UTF-8 / 行尾重建、停止失效与真实释放按钮。
十五组隔离变异在 API29 同轮重验：编译后单条正常断言红、同 APK Core 配置绿、恢复 /
实际 APK / SHA256；560 份输入一致。最终两设备模型复验的恢复 APK 摘要相同，API29
前置八 SSH 使用初始 APK，分轮见插件计划 P5 第六批，不称全矩阵同 APK。初轮 JSON
夹具共享对象污染与 API36 释放等待失败单列；停止夹具改为原期限内等终态 / !launchBusy，
再点击实际启用的释放按钮，不将 Runtime 终态当成 owner 已释放，也不声称确认底层释放
缺陷。TalkBack / IME / 键盘、16 KiB / 真机、diff IPC、ctx.views / 多视图、完整用户点击 /
生命周期及 P5 / P1–P6 仍未完成，默认 JS-off，未发行，不增加 macOS CI 二十份。
旧 table 共享字节 / 方法接线两组在 API36 另复验，23 项基线 / 最终与四条 SSH 通过，
正常断言红 / 同 APK Core 绿 / 恢复 / SHA256；不称旧十二组全重跑。根工程 JS-enabled
Debug / 测试 APK、未签名 Release / 实际 R8 与四模块 lint、默认 Core 588/181、C 格式
19.1.7 / 257 文件、Apple 178/3、JNI 144/37 通过。无本片 enabled Core 全套、macOS App /
Release 或完整 Android 客户端结论，源码、分轮摘要与失败记录见插件计划第六批。

**插件 P5 Portable task（2026-11-15）**：任务 descriptor / snapshot / 取消请求与回执、
精确 uint64、2048 字节及单调状态 / 终态校验已接；官方 task primes 真实分步筛法每
pump 最多 512 操作，取消受理与确认停止分开，未知后只读观察、不自动重放。7,642 项
与十五组隔离变异通过，其中四组执行实际 Core 业务；编译后正常断言红、同 archive
配置绿、恢复 / 产物 / SHA256。公开 Core / Session / QuickJS 九场景在本机及自有
API29 / 36 arm64、4 KiB、无窗口 standalone 探针通过；百万完整计算仅 Node 算法，
实际 Core 百万场景为取消。丢失确认是受控 Session 时钟，非真实 SSH 丢包。初始 API36
25 ms CPU 失败后将分配移至 worker 并去重复校验，原预算完整重跑通过，构造 / 编译
失败均单列。SDK 九模块及生成身份 / 输入闭包同步，模块 64 / 65 边界与原一组变异、
SDK 十七场景 / 36 schema、旧 table 56 / diff 2,107 项通过，未称旧全组变异重跑。
enabled Core 822、默认 JS-off Core 588/181、CLI 12 / stdio 17 / IPC 36、macOS 独立
Release、Android Debug / 测试 APK / 未签名 Release / 四模块 lint、C 格式 19.1.7 / 258、
Apple 178/3 与 JNI 144/37 通过；R8 为 UP-TO-DATE。无新 ABI / Native task UI / 实际 SSH /
APK 行为 / 真机或发行证据；缺 ui.task 显式 UNSUPPORTED，完整 P5 / P1–P6 未完成，
不增加 CI 二十份。分轮原件、摘要与 sanitizer 边界见插件计划 P5 第七批。

**插件 P5 macOS 原生 task（2026-11-15）**：准确批准命令 views / ui.task 驱动通用
进度、阶段、结果、读取与取消；精确 uint64、唯一取消 ID，回执与停止分开，未知后
显式只读、不重放，RPC failed / cancelled 与任务阶段分开。26 项原生测试与旧 table /
diff 各十五项通过，含浅深主题 × 13 / 28 pt 位图、进程内 AX、真实取消按钮、视图
自动读取 / 未知暂停 / 显式读回后的恢复、长 Unicode / 控制字符失败回包。本机 / 直连 /
两跳各完成 / 空任务 / 百万取消九场景，真实 SDK INVALID_INPUT / effect:none 均经
生产 Native 门面保留。十六组隔离变异正常单条断言红、同复制构件配置绿、恢复 /
SHA256；十一模型组与一视图组另核对象变化。Core 仅 Host 有效 lease 可准入 ui.task，
gterm 显式服务协商；无新 ABI / SDK 身份，不提高预算。未操作系统 VoiceOver / 用户
完整选包点击链，不将模型 unknown 当实际 SSH 丢包 / 重连；Android 原生任务、task
IPC、ctx.views / 多视图、完整生命周期 / 设备、开发工具 / testkit 与 P5 / P1–P6 待完成。
默认 JS-off，未推送 / 发行，不增加 CI 二十份；分轮、失败与其它闸门见插件计划第八批。
本批另有 Core task / table / diff 各三组（各 59 项基线 / 恢复）及 CLI 各两组分套复验；
enabled Core 825、默认 Core 588/181、App 默认档 578 项 / 35 明确跳过 / 零失败、macOS
独立 Release 最终重编 / 重链、Android JS-enabled bridge Release、C 格式 258 文件及
Apple 178/3 / JNI 144/37 通过。无本片 Android APK / 设备 / lint / R8 或打包 / 云端结论。

**插件 P5 Android 原生 task（2026-11-15）**：准确批准命令 views / ui.task 驱动通用
Compose 进度、阶段、结果和实际取消按钮；精确 uint64、零 / 未知、唯一取消 ID、
回执 / 停止分开，未知后显式只读、不重放。标准 Native RPC 门面保留 failed / cancelled /
unknown 与 effect，接受句柄先登记再检查取消 / 代次，声明请求取消不改写任务终态。
共享 parser 抽 envelope，table / diff 成功回包原 3072 / 2048 界限不变，wire 长 Unicode /
控制字符失败另按既有字段界限处理。自有 API29 / 36 arm64、4 KiB、无窗口档各 33 项
任务、旧 table 23 / diff 27 项通过，含浅深主题 × 1× / 2× 字号、实际字形 / semantics /
取消 / 自动读取暂停与恢复；每端直连 / 两跳各完成 / 空 / 百万取消六场景，真实 SDK
INVALID_INPUT / effect:none、停止失效和实际释放按钮通过。十七组 API29 隔离变异
同轮：编译后正常单条断言红、同 APK Core 配置绿、恢复 / APK / SHA256；566 输入一致。
两设备最终恢复 APK 相同，API29 前置六 SSH 使用初始包，分轮见插件计划第九批，
不称全矩阵一次同 APK。最初缺导入、设置中止 / 清理非空及重复锚点未记目标红或通过。
旧 table 在 API36 另复验共享字节 / 方法两组与 23 项 / 四 SSH；根工程 Debug / 测试
APK / 未签名 Release / 实际 R8 / 四模块 lint、默认 Core 588/181、C 格式 258 文件、
Apple 178/3、JNI 144/37 通过。无新 ABI / SDK 身份或预算提高。非 TalkBack / IME /
键盘、16 KiB / 真机、真实丢包 / 重连、完整选包点击链 / 生命周期 / 客户端或发行证据；
task IPC、ctx.views / 多视图及完整 P5 / P1–P6 未完成，默认 JS-off，不增加 CI 二十份。

**插件 P5 开发命令与 RPC testkit（2026-11-15）**：`scripts/gterm-plugin`
init / check / build / pack / test 已接 Portable pair 首片；strict TypeScript、固定
副本 / 导入闭包、Host 三态 / 依赖事实、排他原子发布与可复现 ZIP。真实公开 Core /
QuickJS Executor RPC 夹具验激活、错误 unknown、排队取消、旧 epoch / 撤权、预算
准入与引擎 / fd 释放；配置准入不是 OOM / RSS，请求取消不是业务任务取消。
`scripts/test-plugin-dev.py --mutate` 十六组基线、六组隔离工具 / Native 夹具变异
通过：正常断言红、独立配置绿、恢复绿与 SHA256；54 输入一致。实际 TERM 中断
回收活跃 worker 并清空固定副本。默认 Core 588/181、C 格式 19.1.7 / 259 文件、
Apple 178/3、JNI 144/37 通过；无 enabled Core 全套或平台 App / APK 新结论。
前置 schema / 发布路径 / 对照参数失败单列，分轮与收据见插件计划 P5 第十批及
[开发指南](docs/how-to/plugin-development.md)。无新 ABI / SDK 身份、安装或权限
授予。macOS arm64 手工证据，不增加 CI 二十份；Node / Exec 开发、Client 激活、
完整资源 / Native UI / SSH / 设备 / 生命周期及 P5 / P1–P6 仍未完成。

**插件 P5 本地视图注册服务（2026-11-15）**：`ctx.views` 通过 Host-only 显式服务
注册 table / diff / task 方法绑定；核对准确命令 / 必需能力 / 活跃授权 / 方法方向，
八个活动注册、独立不复用句柄、旧 close 不移除重开项，关闭不冒充业务取消。
新增三 C ABI 入口与 Swift / JNI / Kotlin 门面，原生面板尚未消费；注册不是渲染。
十九项真实 Core / 包命令约束、四十四项 Portable 值 / adapter 与九组隔离变异通过：
正常断言红、同库配置绿、恢复 / SHA256，272 输入一致。优化档 ASan / UBSan；
Debug sanitizer 前置失败单列，非已绿，未用编译失败 / 崩溃冒充目标红。
SDK 十模块 / 常规值闭包八模块、身份重新生成，64 模块与既有 Runtime 预算不变。
默认 Core 588/184、enabled / 锁定 SSH Core 844、C 格式 262、Apple 181/3、JNI 147/37，
macOS 独立 Release、Android JS-enabled 测试 APK / 未签名 Release / 实际 R8 / 四模块
lint 通过。旧 SDK / CLI、两组能力 / dispose 及一组 heap 变异分轮见插件计划第十一批。
无新原生 UI / AVD / 设备 / 业务 SSH 或发行结论；完整多视图、任务 IPC、完整开发 /
生命周期 / 客户端矩阵及 P5 / P1–P6 未完成，不增加 CI 二十份。见
[视图注册指南](docs/how-to/plugin-views.md)。


**插件 P5 macOS 多视图消费（2026-11-15）**：JS-enabled 产品按准确命令 ui.views /
标准 kind / RPC allowlist 显式启用注册表，通用组件按 Runtime / epoch / 独立 handle
保留分页和模型；关闭返回新快照，旧项 / 晚到 RPC / 快照不能复活。描述符 ID 须与
注册一致，关闭失败显式重试；task 关闭不取消业务。同 Runtime 八个业务 scope 的
观察账本保留取消 ID / 未知及终态，不因重开重放，不因满额淘汰旧账。Android 产品
未接多视图，必需 ui.views 显式不支持。十九模型 / 两服务与 profile、旧 table 十五 /
diff 十五 / task 二十六项，八组正常单条断言红 / 同构件配置绿 / 对象变化 / 恢复 /
SHA256；相同官方包本机 / 直连 / 两跳与屏幕外透明面板真实分页、关闭、重开和取消
按钮通过。默认 Core 588/184、enabled 844、App 默认档 599 项 / 22 明确跳过 / 零失败
及真实 WebKit CSS / PNG 通过。无新 ABI / SDK 身份或 Runtime 预算提高；非系统
VoiceOver / 完整键盘、真实断网重连 / 用户选包整链，Android 多视图、task IPC、
完整开发 / 生命周期 / 设备矩阵及 P5 / P1–P6 待完成，不增加 CI 二十份。分轮原件、
前置失败与构建证据见插件计划 P5 第十二批及[指南](docs/how-to/plugin-views.md)。


**插件 P5 Android 多视图消费（2026-11-15）**：JS-enabled 产品按准确命令 ui.views /
kind / RPC allowlist 显式启用注册表，Handler worker 核对真实 Session / epoch 及
Runtime UUID；通用 Compose 控件按 handle 保持独立模型，关闭返回新快照，旧句柄 /
RPC 不可复活。关闭失败保留并恢复轮询；task 关闭不取消业务，有界历史保留未知
取消 ID 和终态，不因重开重放。自有 API29 / 36 arm64、4 KiB、-no-window 各 27 项
新模型 / UI，旧 table 23 / diff 27 / task 33 分轮与各直连 / 两跳通过，含浅深主题 ×
两档字号 / 字形像素和实际分页 / 关闭 / 重开 / 取消 / 释放。十三组 API29 隔离变异
同轮正常单条断言红、同 APK Core 配置绿、恢复 / 安装字节 / SHA256，576 输入一致；
两端最终恢复 APK 一致，初始 / 恢复及前置时序失败分轮见插件计划第十三批。
默认 Core 588/184、C 格式 262、Apple 181/3、JNI 147/37、Debug / 测试 APK、未签名
Release / 实际 R8 与四模块 lint 通过。无新 ABI / SDK 身份或预算提高；非系统
TalkBack / 键盘 / IME、16 KiB / 真机、真实断网重连、用户选包整链或完整客户端；
task IPC、完整开发 / 生命周期及 P5 / P1–P6 仍未完成，不增加 CI 二十份，未发行。


**插件 P5 前台 IPC 的 task / 多视图（2026-11-15）**：复用一次性 Attachment /
Native 标准组件，自有后台 PTY + 独立 attacher；核对 PID、入口撤销、退出、重放拒绝
及原 PTY 零协议字节。macOS 本机 / 直连 / 两跳十二 task + 三多视图、Android 自有
API29 / 36 arm64、4 KiB、无窗口十六 task + 四多视图通过；未知取消注入先停前台、
登记取消 ID 再杀同一自有进程，保持 UNCONFIRMED / 禁用 / 无重放，不声称 handler
已收到取消。正常受控传输出口 7 / 注入 -9 不当业务成功；确认取消另由 read 回包。
复验发现 Runtime 撤权后 disposer 误受 stopped / 旧 CPU 截止中断，现仅同步清理用
同一 quantum 的新截止，alive / grants 仍撤销；真实 throw / 无限清理继续报错。
新增 raw / public 四项，Runtime / views 55 项、六组隔离 Runtime 变异，正常断言红 /
同库配置绿 / 恢复 / SHA256 通过；原两跳空任务十二次复验与 INT / TERM 清理通过。
Native 分轮模型 / UI / APK / 输入与前置失败详见插件计划第十四批。enabled Core
848、默认 Core 588/184、CLI 12 / stdio 17 / IPC 36 与 IPC 十六组变异、两服务各三项
拒绝 / 两变异通过；Apple 默认宿主 599 / 22 明确跳过 / 零失败、WebKit 与 macOS
Release，Android Debug / 测试 APK / 未签名 Release / 四模块 lint、C 格式 262、
Apple 181/3、JNI 147/37 通过；本轮 R8 为 UP-TO-DATE。无 ABI / SDK 身份或预算
提高；非系统辅助技术、16 KiB / 真机、真实断网重连、用户选包整链 / 完整生命周期 /
开发 / 分发矩阵或发行结论；P5 / P1–P6 仍未完成，不增加 CI 二十份。


**插件 P5 开发 testkit 的 Client 激活（2026-11-15）**：`scripts/gterm-plugin test
--side client` 显式选择已检查包的 Client，默认仍为 Executor；项目和已构建目录均
可测，脚手架两端各注册 typed echo。真实 Core / QuickJS 只激活所选侧，Native 对端
按准确 receiver / 自己的 lease 发送 RPC，不冒充双 JS 互调。Host 事实 side 不匹配
拒绝，所选侧自己声明 session.rpc；兼容三态 / 授权边界不变。二十一项工具基线、
十组隔离变异（新增角色 / 声明 / 回包 / Host side 四组）正常断言红、独立 Core
配置绿、恢复 / SHA256；56 输入一致。Client 专有返回值 / Executor 对照、错误值 /
outcome、激活 / 清理失败、旧 epoch / 撤权 / 排队取消、预算准入和 engine / fd 释放
通过。前置 NULL 对端清理夹具失败保留，修正后通过；产品 Core 未改，无 sanitizer
新结论。默认 Core 588/184、C 格式 262、Apple 181/3、JNI 147/37 通过；无新平台
产品 / ABI / SDK 身份 / 预算变化或 App / APK / 设备 / SSH / 发行结论。双 JS 激活、
Node / Exec 开发、文件 / 进程 / UI testkit 与完整 P5 / P1–P6 仍未完成，不增加 CI
二十份；分轮 / 收据见插件计划第十五批与[开发指南](docs/how-to/plugin-development.md)。


**插件 P5 开发 testkit 的双 JS 端往返（2026-11-15）**：`test --side paired`
分别核对 Client / Executor 兼容事实与固定包 digest，两个 Runtime 各自接管 Session；
准确方法表 / receiver，Native 控制只在原 worker 借用原 lease，不自行销毁 Runtime
所有的 Session。脚手架真实 Client → Executor → Client 回调往返与反向根调用、
unknown / 错误值、排队控制请求取消 / 旧 epoch / 撤权、两入口激活 / 预算准入和
两 engine 独立释放 / fd 平衡通过，不冒充业务取消或跨进程 RSS。二十六项工具基线、
十四组隔离变异（新增 Executor pump / 方向 / 释放 / 事实四组）正常断言红、独立
Core 配置绿、恢复 / SHA256；57 输入一致。C11 / -Wall / -Wextra / -Werror、
RelWithDebInfo，无 sanitizer 新结论。默认 Core 588/184、C 格式 263、Apple 181/3、
JNI 147/37 通过。无产品 Core / 平台 / 公共 ABI / SDK 身份 / 预算变化或新 App /
APK / 设备 / SSH / 发行结论；Node / Exec 开发、文件 / 进程 / UI testkit 和完整
P5 / P1–P6 仍未完成，不增加 CI 二十份；见插件计划第十六批与[开发指南](docs/how-to/plugin-development.md)。


**插件 P5 Node / Exec 包开发（2026-11-15）**：通用 check / build / pack 支持
Portable Client 配 Node / Exec 执行档，按各脚本入口隔离 strict TypeScript 与锁定
Node 22 开发类型；Portable 不加载 Node 类型，来源必须可达且共享输出一致。
Node ESM 静态闭包、Exec 非空字节、完整性 / 可复现 ZIP、显式 variantId 与 Host
三态保留；不执行入口 / 项目钩子或自动安装执行端依赖。Executor / paired test
仍只执行 Portable，其它兼容运行档明确 4 / TEST_RUNTIME_UNSUPPORTED；Node 包
的 Portable Client 已在真实 QuickJS 验证。初轮发现三斜线引用可引入 Node 类型，
实际复现 check compatible 后 TERM 中止；修为禁止 path / types / lib references，
不能借类型或快照外文件绕过闭包。最终 32 项基线、19 组隔离变异正常断言红 /
独立配置绿 / 恢复 / SHA256；57 输入一致，另四项预编译 ESM 手工补证。默认 Core
588/184、C 格式 263、Apple 181/3、JNI 147/37 通过。无产品 Core / 平台 / ABI /
SDK 身份 / Runtime 预算变化；无新 App / APK / 设备 / Node / Exec 业务执行 /
SSH / 发行结论，无 sanitizer 新结论。Node / Exec 开发执行、服务 testkit 与
完整 P5 / P1–P6 仍未完成，不增加 CI 二十份；见插件计划第十七批及开发指南。


**插件 P5 Node / Exec 开发执行（2026-11-15）**：单侧 `gterm-plugin test` 需显式
execution JSON / 本次执行授权 / Executor Host 事实；Node 固定可执行文件 / SDK
catalog，Exec 来自已检查包，版本 / 字节 / 握手与进程生命周期复用公开 Core。
只传内部 TMPDIR，不开放未知配置或环境字段；无配置仍 UNSUPPORTED，Client /
paired 拒绝外部执行配置。真实 Node handler / 自有 C Exec 协议 peer 通过成功值 /
unknown、排队控制取消 / 旧 epoch / 撤权、激活 / reap / 暂存 / fd；不冒充业务取消
或进程 RSS 沙箱。Node / Exec 各实际 INT / TERM 四次中断均在观察到活跃 handler
后发信号，130 / 143、子进程退出及私有目录回收通过；Native owner 先有界关闭，
工具组有限等待后停止。38 项基线、23 组隔离变异正常断言红 / 独立 Core 配置绿 /
恢复 / SHA256，64 源及 35 个 Node / SDK 构件输入一致。实际 Node 26.4.0，另验
中文 / emoji / 单引号 / `$()` 原始 argv。默认 Core 588/184、C 格式 263、Apple
181/3、JNI 147/37 通过；无 sanitizer 新结论，无产品 Core / 平台 / ABI / SDK
身份或 Runtime 预算变化，不记新 App / APK / 设备 / SSH / 发行。含外部执行的
双侧测试、服务 testkit 与完整 P5 / P1–P6 仍未完成，不增加 CI 二十份；见插件
计划第十八批及[开发指南](docs/how-to/plugin-development.md)。


**插件 P5 Node / Exec 双侧开发执行（2026-11-15）**：`test --side paired` 支持
显式外部执行配置 / 本次授权；Client 保持 Portable，Executor 才用其 Node / Exec
事实和进程服务。两份固定包 / 独立 Host 检查，真实 Client → Executor → Client
回调及反向根调用、转义 / 中文 / emoji、错误值 / unknown 均验；没有宿主假业务
响应。两个 owner 分别激活 / 释放，外部子进程 reap / 暂存 / 包引用 / fd 平衡通过；
不据释放 flag 声称进程 RSS 验收。双侧 Node / Exec 各 INT / TERM 四次（本轮含
原单侧共八次）均观察活跃 handler 后注入，130 / 143、子进程退出及目录回收通过。
44 项基线、27 组隔离变异正常断言红 / 独立配置绿 / 恢复 / SHA256，64 源及 35
固定构件一致；Node 26.4.0，macOS arm64 / C11 / -Werror，无 sanitizer 新结论。
默认 Core 588/184、C 格式 263、Apple 181/3、JNI 147/37 通过。无产品 Core /
平台 / ABI / SDK 身份或 Runtime 预算变化，不记新 App / APK / 设备 / SSH /
发行；服务 testkit 与完整 P5 / P1–P6 仍未完成，不增加 CI 二十份；见插件计划
第十九批及[开发指南](docs/how-to/plugin-development.md)。


**插件 P5 显式资源开发测试（2026-11-15）**：`test --resource-root 绝对目录
--allow-resource-access` 为单侧 Executor 接真实公开 Store / resources Session /
Runtime，Portable / Node / Exec 均验 64 KiB 发布与 Native 快照字节 / SHA256。
所选 variant 须声明三能力，Host 拒绝仍拒绝；缺授权、Client / paired 根不执行，
默认 RPC-only 不增权。只读 workspace、256 KiB 单资源；Portable 另验空文件、
越界 / 外根 / 链接拒绝。错误 lease 无字节、关闭后保留 pin、Store 销毁 WAIT、
显式 release、预约 / 引用 / fd 归零逐项核验。49 项基线 / 32 组隔离变异正常
断言红 / 独立配置绿 / 恢复 / SHA256，C / inc 必须编译后 ASSERT；65 源与 35
固定构件一致。报告范围标签修正前主动中断轮退出 143、无收据，不计整轮通过。
默认 Core 588/184、C 格式 264、Apple 181/3、JNI 147/37 通过；macOS arm64 /
C11 / -Werror，无 sanitizer 新结论。无产品 Core / 平台 / ABI / SDK 身份或
Runtime 预算变化，不记新 App / APK / 设备 / SSH / 发行。双侧资源、完整文件 /
资源中断矩阵、进程 / UI 服务 testkit 及 P5 / P1–P6 仍未完成，不增加 CI 二十份；
见插件计划第二十批及[开发指南](docs/how-to/plugin-development.md)。


**插件 P5 双侧资源开发测试（2026-11-15）**：`test --side paired` 可显式授权
Executor 只读根；Client 仅声明收件，workspaces 实测为空。Portable / Node / Exec
的真实双侧 RPC 组合 Native 快照全字节 / SHA256 / lease / 关闭后 pin；契约可选
resourceConsumer=client，Client SDK 实际 take / 分块 read / EOF / release，回执
经实际 Executor 返回并精确匹配 expected.resource。Native 观察真实 Store 收件和
关闭 owner 前无待件 / pin，匹配 JSON 但无发布的夹具仍断言红；两消费档不抢同一
队列。根方法两方向、64 KiB 全字节、两侧权限 / 声明、路径边界及实际释放通过。
57 项基线、39 组隔离变异正常断言红 / 独立配置绿 / 恢复 / SHA256，65 源与 35
固定构件一致；C / inc 与 Client pin 注入要求编译后的 Native ASSERT。补实际
Store 观察前中断 143、授权夹具未复原 manifest 的测试步骤失败单列，不计通过。
默认 Core 588/184、C 格式 264、Apple 181/3、JNI 147/37 通过；macOS arm64 /
C11 / -Werror，无 sanitizer 新结论。无产品 Core / 平台 / ABI / SDK 身份或
Runtime 预算变化，不记新 App / APK / 设备 / SSH / 发行；完整资源 / 进程 / UI
服务测试、资源实际中断、辅助技术 / 生命周期及 P5 / P1–P6 仍未完成，不增加
CI 二十份；见插件计划第二十一批及[开发指南](docs/how-to/plugin-development.md)。


**插件 P5 业务进程开发测试与回收修复（2026-11-15）**：`test --process-json`
配独立 `--allow-business-processes`，显式程序 / 摘要 / Host 已核对版本、cwd、
选定环境与预算，三运行档单侧 / paired。Client 无 process / workspaceHandle；
业务字节、EOF、stderr / exit 7、SIGTERM、拒绝 / unknown、实际 PID 回收分别验。
67 基线 / 45 隔离变异正常断言红 / 独立配置绿 / 恢复 / SHA256，68 源 / 35
固定构件一致；新六次业务子进程 INT / TERM 与原八次 Executor 中断分开记账。
修复 Core 四缺陷：QuickJS 不可捕获 InternalError 用于正常 spawn 拒绝、JSON
非 NUL 字符串直接当 argv、leader / guard 重复 fence，以及未回收时 wire 的
running / signal 不一致。Core 64 场景 / 三组新 sanitizer 变异、拒绝八场景 /
一组 sanitizer 变异通过，分别 766 / 750 源最终一致；ASan / UBSan，非 LSan。
原拒绝 / argv / 组信号 / 及时性失败、两夹具编译失败、NDEBUG 关闭断言的编译失败
及 SSH-off 全套的三项失败均单列，不计红或整轮绿；不降低时间 / 回收断言。
最终默认 Core 588/184、JS-on / SSH-on / Debug 849、C 格式 265、Apple 181/3、
JNI 147/37 通过。无新 ABI / SDK 身份 / 平台源码或生产预算变化，无新 App /
APK / 设备 / SSH 产品档 / 云端发行；完整进程 / 资源 / UI 服务矩阵、辅助技术 /
生命周期与 P5 / P1–P6 仍未完成，不增加 CI 二十份；见插件计划第二十二批及
[开发指南](docs/how-to/plugin-development.md)。


**插件 P5 开发 testkit 的 Client 视图注册（2026-11-16）**：显式 kinds / 本次授权，
Client 单侧与 Portable / Node / Exec paired 核对真实 Core 注册快照、独立关闭、
旧 / 外来句柄、撤权及 Executor UI 拒绝；不提供产品 ui.views 容器或原生渲染。
契约可显式指定 1..10000 ms 请求期限，缺省 1000，完整回包与过期 unknown 分开；
自有业务夹具显式 4000，原有子进程 3000 ms 预算不变，1200 ms 延迟护短请求失败 /
长请求实际字节成功 / 再缩短仍失败。76 项完整基线与最终 Client 基线 / 54 组隔离
变异为分轮手工证据，正常断言红、独立配置绿、恢复绿与 SHA256；70 源 / 35
固定工具输入及静态库最终一致。初轮业务 unknown、恢复失败、三条变异配方导致
的编译失败和后续诊断均单列，不计通过或声称已修复原偶发延迟。
JS-on Core views 15 项、默认 JS-off / SSH-on gate 588/184、C 格式 266 文件、
Apple 181/3 与 JNI 147/37 通过；无平台源 / 公共 ABI 变化，不记新 App / APK /
真机 / UI E2E。不增加 CI 二十份，ui.views 容器、三服务组合与完整开发 / 性能 /
平台矩阵及 P5 / P1–P6 仍未完成，收据及限制见插件计划第二十三批。


**插件 P5 无窗口多视图容器与顺序场景（2026-11-16）**：testkit 显式 registry=true
选择 Client ui.views，按每步 receiver / 自己的 lease 驱动 request、独立 close
和快照；原 CLI 注册 profile 不自动加授。官方同源 Portable / Node 包实际读取
两表 / diff、任务视图关闭仍 running、重开新句柄、显式取消终态与同 ID ack；
不提供 GUI 呈现。结果精确区分布尔 / 数字，schema 在 Native 前同步编译，拒绝
异步 validator 并关闭 coercion / defaults / 额外字段移除。两次真实 Node 已进入后
INT / TERM 均验实际回收与目录消失。原 76 回归、新十场景 / 十四变异通过（未重跑
旧 54 变异）；目标正常断言红、独立配置绿、恢复绿 / SHA256，最终新 70 / 旧 72
份指定源与各 35 份固定工具输入一致。首次输入限额变异分类过严及补列私有 SHA
头的收据重跑均单列，不把无效轮次当整轮通过。Core views 15 项、默认 gate
588/184、C 格式 267、Apple 181/3 与 JNI 147/37 通过。无平台源 / 公共 ABI 变化，
不记新 App / APK / 真机 / GUI E2E，不增加 CI 二十份；完整开发 / 原生 UI / 三服务
组合 / 性能矩阵与 P5 / P1–P6 仍待完成，见插件计划第二十四批。


**插件 macOS 页面控件链（P2 第五十六批，2026-11-16）**：
`python3 -B scripts/test-plugin-pane.py --gterm 显式路径 --mutate --out tmp/本轮/receipt.json`
在隔离 Apple 副本验证目录选择注入、实际摘要批准 / 原生编辑 / 启动 / Session 批准按钮、
本轮独占 loopback SSH 的官方 Preview Markdown 精确收件、滚动后 OCR 绘制与停止释放。
一条控件链及五组变异通过：实际生产对象重编、单条正常断言红、同副本独立配置绿、
恢复绿与 SHA256；539 份指定源 / 实际 gterm / XCTest / 三链接 archive 收据核对一致。
系统文件面板、OS VoiceOver / 键盘焦点、该 UI 链的双跳 / 多格式 / 完整生命周期未验。
AppKit 内部替身、编辑 / busy / 滚动前置失败、未定位的清理 InvalidTransition 及旧对象名
收据失败单列，不计通过。透明离屏且禁止激活；默认 JS-off、不增加 CI 二十份，
不记完整 UI E2E 或 P2 / P1–P6 完成；细节与构建 / 回归见插件计划第五十六批。


**插件 macOS 多格式 / ProxyJump 页面（P2 第五十七批，2026-11-16）**：
`test-plugin-pane.py --route both --mutate` 的最终六条链 / 九组完整变异通过。
Markdown + PNG、独立 PNG、HTML + CSS / PNG 各直连与两跳，实际密钥提示端口序列、
原始收件 / SHA256、原生 OCR / 像素、页面实际嵌入 WKWebView 的 CSS / PNG 快照、
附件选择 / 返回和停止移除；目标正常单条断言红、同副本配置绿、重编对象 / 复原绿 / SHA。
540 份指定源收据；gt_timeout 有界，最终 INT / TERM 在实际变异时分别退出 130 / 143，
副本清除和原件摘要不变，不据此称任意后代 / fd 矩阵。初始部分轮有意 INT、修复前
直连 Markdown OCR 基线失败均单列，未计最终整轮；按异步图片最新布局重滚动，不提高
10 秒观察期限。目录选择仍注入；系统文件面板 / OS 辅助技术 / 焦点 / 完整格式与生命
周期矩阵未验，无生产 UI / 公共 ABI / SDK 变化，默认 JS-off、不增加 CI 二十份，
不记 P2 / P1–P6 完成。原生构建、回归和失败记录见计划第五十七批；未推送或发行。


**插件 HTML 临时存储 / 显式撤销（P2 第五十八批，2026-11-16）**：
旧实现每视图独立 / 非持久存储断言在实际编译后正常单条红。生产配置现每次新建
nonPersistent store；dismantle 显式关闭 Coordinator，停止加载 / 解绑 delegate / 释放
保留文档并撤销 scheme resolver；晚更新和未来读不能恢复。六条真实页面链强持有
已移除 / 停止的 WKWebView，仍拒绝原附件读取，返回 HTML 须新 store，CSS / PNG
继续绘制。十四资源基线、原九＋新十二共 21 组完整隔离变异通过：正常单条断言红、
同副本配置绿、实际对象重编、恢复绿 / SHA256。540 源和三 archive 收据一致。
系统文件面板仍注入；不据此称历史 WebKit 存储清除、物理内存归零或任意并发回包 /
后代 fd / 完整生命周期矩阵完成。未改公共 ABI / SDK / Android，不增加 CI 二十份；
P2 / P1–P6 未完成，构建 / 回归 / 失败证据见计划第五十八批；未推送或发行。


**离线签名发布候选（P6 首批，2026-11-16）**：开发工具 release sign / verify
以 Ed25519 / 固定域绑定精确描述、ZIP 字节与包身份；只接受调用者显式公钥渠道策略 /
撤销 / 精确发布者与插件范围。私钥留在外部 signer；测试一次性 KeyObject 从不导出，
公开输入 / 64 字节签名 / 诊断限额及实际 INT / TERM 清理，ZIP32 CRC / 范围 / 路径 /
raw DEFLATE EOF / 膨胀预算保护。34 场景、16 组字节码编译后的行为断言 / 独立 Core
配置绿 / 复原 / SHA256 通过；13 组误接受，三组拒绝类型 / 提前边界改变分别记账。
541 源 / 845 工具输入与旧 76 回归 / 74 源 / 35 构件一致，旧 54 变异未重跑。
错误夹具 / 控制参数 / 标记预扫失败未计绿；完整证据见插件计划 P6 首批。
无 App / Core / 官方公钥 / 生产平台 signer / 安装升级回滚接线，不以工具结果升级
App 发布者状态或运行授权；策略只为本次快照，无网络新鲜度 / 防回滚。未改 C ABI /
平台源、默认 App JS-off，不增加 CI 二十份，不记 P6 / P1–P6 完成，未推送或发行。

**macOS 目录签名核对（P6 第二批，2026-11-16）**：显式公开策略 / 描述 / 签名经
CryptoKit 及有界 JSON 核对，Controller 绑定 Core 不可变 checked metadata；实际
插件页面独立确认策略及核对，输入修改 / 失败 / 晚到结果不沿用旧证明，不批准来源或
Session。CLI 精确解析修正 float 舍入，合法整数值写法保留。原生 11 项 / 10 组同轮
变异与发布 37 场景 / 17 组通过，正常断言或明确行为差异红、独立配置绿、恢复及
SHA256；1029 源 / 848 构件、542 源 / 845 工具输入一致。前置测试编译与 AX 展开
失败未计通过；系统选包面板仍注入，不称 OS VoiceOver 或完整 UI 生命周期验收。
此片只核对目录签名归属，不核对 ZIP 内容、不安装 / 升级 / 回滚或保存信任；无生产
公钥 / signer、Android / 其它端原生验签或真机证据。不改 C ABI，不增加 CI 二十份，
P6 / P1–P6 未完成。构建 / 闸门及收据见插件计划 P6 第二批；未推送或发行。

**macOS 签名 ZIP 版本发布（P6 第三批，2026-11-16）**：先以 CryptoKit / 系统 libz
核对真实归档，再由 Core capture / check 固定内容，页面展示身份、权限目的 / 程序、
SDK / 协议及执行端依赖声明；显式安装到私有只读版本目录，不调用外部解压器或执行
入口。flock、64 条目限额、成员集合 / fd 文件戳、fsync / RENAME_EXCL、已有空槽
不覆盖；父同步失败保留已发布效果和持久性未知。取消 / 关闭 / 失败清理持有原 owner
并可重试，不按前缀采纳或删残留。28 项原生 / 实际控件、旧十＋新十四共 24 组同轮
变异通过：实际对象重编、正常断言红、独立配置绿、恢复和 SHA256；1034 源 / 848
工具与 archive 收据一致。先行清理错误可见性失败、主动中断与收据 scope 修正单列，
不计最终通过。旧包 / 目录保留不冒充活跃 Runtime 引用验收；缺跨重启列表、活动版本、
升级 / 回滚 / 禁用、崩溃持久归属与残留维护、断电及其它端 / 真机证据，不部署远端
Executor、不授予来源或 Session。未改 C ABI，不增加 CI 二十份；默认 App JS-off，
P6 / P1–P6 未完成，平台构建 / 闸门见计划第三批，未推送或发行。

**macOS 活动版本与跨重启列表（P6 第四批，2026-11-16）**：列表只读声明不认证
发布者；显式当前策略验签＋Core 重读后选择 / 回滚，不从安装内策略继承信任，不
恢复来源批准 / Session。本机选择以原始 SHA256 CAS、nonce、独立插件代次、根锁 /
文件同步 / 原子替换保护；另一 owner 及旧批准不能复活，缺失 / 损坏不得退回同名
开发包；不可读的所选版本仍可显式禁用。39 项本地原生 / 实际控件、1 条自建单跳
SSH 的活跃 Runtime 链与旧 24＋新 9 共 33 组通过：实际对象重编、正常断言红、
独立配置绿、复原及 SHA256。切换 / 禁用后旧 Runtime 仍 ACTIVE / 原摘要，继续
发布 Native 文件并取得 peer confirmed / 精确 SHA，最后正常停止；1049 指定源 /
849 工具及构件一致。旧只读变异的后续 unexpected 未计正常红，修正测试后全量
复跑；不是所有业务 / tmux / 设备 / 长时换网或云端验收。不改 C ABI，不增加 CI
二十份，默认 App JS-off；自动下载更新、卸载 / 回收、崩溃持久归属 / 遗留维护、
其它端管理及 P6 / P1–P6 未完成，构建与闸门见计划第四批；未推送或发行。


**macOS 显式插件版本回收（P6 第五批，2026-11-16）**：完整有界清单经确认 / CAS /
fd 身份重核；已启用或被 Session / Runtime 共享引用占用时保留。实际关闭完成才释放
引用，正常停止后可回收，来源 / Session 不迁移，禁用记录保留。删除意图先同步、
改名再同步后才逐项 unlink；新文件 / 替换文件保留，无启动重放、前缀采纳或远端 /
业务文件清理。本次删除、已不存在观察与持久性未知分开；原树完整且未改名可显式
撤销，进入删除后拒绝撤销，无关目标对象不能证明已开始。62＋2 SSH / 46 组完整先行
与收尾两状态修正后的 63＋2 SSH / 新两组为分轮手工证据，共 48 个不同保护：实际
对象重编、正常断言红、独立配置绿、恢复及 SHA256。1051 源 / 849 工具与 archive，
两轮间仅五份相关源变化，未称最终源码重跑旧 46 组。恢复变异后续读取造成的两条
unexpected 与两次配方预匹配失败未计通过，未放宽分类。JS-enabled / 默认 Release、
默认宿主 109 项 / 34 跳过 / 零失败及独立 WebKit CSS / PNG、默认 Core 588/184、
C 格式 19.1.7 / 267、Apple 181/3 与 JNI 147/37 通过。不改 C ABI / SDK 身份，不增
CI 二十份，默认 JS-off；无实际断电 / SIGKILL、安装孤儿、所有并发 / 文件系统 /
异版本业务维护性能、其它端 / 真机 / 云端或发行证据，P6 / P1–P6 未完成。详见
插件计划第五批及开发指南；未推送。


**macOS 回收实际进程死亡（P6 第六批，2026-11-16）**：工程 tmp 内声明夹具 / 独立
原生子进程，nonce / PID / dev / inode 就绪且仍存活后精确 SIGKILL，新 Controller
列表 / 记录 / close 不重放，显式继续。十三场景与六组实际生产对象重编变异通过：
正常断言红（零 unexpected）、同构件独立配置绿、恢复和 SHA；25 次刻意死亡与
58 条正常绿 / 六条正常红分开记账。驱动自身 INT / TERM 在确认 native 持有引用后
注入，退出 130 / 143、实际子进程退出 / 私有目录清理通过。1035 源 / 四份实际工具
与 archive 一致。首轮替换根夹具跨父目录移动的 EACCES 不计绿或目标红；修正
自有夹具权限处理后全量重跑。23 项维护 / 配置基线、默认 Release、默认宿主
40 项 / 10 跳过 / 零失败及独立 WebKit CSS / PNG、Core 588/184、C 格式 267、
Apple 181/3、JNI 147/37 通过。不改生产源码 / C ABI / SDK，不认证声明 / 运行
插件，不据此称真实 Runtime 跨进程整链、GUI 重启 / 远端业务退出、断电、驱动
SIGKILL 自清理、安装孤儿或完整 FS / 并发 / 设备矩阵。默认 JS-off，不增 CI 二十份，
P6 / P1–P6 未完成；收据 / 限制见插件计划第六批，未推送 / 发行。


**macOS 安装暂存持久归属（P6 第七批，2026-11-16）**：先持久精确预约，再 mkdir /
登记 dev / inode / uid / birth，未确认身份不采纳目录；发布前记目标，实际同一目录
在目标出现才观察 published，只退役元数据。abandoned 经完整快照 / 显式确认、
格式 2 删除意图及显式继续维护，旧安装版本格式 1 仍读；根权限创建后变化也拒绝。
41 原生数据 / 页面 / 既有维护基线、九次实际死亡、八组隔离变异通过：实际对象重编、
正常红 / 独立配置绿 / 恢复 / SHA；1039 源 / 四份工具。两次预期保留清单 / 身份
类型的测试判据错误未计绿，修正后重跑；空 ledger 目录与已退役记录另验，保留原
暂存应移除断言。既有 enabled 签名安装 / 版本 / 回收 63＋2 SSH / 48 组完整复验，
1057 源 / 849 构件；默认旧实际死亡十三场景 / 六组复验。驱动 INT / TERM 的实际
进程退出 / 私有树清理通过，不将 SIGKILL 注入算目标红。两档 Release、默认宿主
103 项 / 33 跳过 / 零失败及独立 WebKit、Core 588/184、C 格式 267、Apple 181/3、
JNI 147/37 通过。不改 C ABI / SDK 或 Runtime 预算，不增 CI 二十份；默认 JS-off。
无完整磁盘故障 / 断电 / 同 uid 恶意并发、旧未登记孤儿维护、其它端 / 真机 /
云端结论，P6 / P1–P6 未完成。精确命令 / 分档收据见插件计划第七批，未推送。


**macOS 未知目录显式核对（P6 第八批，2026-11-16）**：私有安装根未知目录保持归属
未知，根 / 子身份令牌与完整有界成员预览、单独勾选确认建立本次删除决定；不按前缀
采纳、不补写安装来源、不退役旧未确认归属记录。启用选择、共享引用、根 / 目录 /
内容替换拒绝旧预览；格式 3 意图无启动重放，显式继续 / 未开始时撤销，格式 1 / 2
仍读。60 项数据 / 实际后台页面 / 既有维护基线、五次实际死亡及八组正常断言红 /
独立配置绿 / 复原 / SHA 通过，1043 源 / 四份工具。首轮启用变异因测试未释放意外
预览而出现 pending / unexpected，未计正常红，修正收尾后八组完整重跑；配方未匹配
亦未计通过。旧暂存 41 / 九死亡 / 八组、旧版本十三场景 / 六组完整复验；签名安装 /
版本 / 回收 enabled 63＋两真实 SSH / 相关 removal 十五组通过，1061 源 / 849 工具，
其余签名变异未记本轮复跑。三套驱动 INT / TERM 实际退出 / 私有夹具清理通过，
SIGKILL 不算目标红。两档 Release、默认宿主 125 / 26 跳过 / 零失败与独立 WebKit、
Core 588/184、格式 267、Apple 181/3、JNI 147/37 通过。不改 C ABI / SDK，不增
CI 二十份，默认 JS-off；顶层非目录 / 私有元数据、损坏 ledger、完整磁盘故障 /
恶意同 uid / 其它端及真机矩阵未完成。P6 / P1–P6 未完成，收据与限制见插件计划
第八批；仅本地提交，未推送 / 发行。


**macOS 当前策略复核与批量禁用（P6 第九批，2026-11-16）**：显式独立策略快照复核
全部已启用选择，签名 / Core 内容接受、策略拒绝与无法核验分开；读取失败不当撤销。
默认全不选，仅禁用明确勾选的被拒绝 / 未知选择，一次原子 CAS / 新代次，根身份 /
策略摘要 / revision 或旧票据变化拒绝；接受 / 未勾选 / 已禁用项保留。观察不加载 /
批准来源，禁用阻止跨 Controller 新建，已有 Runtime 需单独停止。最终默认数据 /
后台控件 / 配置 22 项及十六组实际对象正常断言红 / 配置绿 / 复原 / SHA 通过；
1049 源 / 四工具。enabled 88 项、三真实 SSH 生命周期与旧签名八＋新增真实 Core
两组通过，1067 源 / 849 工具；实际篡改包与跨 Controller 禁用另验，不借 metadata
注入证明 Core 内容。首轮表单 busy、三次预跑主动结束、测试错误类型编译失败、调试
Node 链接拒绝及漏接收目录的四条 invalidHostProfile / unexpected 均未计通过；只
修测试和驱动诊断，生产 HostProfile 保护保留，修正后完整重跑。两档 Release、默认
宿主 150 / 30 跳过 / 零失败及独立 WebKit、Core 588/184、格式 267、Apple 181/3、
JNI 147/37 通过。无新 C ABI / SDK，不增 CI 二十份，默认 JS-off；非联网新鲜度 /
持久信任根 / 防回滚 / 自动升级、运行全局撤权、其它端 / 真机 / 云端证据。P6 /
P1–P6 未完成，收据与边界见插件计划第九批；仅本地提交，未推送 / 发行。


**macOS 升级／回滚选择预览（P6 第十批，2026-11-16）**：管理页先验签 / Core 捕获，
raw manifest 摘要绑定后展示能力、完整权限目的 / 程序和执行端声明；旧版经同策略
核验才比较，未知差异为 nil，不当空 / 不判版本先后。候选共享引用保留到取消或
确认，确认重核根 / 候选对象、策略 SHA、选择 CAS 并重新捕获；选择不继承来源 /
Session，旧运行保留快照。取消纳入活动计数，释放完才开放后续维护。最终默认
19 项 / 八组生产对象正常断言红 / 配置绿 / 复原 / SHA 通过，1056 源 / 四工具；
enabled 114 项、四真实 SSH 生命周期及旧 catalog 七＋新选择七组通过，1074 源 /
849 工具。根夹具未杀变异与迁父 EACCES 不计绿，保留根拒绝断言并修正自有夹具；
首轮真实 UI 一条 unexpected 经定向捕获确认为策略编辑取消未完成的 occupied，
修复实际忙碌生命周期，cancel-busy 变异回放原缺陷正常红。所有前置失败单列，
最终完整专项与默认档复跑通过。两档 Release、默认宿主 176 / 38 跳过 / 零失败与
独立 WebKit、Core 588/184、格式 267、Apple 181/3、JNI 147/37 通过。无新 C ABI /
SDK / Runtime 预算，不增 CI 二十份，默认 JS-off。非其它端 / 真机 / 云端、OS
辅助技术 / 全大字号主题、所有并发 / 磁盘故障、自动升级 / 防回滚证据。P6 /
P1–P6 未完成，收据 / 范围见插件计划第十批；仅本地提交，未推送 / 发行。


**Android 发布签名后端（P6 第十一批，2026-11-16）**：新增
`gt_ext_release_verify`、Swift / JNI / Kotlin，Core 589 CTest / 185 ABI、Apple 182/3、
JNI 148/37。Android 显式复用已锁定 libcrypto；host 默认关闭新 crypto 开关，macOS
产品仍用 CryptoKit。域含一个 NUL / 精确描述字节、共享预算 / 释放，AUTH / INTERNAL /
UNSUPPORTED 分开；严格 v1 策略 / 撤销及真实 Core 固定包四字段绑定已接原 worker，
不批准来源或 Session。API29 / 36 arm64、4 KiB 页无窗口模拟器各 42 项同源组合；
API36 十一组设备变异与共享 C 九组 / 四故障路径、ASan/UBSan 通过，正常断言红 /
独立配置或参数绿 / 复原 / SHA 分开记账。700 指定源一致；两设备组合所用 App APK
摘要不同，测试 APK 相同，不能写成同一 APK 双设备验收。新 Swift 绑定两项、默认
宿主 15 项 / 1 明确跳过 / 零失败与独立 WebKit、两端 Release / Android 四模块 lint、
格式 271 通过。误带测试类、ABI 拒收的初始 JNI 配方、早轮 crash 写共享系统 tmp、
发现记录准备判据、冷启动两条 CPU_BUDGET / 准备超时及临时 Debug 签名不一致均
保留，不计通过；最终隔离启动 / 真实 CPU 空闲后完整复跑，未提高生产预算、未删
共享历史诊断。证据 / 摘要见插件计划第十一批。仅内部数据入口，Android 签名文件 /
策略确认页面、ZIP 安装 / 版本 / 维护及真机 / 16 KiB / 完整分发矩阵未完成；默认
App JS-off，不增 macOS CI 二十份，P6 / P1–P6 未完成，仅本地提交，不推送 / 发行。

**Android 签名材料与策略确认（P6 第十二批，2026-11-16）**：Settings 的真实系统选择器
捕获描述 / 签名 / 渠道策略字节与 SHA，显式确认具体策略快照后接 Controller / Core；
重选 / 撤回 / 离页清证明，旧异步结果不恢复，来源 / Session 独立。读取按实际 EOF /
错误 / 上限与只读 fd，API29 适配器、合作取消及进程内共用的两个读取名额保留至 IO
实际退出；无外部 provider 硬期限承诺。API29 / 36 arm64、4 KiB 页无窗口模拟器
各 62 项同源同 APK 组合，API36 十八组正常断言红 / 同 APK Core 绿 / 复原 / SHA
通过，706 输入一致。两档 Android Release / R8、普通 Debug / 测试 APK、四模块
lint、格式 271、Apple 182/3、JNI 148/37 通过。Core 最终 589/589 / 185 ABI；前置
process_stream_boundary_poll-retry 启动失败一次，原样定向二十次及隔离诊断四十次
未复现，根因未确认，未改 C 或断言且不宣称修复。取消夹具 / UI 时钟、API30 lint、
系统选择回包与异步返回时序的前置失败单列，不计绿。范围 / 摘要见插件计划第十二批，
不增 CI 二十份，默认 JS-off；ZIP 安装 / 版本 / 维护及真机 / 16 KiB / 全客户端未完成，
P6 / P1–P6 未完成，仅本地提交，不推送 / 发行。

**Android 签名 ZIP 预检（P6 第十三批，2026-11-16）**：内部 worker 先认证描述 / 策略，
有界 pread / raw DEFLATE / CRC / 完整范围核对后由真实 Core capture / check 绑定四字段；
单次应用私有缓存退出清理，不登记来源 / 安装或授予 Session。十八项 ZIP / fd / 取消
与五项真实 Core 新用例加入组合，API29 / 36 arm64、4 KiB 页无窗口模拟器最终各
85 项、同源同 APK 通过，710 指定输入一致。API36 二十六组正常断言红 / 同 APK
Core 绿 / 复原 / SHA 通过，包含实际签名 / Core 身份与缓存清理。首轮 coroutine
位置 / 测试访问器编译失败及三条 UNKNOWN 对 STALE / NOT_FOUND 断言错误均保留，
修正测试 / 调用方取消上下文，未折叠生产三态。Core 589/589 / 185 ABI、格式 271、
Apple 182/3、JNI 148/37、两档 Android Release / R8 / 四模块 lint 与最终普通 Debug /
测试 APK 通过；此前进程边界偶发失败未在本片声称修复。输入须为
已捕获的可定位只读文件；URI 导入 / 原生 ZIP 安装页、持久发布 / 版本 / 维护、同 uid
恶意并发、真机 / 16 KiB / 完整矩阵未完成。默认 JS-off，不增 CI 二十份，P6 /
P1–P6 未完成；证据 / 收据见插件计划第十三批，仅本地提交，不推送 / 发行。

**Android ZIP 导入与完整声明预检（P6 第十四批，2026-11-16）**：四个真实 SAF 选择、
策略快照确认、16 KiB 分块 / 72 MiB 捕获、无文件名只读 fd / 借用生命周期已接原生页。
大小材料共用两个读取名额到实际 IO 退出；dispatcher 取消未交付结果也关闭 owner。
原始清单绑定 Core raw SHA，再严格解码完整权限目的 / 程序 / SDK / 协议 / Executor
依赖；独立 worker / HostProfile 快照不占当前 Client pump，不登记来源 / 安装或授予
Session。API29 / 36 arm64、4 KiB 页无窗口模拟器最终各 109 项同源同 APK，720 输入
一致，API36 三十三组正常断言红 / 同 APK Core 绿 / 复原 / SHA 通过。首轮第三十二组
目标 / 复原均为 DocumentsUI 夹具失败，未计红 / 绿；本地文件行改真实 finger 触摸，
仍验实际回包，UI 变异另匹配具体预期断言，原共享旧 helper 未改。完整重跑后通过，
旧三十一组不重复累加。Core 589/589 / 185 ABI、格式 271、Apple 182/3、JNI 148/37
及两档 Android Release / R8 / 四模块 lint、最终普通 Debug / 测试 APK 通过；
旧进程边界偶发失败未声称修复。尚非持久安装 / 版本 / 全局撤销 / 死亡维护，
非不合作 provider 硬期限、真机 / 16 KiB / 辅助技术或完整矩阵。默认 JS-off，不增
CI 二十份，P6 / P1–P6 未完成；范围 / 收据见插件计划第十四批，仅本地提交，不推送。


**Android 签名版本原子安装（2026-11-16，插件 P6 第十五批）**：完整预检后独立安装
确认，原材料 / 签名和完整声明重核对，真实 Core 检查目标 payload；根父目录同步、
先持久意图后同文件系统暂存、锁 / fd / 已知树完整核对、只读冻结和不覆盖 renameat2
已接。预发布仅清理仍可核对的本次内容；发布后同步未知仍保留目标和意图，不回滚，
重启不自动重放 / 收养 / 删除。不登记来源、授予 Session、启动 Runtime / 远端部署
或选择活动版本。API29 / 36 自有无窗口 arm64、4 KiB 页各 146 项正常结束，零失败 /
错误 / 跳过，同 APK 和 730 输入一致；40 组实际编译后正常单条断言红、同 APK Core
绿、复原绿和 SHA，六个真实进程死亡检查点单独记账。初始公共 API 编译错误、冻结
目录移动夹具失败、可靠管道修正前的取消调度超时、早期 SIGINT 中止及误选 JVM
lint task 均保留且未计通过。最终 Core 589/589 / 185、格式 272、Apple 182/3、
JNI 148/37、两档 Android Release/R8 / 四 Android 模块 lint 及普通 Debug / 测试 APK
通过；启用档实际核对 JNI 保留规则 / ELF 导出，不称 Release 设备验收。默认 JS-off，
不增 CI 二十份；版本列表 / 启停回滚 / 运行引用、持久残留维护与更强归属、恶意同
uid / 完整磁盘故障 / 断电、真机 / 16 KiB / 完整 provider / 辅助技术与客户端仍待
完成，P6 / P1–P6 未完成。收据见插件计划第十五批，仅本地提交，不推送 / 发行。


**Android 已保存版本观察与选择记录事务（2026-11-16，插件 P6 第十六批）**：设置只读
观察 / 刷新、未核验声明标签、活动记录及损坏未知分开，跨 Controller / Activity
重开保留，不创建根 / 采集加载包 / 采纳选择 / 授予来源或 Session / 删除。底层
严格格式 1、两次原始修订 CAS、新 generation / nonce、只读候选原子替换及同步
未知回执；版本共享 fd / borrow 生命周期为独立组件，未联动实际运行 / 回收。
API29 / 36 自有无窗口 arm64、4 KiB 各 177 项正常结束 / 零失败错误跳过，同源同
APK / 739 输入；45 组真实编译后单条正常断言红、同 APK Core 绿、恢复绿与六源
SHA。变异复原重编译的 App 字节变化未定位，不冒称可复现构建，以最终同 APK
两设备为准。Core 589/589 / 185、格式 272、Apple 182/3、JNI 148/37、两档 Android
Release/R8 / 四模块 lint 和普通 Debug / 测试 APK 通过。pin 的 JNI ELF 导出已验，
当前 R8 keep seeds 未列该未消费入口，不据此推断 dex 去留。旧变异 / 死亡专项未
冒称重跑；默认 JS-off，不增 CI 二十份。活动选择 / 启停回滚的策略 / Core 再验证、
新运行批准代次 / Session 准入、旧实例引用 / 回收及残留维护继续；非新跨进程死亡 /
断电 / 真机 / 16 KiB / 完整 provider / 辅助技术或全客户端验收，P6 / P1–P6 未完成。
精确范围和收据见插件计划第十六批，仅本地分批提交，不推送 / 发行。


**Android 活动选择、禁用回滚与旧实例引用（2026-11-16，插件 P6 第十七批）**：显式
可信策略 / 真实 Core 再认证、完整新旧声明及未知差异、独立确认与精确 CAS 已接；
每次选择更新批准代次，新来源 / argv / Session 准入不继承旧授权或回退同名开发
目录。已创建 Session / Runtime 保留原包和共享版本 fd，实际结束释放后关闭引用；
禁用仅阻止后续新实例，不替代来源批准 / Session consent，不部署 / 自动运行。
API29 / 36 自有无窗口 arm64、4 KiB 最终同源 753 输入 / 同 APK 各 201 项正常结束，
零失败错误跳过；另各两条真实两跳 SSH、独立远端完成和引用专项。36 新保护分轮
19＋17、旧 worker / 页面四组实际编译后单条正常断言红、同 APK Core 绿、恢复绿 /
SHA。早期非 void / 重复审查构造、SAF 手势、版本握手 / 回执时序、Main dispatcher、
旧 ZIP 布局异常和未杀死的冗余探针均单列，不计正常红 / 通过，不删断言换绿。
首十九组后的变化为测试 / 夹具与驱动，生产源相同，不称一次全量同源码变异；原
其它变异 / 六死亡点未冒称重跑。复原重编译 APK 字节变化未定位，不称可复现构建。
Core 589/589 / 185 ABI、格式 272、Apple 182/3、JNI 148/37、两档未签名 Release /
实际 R8 / 四模块 lint 与普通 Debug / 测试 APK 通过；启用档 JNI lock / publish /
pin 的 mapping / keep seeds 与实际 ELF 导出已验，不称 Release 设备执行。默认
JS-off，不增加 CI 二十份；版本回收 / 持久残留维护、Android 当前策略批量复核与
更强归属继续，非新跨进程死亡 / 断电 / 磁盘故障 / 恶意同 uid / 真机 / 16 KiB /
完整 provider / 辅助技术或全客户端 / 发行验收，P6 / P1–P6 未完成。精确失败与收据
见插件计划第十七批，仅本地分批提交，不推送 / 发布。

本批收尾：自有 AVD 均退出 0，停止所属构建 JVM；tmp 验后清理 63 顶层项 / 8738
普通文件 / 267225851 逻辑字节，原 eq 三件和根目录保留且摘要不变；旧 65439 /
2637 未动。753 指定输入清理后复算一致；细节见插件计划第十七批。


**Android 显式版本回收与删除意图（2026-11-16–17，插件 P6 第十八批）**：完整清单与
单独确认、目标独占引用、选择 / fd / 内容再核对，先持久意图再不覆盖移开并逐项
核对删除；明确继续 / 尚未移除时撤销，无启动 / 刷新重放或前缀收养。API29 采用
非零 inode generation，API30+ 创建时间，缺能力显式拒绝。旧开发 Session / Runtime
后关联已安装相同摘要也先准备 / 发布后转交共享引用；实际释放后才可回收，保留
禁用选择 / 未知对象，磁盘效果、已不存在观察、同步与内存清理错误分开。自有 API29 /
36 no-window arm64 / 4 KiB 最终各 235 项组合、另各四条实际两跳 SSH，固定同 APK /
1596 指定输入；最终 27 新保护同源完整扫描、旧 catalog 三组正常断言红 / 同 APK Core 绿 /
复原 / 六生产源 SHA 通过。前置 24 组分轮 / 25 组为历史，不累加保护数；收尾另护全目录已不存在时的回执清除、
以及文件 / 目录同步失败仍准确记已删除效果。statx 的 API29 SIGSYS、路径 / 硬链接夹具、取消测试编译、系统文件
选择器 / 双 observer 争锁及错误 debug 证书环境失败均单列，不计红或通过。Core
589/185、格式 272、Apple 182/3、JNI 148/37、两档 Release/R8/四模块 lint 与普通
Debug/测试 APK 通过，启用档五个相关 JNI 的 mapping / keep seeds / ELF 已核对。
非新实际杀进程 / 断电 / 所有文件系统 / OS 升级 / 同 uid 恶意写入 / 真机 / 16 KiB /
完整 provider / 无障碍或全客户端；默认 JS-off，不增 CI 二十份。安装暂存 / 未知
残留维护、Android 当前策略批量复核继续；P6 / P1–P6 未完成，仅本地分批提交，
不推送 / 发行。范围、原件摘要与清理收据见插件计划第十八批。

前置清理：自有 AVD 均退出 0，所属 JVM 78096 / 78283 已结束，旧 65439 / 2637 未动。
验后清理 tmp **62 个顶层项 / 689 个目录 / 8156 份普通文件 / 417170354 逻辑字节**
（397.84 MiB）；镜像由 owned context 先行清理，不把逻辑字节当 APFS 实际回收量。
仅保留原 eq 三件和 tmp 根，三件 SHA256 均不变；1596 份指定源输入清理后复算一致。

最终收尾：全部 owned AVD 退出 0，所属 JVM 71445 / 71649 已结束；旧 65439 / 2637 未动。
补正阶段清理 tmp 32 个顶层项 / 623 目录 / 8072 普通文件 / 265159930 逻辑字节
（252.88 MiB），不等同 APFS 实际回收；原 eq 三件摘要不变，tmp 根保留。
1596 指定输入清理后复算 `81447f532de0d6850e233489f3bd722220e12073a1269f8473ef38adc2d32b04` 一致。


**Android 安装暂存归属维护（2026-11-17，插件 P6 第十九批）**：新安装格式 2 预约 /
强父子身份、暂存自身独占引用和严格有界七态只读观察已接；已登记遗留暂存经完整
快照 / 单独确认进入格式 2 删除意图，已发布 / 已不存在记录另确认只退役元数据。
部分可写文件的允许仅限已确认暂存，已安装版本的只读要求保持；目录删除 / 元数据
退役 / 版本来源缓存分开，重启 / 刷新无重放。未知 / 无登记 / 旧弱归属对象保留，
后续独立核对及 Android 当前策略批量复核继续。新增 21 保护分轮 19＋2、旧删除 27 /
安装 6 / 列表 3 复验均正常单条断言红、同 APK Core 绿、恢复绿与生产源 SHA；首次
页面等待超时与死亡用例命令路径断言失败单列，不计通过或正常红。本批设备、实际
进程死亡、构建、输入摘要与清理收据见插件计划第十九批。自有 no-window API29 /
36 arm64、4 KiB 页同 APK 各 261 项、各四条两跳 SSH、各九场景 / 十次实际 PID
结束通过，1605 指定输入一致；不增加 CI 二十份，默认
JS-off，P6 / P1–P6 未完成，非真机 / 16 KiB / 完整提供方 / 无障碍、断电 / 所有文件
系统 / OS 升级 / 同 uid 恶意并发或全客户端验收，仅本地分批提交，不推送 / 发行。

本批 Core 589/185、格式 272、Apple 182/3、JNI 148/37、两档 Release/R8/四模块 lint
和普通 Debug/测试 APK 通过，启用档五 JNI 的 mapping / keep seeds / APK ELF 已验；
不称 Release 设备 / 云端 / 签名发行或干净检出证据。


本批收尾：两个 owned AVD 均退出 0，构建 JVM 40167 / 40321 已结束，旧 65439 /
2637 未动，adb 无在线设备。验后清理 tmp 55 个顶层项 / 693 目录 / 8597 普通文件 /
331103022 逻辑字节（315.76 MiB），镜像此前已由 owned context 清理；
不把逻辑字节当 APFS 实际回收量。仅保留原 eq 三件与 tmp 根，三件 SHA256 均不变。
1605 指定输入清理后复算 `80e393e0f1147ffa63e03d34d8fcc8282da9bcef5d70f5d7d8e4455203da3145` 一致。


**Android 归属未知目录的完整核对（2026-11-17，插件 P6 第二十批）**：私有根当前身份 /
名称 / 子目录完整 stat 令牌、完整有界快照、额外未知确认及格式 3 删除意图已接。
已知版本 / 暂存保持原入口，损坏选择 / 归属不降为空；不支持名称、顶层非目录与
私有元数据保留。新决定不补来源 / 退役旧记录或清版本来源缓存，无重启 / 刷新
重放；启用但声明坏的目录仍须先禁用。新增 21 保护在最终生产 / 测试源完整重跑，
旧删除 27 / 暂存 3 / 列表 3 复验通过：正常单条断言红、同 APK Core 绿、恢复绿 /
生产源 SHA。前置字段编译失败、可写 pin 夹具失败和异步测试断言崩溃单列，未计
通过或正常红。自有 no-window API29 / 36 arm64、4 KiB 页各 285 项组合及四条真实
两跳 SSH；另各新增五场景 / 五次实际 PID 结束、旧安装九场景 / 十次实际 PID
结束通过，同一固定 APK 对、1615 输入一致。构建、收据与清理见插件计划第二十批。
默认 JS-off，不增 CI 二十份，P6 / P1–P6 未完成；当前策略批量复核 / 显式禁用、
损坏元数据 / 旧弱归属、顶层非目录与完整磁盘故障 / 断电 / OS 升级 / 同 uid 恶意
并发 / 真机 / 16 KiB / 提供方 / 无障碍和客户端矩阵继续，仅本地提交，不推送。

本批 Core 589/185、格式 272、Apple 182/3、JNI 148/37、两档未签名 Release/R8/
四模块 lint 及普通 Debug/测试 APK 构建通过；启用档六 JNI 的 mapping / keep seeds /
APK 内实际 arm64 ELF 导出已验，未执行 Release 设备 / 云端 / 干净检出构建。


本批收尾：两个最终 owned AVD 均退出 0，前置失败的所属 AVD 亦已退出；构建 JVM
49968 / 50135 已结束，旧 65439 / 2637 未动，adb 无在线设备。验后清理 tmp 52 个
顶层项 / 646 目录 / 8508 普通文件 / 269036094 逻辑字节（256.57 MiB）；镜像此前
由 owned context 清理，不把逻辑数当 APFS 实际回收量。仅保留原 eq 三件和 tmp 根，
三件 SHA256 均不变；1615 指定输入清理后复算
`2289f7ccde0cb16caabad9ec3b1ad2f865073a507a87f8ea8a301a2365b3b2dd` 一致。

**Android 当前策略复核与显式批量禁用（2026-11-17，插件 P6 第二十一批）**：独立
策略字节快照、真实 Core 包内容 / 身份与签名复核，接受 / 拒绝 / 未知分开；默认不
勾选，只有拒绝 / 未知的准确子集可一次 CAS 禁用并换代次。强根 / 原始策略摘要 /
全选择修订、取消 / 晚到票据、材料 / 信任同步和回执不明保护已接。重捕获走独立
worker；同 / 另一 Controller 禁用后后续准入拒绝，已有 Runtime / 版本引用保持，
不自动停止 / 删除或授予来源。多 tcp:0 导致 ADB SIGABRT、最初 SAF 命中背景标题
及 stdin EOF 探针单列，不计正常断言红 / 整轮通过。具体设备、变异、构件和清理
收据见插件计划第二十一批；默认 JS-off，不增 CI 二十份，P6 / P1–P6 未完成。

本批最终自有 no-window API29 / API36 arm64、4 KiB 页同一固定 APK 各 311 项与各
六条真实两跳 SSH 通过，十二份独立 Executor 完成证明已验。策略新 27、原 Catalog
45 / 发布签名 11 / 选择模型 4，以及 API29 旧 token / 新 stat 两组分轮正常单条
断言红 / 同 APK Core 绿 / 复原绿 / SHA；首次旧目录夹具未建立 stat 变化的
311 项组合失败单列，显式改变 mtime 后两设备完整重跑。1623 指定输入最终一致。
Core 589/185、C 格式 272、Apple 182/3、JNI 148/37、两档未签名 Release/R8/
四模块 lint 与普通 Debug/测试 APK 通过，启用档六 JNI 的 keep / 实际 DEX / APK
ELF 已验；非 Release 设备 / 发行 / 真机 / 16 KiB 页或完整客户端验收。

本批收尾：所有 owned AVD 已退出 0，adb 无在线设备；构建 JVM 34058 / 34236 已
结束，旧 65439 / 2637 未动。验后清理 tmp 65 个顶层项 / 782 目录 /
9195 普通文件 / 284020250 逻辑字节（270.86 MiB），不把逻辑大小
当 APFS 实际回收量；镜像此前已由 owned context 清理。仅保留原 eq 三件和 tmp 根，
三件 SHA256 均不变；1623 指定输入清理后复算 `23f748be0dc8a2ebb11ac413e13ea8581cdd1c93701d850cd56ba81ca55b5fe3` 一致。

**Android 升级／回滚 Client 能力声明差异（2026-11-17，插件 P6 第二十二批）**：
第十七批已有的完整权限 / 真实 Core 预览与两步确认继续使用；本批补 Client 必需
能力双向增删、共同项排除、已禁用旧选择仍参与比较及能力 / 权限未知提示。旧版
缺失 / 验签失败不降为空，完整候选仍可独立确认；原始 manifest 摘要绑定和新来源
批准不放开。独立公共签名夹具改变能力 / 用途 / 程序 / Executor variant，入口
主动 throw；纯 DTO 比较与真实 Core / 原生页面证据分开，具体收据见插件计划
第二十二批。默认 JS-off，不增 CI 二十份，P6 / P1–P6 未完成，仅本地提交不推送。

本批最终自有 no-window API36 / API29 arm64、4 KiB 页同一 APK 各 321 项与各六条
真实两跳 SSH 通过；十二份独立 Android Executor 及十二份主机对照证明已验。
新十二组完整重跑、原选择三十六组完整复验（含五个真实 Runtime 保护）均正常单条
断言红 / 同 APK Core 绿 / 恢复绿 / SHA；首隐藏控件配方的空值收窄编译失败未计红。
1628 指定输入一致，Core 589/185、格式 272、Apple 182/3、JNI 148/37、两档未
签名 Release/R8/四模块 lint 与普通 Debug/测试 APK 通过，启用档六 JNI 的实际
DEX / ELF 保留已验。非真机 / 16 KiB 页 / Release 设备 / 云端或全客户端验收。

本批收尾：全部 owned AVD 退出 0，adb 无在线设备，构建 JVM 78436 / 78625 已
结束，旧 65439 / 2637 未动。验后清理 tmp 63 顶层项 / 803 目录 / 8954
普通文件 / 274106615 逻辑字节（261.41 MiB），不等于 APFS 实际回收量；
镜像此前由 owned context 清理。仅保留原 eq 三件与 tmp 根，三件 SHA256 不变。
受测生产 / 断言输入均保持原字节；唯一验后生成器注释补正已在计划单列。
1628 最终输入清理后复算 `50ad56f795a88b4f8e443f9c726607f39ab8c028ba5ea7d224853e3c648f03ad` 一致。


**Android 插件页面返回终端（P2 第五十九批，2026-11-17）**：按会话 UUID 捕获实际
IME，返回原 PTY / Surface 时一次性恢复焦点与开 / 关，已退出视图拒绝输入。最终
API29 / API36 arm64 / 4 KiB 页同 APK 各 127 项与各三条真实 SAF / 两跳 Preview 链
通过，OS 按键 PID / Runtime 实例 / 资源标识、停止释放及安装包 / 1631 源摘要核对
一致。新九组为八加一分轮（内部清空探针撤回，不计最终组），旧会话十组 / 布局四组
复验：实际编译后指定单条正常断言红、同 APK Core 绿、恢复重编绿和源码 SHA256。
Core 589/185、格式 19.1.7 / 272、Apple 182/3、JNI 148/37、两档未签名 Release/R8 /
四模块 lint 与 Debug / 测试 APK 通过。前置配置 / 编译 / 测试步骤与快照身份错误单列，
精确收据见插件计划第五十九批。默认 JS-off，不增 CI 二十份；非真机 / OEM 输入法 /
TalkBack / 16 KiB / Release 设备 / 云端 / 全客户端验收；P1–P6 继续，仅本地提交。

本批已恢复默认 JS-off Debug / 测试 APK；owned AVD 全退出 0、adb 无在线设备，
自有构建 JVM 52313 / 52481、54133 / 54290 已退出，原 65439 / 2637 未动。
验后清理 tmp 37 顶层项 / 661 目录 / 8459 普通文件 / 226194941 逻辑字节及四链接，
仅留原 eq 三件与 tmp 根，三件摘要不变；镜像由 context 先行清理，不称 APFS 实际
回收量。清理后 1631 源仍为 `e5ba59b355d348c56514c336a5d933e1c071bc7de3a8d79d95b13a62adbc66d3`。


**Android 静态 HTML 原生预览（P2 第六十批，2026-11-17）**：独立合成 HTTPS origin
只映射固定文档与显式 CSS / 静态栅格快照；切换 / 更新 / 移除撤销未来读取。API29 /
API36 arm64 / 4 KiB 页同源同 APK 各 33 项组合、各六条真实 SAF / 双跳 Preview 链，
`mutation-check-android-html.py` 完整二十一组正常断言红 / 同 APK Core 绿 / 恢复绿 /
SHA256 通过。WebView 91 / 133、默认 Core 589/185、格式 272、Apple 182/3、JNI 148/37
与 JS-on / off Release / R8 / 四模块 lint 通过；最终恢复默认 Debug。前置主框架拒绝、
释放时序及变异日志传输异常单列，精确范围 / 收据见插件计划第六十批。不增 CI 二十份，
非完整格式 / 生命周期 / 真机 / 16 KiB / 发行证据；P1–P6 继续，仅本地提交。


**Android HTML 精确 UTF-8 路径（P2 第六十一批，2026-11-17）**：保留原始码点，
拒绝非法 UTF-8 替换别名及二次百分号解码。原实现三条正常断言红 / 同 APK Core 绿；
修复后 API29 / API36 arm64 / 4 KiB 页同源同 APK 各 36 项组合、各七条真实 SAF /
双跳链，HTML 原二十一＋新三组完整变异通过，逐组正常断言红 / Core 绿 / 恢复绿 /
SHA256。两档 Release / R8 / 四模块 lint、Core 589/185、Apple 182/3、JNI 148/37 与
格式 272 通过，默认三格式测试与 Unicode 实际像素已核对。收据见插件计划第六十一批；
不增 CI 二十份，不以此记完整格式 / 生命周期 / 真机 / 16 KiB / 发行通过。


**macOS HTML 精确路径（P2 第六十二批，2026-11-17）**：UTF-8 Data 键 / 前缀 /
斜杠处理避免规范等价与字形簇混淆；三条原实现正常断言红 / 同 XCTest 配置绿。
实际直连 / ProxyJump 各四条页面链、19 项资源组件及 `test-plugin-pane.py` 原二十一
＋新五组完整变异通过，逐组实际编译、单条正常断言红 / 独立配置绿 / 恢复绿及
SHA256。两档 arm64 Release、默认 Core 589/185、格式 272、Apple 182/3、JNI 148/37
通过；恢复普通 JS-off。目录选择仍为注入夹具，窗口透明 / 离屏并禁止激活；非完整
格式 / 解码预算 / 生命周期 / 系统面板 / 设备 / 发行证据，不增 CI 二十份。收据见
插件计划第六十二批，仅本地分批提交。


**macOS HTML 图片预算（P2 第六十三批，2026-11-17）**：后台准备静态首帧 PNG，
原始 1 MiB、两轴 2048、整包 4,194,304 像素与独立派生缓冲限额；实际容器核对，
重复路径拒绝、附件更新替换视图并撤销旧读。原实现三条正常断言红 / 独立配置绿；
十条直连 / ProxyJump 页面链、28 项组件及原二十六＋新十组完整变异通过，逐组
实际编译、单条正常断言红 / 独立配置绿 / 恢复绿与 SHA256。两档 arm64 Release、
显式 JS-off / tests-on Core 589/185、格式 272、Apple 182/3 与 JNI 148/37 通过。
夹具颜色空间、变异位置与 tests 缓存前置问题未计整轮绿，收据见插件计划第六十三批。
未改 SFTP 上限，不增 CI 二十份；非 RSS 沙箱 / 完整格式、系统面板、生命周期、
设备或发行验收；P1–P6 继续，仅本地提交，窗口后台离屏。
