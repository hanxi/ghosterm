# Ghosterm

**A5 冲突决策与执行清单（§9.63，2026-10-31）**：请求/修订归属、保留两者/跳过、本次全部、不可变清单及有界名称规划已接；15 项定向、42 组变异通过。真实本机竞态、远端→本机及 SAF 新文档集成已验，尚无任务/页面消费者，见[实施记录](docs/reference/android-implementation.md#963-a5-请求内冲突决策与执行清单2026-10-31)。

**历史切片：A5 复制来源清单（§9.60，2026-10-31）**：选中后复制、原来源卡片、完整清单、替换/清空确认已接。9 项设备定向、16 组新变异及既有浏览 18 / 远端产品 14 组复验通过；不读文件内容、不写系统文本剪贴板，跨页/旋转保留元数据。粘贴执行和目录任务仍待接，见[实施记录](docs/reference/android-implementation.md#960-a5-复制来源清单与文件页面2026-10-31)。

**历史切片：A5 SAF 文档写入（§9.59，2026-10-31）**：`SafFilePublisher` 已接真实树授权下的新文档创建、写端关闭及内容回读核验；16 项设备定向、30 组变异通过。请求保留未知/已创建/完成与 URI，取消后租约等实际 worker 结束再释放；没有通用 SAF 原子提交或自动删除。该片当时复制页面未接；§9.60/§9.61 已补来源清单与扫描，目标预检后由 §9.62 接入，跨远端关系和粘贴任务仍待接，见[实施记录](docs/reference/android-implementation.md#959-a5-saf-新文档写入2026-10-31)。

**历史切片：A5 应用私有目录发布（§9.58，2026-10-31）**：`LocalFilePublisher` 已接常规文件的独占候选、写端关闭及 NOREPLACE 提交；十项设备定向、24 组新增与 23 组既有暂存变异通过。来源快照仍匿名，命名目标候选位于 HOME 外的私有目录。该片当时 SAF 写入尚未接入，后由 §9.59 补齐；来源扫描后由 §9.61 补齐；目标目录创建、任务预算/进程恢复和粘贴执行仍待接入。失败记录、完整设备及跨端验证见[§9.58](docs/reference/android-implementation.md#958-a5-应用私有目录非覆盖发布2026-10-31)。

**历史切片：共享发布接口（§9.57，2026-10-31）**：远端非覆盖发布、Swift/JNI/Kotlin 绑定及专项已验；46 项 CTest / 88 ABI、Apple 86/2、JNI 48/40。Android 32 项定向及十组发布变异通过，既有读取/创建/owner 13＋9＋18 组独立复验。两端原生构建通过；该片当时复制清单、任务、SAF 目标和 macOS 文件产品迁移未完成；私有目录发布和 SAF 新文档写入后由 §9.58/§9.59 补齐，任务与桌面迁移仍待完成。完整设备结果及协议/变异范围见[§9.57](docs/reference/android-implementation.md#957-a5-远端非覆盖发布2026-10-31)。

**最新完整设备验证（§9.63）**：最终产物 **342 项通过，零失败/错误/跳过**，771.699 秒；仅 API 36 arm64 / 4 KB 页模拟器。685 份非 Markdown 源码摘要不变，安装主/测试 APK 与构建摘要一致。已接请求内冲突决策和不可变逻辑执行清单；跨远端关系、目录创建、任务/粘贴页面仍未完成。§9.62 的 327 项保留为历史。

**当前进度（2026-10-31）**：Android 已交付主机/终端、只读文件页面、本机/SAF 与远端匿名暂存前置，复制粘贴任务与完整设置仍待实现。macOS 主流程已交付，配置事务、受控文件接口迁移和异常处理继续单独收尾；[路线图](docs/reference/roadmap.md#当前进度总览)区分实现、专项证据与待办。

**libghostty 驱动的多端终端** × **VS Code 式 `~/.ssh/config` 图形管理** × **WinSCP 式 SFTP 文件管理器**。

## 技术栈

| 层 | 选型 |
|---|---|
| UI | **各端原生**：SwiftUI(macOS/iOS) · Compose(Android) · ArkUI(HarmonyOS) · WinUI3(Windows) · GTK4(Linux) · Web |
| 终端引擎 | **full libghostty**（Apple/Linux，Metal/GL4.3）· **libghostty-vt + 自绘**（Web/Android/HarmonyOS） |
| 共享核心 | **C11**（`core/`）：ssh(libssh2) · sftp · ssh_config · known_hosts · PTY（端口转发待实现） |
| 接缝 | `UI → TerminalSurface → Transport(本地PTY/SSH) → Core` |
| 环境包 | 规划按需下载：Android Termux/proot · iOS iSH（当前仅规范，无安装器，不进 App 包） |

**不用跨端 UI 框架，不用 Rust。** 决策记录见 [docs/architecture/overview.md](docs/architecture/overview.md)。

## 目录

```
core/           C 共享库（唯一跨语言边界：include/ghosterm.h）
terminal/       各端 TerminalSurface（full libghostty 或 vt+自绘）
apple/ android/  现有宿主 + UI（其余端 windows/linux/harmony/web 为规划）
assets/         品牌资源：应用图标母版 SVG + 各端派生（scripts/build-icons.sh）
runtime-payload/ 环境包清单规范（下载/校验/安装器待实现，二进制不入库）
patches/        上游补丁（每个带 WHY.md）
scripts/        构建 / 闸门 / 变异验证
docs/           架构 · 能力矩阵 · 许可 · 路线 · Apple UI 已知坑
```

## 快速开始

```bash
# macOS：使用完整 Xcode 的一致工具链；按实际安装路径调整
# 在同一 shell 运行后续 Core / Swift / 打包命令
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
mkdir -p "$PWD/tmp"
export TMPDIR="$PWD/tmp"

# 主机 SSH 依赖：首次允许联网准备，之后可离线校验
# macOS 先安装 cmake、pkg-config、openssl@3，准备 C 编译器与 Python 3.12+
python3 scripts/build-host-ssh2.py --fetch

# 离线闸门（依赖产物及收据已准备；要求 cmake ≥3.19）
bash scripts/gate.sh                       # 核心：构建 + ssh_config 对拍 + ABI + 支持集双向校验

# Apple 端（需要 zig 0.16.0 / Xcode / Metal toolchain）
bash scripts/build-ghostty.sh              # 隔离源码打补丁 → build/GhosttyKit.xcframework
bash scripts/gate-ghostty-patch.sh         # 补丁与树一致 + 补丁符号真的导出
bash scripts/terminal-gate.sh              # TerminalSurface（字节 → libghostty 网格）
bash scripts/apple-app-gate.sh             # 绑定 ABI 对拍 + 主机列表 + 真 sshd e2e

# Android（SDK/NDK/JDK 等准备见 android/README.md）
bash scripts/build-android.sh              # arm64 debug APK + 设备测试 APK

# 变异验证：证明闸门有牙，不是空跑
bash scripts/mutation-check-sshconfig.sh
bash scripts/mutation-check-ssh.sh
bash scripts/mutation-check-ghostty-patch.sh
bash scripts/mutation-check-terminal.sh
bash scripts/mutation-check-sftp.sh
bash scripts/mutation-check-icons.sh
bash scripts/mutation-check-package-mac.sh

# 应用图标：母版 SVG → 各端图标（需要 rsvg-convert / ImageMagick / python3）
bash scripts/build-icons.sh                    # 全端生成
bash scripts/build-icons.sh check              # 产物与母版是否一致

# macOS 打包：.app + .dmg + .zip + SHA256SUMS（详见下方「跑起来」）
bash scripts/package-mac.sh                    # 出包到 dist/
bash scripts/package-mac.sh --help             # 全部选项（版本/签名/公证/最低系统）
```

核心默认构建要求已校验的 `build/host-native`，不再自动探测 Homebrew libssh2 或在缺库时退回 stub。`gate.sh` 只离线校验；`build-core.sh` 可以从已缓存源码重建依赖，下载仍需显式 `--fetch`。详细准备、无 SSH 构建和未验收边界见[主机依赖说明](docs/reference/host-ssh2-build.md)。真实 SSH/SFTP 测试另需 sshd 与 ssh-keygen；显式关闭 libssh2 的构建返回 `GT_ERR_UNSUPPORTED`，不等于 SSH 功能通过。Apple 宿主闸门缺 GhosttyKit 产物会失败，缺 Swift 或无法启动 sshd 时有显式 SKIP；验收必须检查实际覆盖，不能只看退出码。

## 跑起来（macOS）

安装最新 dev 版（自动校验并清除 quarantine；升级前先退出 Ghosterm）：

```bash
curl -fsSL https://raw.githubusercontent.com/hanxi/ghosterm/main/scripts/install-macos.sh | /bin/bash
```

安装到 `/Applications/Ghosterm.app`，后续升级重复执行；[安装说明](docs/how-to/dev-build.md)。

从源码构建：

```bash
bash scripts/build-mac-app.sh        # 出包：tmp/Ghosterm.app（自包含的普通终端包）
open tmp/Ghosterm.app                # 普通终端；通过连接选择器进入 SSH/SFTP 与主机管理
```

包是**自包含**的（core 与 libghostty 静态链接，libssh2 嵌入
`Contents/Frameworks/`）——目标 Mac 不需要 Homebrew。当前发行包为 arm64、最低 macOS 26；
源码声明的 macOS 13 不等于包的运行下限，打包脚本按实际嵌入依赖计算并验证 minos。
ad-hoc 签名；Developer ID + 公证是显式遗留项（待证书，见 roadmap）。

可选后台会话从 `+ → 新建连接 → 打开为后台会话` 或已有终端标签右键菜单打开；目标需安装 tmux >=3.2a，
不影响普通终端。主机选择从连接选择器进入，主机编辑在“管理 SSH 配置”；侧栏只列活动标签。操作与保留边界见[使用 tmux 后台会话](docs/how-to/tmux-sessions.md)。

不带包直接跑（开发循环）：

```bash
cd apple/GhostermApp && swift run GhostermApp                 # GUI
swift run GhostermApp --local                                 # 无窗口冒烟（打网格文本）
swift run GhostermApp --ssh-smoke <config> <host别名>          # 无 UI 走 AppModel 全链路
```

## 当前状态（2026-10-31）

下列状态按当前产品调用方记账。§9.57 已补远端非覆盖发布及 Swift/JNI/Kotlin 绑定；此前 C 草稿的绑定缺口已解决，复制任务与桌面迁移仍待完成。[路线图](docs/reference/roadmap.md#当前进度总览)分别记录底层接口、实际产品和各轮验收结果。

Android 已接主机首页、本地/SSH 终端、本机/SAF/远端只读文件页面、点按打开和长按选择；文件连接独立于终端。最近完整通过产物为 §9.63 的 **342 项通过**，仅限 API 36 arm64 / 4 KB 页模拟器；新增匿名暂存与来源快照属于数据层，§9.60 已接复制按钮、来源卡片与完整清单；粘贴任务和 16 项偏好仍待实现；远端发布、应用私有目录常规文件提交与 SAF 新文档写入已由 §9.57–§9.59 补齐。远端文件页面切片的 236 项保留为历史，详见 [实现计划](docs/reference/android-implementation.md#9-分阶段实施与依赖)。

A4 本机/SAF 浏览已交付，§9.48 的完整 214 项保留为历史；§9.49 保留中间轮失败，最终产物已独立完整复验 214 项通过。§9.47 已完成受控 SFTP 打开与主机补丁依赖接线：17 项协议用例、十组接口变异、十一组依赖守卫和既有打包变异通过。Android 后端与页面已在 §9.51/§9.52 接入，macOS 文件产品尚未接新入口；[共享依赖与验收状态](docs/reference/host-ssh2-build.md)、[路线图](docs/reference/roadmap.md#当前进度总览)列出后续工作。

本机文件浏览、UTF-8 只读预览、点按打开/长按多选及 SAF 授权已接入，远端浏览已由 §9.52 接入，复制任务尚未接；§9.42 的草稿构建问题已解决，当前证据见 [A4 实施记录](docs/reference/android-implementation.md#943-a4-本机与-saf-文件浏览首片2026-10-30)。

进度以[路线图当前总览](docs/reference/roadmap.md#当前进度总览)为准；以下区分生产实现与历史验收，不把原型中的可交互演示记为已实现能力。

- **Android**：按[唯一 HTML 方案](docs/design/android-prototype.html)和[详细实现文档](docs/reference/android-implementation.md)开发。A0/A1 已完成 API 36 arm64 / 4 KB 页模拟器档；A2 本地终端、A3 SSH 与 A7 身份/主机验证子页部分交付。最新 §9.63 完整 **342 项通过**；本机/SAF/远端暂存与只读租约已接，复制任务尚未接入，§9.44 的偶发失败保留追踪；A3 其余网络/取消收尾（候选推进已接）、A4 预览缓存/外部打开与 A5/A6 四方向复制、16 项设置、后台与发行矩阵仍待完成。
- **macOS**：普通终端、统一标签/文件工作区、传输主流程及显式 tmux 已交付。配置事务/表单、安全存储、完整剪贴板、文件失败处理与发行仍有缺口；[Apple 实现说明](apple/README.md)和[路线图待办](docs/reference/roadmap.md#macos-下一步与共享缺口)列出源码事实与完成条件。macOS 尚无 TCP 失败后遍历 DNS 备用地址的实现；Android 已按 §9.40 接入候选推进。Android 普通 shell 已接剩余预算和取消；Swift 新接口已验，但 macOS 产品尚未改用，认证可取消不等于桌面通道打开已可取消，见 [通道边界](apple/README.md#ssh-认证后的通道打开边界)。
- **共享核心**：§9.57 验收基线为 **46 项 CTest / 88 ABI**，Apple 86/2、JNI 48/40；受控 shell/SFTP 打开、目录浏览、有界预览、独占暂存创建、受控读取到 fd 与三份依赖补丁已验。Android 已消费 shell 入口，SFTP 已接专用 owner/JNI/Kotlin 后端，文件页面已接；macOS 产品尚未改用受控 shell/SFTP。TOFU/移除共同事务已交付，Android 管理页在 §9.39 验收，macOS 无管理页；共享修复不代表桌面配置、设置或文件提交事务已完成。

| 能力 | 状态 |
|---|---|
| C 核心骨架 + `ghosterm.h` v0 ABI | 🟡 当前 88 符号对拍通过，包含远端非覆盖发布；受控打开、浏览与 stat 标志已验，Android 只读文件页面已接，macOS 产品仍待迁移，接口尚未冻结，旧会话 ABI 已移除 |
| `ssh_config` 解析 / 无损往返（未编辑） | ✅ 与原文逐字节相同 |
| `ssh_config` 求值 | ✅ typed 视图 + 通用视图（`ssh -G` 等价）；42 键 × 6 host 全表语义对拍 |
| `ssh_config` 编辑 API | 🟡 9 个原夹具 + 5 个补充用例逐字节对拍；非连续重复指令已按实际匹配行编辑，已增加三字段来源与包含文件/重名块编辑保护；已接入仅替换目标别名 token 的重命名接口；全部字段视图仍待补齐 |
| `ssh_config` `Include` 展开 | ✅ OpenSSH 同口径（相对路径基准 = ~/.ssh、glob、嵌套 + 深度上限 16）；6 组断言 + 变异 I1~I4 |
| 本地 PTY transport（forkpty） | ✅ POSIX 实现（macOS/Linux 证据；Android A2 首片已有 API 36 模拟器定向/变异证据，整个阶段未完成） |
| SSH（libssh2）connect / auth / hostkey TOFU / ssh-shell | 🟡 密码/密钥、可取消认证与撤销优先的主机验证已有真 sshd 证据；CA-only、未知标记和 hashed 条目显式报不支持，见 [主机信任边界](core/tests/README.md#主机信任与撤销)。共享连接前配置检查已接两端；高级选项执行仍未实现，Android SSH 后端已绑定，产品、SAF 导入、凭据、身份设置与信任记录管理连同文件首片已有完整 214 项设备回归证据，A3 余项待收尾 |
| SFTP（list / stat / 上传 / 下载 / job 三态） | 🟡 基础原语有真 sshd e2e 与变异证据；覆盖截断与关闭结果检查已补齐（Android 计划 §9.53）；独占暂存创建与远端非覆盖发布已接（§9.54/§9.57），产品任务接线、旧下载完整长度校验及过载下终态保障仍待补齐 |
| **macOS 原生宿主** | 🟡 已交付主要功能；配置写盘与私钥安全存储接线仍有缺口，见路线图 |
| ├ TerminalSurface（full libghostty + Metal） | ✅ 上游补丁提供 host IO 后端（`patches/libghostty/`） |
| ├ SwiftUI 宿主 + `ghosterm.h` 绑定 | ✅ 窗口 / 侧边栏 / 多标签页 / 分屏 / 设置页 / 三态对话框；当前绑定 ABI 对拍为 88 项中引用 86 项、另 2 项显式登记，受控文件新入口未接产品；改名绑定不等于 macOS 表单已接入 |
| ├ 标签 / 窗格 | ✅ 单窗口顶部或侧边标签、UUID 重排、唯一加号、Ctrl+Tab / Ctrl+Shift+Tab 循环切换；统一终端/文件工作区、分屏预览、焦点与停画。**遗留**：标签/布局跨重启持久化（设置偏好已持久化） |
| ├ 配色集随包 + 主题选择 | ✅ Ghostty 配色集随包（`Contents/Resources/ghostty/themes`，数量由构建输出决定）+ 资源目录推导；出包包内配置检查验主题被引擎接受 |
| ├ 终端设置 | 🟡 配置由 Ghostty 解析，字号/配色已有真实 surface 证据；`SettingsStore` 重复键编辑、读取错误与冲突判定仍有缺口，不能把正常配置的测试扩大为全部保存路径正确 |
| ├ 终端系统复制粘贴 | 🟡 `beffb49` 已接标准 `copy:` / `paste:` 与 ⌘C/⌘V；真实选区和模式感知粘贴已有专用测试。全局菜单自动路由、粘贴确认及引擎剪贴板回调仍待验收，见路线图 P1 |
| ├ ssh_config 图形编辑（M3） | 🟡 编辑器、通配块与 Core 稀疏 API 已实现；共享重复指令误删已修复。macOS 表单仍简化扫描/提交全部字段，备份错误被忽略、替换先删后移，保真与事务保障待修复 |
| ├ 文件工作区与传输 | 🟡 本地/SFTP 统一窗格、队列、拖放与 Quick Look；独立文件连接。控制异常/未知结果、目录内链接跳过明细尚待修复；目录冲突 UI 等验收缺口见交接文档 |
| ├ 文件查询与搜索 | 🟡 正常浏览/搜索已有接线；存在性查询折叠错误，树加载失败被缓存为空；搜索及递归权限遗漏部分遍历错误，远端搜索未检查退出状态且取消未传入后端；传输入队/控制仍可能同步阻塞主线程，见 [Apple 说明](apple/README.md#文件查询与线程边界) |
| └ 渲染 | ✅ 三层证据：字节→网格、渲染层已挂、**像素级**（抓自进程内视图帧比颜色，含对照与变异 P1~P3）——抓自进程内的视图不需要录屏权限 |
| 打包分发（脚本 + CI + DMG + 校验和） | ✅ `scripts/package-mac.sh` + `macos-release.yml`；ad-hoc 签名已可出包，**Developer ID + 公证待证书**（显式遗留）。工作流有路径过滤/取消，不保证每个提交出包，见 [CI 触发范围](docs/how-to/dev-build.md#ci-是怎么串起来的) |
| 可选 tmux 后台会话 | 🟡 macOS 与 Linux SSH 的 tmux 3.2a、3.6a；用户显式打开列表、选择及重新连接；保留边界见使用说明 |
| Android | 🟡 A0/A1 已在 API 36 arm64 模拟器档完成；[实现进度](docs/reference/android-implementation.md)，A2 本地终端首片已交付，文本操作切片已验；选区边缘滚动/Ctrl 生命周期切片已验；Gboard 拼音基本路径与按键重复/释放切片已验；已有模拟器 debug 性能基线；真机/长负载性能和完整键盘矩阵待补；SSH 后端已验，产品、SAF 导入、凭据、身份设置与信任记录管理连同本机/SAF 与远端文件页面通过 §9.63 最终产物的完整 342 项设备回归；远端文件页面已接；复制任务尚未接入 |
| iOS / 其余端 | ❌ 原生宿主未交付 |

macOS／共享 PTY 已修复 SIGWINCH 通知，真实本地 shell→系统 ssh→远端 tmux 的窗口尺寸同步证据见[tmux 实施记录](docs/reference/optional-tmux-implementation.md#后续-pty-尺寸通知修复2026-10-30)。Bash/Zsh 鼠标残留清理脚本须显式安装到实际 shell 所在主机；真实休眠和用户远端未验，见[使用说明](docs/how-to/mouse-report-recovery.md)。各次 Android 测试数量保留在[实现记录](docs/reference/android-implementation.md#9-分阶段实施与依赖)，不与最新完整套件相加。

进度看板见 [docs/reference/roadmap.md](docs/reference/roadmap.md)；开发铁律见 [AGENTS.md](AGENTS.md)。

## 许可

**MIT License**（`LICENSE`）。随包组件（libghostty / libssh2 / OpenSSL 等）
各自按其许可分发；iSH/Termux 这类 GPL 组件若日后随包分发，仍需履行
其自身的 source offer，安排见
[docs/explanation/licensing.md](docs/explanation/licensing.md)。

> 历史版本（Lynx + Rust 实现）归档在 git tag `archive/lynx-rust-v1`，
> 其 `ssh -G` 黄金夹具已迁入 `core/tests/fixtures/`。
