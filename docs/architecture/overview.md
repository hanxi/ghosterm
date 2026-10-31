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
   当前 Swift 与 Android JNI 引用已有对拍；已验通道切片 C ABI 为 81 项、Apple 79/2；受控 SFTP 切片为 82 项、Apple 80/2、JNI 40/42（§9.47）；受控浏览切片为 83 项、Apple 81/2、JNI 40/43（§9.49，当时文件产品待接）；§9.54 为 86 项、Apple 84/2、JNI 46/40；§9.56 为 87 项、Apple 85/2、JNI 47/40；§9.57 发布接口为 88 项、Apple 86/2、JNI 48/40；§9.65 受控目录创建为 89 项、Apple 87/2、JNI 49/40，Android 远端只读产品已接，独占暂存创建尚未接复制产品；SSH 后端切片 JNI 为 32/42，身份容器切片为 33/41，IdentityFile 来源查询切片为 34/41，主机信任快照切片为 38/41，事务/移除绑定为 39/41，通道取消入口替换旧引用后为 39/42（产品首片见 Android 计划 §9.27，来源查询见 §9.34，信任快照与绑定见 §9.36，写入事务见 §9.37）；整个 A2/A3 尚未完成。引用数量以对应检出运行脚本为准；NAPI 尚未实现。新增绑定须对拍并真实编译，符号通过不等于页面功能完成。

## 会话重构的当前进度

共享错误类型位于 `core/include/ghosterm/types.h`；所有平台绑定仍经 `ghosterm.h`
聚合入口进入，公开子头不反向包含聚合头。`ctest public_headers` 验证独立编译和依赖无环。

`core/src/transport.c` 现为公共门面，只持有 backend、类型与 EOF 状态。
私有 `core/src/io/backend.h` 定义统一 I/O 操作表：POSIX PTY 的 fd、子进程、waiter、
输入尾队列位于 `io/pty_posix.c`；SSH shell 适配位于 `io/ssh_shell.c`，libssh2
会话和通道实现仍在 `ssh.c`。后端不依赖 surface 或平台 UI，宿主不访问私有头。

普通连接直接使用本地 PTY / SSH shell，不依赖后台进程或旧会话记录。

