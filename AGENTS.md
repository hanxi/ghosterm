# AGENTS.md — Ghosterm 开发铁律

> 本文件是**验收闸门的唯一事实来源**。与代码冲突时以本文件为准，改规则须走 PR 并说明理由。

## 项目一句话

Ghosterm = **libghostty 驱动的多端终端** × **VS Code 式 `~/.ssh/config` 图形管理** × **WinSCP 式 SFTP 文件管理器**。

技术栈：**各端原生 UI + 共享 C 核心（libghostty / libssh2）+ Transport 接缝**。
**不用跨端 UI 框架，不用 Rust。**

## 当前重构决定

2026-10-28 起按 `docs/reference/plan-optional-tmux.md` 实施：普通本地 PTY/SSH 为默认入口，
后台会话由用户显式选择 tmux；整体退役自有 sessiond、私有恢复协议、安装与随包 runtime，
不保留旧 ABI/记录的兼容层。依赖闭包、命令决策与验收进度见
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
| `vendor/` | 上游 submodule（ghostty、ish-arm64），**锁 pin** | 不允许就地改；改动一律走 `patches/` |
| `runtime-payload/` | 环境包（Android Termux/proot、iOS iSH） | 只放清单与下载/校验逻辑，二进制不入库 |
| `patches/` | 上游补丁 | 每个补丁必须带 `WHY.md` |
| `scripts/` | 构建/闸门脚本 | — |
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
（不是第二份真相），解析不到即当场红，「复验」step 再从固定 URL 取回对答案。它只解决**检测**，
`signature=adhoc` + `notarized=false` 就是「不许自动安装」的机器可读声明。
app 侧的检测入口只有一处：**关于窗口**（`Ghosterm → 关于 Ghosterm`）版本号下面那一行，
**只在用户点「检查更新」时联网**（无启动检查、无后台轮询）；结论三态不折叠，
「本机没 commit 记录 / 包带未提交改动 / 渠道不对 / 取不到 / 读不懂」全都报「查不出来」，
不报「已是最新」；有新版时只给下载直链，不画「安装」按钮。口径见
`docs/how-to/dev-build.md`，牙见 `scripts/mutation-check-app-update.sh`。

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

### C ABI 符号表（改 `core/include/ghosterm.h` 或 `core/src/*` 时另加）

`ghosterm.h` 是**单一事实来源**。新增/删除/改签名后必须：

1. 同步实现与各端绑定；
2. 跑 `scripts/check-abi.sh`：`nm` 静态库实际导出名 ↔ 头文件声明，**双向对拍**（漏导出与多泄漏都拦）。

> **当前覆盖范围（诚实标注）**：`check-abi.sh` 对拍「`core/` 内部实现 ↔ 头文件」；
> `check-abi-apple.sh` 另对拍 Swift 的引用名集合；`check-abi-android.py` 对拍 Android JNI 引用。
> A1 验收时为 Apple 71/2、JNI 11/62（引用/显式登记未用）；A2 本地终端首片 JNI 为 19/54，整个 A2 阶段尚未完成。连接检查接口加入后公共符号为 74 项，Apple 72/2；JNI 配置检查切片 20/54，SSH 后端切片为 32/42，身份容器首片加入后为 33/41；IdentityFile 来源查询切片为 75 个公共符号、Apple 73/2、JNI 34/41；主机信任记录快照切片为 79 个公共符号、Apple 77/2、JNI 38/41；共享信任写入与移除绑定为 80 个公共符号、Apple 78/2、JNI 39/41；已验通道取消切片为 81 个公共符号、Apple 79/2、JNI 39/42（新入口替换旧 shell 引用）；受控 SFTP 切片为 82 个公共符号、Apple 80/2、JNI 40/42（§9.47），只有绑定/helper，无文件产品接线。§9.45 Android 依赖补丁已验六场景及完整 214 项；§9.47 主机补丁依赖已接默认构建，17 项协议、十组接口变异、十一组依赖守卫与既有打包变异通过，详见 `docs/reference/host-ssh2-build.md`。§9.56 为 87 个公共符号、Apple 85/2、JNI 47/40，专项和最终完整 Android 260 项通过；首轮失败及测试同步修正仍保留记录。§9.57 为 88 个公共符号、Apple 86/2、JNI 48/40，远端非覆盖发布与绑定专项及最终完整 Android 266 项通过（API 36 arm64 / 4 KB 页模拟器）。实际数量以当前检出的脚本输出为准；符号对拍不替代平台行为验收。
> 各次接口与平台验收分别记在 `docs/reference/roadmap.md`，不得借旧结果记新接口通过。
> Android 已验收组件/配置绑定；A2 首片已接本地 Transport、Canvas/InputConnection 与会话生命周期，整个终端阶段未完成。连接配置检查已接两端；Android SSH native owner/认证桥及远端终端后端已接，产品 DNS/认证页面与 Keystore 容器首片已接（实现计划 §9.27），整个 A3 仍未完成；SFTP 文件专用 owner/JNI/Kotlin 后端已由 §9.51 接入，远端只读页面已由 §9.52 接入，复制任务与 NAPI 尚未接线。
> 新增平台绑定必须把引用名集合纳入对拍；签名兼容仍须各端真实编译验证。

