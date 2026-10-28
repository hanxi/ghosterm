# Ghosterm

**libghostty 驱动的多端终端** × **VS Code 式 `~/.ssh/config` 图形管理** × **WinSCP 式 SFTP 文件管理器**。

## 技术栈

| 层 | 选型 |
|---|---|
| UI | **各端原生**：SwiftUI(macOS/iOS) · Compose(Android) · ArkUI(HarmonyOS) · WinUI3(Windows) · GTK4(Linux) · Web |
| 终端引擎 | **full libghostty**（Apple/Linux，Metal/GL4.3）· **libghostty-vt + 自绘**（Web/Android/HarmonyOS） |
| 共享核心 | **C11**（`core/`）：ssh(libssh2) · sftp · ssh_config · known_hosts · 端口转发 · PTY |
| 接缝 | `UI → TerminalSurface → Transport(本地PTY/SSH) → Core` |
| 环境包 | 按需下载：Android Termux/proot · iOS iSH（不进 App 包） |

**不用跨端 UI 框架，不用 Rust。** 决策记录见 [docs/architecture/overview.md](docs/architecture/overview.md)。

## 目录

```
core/           C 共享库（唯一跨语言边界：include/ghosterm.h）
terminal/       各端 TerminalSurface（full libghostty 或 vt+自绘）
apple/ android/ windows/ linux/ harmony/ web/    各端宿主 + UI
assets/         品牌资源：应用图标母版 SVG + 各端派生（scripts/build-icons.sh）
runtime-payload/ 环境包清单与下载/校验（二进制不入库）
patches/        上游补丁（每个带 WHY.md）
scripts/        构建 / 闸门 / 变异验证
docs/           架构 · 能力矩阵 · 许可 · 路线 · Apple UI 已知坑
```

## 快速开始

```bash
# 离线闸门（无网可跑；要求 cmake ≥3.19 与 C 编译器）
bash scripts/gate.sh                       # 核心：构建 + ssh_config 对拍 + ABI + 支持集双向校验

# Apple 端（需要 zig 0.16.0 / Xcode / Metal toolchain）
bash scripts/build-ghostty.sh              # vendor 打补丁 → GhosttyKit.xcframework
bash scripts/gate-ghostty-patch.sh         # 补丁与树一致 + 补丁符号真的导出
bash scripts/terminal-gate.sh              # TerminalSurface（字节 → libghostty 网格）
bash scripts/apple-app-gate.sh             # 绑定 ABI 对拍 + 主机列表 + 真 sshd e2e

# 变异验证：证明闸门有牙，不是空跑
bash scripts/mutation-check-sshconfig.sh        # 14 项
bash scripts/mutation-check-ssh.sh              #  6 项
bash scripts/mutation-check-ghostty-patch.sh    #  6 项
bash scripts/mutation-check-terminal.sh         #  5 项
bash scripts/mutation-check-sftp.sh             #  6 项
bash scripts/mutation-check-icons.sh            #  7 项
bash scripts/mutation-check-package-mac.sh      # 11 项

# 应用图标：母版 SVG → 各端图标（需要 rsvg-convert / ImageMagick / python3）
bash scripts/build-icons.sh                    # 全端生成
bash scripts/build-icons.sh check              # 产物与母版是否一致

# macOS 打包：.app + .dmg + .zip + SHA256SUMS（详见下方「跑起来」）
bash scripts/package-mac.sh                    # 出包到 dist/
bash scripts/package-mac.sh --help             # 全部选项（版本/签名/公证/最低系统）
```

闸门要求本机有 `cmake`（≥3.19）与 C 编译器。**无网可跑**；Apple 端闸门
在缺 zig/sshd 时显式 SKIP，不假装通过。

## 跑起来（macOS）

```bash
bash scripts/build-mac-app.sh        # 出包：tmp/Ghosterm.app（自包含的普通终端包）
open tmp/Ghosterm.app                # 双击即用：主机列表 / 终端 / 编辑器 / SFTP
```

包是**自包含**的（core 与 libghostty 静态链接，libssh2 嵌入
`Contents/Frameworks/`）——拷到别的 arm64 Mac 也能跑，不需要 Homebrew。
ad-hoc 签名；Developer ID + 公证是显式遗留项（待证书，见 roadmap）。

可选后台会话从 `+ → 本机后台会话…` 或主机右键菜单打开；目标需安装 tmux >=3.6a，
不影响普通终端。操作与保留边界见[使用 tmux 后台会话](docs/how-to/tmux-sessions.md)。

