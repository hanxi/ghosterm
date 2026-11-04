# Ghosterm

**原生终端、SSH 配置管理与 SFTP 文件工作区。**

Ghosterm 使用各端原生 UI、共享 C11 核心和 libghostty。默认打开普通本地 PTY / SSH，后台会话由用户显式选择 tmux。当前提供 macOS 与 Android 开发版，其余端尚未交付。

[产品网站](https://hanxi.cc/ghosterm/) · [下载 dev 版](https://github.com/hanxi/ghosterm/releases/tag/dev) · [文档导航](docs/README.md) · [当前状态](docs/reference/current-status.md)

## 安装

- **macOS arm64**：[安装与升级](docs/how-to/dev-build.md)。开发包使用 ad-hoc 签名、未公证；最低系统以随包 `BUILD-INFO.txt` 为准。
- **Android arm64，Android 10+**：[下载 APK](https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-android-arm64-dev.apk)，[发行与校验说明](docs/how-to/android-release.md)。

滚动 dev 资产会更新，两端独立出包。源码进度、当前发行及历史完整测试分别见[当前状态](docs/reference/current-status.md)，完整设备/输入法/网络/文件服务矩阵仍在推进。

## 能做什么

| 平台 | 已接入的主要功能 |
|---|---|
| macOS | full libghostty / Metal，本地与 SSH 终端，标签/分屏，SSH 配置界面，本地/SFTP 文件窗格、传输与 Quick Look，显式 tmux |
| Android | libghostty-vt / Canvas，本地与 SSH，身份/主机验证，SAF/本机/SFTP 文件浏览与复制任务，历史核对/清理，主题/字体/Kitty，显式远端 tmux |

功能仍有边界，特别是 macOS 配置保存、安全存储和文件失败处理；详见[能力矩阵](docs/reference/capability-matrix.md)与[路线图](docs/reference/roadmap.md)。

## 架构与目录

`UI → TerminalSurface → Transport → Core`；终端字节由 transport 直达 surface，不经过 UI 事件队列。各端仅通过 `core/include/ghosterm.h` 消费 C ABI。不使用跨端 UI 框架或 Rust。

| 目录 | 用途 |
|---|---|
| `core/` | C11 / POSIX 共享核心：SSH、SFTP、ssh_config、known_hosts、PTY |
| `terminal/` | 各端 TerminalSurface；Apple full libghostty，Android vt + 自绘 |
| `apple/`、`android/` | 原生宿主与 UI |
| `assets/`、`patches/` | 品牌母版和派生资源、带 WHY 的上游补丁 |
| `runtime-payload/` | 后续按需环境包规范，当前没有安装器 |
| `scripts/`、`docs/`、`site/` | 构建/闸门、文档、GitHub Pages 落地页 |

## 开发

macOS 使用完整 Xcode；依赖准备见[主机 SSH 构建](docs/reference/host-ssh2-build.md)。

```bash
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
mkdir -p "$PWD/tmp"
export TMPDIR="$PWD/tmp"
export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
python3 scripts/build-host-ssh2.py --fetch
bash scripts/gate.sh
```

[Apple 构建](apple/README.md) · [Android 构建](android/README.md) · [终端接缝](terminal/README.md) · [网站维护](site/README.md)

提交与验收规则以 [AGENTS.md](AGENTS.md) 为准；CI 绿不等于全部手工闸门通过。历史实现和测试记录保留在文档中，不作为当前提交的通过证明。

## 许可

[MIT License](LICENSE)。随包组件分别遵守其自身许可，详见[许可说明](docs/explanation/licensing.md)。
