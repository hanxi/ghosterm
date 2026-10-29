# 架构总览

## 为什么是「原生 UI + 共享 C 核心」

前一版（Lynx + Rust）的核心痛点不在语言，而在**接缝位置**：

- 把「共享 UI」当成赌注，实际共享的只有 ~6–7k 行薄 UI；
- 而真正厚的部分（原生渲染面、SSH/SFTP、ssh_config）本来就跨不了端，
  却为它们付了跨端框架的全部成本（框架怪癖、桥接、构建链、iOS 线程模型）。

结论：**UI 不跨端，能力跨端。** 于是有本架构。

## 分层

```
┌────────────────────────────────────────────────────────┐
│ UI（各端原生，互不共享代码）                             │
├────────────────────────────────────────────────────────┤
│ TerminalSurface —— 字节 ⇄ 屏幕（每端一个实现）           │
│   Apple/Linux: full libghostty (Metal/GL4.3)             │
│   Web/Android/HarmonyOS: libghostty-vt + 自绘            │
├────────────────────────────────────────────────────────┤
│ Transport —— 字节流（本地 PTY / SSH 同一接口）           │
├────────────────────────────────────────────────────────┤
│ Core（C 共享库）—— ssh / sftp / ssh_config / known_hosts │
│   POSIX PTY；端口转发 / ConPTY 待实现                    │
└────────────────────────────────────────────────────────┘
```

## 三个关键决策

1. **TerminalSurface 与 Transport 解耦**：`transport` 只管字节进出，`surface` 只管
   把字节变成屏幕。于是「SSH vs 本地 shell」「Metal vs 自绘」互不污染。
2. **字节流不过事件队列**：终端输出由会话线程 `transport_read → surface` 直连；
   事件队列只承载低频状态。这是性能红线。
3. **C ABI 是唯一跨语言边界**：`core/include/ghosterm.h`。opaque handle + POD，
   当前 Swift 与 Android JNI 引用已有对拍；A1 JNI 绑定 11 项；A2 首片为 19 项（新增 Transport 与错误读取），54 项显式登记待办；整个 A2 尚未完成。引用数量以对应检出运行脚本为准；NAPI 尚未实现。新增绑定须对拍并真实编译，符号通过不等于页面功能完成。

## 会话重构的当前进度

共享错误类型位于 `core/include/ghosterm/types.h`；所有平台绑定仍经 `ghosterm.h`
聚合入口进入，公开子头不反向包含聚合头。`ctest public_headers` 验证独立编译和依赖无环。

`core/src/transport.c` 现为公共门面，只持有 backend、类型与 EOF 状态。
私有 `core/src/io/backend.h` 定义统一 I/O 操作表：POSIX PTY 的 fd、子进程、waiter、
输入尾队列位于 `io/pty_posix.c`；SSH shell 适配位于 `io/ssh_shell.c`，libssh2
会话和通道实现仍在 `ssh.c`。后端不依赖 surface 或平台 UI，宿主不访问私有头。

普通连接直接使用本地 PTY / SSH shell，不依赖后台进程或旧会话记录。

## 可选后台会话

后台保活按 [可选 tmux 计划](../reference/plan-optional-tmux.md) 实施。每个格子连接一个
tmux session，采用普通 PTY 客户端；tmux 内部的 window/pane 不映射到 Ghosterm 布局。
管理命令由共享 C 模块执行，交互字节保持 Transport 与 TerminalSurface 直连。
断线后由用户刷新列表并重新选择，不按旧名称自动创建或恢复，也不重放输入。

自有 sessiond、私有恢复协议、快照/replica、安装与随包 runtime 已退役。
共享管理模块与 macOS 原生列表通过普通 Transport 接入；不持久化 session ID、不启动恢复轮询。实际支持范围见 [能力矩阵](../reference/capability-matrix.md)，
切片与验收见 [实施清单](../reference/optional-tmux-implementation.md)。

## 终端引擎分档（哪些端不能用 full libghostty）

`libghostty` 的渲染后端只有 `metal` / `opengl` / `webgl`，其中 `opengl` 要求
**OpenGL 4.3 core profile**（EGL + `#version 430 core` shader）。**Android 只有
OpenGL ES**，故 full libghostty 在 Android/HarmonyOS **没有可用渲染后端**——
这就是 Chuchu / libghostty-ohos 都走 `libghostty-vt + 自绘` 的原因。