POSIX PTY 在子进程 exec 前清空继承的信号屏蔽集合，调用线程的集合保持不变。仅更新 `TIOCSWINSZ` 不足以保证下游 ssh/tmux 重排：它们还须收到 SIGWINCH。`4924a2d` 已修复这条通知链，真实 macOS 窗口到本地 tmux、以及本地 shell→系统 ssh→远端 tmux 的无输入放大/缩小证据见[实施记录](../reference/optional-tmux-implementation.md#后续-pty-尺寸通知修复2026-10-30)。

Android 连接编排已由 native owner 顺序推进候选地址：只在 TCP 阶段换地址，共用 DNS 后剩余总预算，协商开始后不因认证/信任失败换服务器。Core 认证器仍消费单个已解析地址，主机验证使用原始 hostname；TerminalSurface 不感知候选或重试。macOS DNS 仅在所选族缺记录时换族、首个有效答案即完成，TCP 备用地址推进仍待实现。两端分别记账，见 [Apple 网络说明](../../apple/README.md#ssh-地址选择与取消边界)与 [Android §9.40](../reference/android-implementation.md#940-ssh-候选地址与-tcp-取消2026-10-30)。

认证完成和终端可输入是两个阶段。Android 已通过 `gt_transport_open_ssh_shell_controlled` 将原连接余额和取消信号交给通道 owner；channel-open/PTY/shell 共享该预算，Surface 不感知认证或取消。Swift 同一接口已绑定并验真实 sshd，macOS 产品仍使用旧 `gt_transport_open_ssh_shell` 的独立 5000 ms 准入预算，其认证定时器成功后撤销，剩余预算/取消尚未传入打开阶段。桌面后续须接入原请求上下文，不能在 UI 线程直接关闭底层连接，见 [通道边界](../../apple/README.md#ssh-认证后的通道打开边界)及 [Android 实施契约](../reference/android-implementation.md#认证后打开-shell-的实施契约)。

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

控制面能力按“Core 契约 → 平台绑定 → 产品消费者 → 实际验收”分别记账：Swift 有受控文件绑定而桌面仍用旧 job，Android 已有匿名暂存而文件页尚无复制任务，均不能跨层推定完成。§9.57 已补远端非覆盖发布及两端绑定，原未绑定 C 草稿的状态已过时；发布由独占 owner 调用，失败保留结果而不是变成普通成功事件。§9.58 的 `LocalFilePublisher` 则经 Android 文件系统适配器提交应用私有目录文件，不新增 Core ABI；命名目标候选与匿名来源快照分开。§9.59 的 SafFilePublisher 经 ContentResolver 创建新文档并关闭后回读；副作用状态和 fd 租约由独立 Request 保持，不新增 Core ABI。§9.61 已接独立来源树扫描，§9.62 已接目标预检，§9.63 已接请求内冲突决策与逻辑执行清单，§9.64 已接本机/SAF 单目录创建；§9.65 已补受控远端单目录 Core/Swift/JNI/Kotlin 请求；跨远端关系、全局任务、任务认证产品接线和进程恢复仍待实现；§9.66 已接统一目录服务，§9.67 已接独立任务连接作用域，§9.69 已接数据层复制执行器。具体映射见 [共享文件接口与产品接线](../../apple/README.md#共享文件接口与产品接线)。

共享认证的内存私钥 `FILE` 错误分类已修复；Android 加密 OpenSSH ed25519 口令重试与密码产品流程通过完整 139 项设备回归。公共 ABI 与字节通路不变，macOS 另完成重新链接及认证专项回归；历史失败、设备与格式边界见[§9.31](../reference/android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。

macOS 已由 `TabWindowController` 单窗口管理顶部/侧边标签；`WorkspaceLayout` 是终端与文件窗格唯一可写布局，`WorkspaceHostView` 保持宿主身份。旧多窗口标签组、独立双栏文件组件已退役。文件/配置等控制面经平台模型与绑定进入 Core，不经过终端字节通路。

平台接线须独立记账：macOS `TerminalHostView` 的标准 `copy:` / `paste:` 已经连接真实选区、系统剪贴板和模式感知粘贴；full 引擎剪贴板回调仍占位，标准菜单路径也未提供粘贴确认。配置事务、主机表单、私钥存储和传输异常也仍有缺口，详见[macOS 当前实现与限制](../reference/capability-matrix.md#macos-当前实现与限制)。Android 的原生实现不能作为这些 Apple 路径已完成的证据。

文件控制面应把网络工作和队列等待放在异步 owner，UI 只接收状态。异步读取还须绑定请求身份：macOS 当前目录导航没有代次，串行执行仍会发布已经被新意图替代的中间结果；历史栈应随成功导航提交，目录树缓存须在刷新时失效。实际缺口见 [Apple 导航边界](../../apple/README.md#目录导航与缓存边界)。macOS 当前 `SFTPFileBackend` 的 `queue.sync` 保证串行，却没有保证调用方不阻塞；`TransferManager` 在 MainActor 同步调用它，属于待修的响应性缺口。搜索 token 目前只防止旧结果覆盖页面，不等于后端任务取消；文件树把加载失败缓存为空、本地搜索和递归权限忽略遍历异常，也是控制面的待修缺口；详情与修复边界见 [Apple 文件查询与线程边界](../../apple/README.md#文件查询与线程边界)。

SFTP 的单 worker 保证的是每个 `gt_sftp_t` 内 job 的 FIFO，事件泵统一消费不等于全局限并发。§9.53 已修复共享下载打开顺序、上传截断和关闭结果；§9.54 新增专用 owner 的 fd→远端独占暂存创建，§9.57 已补远端非覆盖发布接口，但桌面产品提交事务与负深度遍历硬护栏仍须补齐；不能把 job OK 或百分比作为原子提交与完整扫描的保证。macOS 与 Android 四方向复制共用这些前置，见 [Apple 传输边界](../../apple/README.md#传输提交调度与重试边界)。

Android §9.55 的匿名来源快照是平台数据层设施：本机/SAF 流先写入 O_TMPFILE，再封成只读租约；它不属于终端字节流，也没有新增共享 C ABI。§9.56 已补受控远端读取和 fd 释放通知，该切片专项及完整 260 项设备回归通过；§9.57/§9.58 后续补齐远端非覆盖发布与应用私有目录常规文件提交。§9.59 已补 SAF 新文档写入；§9.60 的不可变 FileClipboardSnapshot 由 Activity 的 FilesViewModel 持有，已接复制清单 UI，只保存元数据。§9.61 的 FilePasteRequest 固定源/目标意图，FileTreeScanner 经注入的目录读取函数枚举并复核来源元数据，不持有连接或写目标。§9.62 已接平台目标读取、关系检查与冲突元数据；不同远端命名空间关系仍未知。§9.63 已接纯 Kotlin 的请求内冲突决策和只读执行清单，不持有连接或预留名称。§9.64 已接本机/SAF 单目录创建；§9.65 已补受控远端单目录请求；任务 store、冲突页面、任务认证产品接线与粘贴入口仍未实现；§9.66 已接统一目录服务，§9.67 已接独立任务连接作用域，§9.69 已接复制执行器。macOS 旧下载 job 的 FSTAT 仅供进度，EOF 成功不校验完整源长度；关闭结果修复不等于源一致性已验证，见 [下载边界](../../apple/README.md#旧下载的完整性边界)。两端任务实现须分别确认取消完成和 fd/连接实际释放，不能把协程返回或 UI 终态作为资源已经回收的凭据。

Android 与 `terminal/android` 已有 A0 Gradle/JNI/VT 交叉构建和 API 36 arm64 模拟器装载证据，A1 主机/配置、来源/改名、导入预览与错误恢复已完成当前模拟器档验收，A2 首片已接 `/system/bin/sh` → 原生 owner → VT → Canvas；JNI 读取网格快照，不搬运终端输出字节。`SessionsViewModel` 持有会话，页面返回保留 owner；本地首片证据见实现计划 §9.13，已提交文本操作与验证见 §9.14，已交付的选区边缘滚动/Ctrl 生命周期验证见 §9.15；已验 Ctrl 标点、Gboard 回车及真实拼音基本路径（§9.16）；按键事件经私有 JNI 送 native owner，由 VT 处理重复/释放，失焦与换 surface 前清理按键归属（§9.17）。修饰键/数字键盘/组合音符切片已验（§9.18）；§9.21 已接输入后短时快照调度并取得模拟器 debug 性能基线，输出通路不变；真机/release/长负载与完整输入矩阵待补，整个 A2 未完成，SSH 产品首片已通过完整 130 项设备回归（§9.27）；实现以[最终原型](../design/android-prototype.html)和[详细计划](../reference/android-implementation.md)为基准。iOS 仍无可用宿主/引擎产物，不能把 macOS SwiftPM 包称为已跨 iOS。

Android 文本操作继续沿 TerminalSurface 接缝：带序号的控制请求由 native owner 执行，选区交给 VT 跟踪，复制结果按需返回；系统剪贴板由平台视图读取/写入，文件待粘贴清单保持独立。粘贴确认保留请求时的文本快照，清屏不向 transport 写入命令。已提交文本切片的验收见 Android 实现计划 §9.14，后续原子滚动/选区端点与 Ctrl 验证进度见 §9.15，不能以接口存在代替完整输入法/性能验收。

共享认证的主机信任已保留 known_hosts 标记：撤销优先拒绝，读取失败与未支持规则分别报错；Swift 已消费新增撤销状态，Android SSH native owner 与类型化认证桥已按 §9.25 接入，产品 DNS/页面首片见 §9.27。哈希主机名/证书验证未实现，TOFU 写盘错误已阻断认证（§9.23）；§9.37 已改为共享备份与原子替换事务，确认之后在锁内重新核对信任规则；共享连接配置检查已接两端，macOS 在 DNS 前拒绝未支持的配置，Android 首页显示拒绝来源，并在重新检查实际目标后连接；默认工厂、产品页面与多会话已通过完整回归（§9.27）。实现与各端证据见 [Android §9.22](../reference/android-implementation.md#922-a3-共享主机信任前置2026-10-30)，不将共享 API 的进展当成所有原生端已交付。

Android 身份设置已接通列表/详情、OpenSSH 公钥指纹、SAF 导入与确认移除；请求由独立 ViewModel 持有，IO 在后台执行，详情只保留公开信息。身份设置的八项定向和十组变异已有证据；后续主机关联切片完整 160 项设备回归通过，见[§9.35](../reference/android-implementation.md#935-主机身份关联与引用详情2026-10-30)。主机身份引用写入同一 SSH 配置保存事务，引用详情从共享来源查询派生；`ghosterm-identity:<UUID>` 是本安装容器标识，不是跨端可用密钥路径。认证只按配置解析候选，其他身份需用户明确选择。主机信任记录管理已按 §9.39 接入；A7 的 16 项偏好尚未完成。

Android 文件控制面已有 `FileBrowserSource` 与 `LocalFileBrowser`：本机 HOME 使用平台文件 API，SAF 保留系统文档 URI，经后台 IO 返回条目/预览。`FilesViewModel` 用代次隔离结果，文件页不持有或关闭终端连接；本地 fd 显式释放、SAF 流取消和旋转后 PID 保持已验（实现计划 §9.43）。§9.51 另已接 `RemoteFileSession`/`files_jni.c` 的专用 SSH/SFTP owner、generation registry 与单请求结果；§9.52 的 `FileBrowserRepository` 汇合本机/SAF 与远端 source，页面经当前配置和共用认证建立独立文件连接；四方向复制与事件泵留在 A5/A6。

SFTP 受控打开与主机依赖前置已按 §9.47 验收：Core controlled opener、Swift 重载和 Android native helper 已存在，默认主机构建/SwiftPM 消费补丁 libssh2。17 项协议、十组接口变异、十一组依赖守卫及既有打包变异通过。macOS 产品仍用无参 `sftp()`，Android owner 已按 §9.51 消费受控接口；页面/repository 已按 §9.52 接线，预览缓存/外部打开待补，详见[主机依赖说明](../reference/host-ssh2-build.md)。

受控目录浏览（§9.49）已通过协议与变异：Core/Swift 将规范路径、全部条目和目录关闭合成一次结果，要求独占 SSH/SFTP 且不与传输 job 共用。该切片为 83 ABI、Apple 81/2、JNI 40/43；当时 JNI 登记待接，后续 owner 由 §9.51 接入；macOS 产品尚未消费新入口或元数据。任务 ID 在移交 worker 前保存，exec 的局部超时在解锁前恢复；两项不替代预览、会话关闭和文件提交契约。§9.50 新增单预算的 LSTAT/只读 OPEN/FSTAT/READ/CLOSE 预览接口，该切片为 84 ABI、Apple 82/2、JNI 40/44；§9.51 新增 owner-only abort 后为 85 ABI、Apple 83/2、JNI 45/40；返回有界原始字节，Android 平台已接解码、页面代次和只读预览；缓存/外部打开待补，macOS 产品未消费。详见[路线图](../reference/roadmap.md#macos-下一步与共享缺口)。

### 任务资源与显示状态的边界

Android 当前 `RemoteFileSession.close()` 仅撤销句柄并通知停止；fd 完成管道保护快照租约，目录请求完成管道保护该次操作，两者不代替通用的连接关闭等待。§9.67 新增 closeAndAwait，每连接通过最后 native 引用释放后的 EOF 通知；FileTaskConnections 独立取得源/目标并在不可取消清理中等各自结束，再释放连接名额，§9.69 的数据层执行器已消费该作用域，应用级任务持有与认证页面仍待接，详见 [实施顺序](../reference/android-implementation.md#617-独立任务连接与执行器实施顺序)。

macOS 当前 `TransferManager.jobs` 同时承担显示列表和字节汇总来源，清除终态子行会影响活动父任务的后续汇总；目录暂停也尚未覆盖后台扫描/创建。任务结果账目、资源生命周期和可清除历史须分别建模，不能把正常路由的 UI 状态当作底层已暂停/释放的证据；实际限制见 [Apple 说明](../../apple/README.md#目录暂停与汇总边界)。

Android §9.69 已在 data 层组合连接、快照和发布操作，在 domain 层保留逐项结果并通过本片验收；只有测试调用，尚未接应用级任务持有者。应用页面仍按 [§6.18](../reference/android-implementation.md#618-复制执行器结果与页面接入契约)实施。macOS `DirectoryCompletion` 另有混合取消/跳过依赖回调顺序的问题；两端任务模型及验收各自独立，不能从 Android 数据层验收推定桌面已迁移。

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

Android SSH 与本地终端共用 `session_jni.c` owner，SSH 认证完成后仍由同一线程持有 Transport/VT；状态快照和一次性响应属于控制面。`RemoteSession` 接受已解析候选地址集合和原始 hostkey 身份；DNS、认证页面与 Keystore 容器在平台控制面实现，已由 `c53cfb8` 交付并通过 §9.27 的完整 130 项设备回归。SAF 私钥导入已接原请求归属、旋转保留与可取消只读流，并通过五项定向、七组变异及完整 135 项设备测试，见[§9.29](../reference/android-implementation.md#929-saf-私钥导入生命周期2026-10-30)。历史通道预算限制见 §9.25；剩余预算与取消已在[§9.41](../reference/android-implementation.md#941-ssh-通道打开预算与取消2026-10-30)完成当前模拟器档的接线与通道验收；macOS 产品仍使用旧入口，不能以 Swift 重载或 Android 设备结果代替桌面产品验收。

known_hosts 管理读侧使用共享 `gt_known_hosts_*` 不可变快照，两端仅转换值与所有权。`gt__kh_split_line` 同时服务既有认证枚举，快照的 `recognized` 仅是词法形态，不能替代撤销优先/能力拒绝等认证判定。管理写侧已由 `known_hosts_store.c` 将 TOFU 与 `gt_known_hosts_remove` 纳入同一进程内互斥、稳定文件锁、备份与原子替换；Swift/JNI 保留 status、committed 和 conflict。锁等待有界不等于整个文件系统操作有界；平台必须在后台调用，提交后核验失败不能报告“未改动”。只保证协作写者协调，不承诺任意外部写者或断电恢复。Android repository/指纹/页面已有 11 项定向、17 组变异与完整 175 项设备回归，macOS 没有管理页面；平台证据不能由共享绑定验收代替。实现与历史草稿边界见[§4.5](../reference/android-implementation.md#45-主机信任记录管理)、[§9.39](../reference/android-implementation.md#939-主机验证记录管理页面2026-10-30)。
