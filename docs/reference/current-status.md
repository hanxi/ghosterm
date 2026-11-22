# 当前状态

非插件产品源码说明于 2026-11-18 对照工作区复核。macOS SFTP owner、staged download、Quick Look 硬上限和事件终态保护的定向结果见[两端待办核对](platform-todos.md)。线上发行和插件专项更新至 2026-11-22。滚动 dev 下载应核对随包收据。

## 源码与发行

| 范围 | 核对结果 |
|---|---|
| 2026-11-04 产品源码基线 | `07b278e`，已包含 Android 设备存储浏览、触屏/连发、文件连接复用、专注终端与菜单输入法修正 |
| 2026-11-22 线上 macOS dev 快照 | `ef15c168` / build 157 / arm64 / 最低 macOS 15.0 / ad-hoc / 未公证 |
| 2026-11-22 线上 Android dev 快照 | `ef15c168` / clean / release / arm64-v8a / min SDK 29（Android 10），8,968,725 字节，R8 与资源裁剪开启 |
| 发行查询来源 | [macOS version.json](https://github.com/hanxi/ghosterm/releases/download/dev/version.json)、[Android build-info](https://github.com/hanxi/ghosterm/releases/download/dev/android-build-info.json)，2026-11-22 实际读取 |

GitHub 线上仅有 `dev` 预发布。两端已发布资产各自通过签名与来源审计。现有 `v0.1.0` 标签指向 `c10752dc`，正式 Release 尚未创建，生产插件渠道尚未配置。2026-11-04 的 macOS `3345893` 和 Android `d9ef45a` 属历史发行快照。

手动触发的 [macOS 0.1.1 候选构建](https://github.com/hanxi/ghosterm/actions/runs/45010306745) 关闭了草稿上传，在 `ef15c168` 的 Core 闸门遇到 SSH PTY resize 断言失败。测试改为等待不可由输入回显伪造的就绪标记，并对实际 PTY 尺寸有界采样。先行修订通过默认 Core 612 项及 207 ABI，最终精确尺寸匹配再通过三次定向复验。隔离编译改用错误尺寸 `40 1000` 时触发唯一目标断言，恢复后通过。临时目录已核实清理，修订仍待云端复验。

## 产品范围

- macOS：full libghostty + Metal、普通本地 PTY/SSH、标签/分屏、SSH 配置界面、本地/SFTP 文件工作区与显式 tmux。配置保存事务、私钥安全存储生产接线、文件错误/完整性和受控接口迁移仍有缺口，见[路线图](roadmap.md#macos-下一步与共享缺口)。
- Android：libghostty-vt + Canvas、主机/身份/信任管理、本地/SSH、SAF/本机/SFTP 浏览与复制任务、持久只读历史、显式核对/清理、主题/字体/Kitty、可选远端 tmux。设备共享存储来源暂不支持作为粘贴目标；环境包延后。
- iOS、Windows、Linux、HarmonyOS、Web 原生客户端尚未交付。落地页是产品介绍网站，不是 Web 终端。
- 默认普通终端，远端主机统一面板可自动查询 tmux，普通终端无需等待；创建/attach 仍由用户显式选择。本地默认和普通终端快捷入口不探测。自有 sessiond、私有恢复协议及随包 runtime 已退役。
- 非插件后续源码：Android 已有固定会话按钮/同窗口覆盖面板与切换保持 IME（§9.207–§9.208）、字形裁切/半圆接缝（§9.209/§9.212–§9.213）；两端 ProxyJump 已接普通终端/SFTP/tmux（§9.214）。macOS 已接用户选择下载/重启的 Sparkle/Ed25519 更新与清单/渠道校验，见[更新说明](../how-to/macos-updates.md)。这些本地实现与专项不更新上方历史发行快照。
- 插件源码候选 `560e85ac` 已接共享 C 通道、Portable／Node／Exec、原生预览和标准视图、签名安装、版本选择、撤销与回滚。gterm 支持显式 relay、IPC 和开发包命令入口。普通 App 默认关闭 QuickJS。系统选包、VoiceOver 和生产发行仍待验，Claude 与 Codex 真实后端联调按用户安排暂缓。具体范围见[插件计划](plan-plugin-bridge.md)。

详细平台差异见[能力矩阵](capability-matrix.md)。

## 验证范围

- 2026-11-22 插件专项先后通过两档 Core 闸门（612／874 项，207 ABI）、Apple／JNI 对拍、macOS 签名专项（124 项与 7 项回环 SSH）、Android API 29／36 的签名渠道 HTTPS（含 API 36 的 16 KiB 页）以及开发工具 76 项。一轮双端正式形态构建在 `0a0cf885` 使用测试签名通过九项资产联合审计。实际安装、完整客户端回归和发行证据见[插件计划](plan-plugin-bridge.md)各批。
- 最近同源完整 Android 记录：`ae164f1`，API 29 / arm64 / 4 KiB 与 API 36 / arm64 / 16 KiB 各默认完整 942 项通过（[§9.191](android-implementation.md#9191-会话快照修正后的双系统完整-942-项2026-11-03)）。后续修改没有借用这轮结果声称新源码完整通过。
- 后续非插件专项还包括 §9.205–§9.214 的连接提示、会话/IME、字体、统一入口与 ProxyJump；各自按日志记录的系统/页大小/分轮范围成立，不将其记为新完整回归。
- 后续本地定向：§9.200–§9.204 分别保存设备存储、触屏/连发、文件 owner 复用、全屏布局及菜单 IME 的分轮结果和失败；当时 Core 记录为 51 CTest / 95 ABI，静态绑定对拍为 Apple 91 引用/4 登记未用、JNI 57 引用/38 登记未用。各原生构建、lint 和变异的范围见对应节，不相加冒充全客户端回归。
- 插件历史第二十二批（2026-11-06）：该轮 Core 348 CTest / 140 ABI、Apple 136/4、JNI 102/38；25 个本地 / 真实边界、两三跳允许 / 拒绝四 SSH 场景、Swift 7 与 API 36 arm64 / 4 KiB 页 Android 7 项通过。允许链路双向各 256 KiB / 80 确认 RPC，独立资源和 RPC 完成记录各六份。新 26 C / 4 Swift / 6 JNI 与旧 Store / Transfer / 公开 Session / RPC 的 99 C / 5 Swift 分套通过，共 140 组手工变异，不增加 macOS CI 二十份。严格 C11、关闭 provider / 无 SSH 变体、强制重链接的 macOS App、Android 未签名 Release/R8 与四模块 lint 通过；仅源码 / 隔离专项，不更新上面的发行或完整客户端回归结论。
- 插件 P2 第五十九批（2026-11-17）：Android 页面返回原 PTY 的焦点 / IME 已修复。API29 / API36 arm64 / 4 KiB 页同 APK 各 127 项和各三条真实 SAF / 双跳 Preview 链通过；九组新变异分轮、旧十四组通过。Core 589/185、Apple 182/3、JNI 148/37、格式 272 与两档 Release/R8 / 四模块 lint 通过；来源 / 失败与限制见[插件计划](plan-plugin-bridge.md)。仅本地专项，不更新上方历史发行快照，不代表新完整客户端或用户真机验收。
- 插件 P2 第六十批（2026-11-17）：Android 静态 HTML / 显式 CSS 与图片已接原生 WebView。API29 / API36 同源同 APK 各 33 项组合与各六条真实双跳链、21 组完整变异通过；两档 Release / R8 / 四模块 lint、Core 589/185、格式 272 及绑定对拍通过。版本 / 摘要 / 失败见[插件计划](plan-plugin-bridge.md)，不代表完整客户端 / 物理设备或发行验收。
- 插件 P2 第六十一批（2026-11-17）：Android HTML 修复 NFD 路径改名与非法 UTF-8 替换别名；API29 / API36 同源同 APK 各 36 项组合、各七条真实双跳链、完整 24 组变异及两档原生构建通过。Unicode 主文档 / CSS / PNG 与三种格式默认测试名均实际核对；见[插件计划](plan-plugin-bridge.md)，不扩大为完整客户端 / 物理设备或发行验收。
- 插件 P2 第六十二批（2026-11-17）：macOS HTML 修复 Unicode 等价键 / 目录范围和组合字符遮蔽斜杠问题；八条实际直连 / ProxyJump 页面链、19 项资源组件、完整 26 组变异、两档 arm64 Release 及默认 Core 闸门通过。目录选择仍为注入夹具，窗口后台离屏；来源 / 摘要 / 限制见[插件计划](plan-plugin-bridge.md)。
- 插件 P2 第六十三批（2026-11-17）：macOS HTML 附件在后台生成有界静态首帧，单边 2048、整包 4,194,304 像素；原始 1 MiB 与派生 PNG 缓冲分开限额。十条实际直连 / ProxyJump 页面链、28 项组件、完整 36 组变异及两档 arm64 Release 通过。预检失败与范围见[插件计划](plan-plugin-bridge.md)，不提高 SFTP 上限，不代表完整格式 / 进程内存 / 生命周期或发行验收。
- 插件 P2 第六十四批（2026-11-17）：普通 gterm 已接显式开发目录 / 启动配置、修订与摘要选择、规范 argv 和前台 IPC，@cwd 根使用本次实际目录。普通 Release 53 项、ASan/UBSan 含 IO 故障 56 项、新 24＋旧 20 组分轮变异及隔离 Core 闸门通过；见[插件计划](plan-plugin-bridge.md)。端点与 App 附着仍须明确选择，自动发现 / 签名执行端安装、Node / Exec 目录整链和完整路径 / 客户端矩阵继续。
- macOS `4cbfcf4` 完整宿主历史含设置页像素失败；后续字体切片已修正键盘页高度，关联组合 56 通过/1 开发态资源跳过，未重跑完整宿主。终端、绑定和打包专项不能代替完整宿主全绿，见[实施记录](optional-tmux-implementation.md)。
- Android 自动化验证只使用模拟器，继续按具体用例记录当前源码回归、网络故障、提供方、长负载与签名包验证；真机/OEM 行为仅安排人工手测，不作为自动化待办缺口反复报告。合并闸门以 [AGENTS.md](../../AGENTS.md) 为唯一事实来源。

## 历史与临时材料

实施日志、失败和复验保留在原路径及原章节，避免破坏既有引用。文中 `tmp/` 路径是当时的临时取证位置，不是长期可用附件。

此前文档整理轮曾按用户要求清空根目录 `tmp/`（包括旧构建、隔离副本、AVD 与原始临时取证文件），不重新运行或重新认证历史结果。落地页选用的三张原生截图已复制至 `site/assets/`，来源及 SHA256 见[截图收据](../../site/assets/screenshots.json)。后续重验需重新生成夹具/AVD 和临时证据。
