# 当前状态

更新于 2026-11-04。源码、线上发行、完整测试分别记账；滚动 dev 下载应同时核对随包收据，不把本页快照当固定版本承诺。

## 源码与发行

| 范围 | 核对结果 |
|---|---|
| 本轮整理前源码 | `07b278e`，已包含 Android 设备存储浏览、触屏/连发、文件连接复用、专注终端与菜单输入法修正 |
| 线上 macOS dev | `3345893` / build 107 / arm64 / 最低 macOS 15.0 / ad-hoc / 未公证 |
| 线上 Android dev | `3345893` / clean / release / arm64-v8a / min SDK 29（Android 10）；8,691,625 字节；R8 与资源裁剪开启 |
| 发行查询来源 | [macOS version.json](https://github.com/hanxi/ghosterm/releases/download/dev/version.json)、[Android build-info](https://github.com/hanxi/ghosterm/releases/download/dev/android-build-info.json)，2026-11-04 实际读取 |

`07b278e` 的顶部区域与菜单输入法修正已有本地专项，核对时线上尚为 `3345893`。旧 `ae164f1`、`808c0ae` 的发行大小、构建号与测试均为历史，不再作为当前下载描述。两端发布工作流独立，后续可能不是同一提交。

## 产品范围

- macOS：full libghostty + Metal、普通本地 PTY/SSH、标签/分屏、SSH 配置界面、本地/SFTP 文件工作区与显式 tmux。配置保存事务、私钥安全存储生产接线、文件错误/完整性和受控接口迁移仍有缺口，见[路线图](roadmap.md#macos-下一步与共享缺口)。
- Android：libghostty-vt + Canvas、主机/身份/信任管理、本地/SSH、SAF/本机/SFTP 浏览与复制任务、持久只读历史、显式核对/清理、主题/字体/Kitty、可选远端 tmux。设备共享存储来源暂不支持作为粘贴目标；环境包延后。
- iOS、Windows、Linux、HarmonyOS、Web 原生客户端尚未交付。落地页是产品介绍网站，不是 Web 终端。
- 默认普通终端，后台会话由用户显式选择 tmux；自有 sessiond、私有恢复协议及随包 runtime 已退役。

详细平台差异见[能力矩阵](capability-matrix.md)。

## 验证范围

- 最近同源完整 Android 记录：`ae164f1`，API 29 / arm64 / 4 KiB 与 API 36 / arm64 / 16 KiB 各默认完整 942 项通过（[§9.191](android-implementation.md#9191-会话快照修正后的双系统完整-942-项2026-11-03)）。后续修改没有借用这轮结果声称新源码完整通过。
- 后续本地定向：§9.200–§9.204 分别保存设备存储、触屏/连发、文件 owner 复用、全屏布局及菜单 IME 的分轮结果和失败；Core 最近记录为 51 CTest / 95 ABI。各原生构建、lint 和变异的范围见对应节，不相加冒充全客户端回归。
- macOS 完整宿主历史仍有设置页像素失败；终端、绑定和打包专项不能代替完整宿主全绿，见[实施记录](optional-tmux-implementation.md)。
- 真机/OEM 输入法、网络切换、全部提供方/服务器、长时性能及完整发行矩阵仍未完成。合并闸门以 [AGENTS.md](../../AGENTS.md) 为唯一事实来源。

## 历史与临时材料

实施日志、失败和复验保留在原路径及原章节，避免破坏既有引用。文中 `tmp/` 路径是当时的临时取证位置，不是长期可用附件。

本轮按用户要求清空根目录 `tmp/`（包括旧构建、隔离副本、AVD 与原始临时取证文件），不重新运行或重新认证历史结果。落地页选用的三张原生截图已复制至 `site/assets/`，来源及 SHA256 见[截图收据](../../site/assets/screenshots.json)。后续重验需重新生成夹具/AVD 和临时证据。
