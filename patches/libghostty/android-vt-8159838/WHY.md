# Android VT 的 libc 链接

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
