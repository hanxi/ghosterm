# Android TerminalSurface

状态（2026-10-29）：**尚无实现和交叉编译产物**。本目录仅有说明。宿主设计见
[最终 HTML](../../docs/design/android-prototype.html)，开发顺序与验收见[Android 实现计划](../../docs/reference/android-implementation.md) A0/A2。

## 引擎与边界

采用锁定 `vendor/ghostty` 的 **libghostty-vt + 自绘**。full libghostty 的桌面 OpenGL 后端要求 GL 4.3 core，当前没有 Android GLES 后端；不能直接使用 Apple 的 full 引擎 shim。

拟实现：

- NDK 构建 libghostty-vt 与 C 桥，头/库使用同一 pin；不修改 vendor，补丁放 `patches/` 并附 WHY。
- 原生 owner 把 Transport 输出直接送 VT；渲染从 `ghostty_render_state_*` 取得状态，不将输出文本送 Compose。
- A2 先以 Canvas 验证网格、字形、选区、滚动与性能，再固定生产 renderer；需要 GLES 时保持 surface 接缝不变。
- Android `TerminalView` 与 InputConnection 负责组合输入、提交、选区和 IME 锚点；键鼠经过 VT encoder，尺寸同时通知 VT 与 Transport。
- 已有会话的主题/字号/行距实时生效；不可见时停画、owner 继续读流。surface 不知道连接是 SSH、本地还是 tmux。

验收必须覆盖真 PTY/SSH→VT→像素、CJK/组合字符、软硬键盘、旋转与 IME resize、焦点、背压和有界关闭；静态 HTML 不能替代这些证据。
