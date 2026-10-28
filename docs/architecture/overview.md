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

这次拆分保持既有行为：当前 close 仍结束 backend，会话尚未独立于 GUI。
私有 session/attachment/worker/manager 已有基础与恢复原型，生产 daemon、
SSH 断网恢复与产品接线仍未交付。目标架构与未通过的 P0 条件见
[会话重构计划](../reference/plan-session-daemon.md) 和 [会话运行时 ADR](session-runtime.md)。

## 可选后台会话方向（2026-10-28，实施中）

默认本地/SSH 连接已回接普通 PTY/SSH shell，不读取旧会话记录、不探测 tmux、不安装 helper。
后台会话按 [可选 tmux 计划](../reference/plan-optional-tmux.md) 实施，连接由用户从刷新后的
列表选择，每个格子承载普通 tmux attach；不使用 control mode。共享模块负责 tmux 管理，
TerminalSurface 保持后端无关，终端输出直连 I/O owner。

以下自有 sessiond 方向已被替代，其组件、构建、打包和文档正按依赖闭包退役，不能当作
后续交付要求。进度见 [实施清单](../reference/optional-tmux-implementation.md)。

各平台原生客户端经现有 SSH 端口，以不申请 PTY 的 exec channel 承载会话协议；
远端短生命周期连接代理接入当前用户的 sessiond。sessiond 独立持有原 PTY、程序
和无 UI 权威状态，断网只撤销 attachment；新 SSH 连接认证后接回原 session。
自有 sessiond，不直接依赖 tmux，不新增公网端口。普通 SSH 保持独立、无持久承诺。
本地 daemon 同样必须交付，共用同一 sessiond/runtime、协议处理器和管理/恢复逻辑，
本地直连私有端点，远端仅增加 SSH 代理。[共用帧封装](../reference/session-ipc.md)
已有有界流式解析/续送实现；[Unix 端点基础](../reference/session-endpoints.md)
另有两把锁、真实监听/连接与内核 UID 证据；共用 connection/socket-stream 已
组合帧往返、双缓冲、整帧超时/取消与背压读取。服务循环、协议身份/实际 SSH
通道与完整会话接线尚未交付。

客户端与会话宿主分别验收：首条链路为 macOS 客户端 → Linux sessiond，再以
第二种原生客户端验证跨平台；本机 helper 的产品收尾和 Windows ConPTY 都不是
连接 Linux 的前置条件，Windows 客户端渲染/平台适配仍待完成。移动端不依赖本机
后台保活，Web 网关另行设计。部署、身份、断线租约、升级限制与产品先例见
[远端会话架构](remote-session.md)，实施顺序见会话重构计划。

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
