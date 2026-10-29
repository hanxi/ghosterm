# Ghosterm

**libghostty 驱动的多端终端** × **VS Code 式 `~/.ssh/config` 图形管理** × **WinSCP 式 SFTP 文件管理器**。

## 技术栈

| 层 | 选型 |
|---|---|
| UI | **各端原生**：SwiftUI(macOS/iOS) · Compose(Android) · ArkUI(HarmonyOS) · WinUI3(Windows) · GTK4(Linux) · Web |
| 终端引擎 | **full libghostty**（Apple/Linux，Metal/GL4.3）· **libghostty-vt + 自绘**（Web/Android/HarmonyOS） |
| 共享核心 | **C11**（`core/`）：ssh(libssh2) · sftp · ssh_config · known_hosts · PTY（端口转发待实现） |
| 接缝 | `UI → TerminalSurface → Transport(本地PTY/SSH) → Core` |
| 环境包 | 按需下载：Android Termux/proot · iOS iSH（不进 App 包） |

**不用跨端 UI 框架，不用 Rust。** 决策记录见 [docs/architecture/overview.md](docs/architecture/overview.md)。

## 目录

```
core/           C 共享库（唯一跨语言边界：include/ghosterm.h）
terminal/       各端 TerminalSurface（full libghostty 或 vt+自绘）
apple/ android/  现有宿主 + UI（其余端 windows/linux/harmony/web 为规划）
assets/         品牌资源：应用图标母版 SVG + 各端派生（scripts/build-icons.sh）
runtime-payload/ 环境包清单与下载/校验（二进制不入库）
patches/        上游补丁（每个带 WHY.md）
scripts/        构建 / 闸门 / 变异验证
docs/           架构 · 能力矩阵 · 许可 · 路线 · Apple UI 已知坑
```

## 快速开始

