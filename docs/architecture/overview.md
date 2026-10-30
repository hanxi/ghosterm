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
   当前 Swift 与 Android JNI 引用已有对拍；公共 C ABI 当前 74 项，Apple 引用 72 项、2 项登记未用；SSH 后端切片 JNI 为 32/42，身份容器接入后当前为 33/41（产品首片与完整回归证据见 Android 计划 §9.27）；整个 A2/A3 尚未完成。引用数量以对应检出运行脚本为准；NAPI 尚未实现。新增绑定须对拍并真实编译，符号通过不等于页面功能完成。

## 会话重构的当前进度

共享错误类型位于 `core/include/ghosterm/types.h`；所有平台绑定仍经 `ghosterm.h`
聚合入口进入，公开子头不反向包含聚合头。`ctest public_headers` 验证独立编译和依赖无环。

`core/src/transport.c` 现为公共门面，只持有 backend、类型与 EOF 状态。
私有 `core/src/io/backend.h` 定义统一 I/O 操作表：POSIX PTY 的 fd、子进程、waiter、
输入尾队列位于 `io/pty_posix.c`；SSH shell 适配位于 `io/ssh_shell.c`，libssh2
会话和通道实现仍在 `ssh.c`。后端不依赖 surface 或平台 UI，宿主不访问私有头。

普通连接直接使用本地 PTY / SSH shell，不依赖后台进程或旧会话记录。

