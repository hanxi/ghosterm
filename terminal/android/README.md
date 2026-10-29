# Android TerminalSurface

状态（2026-10-30）：**A0 已验；A2 已有未提交的原生 surface、Canvas TerminalView 和 InputConnection，尚未验收**。Gradle `:terminal` 指向本目录；native 子目录由 `:bridge` 的 CMake 统一构建，避免重复 Core。宿主设计见[最终 HTML](../../docs/design/android-prototype.html)，开发与验收见[Android 实现计划](../../docs/reference/android-implementation.md) A0/A2。

## 已实现

`scripts/build-android-native.py` 从锁定 `81598385d608dda4946093d6d6a234d204a2f225` 的隔离副本编译 libghostty-vt，Zig 0.16.0 / NDK r28c / arm64 / API 29，关闭 SIMD 路径以避免当前依赖额外 C++ 运行库。库与头使用同一 pin，不修改 vendor；不使用 Apple full 引擎补丁。当前 A2 另加 Android libc 补丁（`patches/libghostty/android-vt-8159838/WHY.md`，未提交），避免无 SIMD 的 VT archive 带入空的 `calloc/free` 替身；补丁哈希进收据，构建器核验 archive 符号，新检查的变异尚待补齐。

`native/vt_jni.c` 提供版本查询与真实 VT cursor probe。`TerminalEngine` 没有 SSH/本地 shell 类型分支。API 36 arm64 模拟器验证 `CSI 5;10 H` 经真实解析器后光标位置为 `(9,4)`；短路 feed 变异使该断言失败，而 Core 配置往返仍通过。此 probe 是 A0 ABI 冒烟，不是生产终端数据链或渲染证据。

## A2 当前实现与待验收

- 已接：原生 owner 将 Transport 输出直接送 `native/surface.c` 的 VT，`TerminalView` 从受控网格快照读取；不把输出文本送 Compose。
- Canvas 已有真 PTY 的 ANSI/宽字符网格和红绿底像素定向证据；选区、持续输出/滚动性能仍待补齐，再固定生产 renderer；需要 GLES 时保持 surface 接缝。
- `TerminalView` 已实现 InputConnection 与键编码，尺寸同时通知 VT/Transport；组合/删除/提交的端到端用例仍超时失败，不能称输入法已验收；鼠标/选区和真实系统 IME 仍待补。
- 已有会话主题/字号/行距实时生效；不可见时停画、owner 继续读流，surface 不知道连接类别。

当前 `LocalTerminalTest` 5 项通过，`TerminalScreenTest` 3 通过/1 失败（API 36 arm64 / 4 KB 页模拟器）；文档复核只读取已有日志，没有新增设备结果。详见[实施计划 §9.12](../../docs/reference/android-implementation.md#912-a2-本地终端工作区状态2026-10-30未验收)。

最终验收仍须覆盖真 PTY/SSH→VT→像素、CJK/组合字符、软硬键盘、旋转/IME resize、焦点、背压和有界关闭；当前 JNI 冒烟不能替代这些证据。
