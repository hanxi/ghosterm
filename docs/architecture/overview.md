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
│   Android/HarmonyOS: libghostty-vt + 自绘               │
├────────────────────────────────────────────────────────┤
│ Transport —— 字节流（本地 PTY / SSH 同一接口）           │
├────────────────────────────────────────────────────────┤
│ Core（C 共享库）—— ssh / sftp / ssh_config / known_hosts │
│   / 端口转发 / POSIX+ConPTY PTY                          │
└────────────────────────────────────────────────────────┘
```

## 三个关键决策

1. **TerminalSurface 与 Transport 解耦**：`transport` 只管字节进出，`surface` 只管
   把字节变成屏幕。于是「SSH vs 本地 shell」「Metal vs 自绘」互不污染。
2. **字节流不过事件队列**：终端输出由会话线程 `transport_read → surface` 直连；
   事件队列只承载低频状态。这是性能红线。
3. **C ABI 是唯一跨语言边界**：`core/include/ghosterm.h`。opaque handle + POD，
   Swift module map / JNI / NAPI 三端绑定都从这里生成式对拍。

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
可选 tmux 的管理模块和 UI 尚在实施；实际支持范围见 [能力矩阵](../reference/capability-matrix.md)，
切片与验收见 [实施清单](../reference/optional-tmux-implementation.md)。

## 终端引擎分档（哪些端不能用 full libghostty）

`libghostty` 的渲染后端只有 `metal` / `opengl` / `webgl`，其中 `opengl` 要求
**OpenGL 4.3 core profile**（EGL + `#version 430 core` shader）。**Android 只有
OpenGL ES**，故 full libghostty 在 Android/HarmonyOS **没有可用渲染后端**——
这就是 Chuchu / libghostty-ohos 都走 `libghostty-vt + 自绘` 的原因。

Web 端同理：钉住的 libghostty 中 `renderer/WebGL.zig` 只是空 stub
（`pub const WebGL = @This();`，上游 main 已无该文件），wasm 产物只有
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

## 参考实现（均为可复用蓝本）

| 项目 | 平台 | 许可 | 借用点 |
|---|---|---|---|
| [Termini](https://github.com/hudsonkit/Termini) | iOS/macOS | MIT | SwiftUI 终端组件 + SSH transport 缝 |
| [iGhostVT](https://github.com/owngoal-dev/iGhostVT) | iOS/macOS | MIT | 会话/传输分离、多窗口、保活（iOS 侧需自定义固件，非 App Store） |
| [libghostty-spm](https://github.com/Lakr233/libghostty-spm) | Apple（iOS/macOS/visionOS） | MIT | GhosttyKit/GhosttyTerminal SwiftPM 封装；host-managed IO（inMemory）先例 |
| [VVTerm](https://github.com/vivy-company/vvterm) | iOS/macOS | GPL-3.0 | libssh2 生产用法（已上架） |
| [Chuchu](https://github.com/jossephus/chuchu) | Android | MIT | Kotlin + libghostty-vt + libssh2 + 本地 PTY（Zig JNI） |
| [libghostty-ohos](https://github.com/wiedymi/libghostty-ohos) | HarmonyOS | MIT | XComponent + libghostty-vt |
| [Ghostling](https://github.com/ghostty-org/ghostling) | macOS/Linux | MIT | libghostty(-vt) C API 最小用法（Raylib 2D 渲染，单文件 C） |