`libssh2-sftp` 用 `python3 scripts/test-libssh2-sftp.py --fetch --mutate` 验锁定依赖的九场景、七组重新编译的 native 变异及两组隔离收据变异，目标红、独立对照绿、恢复绿与 SHA256；`--android 设备` 用实际 NDK archive 验同九场景。Android 与主机默认构建均应用补丁；`mutation-check-sftp-open.py` 另验 7 C + 2 Swift + 1 无库组，`mutation-check-host-ssh2.py --app 本轮.app` 验十一组依赖/包/CMake 守卫，目标红、独立对照绿、恢复绿与 SHA256。已交付证据见 Android 实现计划 §9.45/§9.47/§9.49，范围见 `docs/reference/host-ssh2-build.md`。手工证据，不增加 CI 20 份，此处仅登记范围。

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

新增任何断言/闸门，都要证明它**有牙**：故意改坏 → 断言变红 → 回滚复绿。
样板见 `scripts/mutation-check-sshconfig.sh`（17 组变异：A1~A9 + B1~B8，另验基线与回滚）。A9 用真实目录触发 fread 错误，断言必须返回失败；同变异下合法配置 golden 对照仍须通过，不能把读取错误当成空配置。B7/B8 护非连续重复指令替换/删除，构建通过后逐字节写回目标红、同变异求值对照绿、恢复绿与 SHA256 一致。
`config-metadata` 以 `python3 scripts/mutation-check-config-metadata.py --apple` 手工执行：8 组 C 变异护来源/块归属、包含与歧义块拒写和重解析路径，1 组 Swift 变异护来源转换。编译成功后目标断言红、独立 API/配置往返对照绿、恢复绿与 SHA256 一致；不计入 CI 20 份。Android 来源、只读页面、显式覆盖/恢复继承、别名重命名及配置错误恢复由 `mutation-check-android-config.py` 的 59 组设备变异覆盖（同 APK 独立对照）；错误恢复另护快照失效、错误可见与跨页保留、重载放弃确认/动作路由、编辑目标刷新及无快照入口禁用。SAF 通过真实系统选择器与测试 APK 专有 provider 验证预览/确认/取消、完整字节导入、错误/重载、外部文档不写入，另验别名/地址/端口校验。`mutation-check-config-rename.py --apple` 另验 12 组 C / 1 组 Swift：目标 token 保真、名称/冲突、归属与重复别名拒绝、重解析错误与对象保留、来源路径、大小写和无操作保真；均编译成功后目标红、独立对照绿、复原绿与 SHA256 一致。均属手工证据，不计入 CI 20 份。
`connection-config` 用 `python3 scripts/mutation-check-connection-config.py` 手工执行 14 组隔离 C 变异，护连接准入、范围/首值/累积规则、条件、值与来源；`mutation-check-android-connection-config.py` 用五组设备变异护 JNI/Kotlin/页面接线。均先编译，再目标红、独立对照绿、恢复绿与 SHA256，未加入 CI 20 份；auth 当前 11 组包括 macOS 准入与来源接线，证据见 Android 实现计划 §9.24。此处登记现有验证范围，不改变原验收要求。
`android-ssh` 用 `ANDROID_SERIAL=设备 ANDROID_SSH_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 14 组：真实隔离 sshd 经 adb reverse 到设备，编译后目标断言红、同 APK 配置对照绿、恢复绿与 SHA256，护认证请求/来源、信任/凭据结果、取消与远端 feed/resize。`ANDROID_SSH_ALL=1` 运行包含真实 SSH 的完整设备套件，缺夹具不跳过；历史后端证据见实现计划 §9.25，后续通道取消见 §9.41，未新增 CI 覆盖。
`android-ssh-network` 用 `ANDROID_SERIAL=模拟器 ANDROID_SSH_TEST_CLASS=com.hanxi.ghosterm.SSHNetworkTest ANDROID_SSH_NETWORK_MUTATE=1 bash scripts/test-android-ssh.sh` 手工执行 14 组，护候选推进/去重、TCP 宽限/总预算、协商及提示等待边界、快照、地址/scope、产品传参和取消；目标红、同 APK 配置对照绿、恢复绿与 SHA256。12 项定向使用真实 SSH 和设备回环双栈代理；TCP 丢包要求模拟器可 `su 0`，仅向测试地址/端口添加本轮独立标记的规则，测试及 runner 的 finally 清理（不承诺 SIGKILL 后仍能执行清理）。当前 `ANDROID_SSH_ALL=1` 包含此组，也要求该模拟器条件。`mutation-check-android-network-cleanup.py` 另在真实规则上验隔离内存变异、独立标记对照及生产文件 SHA 不变；这些是手工证据，不增加 CI 20 份。该候选切片不覆盖真实域名、外部 IPv6、通道打开取消及整套 A3 网络矩阵，见 Android 实现计划 §9.40；后续通道工作区状态单列于 §9.41。
`android-files` 用 `ANDROID_SERIAL=设备 python3 scripts/mutation-check-android-files.py` 手工验证 18 组：真实本机文件/SAF 树选择器、选择/过滤/失败/代次、持久授权/撤权、预览限额/解码/链接、会话保持及流/fd 释放。编译后目标红、同 APK Core 配置对照绿、恢复绿与 SHA256；新增 14 项设备定向包含于完整 214 项回归（API 36 arm64 / 4 KB 页模拟器），见 Android 实现计划 §9.43。远端文件页面后续由 §9.52 接入，复制任务尚未接；不增加 CI 20 份覆盖，此处仅登记已执行范围。
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
`android-terminal`（`python3 scripts/mutation-check-android-terminal.py`）当前有 24 组，按各轮记录在指定 arm64 设备运行：VT/PTY/输入法/Canvas/生命周期目标红，同 APK 独立 Core 配置对照绿，恢复绿与 SHA256。静止画面闪烁另有 T22–T24：真实窗口像素护重绘、闪烁相位与 native 标记，§9.19 与 T8/T19 共选跑五组；不称全量 24 组重跑。`android-text` 当前共 71 组，各轮按同设备/同 APK 对照执行，护 VT 粘贴、选区格式化/坐标/高亮、复制/确认/拖动与本地清屏（含上游隔离补丁），以及按下/重复/释放、失焦/换 surface 归属与辅助键成对点击，以及手柄边缘持续滚动/停止、跨越锚点/重新抓取、最近手柄命中与 Ctrl 单次/锁定/失焦/会话隔离；修饰键/锁定位、数字键盘导航身份和组合音符/设备/IME 隔离由 §9.18 的新增 16 组验证；其他新增组护 Ctrl 标点/Unicode、键盘协议模式与布局字符传递、非法字符显式拒绝、辅助键路由与输入法单独 LF 的 Enter 语义。`android-vt-libc` 用 NDK 真实 archive 验两组分配器替身守卫，再以缓存头损坏验第三组头文件收据守卫，archive 校验为独立对照。均是手工证据，未纳入上述 macOS CI 20 份。各轮完整或选组执行的数量与范围见 Android 实现计划 §9.13–§9.19，不能把切片通过当作整个 A2 完成。
`android-performance` 用 `python3 scripts/mutation-check-android-performance.py` 在指定设备跑四组：延迟输入取帧、破坏实际背景像素、丢弃帧指标和丢弃 IME 提交均须目标红，同 APK Core 配置对照绿，恢复绿并核验 SHA256。第三组验证测量缺失不会被报告为零成本；它不替代 renderer 功能断言。输入像素超时按同负载 PixelCopy/轮询标定，保留绝对下限；吞吐/帧耗时/CPU/内存为设备档位数据，不设跨设备的固定 P95 承诺。均属手工证据，未纳入 macOS CI 20 份；执行范围见 Android 实现计划 §9.21。
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
资产名必须追得上 `package-mac.sh` 的改名——版本号 bump 而 tap 忘改，用户拿到的是 404，
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
（`tmux` 以 `python3 scripts/mutation-check-tmux.py` 跑 26 组隔离变异（当前脚本数量）：缺失判定、
重复/UTF-8 解析、未知策略/结果、明确拒绝、版本、format/分号/shell 引用、身份、禁止 attach 起新 server、
尺寸接线、丢失本地退出状态、关闭标准描述符、取消及探测错误分类。目标断言红、同变异下独立对照绿、SHA 回滚；make 整秒时间戳下强制重链，
不让旧可执行文件冒充变异结果。普通离线 gate 只跑命令与恶意输出夹具；真实本地/SSH
八模式（含已有空 server 与 owner 被 SIGKILL）另以 `test-tmux.py` / `test-tmux-ssh.py` 执行，缺 tmux 时明确失败，不算通过。已与 `tmux-ui` 纳入 CI full 档；Linux 与真实 SSH/断网仍是手工证据。）
（`tmux-ui` 用 `python3 scripts/mutation-check-tmux-ui.py` 验新格子不覆盖任务、detach 保留
断开格子、关闭只断开客户端、未知文案、探测错误分类、路径变更清选择、鼠标输入及拒绝覆盖运行格子，共十组，另护 runner 长路径下 Unix socket 的真实绑定。使用真 tmux +
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
`shell-mouse-recovery` 以 `python3 scripts/test-shell-mouse-recovery.py --mutate --codex --ssh`
手工验证：真实 full libghostty / PTY / tmux 复现窗格内程序异常退出后的鼠标模式残留，
Bash/Zsh 提示符集成自动清理，重连与新 surface 不再产生序列文本输入，正常 TUI 仍实际
收到鼠标事件。八组隔离变异护清理、重复加载、原提示符钩子/退出码和启动文件备份/保真；
目标红、同变异独立对照绿、恢复绿与 SHA256。系统 SSH 经隔离 loopback sshd 跑同组场景；
Codex 使用本机真实 CLI 输入框，不提交测试草稿，未复现不得记为修复。
通知注入及进程暂停不等于整机休眠；远端真实主机及硬件休眠另列证据。
这是实际 shell 的集成（远端须在远端安装），不在唤醒时强制重置终端；不新增 CI 20 份覆盖。
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