Web 端同理：钉住的 libghostty 中 `renderer/WebGL.zig` 只是空 stub
（`pub const WebGL = @This();`），wasm 产物只有
libghostty-vt；[ghostty-web](https://github.com/coder/ghostty-web) 走的也正是
`libghostty-vt`(wasm) + Canvas 2D 自绘。长期目标仍是 full libghostty
（WebGL, wasm）——等上游补上该后端再评估切换。

| 平台 | 引擎 |
|---|---|
| macOS / iOS | full libghostty（Metal） |
| Linux | full libghostty（OpenGL 4.3 core） |
| Web | libghostty-vt + 自绘（wasm；目标 full libghostty，待上游 WebGL 后端） |
| Android / HarmonyOS | libghostty-vt + 自绘 |
| Windows | 待验证（OpenGL on Windows 需 WGL/ANGLE） |

## 当前宿主状态与实现入口

macOS 已由 `TabWindowController` 单窗口管理顶部/侧边标签；`WorkspaceLayout` 是终端与文件窗格唯一可写布局，`WorkspaceHostView` 保持宿主身份。旧多窗口标签组、独立双栏文件组件已退役。文件/配置等控制面经平台模型与绑定进入 Core，不经过终端字节通路。

平台接线须独立记账：macOS full 引擎的系统剪贴板回调仍占位，`sendText` 的编码能力不包含系统读取、选区复制或菜单响应。配置事务、主机表单、私钥存储和传输异常也仍有缺口，详见[macOS 当前实现与限制](../reference/capability-matrix.md#macos-当前实现与限制)。Android 的原生实现不能作为这些 Apple 路径已完成的证据。

Android 与 `terminal/android` 已有 A0 Gradle/JNI/VT 交叉构建和 API 36 arm64 模拟器装载证据，A1 主机/配置、来源/改名、导入预览与错误恢复已完成当前模拟器档验收，A2 首片已接 `/system/bin/sh` → 原生 owner → VT → Canvas；JNI 读取网格快照，不搬运终端输出字节。`SessionsViewModel` 持有会话，页面返回保留 owner；本地首片证据见实现计划 §9.13，已提交文本操作与验证见 §9.14，已交付的选区边缘滚动/Ctrl 生命周期验证见 §9.15；真实中文 IME/完整键盘矩阵/性能待补，整个 A2 未完成，SSH UI 尚未接入；实现以[最终原型](../design/android-prototype.html)和[详细计划](../reference/android-implementation.md)为基准。iOS 仍无可用宿主/引擎产物，不能把 macOS SwiftPM 包称为已跨 iOS。

Android 文本操作继续沿 TerminalSurface 接缝：带序号的控制请求由 native owner 执行，选区交给 VT 跟踪，复制结果按需返回；系统剪贴板由平台视图读取/写入，文件待粘贴清单保持独立。粘贴确认保留请求时的文本快照，清屏不向 transport 写入命令。已提交文本切片的验收见 Android 实现计划 §9.14，后续原子滚动/选区端点与 Ctrl 验证进度见 §9.15，不能以接口存在代替完整输入法/性能验收。

## 参考实现（技术对照）

| 项目 | 平台 | 许可 | 借用点 |
|---|---|---|---|
| [Termini](https://github.com/hudsonkit/Termini) | iOS/macOS | MIT | SwiftUI 终端组件 + SSH transport 缝 |
| [iGhostVT](https://github.com/owngoal-dev/iGhostVT) | iOS/macOS | MIT | 会话/传输分离、多窗口、保活（iOS 侧需自定义固件，非 App Store） |
| [libghostty-spm](https://github.com/Lakr233/libghostty-spm) | Apple（iOS/macOS/visionOS） | MIT | GhosttyKit/GhosttyTerminal SwiftPM 封装；host-managed IO（inMemory）先例 |
| [VVTerm](https://github.com/vivy-company/vvterm) | iOS/macOS | GPL-3.0 | libssh2 生产用法（已上架） |
| [Chuchu](https://github.com/jossephus/chuchu) | Android | MIT | Kotlin + libghostty-vt + libssh2 + 本地 PTY（Zig JNI） |
| [libghostty-ohos](https://github.com/wiedymi/libghostty-ohos) | HarmonyOS | MIT | XComponent + libghostty-vt |
| [Ghostling](https://github.com/ghostty-org/ghostling) | macOS/Linux | MIT | libghostty(-vt) C API 最小用法（Raylib 2D 渲染，单文件 C） |
