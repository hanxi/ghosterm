# Android TerminalSurface

状态（2026-10-30）：**A0 已验；A2 原生 surface、Canvas TerminalView、InputConnection 与生命周期首片已交付；文本操作在未提交工作区接入，整个 A2 未完成**。Gradle `:terminal` 指向本目录；native 子目录由 `:bridge` 的 CMake 统一构建，避免重复 Core。宿主设计见[最终 HTML](../../docs/design/android-prototype.html)，开发与验收见[Android 实现计划](../../docs/reference/android-implementation.md) A0/A2。

## 已实现

`scripts/build-android-native.py` 从锁定 `81598385d608dda4946093d6d6a234d204a2f225` 的隔离副本编译 libghostty-vt，Zig 0.16.0 / NDK r28c / arm64 / API 29，关闭 SIMD 路径以避免当前依赖额外 C++ 运行库。库与头使用同一 pin，不修改 vendor；不使用 Apple full 引擎补丁。A2 另加 [Android libc 补丁](../../patches/libghostty/android-vt-8159838/WHY.md)，避免无 SIMD 的 VT archive 带入空的 `calloc/free` 替身；补丁哈希进收据，构建器核验 archive 符号，两组真实 Android archive 隔离变异已通过。

`native/vt_jni.c` 提供版本查询与真实 VT cursor probe。`TerminalEngine` 没有 SSH/本地 shell 类型分支。API 36 arm64 模拟器验证 `CSI 5;10 H` 经真实解析器后光标位置为 `(9,4)`；短路 feed 变异使该断言失败，而 Core 配置往返仍通过。此 probe 是 A0 ABI 冒烟，不是生产终端数据链或渲染证据。

## A2 当前实现与待验收

- 已接：原生 owner 将 Transport 输出直接送 `native/surface.c` 的 VT，`TerminalView` 从受控网格快照读取；不把输出文本送 Compose。
- Canvas 已有真 PTY 的 ANSI/宽字符网格和红绿底像素定向证据；当前工作区增加 VT 选区高亮与手柄。持续输出/滚动性能仍待补齐，再固定生产 renderer；需要 GLES 时保持 surface 接缝。
- `TerminalView` 已实现 InputConnection 与键编码，尺寸同时通知 VT/Transport；组合/删除/提交已有真实 PTY 字节回读；此前结束标记换行导致的夹具超时已修正。初始焦点与基本硬件按键已由生产 Activity 验证；真实中文 IME 和完整硬件键盘矩阵仍待补。
- `setFontSize` 可更新字号并触发网格 resize；当前默认 14sp，主题/字号/行距设置尚未接入。不可见时停画、owner 继续读流，surface 不知道连接类别。
- 文本动作由 native owner 串行执行：VT 跟踪选区、formatter 复制原始字符、paste 按当前模式编码。平台 View 接系统剪贴板和浮动工具条，确认使用原始快照；URI 不转换为 shell 文本。正常 EOF 后保留 VT 供浏览/复制，显式关闭释放；不承诺错误退出后仍保留。
- 本地清屏使用独立 `0002-local-clear.patch`，清当前屏幕/历史/选区/图像，保留模式、解析器与另一屏幕；不向 PTY 写入。JNI 包含同次构建安装的 patched 头，`terminal.h` 和 archive 都进入缓存校验；不修改 Apple full 引擎补丁集。

最新设备与变异结果见[实施计划 §9.14](../../docs/reference/android-implementation.md#914-a2-文本操作工作区与文档复核2026-10-30)，首片 16 项定向测试与 21 组终端变异保留在 §9.13。证据均限 API 36 arm64 / 4 KB 页模拟器，不扩大为真机或完整 A2。选区已支持可见范围手柄拖动、滚动后选择及全选，边缘拖动自动滚动仍待补。

最终验收仍须覆盖真 PTY/SSH→VT→像素、CJK/组合字符、软硬键盘、旋转/IME resize、焦点、背压和有界关闭；当前 JNI 冒烟不能替代这些证据。
