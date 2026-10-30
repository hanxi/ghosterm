# Android TerminalSurface

最近已验收的完整设备结果为 **164 项通过**，对应已提交的共享信任事务与删除绑定，见[§9.37](../../docs/reference/android-implementation.md#937-主机信任写入事务与移除绑定2026-10-30)。工作区另有未验收的管理页面/公钥解析草稿（§9.38），不在这份历史结果的证明范围内。native owner→Transport→VT 字节通路保持不变；不扩大 API 36 arm64 / 4 KB 页模拟器之外的设备、性能或完整 A2/A3 验收范围。下文数量保留各历史切片记录。

状态（2026-10-30）：**A0 已验；A2 原生 surface、Canvas TerminalView、InputConnection 与生命周期首片已交付；文本操作切片已验，整个 A2 未完成**。Gradle `:terminal` 指向本目录；native 子目录由 `:bridge` 的 CMake 统一构建，避免重复 Core。宿主设计见[最终 HTML](../../docs/design/android-prototype.html)，开发与验收见[Android 实现计划](../../docs/reference/android-implementation.md) A0/A2。

## 已实现

`scripts/build-android-native.py` 从锁定 `81598385d608dda4946093d6d6a234d204a2f225` 的隔离副本编译 libghostty-vt，Zig 0.16.0 / NDK r28c / arm64 / API 29，关闭 SIMD 路径以避免当前依赖额外 C++ 运行库。库与头使用同一 pin，不修改 vendor；不使用 Apple full 引擎补丁。A2 另加 [Android libc 补丁](../../patches/libghostty/android-vt-8159838/WHY.md)，避免无 SIMD 的 VT archive 带入空的 `calloc/free` 替身；补丁哈希进收据，构建器核验 archive 符号，两组真实 Android archive 隔离变异已通过。

`native/vt_jni.c` 提供版本查询与真实 VT cursor probe。`TerminalEngine` 没有 SSH/本地 shell 类型分支。API 36 arm64 模拟器验证 `CSI 5;10 H` 经真实解析器后光标位置为 `(9,4)`；短路 feed 变异使该断言失败，而 Core 配置往返仍通过。此 probe 是 A0 ABI 冒烟，不是生产终端数据链或渲染证据。

## A2 当前实现与待验收

- 已接：原生 owner 将 Transport 输出直接送 `native/surface.c` 的 VT，`TerminalView` 从受控网格快照读取；不把输出文本送 Compose。
- Canvas 已有真 PTY 的 ANSI/宽字符网格和红绿底像素定向证据；`12c88ba` 已接入 VT 选区高亮与手柄。模拟器 debug 持续输出/滚动基线已见 §9.21；仍须补真机/release/长负载证据再固定生产 renderer；需要 GLES 时保持 surface 接缝。
- `TerminalView` 已实现 InputConnection 与键编码，尺寸同时通知 VT/Transport；组合/删除/提交已有真实 PTY 字节回读；此前结束标记换行导致的夹具超时已修正。初始焦点与基本硬件按键已由生产 Activity 验证；真实 Gboard 拼音基本路径已按 §9.16 验证，完整软硬键盘矩阵仍待补。
- `setFontSize` 可更新字号并触发网格 resize；当前默认 14sp，主题/字号/行距设置尚未接入。不可见时停画、owner 继续读流，surface 不知道连接类别。
- 文本动作由 native owner 串行执行：VT 跟踪选区、formatter 复制原始字符、paste 按当前模式编码。平台 View 接系统剪贴板和浮动工具条，确认使用原始快照；URI 不转换为 shell 文本。正常 EOF 后保留 VT 供浏览/复制，显式关闭释放；不承诺错误退出后仍保留。
- 本地清屏使用独立 `0002-local-clear.patch`，清当前屏幕/历史/选区/图像，保留模式、解析器与另一屏幕；不向 PTY 写入。JNI 包含同次构建安装的 patched 头，`terminal.h` 和 archive 都进入缓存校验；不修改 Apple full 引擎补丁集。

文本动作首片结果见[实施计划 §9.14](../../docs/reference/android-implementation.md#914-a2-文本操作工作区与文档复核2026-10-30)，首片 16 项定向测试与 21 组终端变异保留在 §9.13。证据均限 API 36 arm64 / 4 KB 页模拟器，不扩大为真机或完整 A2。已提交选区支持可见范围手柄拖动、滚动后选择及全选。边缘自动滚动、跨越锚点/重新抓取、最近手柄命中与 Ctrl 生命周期已交付，完整 85 项设备测试与 35 组文本/21 组终端变异见[§9.15](../../docs/reference/android-implementation.md#915-选区与-ctrl-工作区进度2026-10-30)。

最终验收仍须覆盖真 PTY/SSH→VT→像素、CJK/组合字符、软硬键盘、旋转/IME resize、焦点、背压和有界关闭；当前 JNI 冒烟不能替代这些证据。

Ctrl 标点与键盘协议切片见[§9.16](../../docs/reference/android-implementation.md#916-ctrl-标点与键盘协议2026-10-30)：单个 ASCII 提交交 VT 编码，Unicode/批量提交保持原文；私有按键接缝携带布局原始字符，按当前 Kitty/modifyOtherKeys 模式编码已测按键；InputConnection 单独 LF 经 Enter 编码，整段提交保持原文。本片完整 90 项设备测试通过，文本/终端变异分别选跑 12/9 个不同组；真实 Gboard 拼音候选、删除与提交有手工字节证据。该片结束时，真实外接设备、死键与完整重复/释放事件矩阵仍待实现和验收；后续见 §9.17。

按键事件后续切片见[§9.17](../../docs/reference/android-implementation.md#917-按键重复释放与焦点归属2026-10-30)：按下/重复/释放由 VT 编码，软 Ctrl 跟随同一按键，失焦和换 surface 前释放给原 owner；辅助键与 IME 按键点击成对发送。完整 94 项设备测试、18 组文本与 2 组生命周期选组变异通过；首轮失焦测试补强和 Gboard 夹具修正保留在实施记录。独立修饰键、死键、数字键盘、真实外接键盘与性能仍待补齐。

独立修饰键、数字键盘和组合音符后续切片已完成本片验收，沿私有按键接缝交 VT 编码，并按输入设备隔离组合状态。完整 99 项设备测试及 24 组选组变异通过，见[§9.18](../../docs/reference/android-implementation.md#918-修饰键数字键盘与组合音符2026-10-30)。不扩大为全部物理键、布局或性能已交付。

静止画面的闪烁文字已修复：隐藏光标且没有新输出时仍自动重绘，普通文字保持可见。该切片完整设备测试 **100 项全通过**，五组渲染变异通过，证据见[§9.19](../../docs/reference/android-implementation.md#919-静止画面的闪烁文本2026-10-30)；不替代 A2 性能与真机验收。

输入后短时取帧已交付：键入/按键成功后唤醒 tick，120ms 内按显示帧取快照，随后恢复 32ms；隐藏仍停画，native owner 持续读流。完整 **101 项设备测试**、新增四组性能变异及终端四组/文本一组回归通过，见[§9.21](../../docs/reference/android-implementation.md#921-输入后短时取帧与性能基线2026-10-30)。性能证据仅限当前模拟器 debug 档；原持续按显示帧轮询的实验因 CPU/吞吐代价未采用。

Android SSH 后端已在[§9.25](../../docs/reference/android-implementation.md#925-android-ssh-认证与远端终端后端2026-10-30)接入相同 native owner、VT 与 TerminalSurface 接口；认证状态是独立控制面，输出不经过 Kotlin。默认 ViewModel 工厂、认证页面、Keystore 容器、多会话选择与远端 Activity 重建已有定向证据，见[§9.27](../../docs/reference/android-implementation.md#927-ssh-产品页面与加密身份容器2026-10-30)。随后修正本地终端测试判据，完整 130 项回归通过；真实域名/IPv6 SSH 与完整 A2/A3 仍待验收；后续 SAF 切片见下文，不能据当前模拟器首片通过声称阶段完成。

SAF 私钥导入生命周期已按[§9.29](../../docs/reference/android-implementation.md#929-saf-私钥导入生命周期2026-10-30)完成五项定向、七组变异及完整 135 项设备回归；历史选择器导航与 Activity 重建等待失败保留在 §9.28–§9.29。终端输出接缝未改变，仍须补其他设备、性能与 A3 剩余产品验收。
