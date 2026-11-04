# 文档导航

从[当前状态](reference/current-status.md)了解源码、线上发行和验证范围；从[能力矩阵](reference/capability-matrix.md)看平台边界，从[路线图](reference/roadmap.md)看待办。验收规则只以 [AGENTS.md](../AGENTS.md) 为准。

## 安装与操作

- [在真实手机上验收 Android 客户端](how-to/android-device-check.md)
- [Android release APK](how-to/android-release.md)
- [拿一份 dev 版 macOS 包](how-to/dev-build.md)
- [自动清理 shell 提示符处的鼠标上报残留](how-to/mouse-report-recovery.md)
- [使用 tmux 后台会话](how-to/tmux-sessions.md)

## 架构

- [架构总览](architecture/overview.md)

## 能力、计划与实施记录

- [Android 最终方案实现计划](reference/android-implementation.md)
- [Android 终端配色与字体](reference/android-terminal-appearance.md)
- [Android 原型视觉落地](reference/android-visual-implementation.md)
- [Apple 端 UI 已知坑（SwiftUI / AppKit）](reference/apple-ui-pitfalls.md)
- [能力矩阵](reference/capability-matrix.md)
- [当前状态](reference/current-status.md)
- [主机 libssh2 构建与 SFTP 打开状态](reference/host-ssh2-build.md)
- [macOS 终端字体选择](reference/macos-terminal-fonts.md)
- [可选 tmux 实施清单](reference/optional-tmux-implementation.md)
- [计划：SFTP → 统一文件浏览器（M4.6）](reference/plan-files-manager.md)
- [可选 tmux 后台会话重构计划](reference/plan-optional-tmux.md)
- [计划：文件面板顶栏修复 + 标签右键菜单](reference/plan-panel-tab-menu.md)
- [macOS 标签栏与窗格界面重构计划](reference/plan-tab-chrome-refactor.md)
- [计划：关闭确认降噪 + ⌘T 继承当前标签页 + cwd 地基](reference/plan-tabs-close-and-newtab.md)
- [路线图](reference/roadmap.md)
- [统一文件窗格工作区 — 交接说明](reference/workspace-handoff.md)

## 设计资料

- [Tern 四种布局参考截图](design/tern-reference/README.md)
- [Android 交互原型](design/android-prototype.html)
- [macOS 标签原型](design/tabs-chrome-prototype.html)
- [原生 macOS 截图与收据](design/tab-chrome-native/)
- [历史外观方案](design/chrome-visual-options.html)

## 解释

- [许可安排](explanation/licensing.md)

## 模块与贡献

- [android/README.md](../android/README.md)
- [apple/README.md](../apple/README.md)
- [assets/icons/README.md](../assets/icons/README.md)
- [core/tests/README.md](../core/tests/README.md)
- [patches/README.md](../patches/README.md)
- [runtime-payload/README.md](../runtime-payload/README.md)
- [terminal/README.md](../terminal/README.md)
- [terminal/android/README.md](../terminal/android/README.md)
- [terminal/apple/README.md](../terminal/apple/README.md)

[网站维护与发布](../site/README.md)。设计原型与历史方案不是当前产品能力承诺。历史章节保留原时间、失败和验收范围；清空后的 `tmp/` 链接仅说明当时的临时位置。

逐份审阅范围、修正点与本轮验证见[文档核对记录](reference/documentation-audit.md)。
