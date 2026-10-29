# Android VT 的 libc 链接与本地清屏

上游对照：`81598385d608dda4946093d6d6a234d204a2f225`。

Android 构建使用 `-Dsimd=false`。该 pin 的 `GhosttyZig.initVt` 因此不链接 libc，
Wuffs 模块会导出 hidden weak `calloc` / `free` 替身。静态 VT archive 链入 JNI
共享库后，原生 C surface 的 `calloc` 实际绑定到 `main.callocStub`，恒返回 NULL；
`free` 同样绑定到空实现。这不是 VT 网格冒烟能发现的问题：它使用 Zig 分配器，
没有覆盖宿主 C 分配和释放。

补丁只让 Android 目标显式链接 Bionic，继续关闭 SIMD，不删除图像或其他 VT 功能。
构建在工程 `tmp/` 的上游隔离副本应用，vendor 保持原 pin；补丁 SHA256 纳入
`build/android-native/arm64-v8a/receipt.json` 输入。Apple full 引擎不应用本补丁。

验证入口：`scripts/build-android.sh` 与设备 `LocalTerminalTest`，后者通过真实
PTY 输出构建、读取和释放 C surface/frame，覆盖 ANSI、宽字符及按键编码。
构建器检查最终 VT archive 不再定义 `calloc/free`（应由系统 libc 提供）；
`scripts/mutation-check-android-vt-libc.py` 另用 NDK 编译的真实 archive 验两组隔离变异（calloc/free），目标红、同变异 clean archive 对照绿、恢复绿与 SHA256。设备与终端变异证据见 [Android 实现计划 §9.13](../../../docs/reference/android-implementation.md#913-a2-真本地终端与生命周期首片2026-10-30)。

上游若修复 Android 无 SIMD 的 libc 链接或消除 Wuffs 替身的宿主符号污染，
升级 pin 时可撤除此补丁；须同时保留原生分配、设备绘制与释放回归。

## `0002-local-clear.patch`

上游 public VT API 的 `ghostty_terminal_reset` 是 RIS，会重置终端模式；向解析器注入
CSI 清屏还可能打断正在接收的 OSC/UTF-8 序列。Android 的“清除本地显示”要求不向
shell 发送命令或控制字符，因此增加小型 `ghostty_terminal_clear_local` C 扩展。
它只清当前屏幕的选区、内容、历史和图像，保留光标、模式、解析器及另一屏幕。
不调用 full engine 的清屏路径（该路径可能发送 FF）。

补丁与 libc 补丁一同在隔离副本应用，两个哈希进入 Android 收据。JNI 使用同次构建
安装的 VT 头；新增声明所在 `terminal.h` 与 archive 一起校验 SHA256。Apple full
引擎不应用此扩展，共享 Core ABI 不变。

验收由设备真实 PTY 驱动：清屏后原命令继续读取预期按键，保留 application-cursor
模式和未完成 OSC；全选/复制验证历史内容消失，备用屏幕清除后主屏内容仍在。
当前工作区的设备/变异结果见 [Android 实现计划 §9.14](../../../docs/reference/android-implementation.md#914-a2-文本操作工作区与文档复核2026-10-30)；
文本切片不扩大为完整 A2 验收。上游提供等价、不写 PTY 的 public local-clear API 后可撤此补丁。