不带包直接跑（开发循环）：

```bash
cd apple/GhostermApp && swift run GhostermApp                 # GUI
swift run GhostermApp --local                                 # 无窗口冒烟（打网格文本）
swift run GhostermApp --ssh-smoke <config> <host别名>          # 无 UI 走 AppModel 全链路
```

## 当前状态（2026-10-28，可选 tmux 重构）

| 能力 | 状态 |
|---|---|
| C 核心骨架 + `ghosterm.h` v0 ABI | ✅ 70 符号，双向对拍；旧会话 ABI 已移除 |
| `ssh_config` 解析 / 无损往返（未编辑） | ✅ 与原文逐字节相同 |
| `ssh_config` 求值 | ✅ typed 视图 + 通用视图（`ssh -G` 等价）；42 键 × 6 host 全表语义对拍 |
| `ssh_config` 编辑 API | ✅ 无损写回（除目标行外逐字节不变）+ 9 用例对拍 |
| `ssh_config` `Include` 展开 | ✅ OpenSSH 同口径（相对路径基准 = ~/.ssh、glob、嵌套 + 深度上限 16）；6 组断言 + 变异 I1~I4 |
| 本地 PTY transport（forkpty） | ✅ 已实现（`pty_transport` 18 项断言） |
| SSH（libssh2）connect / auth / hostkey TOFU / ssh-shell | ✅ `ssh_e2e` 真 sshd 12 项断言 |
| SFTP（list / stat / 上传 / 下载 / job 三态） | ✅ libssh2_sftp 封装 + 真 sshd e2e + 变异验证 |
| **macOS 功能完整（M2–M4.5）** | ✅ 2026-10-12 |
| ├ TerminalSurface（full libghostty + Metal） | ✅ 上游补丁提供 host IO 后端（`patches/libghostty/`） |
| ├ SwiftUI 宿主 + `ghosterm.h` 绑定 | ✅ 窗口 / 侧边栏 / 多标签页 / 分屏 / 设置页 / 三态对话框；绑定 ABI 对拍 68/70（另 2 项显式登记） |
| ├ 终端 UX（M4.5：标签页 / 分屏 / 设置页） | ✅ 布局视觉参照 iGhostVT；后台标签页保持挂载；16 项结构断言 + 设置断言变异 S1~S5。**遗留**：标签页恢复、拖拽重排手势 |
| ├ 配色集随包 + 主题选择 | ✅ 725 套 Ghostty 配色随包（`Contents/Resources/ghostty/themes`）+ 资源目录推导；出包包内配置检查验主题被引擎接受 |
| ├ 终端的设置真的生效 | ✅ 写 Ghostty 语法的配置文件交给引擎解析（不自己维护配置子集）；`font-size` 改完网格密度真的变（11 项断言） |
| ├ ssh_config 图形编辑（M3） | ✅ 编辑器 + 通配块 + 备份 / 冲突检测 / 原子写回 |
| ├ SFTP 文件浏览器（M4） | ✅ 浏览 / 下载 / 上传 / 传输四态（与终端解耦的第二会话） |
| └ 渲染 | ✅ 三层证据：字节→网格、渲染层已挂、**像素级**（抓自进程内视图帧比颜色，含对照与变异 P1~P3）——抓自进程内的视图不需要录屏权限 |
| 打包分发（脚本 + CI + DMG + 校验和） | ✅ `scripts/package-mac.sh` + `macos-release.yml`；ad-hoc 签名已可出包，**Developer ID + 公证待证书**（显式遗留） |
| 可选 tmux 后台会话 | 🟡 macOS 与 Linux SSH 的 tmux 3.6a；用户显式打开列表、选择及重新连接；保留边界见使用说明 |
| iOS / Android / 其余端 | ⏳ 未开始 |

进度看板见 [docs/reference/roadmap.md](docs/reference/roadmap.md)；开发铁律见 [AGENTS.md](AGENTS.md)。

## 许可

**MIT License**（`LICENSE`）。随包组件（libghostty / libssh2 / OpenSSL 等）
各自按其许可分发；iOS 侧 iSH/Termux 这类 GPL 组件若日后随包分发，仍需履行
其自身的 source offer，安排见
[docs/explanation/licensing.md](docs/explanation/licensing.md)。

> 历史版本（Lynx + Rust 实现）归档在 git tag `archive/lynx-rust-v1`，
> 其 `ssh -G` 黄金夹具已迁入 `core/tests/fixtures/`。