```bash
# 离线闸门（依赖已安装；要求 cmake ≥3.19、C 编译器、Python 3）
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

核心闸门要求 `cmake`（≥3.19）、C 编译器与 Python 3。依赖已安装时**无网可跑**；
要覆盖真实 SSH/SFTP，还需可探测的 libssh2、sshd 与 ssh-keygen。缺 libssh2 时核心返回
`GT_ERR_UNSUPPORTED`，不等于 SSH 功能通过。Apple 宿主闸门缺 GhosttyKit 产物会失败，
缺 Swift 或无法启动 sshd 时有显式 SKIP；验收必须检查实际覆盖，不能只看退出码。

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

## 当前状态（2026-10-30）

字段来源接口及 Android 主机表单接线已在[实现计划 §9.5](docs/reference/android-implementation.md#95-字段来源与编辑归属2026-10-29)记录。Core、平台绑定、设备与 UI 分别验收，不能把接口或历史测试数量当作完整客户端交付证据。

Android 已补齐三字段显式覆盖/恢复继承、主机别名重命名及配置错误恢复；SAF 预览/取消/失败路径与表单校验收尾后，47 项设备测试及 59 组配置/页面变异通过，A1 在 API 36 arm64 模拟器档完成，见[实现计划 §9.11](docs/reference/android-implementation.md#911-saf-导入预览与-a1-收尾2026-10-30)。A2 首片已交付本地 PTY、Canvas、输入桥与会话生命周期；文本操作切片已接入终端选择、复制粘贴和本地清屏，当前验证见[实现计划 §9.14](docs/reference/android-implementation.md#914-a2-文本操作工作区与文档复核2026-10-30)。选区边缘自动滚动与 Ctrl 生命周期切片已验，见[§9.15](docs/reference/android-implementation.md#915-选区与-ctrl-工作区进度2026-10-30)；本片另有 Ctrl 标点、Gboard 回车与拼音基本路径的验证，完整 90 项设备测试及选组变异见[§9.16](docs/reference/android-implementation.md#916-ctrl-标点与键盘协议2026-10-30)；按键重复/释放、焦点归属及辅助键成对事件已验（完整 94 项设备测试），见[§9.17](docs/reference/android-implementation.md#917-按键重复释放与焦点归属2026-10-30)。完整软硬键盘矩阵与性能仍待完成。macOS 配置事务、表单保真、终端设置读写、传输控制错误与未知结果继续列为[路线图待办](docs/reference/roadmap.md#macos-下一步与共享缺口)。

| 能力 | 状态 |
|---|---|
| C 核心骨架 + `ghosterm.h` v0 ABI | 🟡 73 符号，双向对拍；旧会话 ABI 已移除 |
| `ssh_config` 解析 / 无损往返（未编辑） | ✅ 与原文逐字节相同 |
| `ssh_config` 求值 | ✅ typed 视图 + 通用视图（`ssh -G` 等价）；42 键 × 6 host 全表语义对拍 |
| `ssh_config` 编辑 API | 🟡 9 个原夹具 + 5 个补充用例逐字节对拍；非连续重复指令已按实际匹配行编辑，已增加三字段来源与包含文件/重名块编辑保护；已接入仅替换目标别名 token 的重命名接口；全部字段视图仍待补齐 |
| `ssh_config` `Include` 展开 | ✅ OpenSSH 同口径（相对路径基准 = ~/.ssh、glob、嵌套 + 深度上限 16）；6 组断言 + 变异 I1~I4 |
| 本地 PTY transport（forkpty） | ✅ POSIX 实现（macOS/Linux 证据；Android A2 首片已有 API 36 模拟器定向/变异证据，整个阶段未完成） |
| SSH（libssh2）connect / auth / hostkey TOFU / ssh-shell | 🟡 普通条目、密码/密钥与可取消认证已有真 sshd 证据；macOS 未执行 ProxyJump 等高级配置且缺连接前能力校验，known_hosts 标记也可能误信任，均列为路线图 P0；Android 尚未绑定 |
| SFTP（list / stat / 上传 / 下载 / job 三态） | 🟡 基础原语有真 sshd e2e 与变异证据；非覆盖创建、覆盖截断及过载下终态保障仍待补齐 |
| **macOS 原生宿主** | 🟡 已交付主要功能；配置写盘与私钥安全存储接线仍有缺口，见路线图 |
| ├ TerminalSurface（full libghostty + Metal） | ✅ 上游补丁提供 host IO 后端（`patches/libghostty/`） |
| ├ SwiftUI 宿主 + `ghosterm.h` 绑定 | ✅ 窗口 / 侧边栏 / 多标签页 / 分屏 / 设置页 / 三态对话框；绑定 ABI 对拍 71/73（另 2 项显式登记）；改名绑定不等于 macOS 表单已接入 |
| ├ 标签 / 窗格 / 设置 | ✅ 单窗口顶部或侧边标签、UUID 重排、唯一加号、Ctrl+Tab / Ctrl+Shift+Tab 循环切换；统一终端/文件工作区、分屏预览、焦点与停画。**遗留**：标签/布局跨重启持久化（设置偏好已持久化） |
| ├ 配色集随包 + 主题选择 | ✅ Ghostty 配色集随包（`Contents/Resources/ghostty/themes`，数量由构建输出决定）+ 资源目录推导；出包包内配置检查验主题被引擎接受 |
| ├ 终端设置 | 🟡 配置由 Ghostty 解析，字号/配色已有真实 surface 证据；`SettingsStore` 重复键编辑、读取错误与冲突判定仍有缺口，不能把正常配置的测试扩大为全部保存路径正确 |
| ├ 终端系统复制粘贴 | ⏳ 引擎文本输入已有证据；系统剪贴板读取/写入/确认与终端菜单路由尚未接通，见路线图 P1 |
| ├ ssh_config 图形编辑（M3） | 🟡 编辑器、通配块与 Core 稀疏 API 已实现；共享重复指令误删已修复。macOS 表单仍简化扫描/提交全部字段，备份错误被忽略、替换先删后移，保真与事务保障待修复 |
| ├ 文件工作区与传输 | 🟡 本地/SFTP 统一窗格、队列、拖放与 Quick Look；独立文件连接。控制异常/未知结果、目录内链接跳过明细尚待修复；目录冲突 UI 等验收缺口见交接文档 |
| ├ 文件查询与搜索 | 🟡 正常浏览/搜索已有接线；存在性查询折叠错误，搜索未检查命令退出状态且取消未传入远端；传输入队/控制仍可能同步阻塞主线程，见 [Apple 说明](apple/README.md#文件查询与线程边界) |
| └ 渲染 | ✅ 三层证据：字节→网格、渲染层已挂、**像素级**（抓自进程内视图帧比颜色，含对照与变异 P1~P3）——抓自进程内的视图不需要录屏权限 |
| 打包分发（脚本 + CI + DMG + 校验和） | ✅ `scripts/package-mac.sh` + `macos-release.yml`；ad-hoc 签名已可出包，**Developer ID + 公证待证书**（显式遗留） |
| 可选 tmux 后台会话 | 🟡 macOS 与 Linux SSH 的 tmux 3.2a、3.6a；用户显式打开列表、选择及重新连接；保留边界见使用说明 |
| Android | 🟡 A0/A1 已在 API 36 arm64 模拟器档完成；[实现进度](docs/reference/android-implementation.md)，A2 本地终端首片已交付，文本操作切片已验；选区边缘滚动/Ctrl 生命周期切片已验；Gboard 拼音基本路径与按键重复/释放切片已验；完整软硬键盘矩阵/性能待补，SSH/文件仍待接入 |
| iOS / 其余端 | ❌ 原生宿主未交付 |

静止画面的闪烁文字已修复：隐藏光标且没有新输出时仍自动重绘，普通文字保持可见。最新完整设备测试 **100 项全通过**，五组渲染变异通过，证据见[§9.19](docs/reference/android-implementation.md#919-静止画面的闪烁文本2026-10-30)；不替代 A2 性能与真机验收。

进度看板见 [docs/reference/roadmap.md](docs/reference/roadmap.md)；开发铁律见 [AGENTS.md](AGENTS.md)。

Android 修饰键、数字键盘与组合音符切片已通过完整 **99 项设备测试**及 **24 组选组变异**，详见[§9.18](docs/reference/android-implementation.md#918-修饰键数字键盘与组合音符2026-10-30)。A2 仍缺完整物理设备/布局矩阵和性能基线，不因本片通过而标为完成。

## 许可

**MIT License**（`LICENSE`）。随包组件（libghostty / libssh2 / OpenSSL 等）
各自按其许可分发；iSH/Termux 这类 GPL 组件若日后随包分发，仍需履行
其自身的 source offer，安排见
[docs/explanation/licensing.md](docs/explanation/licensing.md)。

> 历史版本（Lynx + Rust 实现）归档在 git tag `archive/lynx-rust-v1`，
> 其 `ssh -G` 黄金夹具已迁入 `core/tests/fixtures/`。