POSIX PTY 在子进程 exec 前清空继承的信号屏蔽集合，调用线程的集合保持不变。仅更新 `TIOCSWINSZ` 不足以保证下游 ssh/tmux 重排：它们还须收到 SIGWINCH。`4924a2d` 已修复这条通知链，真实 macOS 窗口到本地 tmux、以及本地 shell→系统 ssh→远端 tmux 的无输入放大/缩小证据见[实施记录](../reference/optional-tmux-implementation.md#后续-pty-尺寸通知修复2026-10-30)。

## 可选后台会话

Shell 提示符集成是独立的用户安装脚本：`scripts/shell-integration/mouse-recovery.*` 在实际 Bash/Zsh 的提示符钩子中输出鼠标模式关闭序列，远端需在远端加载。App、Transport 与 TerminalSurface 不在系统唤醒时强制重置，也不自动安装脚本；它不提供远端 cwd 或新的后台会话机制。边界与已测范围见[鼠标残留恢复](../how-to/mouse-report-recovery.md)。

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

共享认证的内存私钥 `FILE` 错误分类已修复；Android 加密 OpenSSH ed25519 口令重试与密码产品流程通过完整 139 项设备回归。公共 ABI 与字节通路不变，macOS 另完成重新链接及认证专项回归；历史失败、设备与格式边界见[§9.31](../reference/android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。

macOS 已由 `TabWindowController` 单窗口管理顶部/侧边标签；`WorkspaceLayout` 是终端与文件窗格唯一可写布局，`WorkspaceHostView` 保持宿主身份。旧多窗口标签组、独立双栏文件组件已退役。文件/配置等控制面经平台模型与绑定进入 Core，不经过终端字节通路。

平台接线须独立记账：macOS `TerminalHostView` 的标准 `copy:` / `paste:` 已经连接真实选区、系统剪贴板和模式感知粘贴；full 引擎剪贴板回调仍占位，标准菜单路径也未提供粘贴确认。配置事务、主机表单、私钥存储和传输异常也仍有缺口，详见[macOS 当前实现与限制](../reference/capability-matrix.md#macos-当前实现与限制)。Android 的原生实现不能作为这些 Apple 路径已完成的证据。

文件控制面应把网络工作和队列等待放在异步 owner，UI 只接收状态。macOS 当前 `SFTPFileBackend` 的 `queue.sync` 保证串行，却没有保证调用方不阻塞；`TransferManager` 在 MainActor 同步调用它，属于待修的响应性缺口。搜索 token 目前只防止旧结果覆盖页面，不等于后端任务取消；文件树把加载失败缓存为空、本地搜索和递归权限忽略遍历异常，也是控制面的待修缺口；详情与修复边界见 [Apple 文件查询与线程边界](../../apple/README.md#文件查询与线程边界)。

SFTP 的单 worker 保证的是每个 `gt_sftp_t` 内 job 的 FIFO，事件泵统一消费不等于全局限并发。当前共享下载提前打开目标、未检查关闭结果和负深度遍历无硬护栏仍须修复；不能把 job OK 或百分比作为原子提交与完整扫描的保证。macOS 与 Android 四方向复制共用这些前置，见 [Apple 传输边界](../../apple/README.md#传输提交调度与重试边界)。

Android 与 `terminal/android` 已有 A0 Gradle/JNI/VT 交叉构建和 API 36 arm64 模拟器装载证据，A1 主机/配置、来源/改名、导入预览与错误恢复已完成当前模拟器档验收，A2 首片已接 `/system/bin/sh` → 原生 owner → VT → Canvas；JNI 读取网格快照，不搬运终端输出字节。`SessionsViewModel` 持有会话，页面返回保留 owner；本地首片证据见实现计划 §9.13，已提交文本操作与验证见 §9.14，已交付的选区边缘滚动/Ctrl 生命周期验证见 §9.15；已验 Ctrl 标点、Gboard 回车及真实拼音基本路径（§9.16）；按键事件经私有 JNI 送 native owner，由 VT 处理重复/释放，失焦与换 surface 前清理按键归属（§9.17）。修饰键/数字键盘/组合音符切片已验（§9.18）；§9.21 已接输入后短时快照调度并取得模拟器 debug 性能基线，输出通路不变；真机/release/长负载与完整输入矩阵待补，整个 A2 未完成，SSH 产品首片已通过完整 130 项设备回归（§9.27）；实现以[最终原型](../design/android-prototype.html)和[详细计划](../reference/android-implementation.md)为基准。iOS 仍无可用宿主/引擎产物，不能把 macOS SwiftPM 包称为已跨 iOS。

Android 文本操作继续沿 TerminalSurface 接缝：带序号的控制请求由 native owner 执行，选区交给 VT 跟踪，复制结果按需返回；系统剪贴板由平台视图读取/写入，文件待粘贴清单保持独立。粘贴确认保留请求时的文本快照，清屏不向 transport 写入命令。已提交文本切片的验收见 Android 实现计划 §9.14，后续原子滚动/选区端点与 Ctrl 验证进度见 §9.15，不能以接口存在代替完整输入法/性能验收。

共享认证的主机信任已保留 known_hosts 标记：撤销优先拒绝，读取失败与未支持规则分别报错；Swift 已消费新增撤销状态，Android SSH native owner 与类型化认证桥已按 §9.25 接入，产品 DNS/页面首片见 §9.27。哈希主机名/证书验证未实现，TOFU 写盘错误已阻断认证（§9.23），追加提交完整性仍有缺口；共享连接配置检查已接两端，macOS 在 DNS 前拒绝未支持的配置，Android 首页显示拒绝来源，并在重新检查实际目标后连接；默认工厂、产品页面与多会话已通过完整回归（§9.27）。实现与各端证据见 [Android §9.22](../reference/android-implementation.md#922-a3-共享主机信任前置2026-10-30)，不将共享 API 的进展当成所有原生端已交付。

Android 身份设置已接通列表/详情、OpenSSH 公钥指纹、SAF 导入与确认移除；请求由独立 ViewModel 持有，IO 在后台执行，详情只保留公开信息。八项定向、十组变异和完整 147 项设备回归通过；主机引用/信任记录管理与 A7 偏好尚未完成，见[§9.33](../reference/android-implementation.md#933-ssh-身份设置与公钥信息2026-10-30)。

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

Android SSH 与本地终端共用 `session_jni.c` owner，SSH 认证完成后仍由同一线程持有 Transport/VT；状态快照和一次性响应属于控制面。`RemoteSession` 接受已解析地址和原始 hostkey 身份；DNS、认证页面与 Keystore 容器在平台控制面实现，已由 `c53cfb8` 交付并通过 §9.27 的完整 130 项设备回归。SAF 私钥导入已接原请求归属、旋转保留与可取消只读流，并通过五项定向、七组变异及完整 135 项设备测试，见[§9.29](../reference/android-implementation.md#929-saf-私钥导入生命周期2026-10-30)。通道打开的 5 秒预算/取消边界见[§9.25](../reference/android-implementation.md#925-android-ssh-认证与远端终端后端2026-10-30)。
