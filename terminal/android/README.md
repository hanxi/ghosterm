# Android TerminalSurface

状态（2026-10-29）：**A0 引擎交叉编译与 JNI 装载已实现；TerminalView、自绘与 InputConnection 尚未实现**。Gradle `:terminal` 直接指向本目录。宿主设计见[最终 HTML](../../docs/design/android-prototype.html)，开发与验收见[Android 实现计划](../../docs/reference/android-implementation.md) A0/A2。

## 已实现

`scripts/build-android-native.py` 从锁定 `81598385d608dda4946093d6d6a234d204a2f225` 的隔离副本编译 libghostty-vt，Zig 0.16.0 / NDK r28c / arm64 / API 29，关闭 SIMD 路径以避免当前依赖额外 C++ 运行库。库与头使用同一 pin，不修改 vendor；不使用 Apple full 引擎补丁。

`native/vt_jni.c` 提供版本查询与真实 VT cursor probe。`TerminalEngine` 没有 SSH/本地 shell 类型分支。API 36 arm64 模拟器验证 `CSI 5;10 H` 经真实解析器后光标位置为 `(9,4)`；短路 feed 变异使该断言失败，而 Core 配置往返仍通过。此 probe 是 A0 ABI 冒烟，不是生产终端数据链或渲染证据。

## A2 待实现

- 原生 owner 将 Transport 输出直接送 VT，渲染从受控网格快照读取；不把输出文本送 Compose。
- Canvas 首片验证网格、字形、选区、滚动与性能，再固定生产 renderer；需要 GLES 时保持 surface 接缝。
- `TerminalView` 与 InputConnection 负责组合输入、提交、选区与 IME 锚点；键鼠经过 VT encoder，尺寸同时通知 VT 与 Transport。
- 已有会话主题/字号/行距实时生效；不可见时停画、owner 继续读流，surface 不知道连接类别。

验收覆盖真 PTY/SSH→VT→像素、CJK/组合字符、软硬键盘、旋转/IME resize、焦点、背压和有界关闭；当前 JNI 冒烟不能替代这些证据。
