# 会话 daemon 重构计划

状态：macOS 默认本地 daemon / SSH→Linux sessiond 已接线，真实断网与 GUI 强杀恢复通过；完整 P0 与其余平台仍未完成。日期：2026-10-21。

本文件同时保留架构、实施路线与历史原型记录；下面最新进展优先于旧日期中“未交付”的状态。P0–P8 不因某条产品链路通过而整体完成。未经运行验证的能力仍保持待验证。

## 2026-10-21 默认应用入口闭环

默认本地创建、SSH 创建与分屏共用 NativeSessionCoordinator / C runtime；侧栏
提供保留会话管理，应用启动仅恢复获确认的原实例/id/nonce。退出和断开只脱离，
显式关闭走独立管理连接并观察结果；未知停止不会自动重发。Source 低频上报
目录与前台作业状态；终端字节继续在 I/O 侧直送 Surface。

已运行：macOS 293 项应用回归（8 项专用环境跳过），六个独立本地 Source；
默认 Mac→Linux aarch64 原生与 x86_64 QEMU 的首次安装、实际 RootView、
同 PID/目录启动恢复和停止。aarch64 原生与 x86_64 QEMU 默认入口均通过 reset、静默黑洞与原会话
恢复。本地及 Linux aarch64 远端实际 GUI SIGKILL 后重启，以新随机标记命令的响应确认原 Source/PID/
instance/目录仍可交互，再显式停止。详见 [协调器](session-product-coordinator.md)、
[安装](session-native-setup.md)与[打包](session-runtime-package.md)。

首版继续使用严格不兼容的 GTSD/GTVC 二进制契约，不加 protobuf、旧后端适配
或原会话迁移。远端 Mac/Windows、x86_64 原生硬件、完整系统/GPU/
第三方资源预算与 P0 保持未完成；不从现有文本终端闭环推断它们已经完成。

## 2026-10-20 方向调整：远端会话与现有 SSH 端口

交付主线改为「跨平台原生客户端 → 现有 SSH 端口上的无 PTY exec channel →
远端连接代理 → 用户级 sessiond → PTY/程序」。远端 sessiond 独立于 SSH 连接
存活；断网后新建 SSH 连接，认证后接回原 session。自有 sessiond，不直接依赖
tmux，不要求新增公网端口或本机常驻 daemon。普通 SSH 与持久会话显式区分。

部署、身份、静默断网租约、输入不确定性、跨平台格式和产品先例统一见
[远端会话架构](../architecture/remote-session.md)。本节及下方目标/阶段表为当前
计划；末尾各日期的原型记录保留为历史证据，其中「本地优先、SSH 后迁移」的
排期已被本次调整替代。已有 macOS socketpair/SIGKILL 证据不等于远端断网恢复。
本次方向调整不代表新增生产恢复能力。随后补充：本地 daemon 与远端 sessiond
均为必交付，共用同一服务、runtime、协议与管理/恢复逻辑；两条入口仅传输不同。
共用封装的实现与边界见[会话协议](session-ipc.md)，尚无生产监听或 SSH 接线。

## 2026-10-21 实施路线：优先打通 macOS → Linux 全流程

本节记录当前执行顺序，优先级高于旧记录中的并行平台安排。首条产品链路是
**macOS 原生客户端 → 现有 SSH 端口 → Linux 用户级 sessiond**，从首次安装
一直到断网后接回原会话。SSH 连接 macOS/Windows 宿主延后，不阻塞 Linux
闭环；其他原生客户端随后接同一 C ABI。本地 daemon 仍是必交付，共用服务、
runtime、协议及恢复逻辑，不另做一套，不要求远程用户先启动本机 daemon。

已提交的起点为 `220dbf4`：[共用启动与 stdio 代理](session-activation.md)
已在 macOS/Linux arm64 验证。[专用 SSH 长连接候选](session-ssh-link.md)另已接共享客户端和真实 VT，
同平台真实 sshd 场景验证客户端 SIGKILL 后新建 SSH 认证并接回原 PID；这仍
不代替 macOS→Linux 的跨 OS 部署、自动安装、reset/静默丢包与原生产品入口。
原有 396 项测试不代表这些端到端能力已交付。

### 顺序与交付物

| 顺序 | 实现内容 | 完成判据 |
|---|---|---|
| 1. Linux 发布包 | 按 OS/CPU/libc 构建预编译 sessiond、代理及配套 VT 运行库；记录版本、引擎契约、最低运行库、文件 SHA256 和许可 | Linux arm64、x86_64 按实际 runner 分别构建/运行；缺少某组合就明确不支持，不能拿交叉编译成功当运行通过 |
| 2. 首次安装/更新 | macOS 经已认证 SSH 探测 Linux/CPU/libc/用户目录/执行权限，通过 SFTP 上传匹配包到私有暂存目录；逐文件核验，试运行后原子激活 | 用户无需 sudo、远端下载站或编译工具链；依赖缺失/目录不可写/noexec/校验失败给出具体错误；安装中断不破坏已有版本，可回滚 |
| 3. SSH 长连接 | C Transport 的单一非 UI owner 独占一个专用 libssh2 session；management/DATA 分别开无 PTY exec channel，运行可信绝对路径代理 | 两路双向字节流有界、公平、可取消；持续排空独立 stderr，区分暂不可读/EOF/错误/未知退出状态；不能复用一次性 exec 的读循环 |
| 4. 会话首次接通 | 共用 HELLO → CREATE/QUERY_CREATE → ATTACH → DATA HELLO/BIND → 快照/有序增量 → 实际引擎应用屏障 → LIVE | 原生 TerminalSurface 能显示并交互，字节不进 UI 事件队列；只有 LIVE 解锁输入，退出保留只读终态 |
| 5. 断网重连 | 失联后锁定输入并撤销旧连接；有界可取消退避，重新核验 hostkey/认证，严格查询原 instance/session，再取新 token/代次并恢复 | 原 PID/PTY/持续任务仍在；不重建 shell、不重发未知输入；旧租约 BUSY 有界等待，新旧回调不串代 |
| 6. 产品收口 | 接入 macOS 原生连接/安装进度、断连/重连/失败/会话列表/关闭与结束；公开 C ABI 和已有绑定同步，补使用文档与部署包 | 能从未安装的 Linux 走完首次连接、交互、断网、恢复、终止和再次连接；不依赖测试驱动或人工拼装私有组件 |

1/2 和 3 可独立推进；4 需要安装产物与长连接共同就绪；5 基于实际端到端
链路验证。P0/完整资源/副作用闸门仍然有效，不能为了先交网络链路而把 shell
保活或输出尾巴标成准确终端恢复。产品交付必须完成 1–6，并补齐所需引擎证据。

### 远端安装约定

客户端负责上传预编译包，Linux 不现场编译，也不默认执行 apt/yum 等包管理器。
可随包分发的配套用户态库放在版本目录并固定加载路径；系统 libc/动态加载器、
内核/PTY、sshd exec/SFTP 能力和发行包运行条件在激活前检查。不满足时明确
失败，不偷偷安装系统依赖、变更 sshd 或退回普通 SSH。受限账户无 exec、仅
SFTP 或无可执行目录时分别报告能力缺失。

默认使用用户私有安装根目录（例如 `~/.local/lib/ghosterm`），版本产物只读使用，
运行时 socket/锁放在独立私有 runtime 目录；安装目录不承载会话注册表。发布包
和暂存文件禁止其他用户写入，清单限制文件名/类型并拒绝路径穿越与非法链接。
上传完成后校验 SHA256、执行版本/能力探针，再通过原子切换发布新版本；失败
清理暂存，保留上一版本。不能仅凭 SSH 传输成功便标记安装成功。

发布清单分别标明二进制完整性 hash 与跨 OS/CPU 协议/引擎契约，二者不能混用。
运行中的 daemon/会话固定到实际版本及库路径。安装新版本不杀旧实例，不覆盖旧
文件；不兼容实例明确拒绝接入，由匹配版本客户端管理或用户显式结束后切换。
回滚也不得假装将活跃会话迁移到另一引擎版本。安全存储中的凭据只用于 SSH
认证，绝不上传私钥、口令或本地安全存储内容。

### SSH 与重连的所有权

文件浏览/SFTP 继续使用独立 SSH 连接，不能与终端 owner 跨线程共用 libssh2。
终端的管理/数据 channel 共用连接和一次认证，所有 libssh2 调用串行执行；
EAGAIN 的待续写参数与缓冲在重试期间保持有效。stdout 只进协议解析，stderr
有界排空、脱敏，不进终端状态。keepalive 辅助检测，协议双向心跳/租约负责
静默断网期限；不能靠 TCP EOF 或 keepalive 宣称已完成恢复。

重连保留经核验的主机身份、远端用户、daemon instance、session 和 CREATE nonce。
新 SSH 连接重新认证，UNKNOWN hostkey/密钥变化/认证拒绝/不兼容不能无限自动
重试。CREATE 确认丢失先 QUERY_CREATE；过期 UNKNOWN 不再 CREATE。原实例或
原 session 不存在时如实报告。每次 ATTACH 使用新 token/generation；恢复中
输入保持锁定，已发送但未确认的命令报告 UNKNOWN，绝不自动重放。

当前按最快的严格不兼容方案完成 Linux 闭环：不增加旧后端适配、版本协商或
兼容窗口，契约不匹配明确拒绝；安装新包不隐式杀掉旧 daemon/活跃会话。
[协议选型与版本兼容](session-ipc.md#后续议题协议与版本兼容暂不实施)仅记录为
后续讨论，不阻塞本轮交付。Protobuf/可扩展自定义编码、能力协商、管理协议与
快照的分层兼容以及独立升级策略，均等当前全流程跑通后再决定。

### 整套流程验收

- 首次空用户目录 → 上传安装 → 自动后台启动 → 原生终端交互；再次连接复用
  已安装兼容版本和同一用户实例。安装失败、中断、升级、回滚及旧会话保留同测。
- macOS 客户端真实连接 Linux sshd，不以本机 Unix socket 或代理 socketpair
  代替；Linux arm64/x86_64 各自注明已验证组合与缺项。
- TCP reset、无 FIN 的双向静默丢包、代理退出、客户端关闭/崩溃、恢复中再次
  断网、旧租约尚未到期均实测；恢复后核对原 PID/PTY、持续任务及新代输入。
- 断网期间持续输出、查询与任务执行；恢复后验证 vim/less、宽字符/半序列、
  resize/配色、历史与图像资源，确认查询和宿主副作用不重复。
- CREATE/输入确认丢失、hostkey 变化、认证取消、禁止 exec/SFTP、启动杂音、
  stderr 洪泛、慢链路和队列饱和都有可见结果，无静默降级或重复执行。
- 正常退出/取消/连续恢复后记录 fd、线程、内存回收及输入/恢复延迟；对应平台
  构建、核心/ABI 闸门及新增断言的定向变异全绿，才能标记完整交付。

SSH 连接 macOS/Windows 远端宿主、第二种原生客户端和其他宿主部署排在 Linux
这条完整链路之后。延后不等于删除本地 daemon 或放宽跨平台共用代码边界。

## 当前证据与未完成项（2026-10-21）

基于 `9331e42` 完整重跑进程原型，21 类原生场景、320 项变异及 26 类旧
surface 基线通过；历史淘汰、正值双限额与冻结/有序截断状态已加入同次验证。
原始配置/克隆/初始文件暂存及请求内环境副本已接入调用方预算域，展开失败显式传递；全局初始化现以独立 8 MiB 域接入 P0 GUI 同池准入并有序释放，全局 I/O 的双限额/原生栈/宿主入口帧另已接线，框架内部/其余系统与完整账本等仍有缺口，详见后续切片记录。
真实 PTY→VT 的 direct/worker 三轮吞吐、输入分位数、空闲 CPU 与 RSS 已另记录；worker 另已接入自有通知管道与 PTY owner poll，三轮输入 p50 为 0.027–0.043 ms（十二项定向变异），完整 GUI/IPC 性能仍未测。
P1-c 另有固定容量的私有多会话注册表：有序 join/context 回收、保留终态、不可复用 ID 已验（六类基线、十九项变异）；尚未接入生产 daemon/API。
共用 C [宿主 runtime](session-host-runtime.md) 已组合该注册表、创建 ledger、真实
PTY 与 session/attachment 引用；Mac/Linux arm64 九类基线及 25 组变异已验，
状态工厂仍用 C 模型，不替代生产 VT 组合或完整恢复。
管理 ATTACH 与 HELLO/BIND/DETACH 已组合 runtime 引用和单次 token；十二类
基线/41 组变异通过，该组数据处理器仅为观测器，不能当作生产恢复/输入授权。
USER 的 worker 执行许可、命令 ID 与三态结果另已实现私有 C/runtime 路径，
九类基线/34 组变异通过；共用 USER wire/有界异步结果邮箱另有十一类基线/
37 组变异。生产 VT/SSH/客户端接线仍未交付，详见后续记录。
终态独立 frozen 保存/查询/引用后释放已接私有 worker/runtime，状态工厂仍为
C 模型；不代表生产 VT 终态编码、传输或只读恢复已经完成。
共用 DATA 路由另已接捕获/编码、快照/S/L/增量/终态/心跳与错误收尾，真实
socketpair/PTY 仍使用 C 模型编码器和消费者；生产 VT/客户端/SSH 仍待接线。
`daemon/src/vt-state` 的[候选状态工厂](session-vt-state.md) 另已直接接共用 runtime，
真实无 UI VT/PTY 的查询应答、detach 后继续交互、独立冻结/非 owner 编码与
内联图像像素有七类 macOS 基线、十二项变异及 ASan/UBSan 证据。后续
[候选快照容器](session-checkpoint.md) 已接共用 DATA 与真实 VT 副本，合计
十四类基线/37 项变异与 sanitizer 检查；Linux 容器另有五类基线/十六项变异，
Linux arm64 的真实 VT/宿主进程组合另已原生验证，见[宿主进程契约](session-host-process.md)；
生产客户端接线仍未交付，不能据此完成 P0/P3/P4。
共用[attachment 客户端](session-attachment-client.md) 另已实现恢复/应用屏障、
输入锁定、结果三态和有界 deadline，并替换真实 VT 场景里的手写 DATA 接收。
Mac/Linux arm64 客户端十二类基线/四十五项变异通过。
[共用引导/管理客户端](session-client-bootstrap.md)另已接真实 VT 组合，
HELLO/CREATE/ATTACH/BIND 不再手写；十一类基线/35 项变异及完整核心 gate
345 项/57 符号在 Mac/Linux arm64 通过。生产 SSH 入口、原生 GUI 与公开 client ABI 尚未交付，不能将其当成 P5 完成。
[共用 POSIX 宿主](session-host-process.md)另已提供前台 executable、固定连接表和独立
reaper；Mac/Linux arm64 的 8 类基线/16 项变异及 core gate 348 项通过，客户端
SIGKILL 后恢复原 PID 已验。[后台启动与 stdio 代理](session-activation.md)另已接线；
真实 SSH owner/断网重连与生产 UI 仍待完成。
完整命令、环境与分项计数见下方本日集成记录。以下边界仍是验收约束：

| 项目 | 当前证据 | 尚未完成 |
|---|---|---|
| 权威状态与副本 | 钉版本快照/资源、S/L 序号、单一应答 owner、增量恢复的实验原型；含真实 PTY、Metal GUI、SIGKILL 与错误终态 | P0 全部行为逐项收口；实验引擎接口尚未进入生产构建 |
| 交互 | 当前原型验证编码输入、IME 及鼠标/焦点 API 跨进程传递、vim/less/tmux SIGKILL 后恢复及真实程序交互、AppKit 合成鼠标分发与 firstResponder 落点、合成文本按键和模拟 NSTextInputClient 回调、恢复期只读与终态拒绝；旧 surface 基线继续通过 | 系统输入法选字与生产 NSTextInputClient 接线、系统窗口焦点宿主路由、任意主题配置与通用绘制完成屏障仍需核验；固定实验配色档已接快照/增量并验背景像素，尚非产品设置接线 |
| 资源 | 已接入权威对象/快照/恢复队列/PTY/peer/线程容量与 GUI surface/runtime/元数据共享准入；主线程栈和完整 VM guard 已按虚拟容量预留，full surface 的 renderer/I/O 栈与 guard 另按 surface 准入，其入口帧已进宿主分配域并验证初始化失败回收，已注册备用信号缓冲也由入口帧拥有和计费；搜索线程与 App/配置副本/共享字体容器及两项预热线程另已接入宿主域和生命周期；原始配置/克隆/初始文件暂存另已接入同池独立配置事务 | 历史正值双限额与冻结/有序截断状态已在新增原生 P0 场景接线；生产配置/恢复界面、零历史策略仍未交付；其余元数据及未纳入的框架、线程/系统开销边界仍须收口；尚无生产全局准入和完整资源账本 |
| daemon 与产品 | P0 的私有 socketpair 原型能演示 GUI 断开及新进程恢复；共用端点/握手/管理服务/runtime/PTY 另有组件组合证据 | 远端 Linux sessiond、SSH exec 流/代理/租约、真断网恢复、生产可执行组合与 VT/data dispatcher、公开 API、各端接线、发现/恢复 UI 与用户安装仍未交付 |
| 平台与性能 | P0 为 macOS arm64；共用 C runtime/管理组件另有 Linux arm64 原生证据，保留原始日志 | 不代替 Linux/Windows 的完整 daemon/GUI/安装验收，也不代替最终吞吐/输入延迟/空闲 CPU 对比 |

P0 保持未通过；生产交付仍以本文 P0–P8 和末尾完成审计为准。

## 目标与已确定的约束

让各平台原生客户端通过 SSH 连接远端用户级 sessiond；客户端退出或网络断开不终止远端原 shell/PTY，重新认证后可发现并恢复同一会话。本机 daemon 是同一契约的部署选项，不是远程使用的前置依赖。各端继续通过 C ABI 使用共享能力，客户端与会话宿主分别验收。

- 使用 C11；不引入 Rust、Protobuf、通用 RPC 框架。
- 项目处于开发初期，允许直接修改公开 API、目录、绑定及测试，不保留旧 API 包装或旧协议实现。
- IPC 只支持当前协议版本；握手发现版本不一致必须明确失败，不能自动结束仍有会话的 daemon。
- 不提供跨版本会话迁移、旧 helper 保留、双版本路由或快照兼容读取。更新要求先显式结束会话；若发现版本不匹配，仅报错并给出人工清理入口，新客户端不向其发送控制消息，不为旧进程增加兼容代码。
- 第一交付链路是已有 macOS 客户端经真实 SSH 连接 Linux sessiond，再加入第二种原生客户端验证跨平台；各端适配、Linux/macOS/Windows 会话宿主分别取证，不将单端成功当作跨平台交付。
- SSH 协议流与断网恢复提前进入 P2–P5 主线；已有普通 SSH/SFTP/exec、配置编辑保持独立。P7 收口远端部署和故障矩阵，不再承担首次引入 SSH 的责任。
- 复用现有 SSH 端口，exec channel 不申请 PTY，代理 stdin/stdout 承载二进制协议；sessiond 只监听远端私有本地端点，不新增公网监听，不直接依赖 tmux。
- 不承诺 daemon 崩溃、注销或系统重启后的进程恢复。重启服务不能复活原 shell。
- 按需启动用户级 sessiond，不默认注册系统服务或登录启动，不要求管理员权限。服务器登录清理策略可能影响保活，显式用户级托管选项与限制须有原生证据。
- iOS/Android/HarmonyOS 可作为远端客户端，不依赖手机后台保活；本机会话仍按能力选择进程内模式或显式不支持。Web 网关另行设计，不能直接宣称支持原生 SSH TCP 路径。

## 重构起点与要解决的问题

以下是原计划起点；backend/公开头与私有 session 组件的已完成切片见当前证据和历史记录，生产所有权与远端恢复仍待接线。

1. 原 `core/src/transport.c` 混合 PTY/SSH 分派、缓冲、回收和销毁；现已抽取私有 I/O backend。后续 IPC/SSH attachment 不得把这些职责重新堆回公共门面。
2. `gt_transport_close()` 兼具句柄释放和底层会话结束语义。代理连接关闭与会话结束必须拆开。
3. `SessionController.swift` 同时持有 surface/transport、排队输入/resize、驱动读循环，并通过 deinit/stop 管理生命周期。GUI 不应再拥有持久 shell 的生死。
4. 现有 `drainPending()` 使用 `try?`，输入和 resize 错误被吞掉；新路径必须传播错误。
5. 现有部分状态查询绕过会话 owner，`running` 等字段存在多线程访问；需逐一审计，不能只添加一个 daemon 外壳。
6. Windows 后端尚未实现，POSIX 头文件和线程原语未隔离。写 C 不自动等于跨平台。
7. full libghostty 的状态目前随 GUI surface 存活。保存 PTY 输出尾部不能代替终端状态恢复。

## 目标分层和依赖方向

```text
各平台原生 UI → TerminalSurface / 平台接线
                         ↓
                 attachment transport（C）
                         ↓ SSH exec channel（现有端口，无 PTY）
                    远端连接代理
                         ↓ 私有本地 IPC
                 ghosterm-sessiond（远端当前用户）
                         ↓
                   session runtime
                    ↙          ↘
          原始 I/O backend     headless terminal state
          远端 PTY / ConPTY    terminal/ 内的无 UI 引擎适配
```

本机 sessiond 使用同一 runtime/协议，客户端直连本地 IPC；远端连接代理只转发、
按需启动和断连清理，不拥有持久会话。SSH 入口承载协议，远端 shell 的 PTY 归
sessiond；不能将原普通 SSH shell backend 包进本机 daemon 就宣称断网恢复。

依赖是单向的。core 不依赖 terminal 或各端宿主；daemon 是组合根，可以同时链接 core 与无 UI 的 terminal state 适配目标。不得让 daemon 为获得 VT 状态而依赖 AppKit、GTK、Metal 或窗口系统。

headless terminal state 是准确恢复和 detached 期间终端响应的目标方案，是否能与 full libghostty surface 正确衔接必须先通过 P0 验证。不是在 C core 中另写 VT 解析器。

### 控制面、数据面与引擎所有权

会话发现/创建/结束经平台会话控制器与 Transport 接线层调用公开 C ABI；SwiftUI 等视图只发用户意图，不持有 C 句柄。控制面返回低频状态，数据面保持 transport → surface 直连。该区分不授权 UI 绕过铁律 #1。

daemon 中的 terminal-state 是会话权威，GUI surface 是渲染和交互副本。两端均使用钉住的同源引擎，不创建第二套手写 VT 规则。具体责任如下：

| 行为 | 唯一负责方 | 副本约束 |
|---|---|---|
| 解析程序输出、维护模式及查询应答 | daemon terminal-state | GUI 可以更新网格，但不能向 backend 发送查询应答 |
| 键盘/IME、粘贴、鼠标编码 | LIVE 状态的 GUI surface | 模式同步完成才解锁输入；RESTORING 禁止输入，不缓存后重放 |
| backend resize | session worker | GUI 只提请求，权威状态和 backend 按同一顺序应用 |
| 标题、cwd、OSC 133/7501 的会话摘要 | daemon runtime | 恢复导入摘要，不重复执行历史回调；UI 只展示 |
| 剪贴板、通知、打开 URL 等宿主动作 | 当前 LIVE attachment 的平台接线 | detached 不积攒待执行动作，恢复不执行历史动作 |
| 配色/字体/焦点/可见性 | GUI 提交，按作用域处理 | 影响终端查询结果的值须同步权威状态；纯渲染值留在 surface |

现有 `0001-host-io-backend.patch` 的 `Host.queueWrite` 把用户编码输入与解析器应答合并到 `host_write`；`0002` 的 OSC 7501 查询也走该通道。因此不能在客户端通过识别转义字节来过滤应答：用户可能键入完全相同的字节。P0 必须验证在引擎产生处区分应答、用户输入及宿主动作，或在 GUI 副本的解析路径直接禁用应答。补丁须覆盖所有应答源和异步 mailbox，不能只屏蔽 DSR。

影响输入编码的模式与输出必须具有同一个同步边界。若 GUI 异步 mailbox 无法证明“已应用到序号 S”，必须补充引擎 barrier；只在网络层收到 S 不算模式已经应用。输入请求携带 attachment generation 与 GUI 已应用的输出序号，供 worker 校验归属和顺序；不能声称它能消除尚未到达 GUI 的并发模式切换。P0 要验证这些竞态与正常直连终端语义一致。

## 代码布局与构建目标

```text
core/
  CMakeLists.txt
  include/
    ghosterm.h                    唯一公开聚合入口
    ghosterm/
      types.h                     错误、ID、能力、公共 POD
      session.h                   创建/发现/结束会话
      transport.h                 attachment 字节流接口
      config.h / ssh.h / sftp.h
  src/
    base/                         错误、buffer、时钟、资源辅助
    config/                       现有配置能力
    ssh/                          现有 SSH 能力
    sftp/                         现有 SFTP 能力
    io/
      backend.h                   私有 backend 契约
      pty_posix.c
      pty_windows.c
      ssh_shell.c
    session/
      manager.c                   注册表、ID、保留期
      session.c                   状态机、所有权、输入执行
      attachment.c                单客户端附着生命周期
    transport/
      transport.c                 公开门面
      local.c                     进程内 attachment
      ipc.c                       本地 daemon attachment
      ssh_stream.c                SSH exec 流与远端 attachment
    ipc/
      protocol.h / codec.c        私有 wire 定义与编解码
      client.c                    管理连接
      endpoint.c                  命名、身份、连接启动
      ssh_proxy.c                 客户端 channel owner 与代理启动
    platform/
      platform.h                  小范围 OS 抽象
      posix/                      线程、唤醒、IPC、启动、时钟
      windows/                    对应 Win32 实现
  tests/
    unit/ integration/ fixtures/ fuzz/
daemon/
  CMakeLists.txt
  main.c                          参数、退出码、启动组合
  server.c                        管理连接、资源限额
  runtime.c                       session 与 terminal-state 接线
  proxy.c                         远端 SSH stdio ↔ 私有本地端点
terminal/
  session-state/                  无 UI 引擎适配与恢复契约
  apple/ android/ ...             既有 surface 适配
docs/
  architecture/session-runtime.md
  reference/session-ipc.md
  reference/plan-session-daemon.md
```

目录按职责分；只在实施相应模块时创建文件，不提交空后端或假成功实现。`daemon/` 作为顶层可执行产品，与 `core/` 共享库分开，新增目录边界要同步 AGENTS.md。

构建目标建议：`ghosterm_core` 对外静态库；`ghosterm_protocol`、`ghosterm_session_runtime` 等私有目标按实际依赖建立；`ghosterm_terminal_state` 无 UI 引擎目标；`ghosterm-sessiond` 可执行目标。不为每个文件建立一个库。

公开子头只包含 `types.h` 等必要依赖，不反向 include 聚合头，消除当前聚合头循环包含。所有平台绑定仍从 `ghosterm.h` 进入。内部头不安装、不被宿主引用。

session worker 是 backend 和 terminal-state 的共同串行执行者；core runtime 仅通过私有 C 函数表调用 state 的 feed/resize/capture/dispose，不包含 Ghostty 头、不链接 terminal 目标。daemon 组合根注入该适配；移动端由平台接线层注入进程内适配。函数表传 opaque context、有寿命约定的 bytes/POD 和状态码，不把 Zig/Swift 类型穿过边界。terminal-state 的查询应答回调只能向当前 worker 的输入队列提交，不在 feed 调用栈里递归进入 backend；宿主动作只产生低频请求，不同步等待 UI。

构建从顶层 CMake 组合 core、daemon 与 state 目标，同时保留 `cmake -S core` 的独立核心构建。基础 client/协议目标不得自动拉入引擎、PTY 或 GUI 依赖；runtime 才链接 backend，daemon 才组合 state。公共符号对拍继续覆盖完整 `ghosterm_core` 对外集合，私有静态目标不得无意暴露新的公共 ABI。

## 核心对象和所有权

| 对象 | 所有者 | 职责与销毁 |
|---|---|---|
| daemon instance | 会话所在主机的 OS 用户进程 | 持有 instance ID、私有 endpoint、manager；独立于 SSH 连接 |
| SSH/channel 与代理 | 客户端 Transport owner / 远端短生命周期代理 | 传协议、发现断线；销毁只 detach，不结束 session |
| session manager | daemon；移动端可进程内 | 创建/列举/终止会话，持有注册表 |
| session | manager | 持有 backend、worker、终端状态、退出信息 |
| backend | session | 持久远端会话独占该宿主的 PTY/ConPTY；普通 SSH shell 是独立非持久模式 |
| attachment | session | 连接到一个客户端，独立于 session 生存期 |
| transport handle | 客户端 owner | 释放时 detach，不能隐式 terminate |
| terminal surface | GUI | 渲染和交互，不拥有 shell 生命周期 |

首版每会话仅允许一个可交互 attachment；第二次附着返回 BUSY。暂不实现观察者、多写入者和控制权抢占，避免尺寸和终端响应来源不确定。

API 名称在 P1 定稿，至少表达以下独立操作：

```c
gt_session_create(...);
gt_session_list(...);
gt_session_attach(...);      /* 返回 gt_transport_t */
gt_session_terminate(...);
gt_session_forget(...);      /* 仅清理已退出记录 */
gt_transport_close(...);     /* 仅释放 attachment */
```

公开 API 统一状态码返回、输出参数、buffer 所有权及线程规则；避免当前“部分正错误码、部分负返回值、部分 bool”的混用。读取用显式结果区分 DATA/TIMEOUT/EOF/ERROR，退出原因区分正常退出、信号结束、未知；不能将未知退出码填成 0。

session ID 与 daemon instance ID 共同标识对象，daemon 重启后旧 ID 明确失效。跨语言对象仍为 opaque handle，数据只用 POD 与明确寿命的字符串/字节。

## 状态机与用户动作

会话主状态：`STARTING → RUNNING → DRAINING → EXITED → FORGOTTEN`，启动失败进入 FAILED。terminate 进入 STOPPING 后继续 drain，再到终态。attachment 有独立 CONNECTING/RESTORING/LIVE/DETACHED/FAILED 状态。

- 关闭窗口、GUI 退出或 IPC 断线：detach。
- 关闭标签/窗格：默认 detach 并保留可发现会话；“结束会话”是独立显式动作。
- 标签/窗格的 ✕ 与快捷键保持同一 detach 语义，菜单明确标为“关闭视图并保留会话”；另设“结束会话”。会话入口显示未附着的运行会话及数量，支持恢复和显式结束，避免产生无法发现的隐藏 shell。结束时仍沿用 IDLE/JOB/UNKNOWN 的确认规则，UNKNOWN 不当空闲。
- 结束会话：有界优雅停止，再按平台策略强制结束；明确进程组/进程树范围，不承诺回收主动脱离的后代进程。
- shell 退出：drain 所有输出，发送终态，保留可查退出信息；进程退出与 PTY EOF 不可混同，后代可能仍持有 slave。
- drain 设定时限与截断状态，避免后代永久持有 PTY 导致资源无法回收；截断必须可见。
- daemon 默认有活跃会话就常驻；无会话时空闲退出。已退出记录和状态有保留期与数量上限，默认仅内存保存。
- daemon shutdown 有会话时返回 BUSY；force 只能由明确请求触发。

GUI 失去连接显示“连接已断开，会话状态未知”，不能显示“shell 已退出”。重连后重新核验主机并认证、查询原 instance/session；不自动新建替代 shell，不重发未确认键入。静默丢包必须靠有界双向保活/租约撤销旧 attachment，不能只等 EOF；租约内第二次附着返回 BUSY，不抢占。deadline/退避与取消按远端架构验收。

## 线程模型与 I/O

第一版选择每 session 一个 worker，不提前实现复杂跨平台 reactor。每个 backend 的 read/write/resize/terminate 都由同一 owner 执行。既有 waiter 可以暂时保留，但只负责回收与唤醒；须有唯一 wait/reap 所有者。

- 管理线程仅操作 manager 注册表与提交命令，不直接操作 backend。
- 每个命令队列有容量、唤醒与停止机制；pending write 不得无限增长。
- 每次循环对输入、输出、控制请求设工作预算，持续输出不能饿死停止请求。
- resize 只能在没有越过输入/输出顺序边界时合并。
- 查询 child state/cwd 通过 owner 获取或带时间戳快照，不从 UI 任意线程读内部对象。
- 阻塞读取可取消；close 不依赖 UI 主线程同步等待不可控 join。
- 写入结果明确“已接受”与“已交给 backend”的差别；断线后不自动重发未确认键入，避免重复执行命令。

### 并发、取消与释放契约

- manager 锁仅保护注册表、引用取得和删除，不在持锁期间做 I/O、等待 worker 或调用引擎。引擎锁内不调用 manager 或同步请求 GUI；事件先复制必要 POD 再离开锁派发。
- manager 持有 session 强引用，attachment 与进行中的命令持有各自引用。FORGET 只在终态、worker 已退出且引用可安全释放时移除记录；任何 socket 回调不得借用已释放的 session 指针。
- 跨线程只允许提交有界命令、请求取消和读取受保护快照。handle 的普通操作仍归一个 owner；释放前 owner 取消读取、等待自身回调结束、撤销 surface 回调，再释放。GUI 主线程只发起异步关闭并观察完成，不承担 join。
- 关闭连接立即使 attachment generation 失效，撤销 token 并取消恢复任务。backend 已接受的输入不撤回；未接受输入丢弃并记录连接失败。已发布到 worker 的 terminate 不因调用者断线而撤销。
- 输入帧以整帧为入队单位：容量不足时接受 0 字节并返回 BACKPRESSURE；接受后即归 session 所有，由 worker 支持 backend 短写。后续 backend 失败单独报告已接受输入的投递失败，不把已执行前缀伪装成“整帧未接受”。
- 内存不足、引擎错误、编码失败均有显式终态或断连原因。禁止吞错误后继续声称恢复成功。

流量路径：backend → terminal-state → 有界 attachment 队列 → IPC → GUI 会话线程 → surface。字节流不进入 UI 事件队列。

慢客户端超限时断开 attachment 并报告原因，继续排空 PTY。输入超限返回 BACKPRESSURE。全局与会话限额统一配置，起始值可为单帧 1 MiB、单会话待发输出 4 MiB、待处理输入 1 MiB；这些是待压力测试校准的设计值，不是性能承诺。不能在消息拒绝后留下半条输入已被执行却报告“整条失败”的模糊状态。

限额覆盖连接数、会话数、线程数、未完成请求数、列表快照、恢复任务、单个/总图像资源、scrollback、引擎分配和快照临时副本；不能只计算 IPC 队列。单次快照的瞬时双份内存也纳入预留预算。启动与恢复先预留资源，失败原子回收；总预算不足拒绝新建/恢复，不能悄悄杀其他会话。

scrollback 按行数与字节双限额淘汰最旧历史，主/备用屏幕和有效模式不能因此被裁掉；会话摘要记录历史已截断。图像等资源只有在引擎能显式报失败且不破坏当前状态时才能独立拒绝，否则停止该会话并报告资源错误。权威状态失效后不可继续排空并假装仍可准确恢复；预算约束与引擎 allocator 的可实现性也属于 P0。实时输出无需从起点保存，恢复依赖权威快照加有界增量；增量超限取消该 attachment 的恢复，不能截掉中间输出后进入 LIVE。

## IPC 协议 v1

一条管理连接；每个 attachment 独立数据连接。远端模式每条连接对应独立 SSH exec channel/代理/本地端点连接，客户端同一 SSH owner 公平调度；本地模式直接使用 IPC。数据连接通过一次性、短时有效的 attachment token 绑定 instance/session/用户/generation；禁止任意传 session ID 就接管会话。数据代理不得自动启动替代 daemon。

固定 24 字节帧头：magic:u32、version:u16、type:u16、flags:u32、payload_length:u32、request_id:u64。多字节整数统一大端；显式读写字段，禁止映射 C struct。数据连接握手绑定会话后不在每帧重复 session ID。

- 控制请求：HELLO、CREATE、LIST、ATTACH、TERMINATE、FORGET、SHUTDOWN。
- 数据与恢复：INPUT、OUTPUT、RESIZE、RESTORE_BEGIN、RESTORE_CHUNK、RESTORE_END、LIVE_BEGIN、DETACH。
- 响应与事件：RESULT、STATUS、EXIT、ERROR；输出/恢复事件携带递增序号。
- 数据面另设 READY、LIVE_READY、INPUT_RESULT、RESIZE_RESULT、ACTION 与 ACTION_RESULT。INPUT/RESIZE 使用 attachment 内单调 command ID，不能因数据帧 request_id 为 0 而失去确认能力。INPUT_RESULT 的 ACCEPTED 仅表示整帧入队，不表示 shell 已执行；断线时未确认状态为 UNKNOWN，不自动重发。
- string/bytes 采用 u32 长度前缀，创建参数使用显式字段及有界数组；拒绝内嵌 NUL、数量越界、长度溢出和非法 UTF-8 文本。终端 bytes 不做 UTF-8 校验。
- 数据帧 request_id 为 0，管理请求使用非零 ID；RESULT 包含原请求 ID 和状态。
- 单版本精确匹配；保留版本字段用于明确拒绝，不实现多版本协商或兼容适配。
- 未知消息/flags、超大帧、非法状态迁移、半包超时按协议错误关闭连接。
- parser 必须支持任意切包、粘包和短读写；分配前验证长度，未认证连接也有限额和握手超时。
- 长列表分页、恢复分块，不能通过超大单帧绕过限额。

协议只在私有头维护，与公开 C ABI 独立。定义协议文档与固定编码样例；不引入 schema 编译器。

P2 固定每条消息的字段表、合法方向、状态、最大长度、错误码及是否需要回复；上述消息名不是完整 wire schema。序号与 command ID 使用有界 u64，达到上限显式停止对应流，不能回绕。所有 deadline 使用单调时钟。控制请求超时不等于服务端操作回滚：CREATE 在 registry 发布是提交点，客户端通过自己生成的 creation nonce 查询结果；nonce 只用于当前实例内去重，过期后返回结果未知，不盲目重试新建。TERMINATE 对同一会话重复调用返回其当前真实状态，FORGET 后返回 NOT_FOUND。

LIST 首次请求创建有期限、数量受限的元数据视图，后续 cursor 绑定该视图和连接；分页中失效的会话在 attach 时返回 NOT_FOUND。不能用活表数组下标分页导致并发增删时漏项或重复。STATUS 携带状态 revision，重连通过 LIST/查询重建，不依靠低频事件永不丢失的假设。

## 启动、身份与平台抽象

远端模式先经既有 SSH 主机验证/认证，以无 PTY 的 exec channel 启动用户安装位置
的连接代理；代理执行下述本地端点算法。私钥和口令留在客户端认证链路，不送入
sessiond。SSH 管理/数据流使用长期、双向、有界的 channel 接口，不能复用一次性
exec 的收集输出模型。远端安装、SSH channel 退出隔离、登录清理策略、平台矩阵
与版本不匹配操作边界见[远端会话架构](../architecture/remote-session.md)。

启动算法：连接 → 获取用户级启动锁 → 再连接 → 启动 helper → 等待握手就绪 → 释放锁。任何失败清理自身资源。检查旧 endpoint 时不能无条件 unlink，也不能根据 PID 文件存在与否判断存活。

helper 路径由可信安装位置确定，不从 PATH 搜索。Unix 首选启动新 executable，在新进程内建立独立 session；不在 GUI 多线程进程中 fork 后继续运行复杂逻辑。标准流显式重定向、无关句柄默认不继承；锁和 endpoint 生命周期由 daemon 持有。

| 平台 | IPC 与身份 | 生命周期适配 |
|---|---|---|
| macOS/Linux | 私有目录 Unix socket，目录 0700、socket 0600，验证 peer UID | 独立 helper、PTY、进程组、waitpid |
| Windows | named pipe，限定当前用户的 ACL，验证客户端身份与实例 | helper、ConPTY、handle 继承白名单、Job Object 策略 |

运行时目录由平台策略确定。遵守当前仓库“不依赖系统 /tmp”的约定；测试资源全部在工程 tmp/ 下。本机 macOS helper 在 .app 内打包并签名；远端代理/sessiond 使用对应 OS/架构的用户安装包，不依赖客户端 .app 或系统级安装。Windows 与 Linux 安装包分别携带对应架构 helper。开发路径、客户端打包路径与远端正式安装路径分别验证。

每次 CREATE 传入明确的 cwd、shell/direct、argv、初始尺寸和必要环境；禁止后来打开的会话偷偷继承首个 GUI 的过期 cwd/env。先保持当前登录 shell 语义；区分用户环境、SSH_AUTH_SOCK 等动态值与 daemon 自身运行环境。

Windows Job Object 必须实测 GUI 退出不误杀 daemon、terminate 不误伤其他会话；不直接假设 DETACHED_PROCESS 能解决进程树问题。

身份验证在管理连接和每条数据连接分别执行；token 使用 OS 安全随机源、单次消费、有期限，并绑定 instance/session、请求者身份和 attachment generation，不进日志。权限模型保护其他 OS 用户，不能宣称隔离同 UID 的任意恶意进程。Unix 路径逐级验证所有权和目录类型，拒绝 symlink 替换；socket 路径长度越界明确失败。只有持有启动锁且核实节点身份与不可连接状态后才能移除旧 endpoint，不能删除同名任意文件。Windows 同时验证服务端身份，不能只检查客户端。

显式指定桌面 daemon 模式与进程内模式，创建返回实际能力和 instance。移动端最终也使用同一 session/attachment 契约；受平台限制的后台生存期单独标注，不保留旧的“transport 直接拥有 shell”对象模型。Windows 后端通过窄平台抽象使用 Win32；C11+POSIX 现行规则的调整必须在引入该后端前同步 AGENTS.md 并解释原因，不能以计划覆盖现行规则。

## 终端状态恢复：P0 决策闸门

目标是让 daemon 中的无 UI 引擎维护权威状态，GUI attach 导入状态后消费增量。必须先检查钉住的 libghostty/libghostty-vt 接口，不能预设存在 snapshot/import API。

P0 必须回答：

1. 无 UI 引擎能否完整维护主/备用屏幕、scrollback、光标、模式、tab stop、字符集、标题、cwd 等恢复必需状态？图像、链接等资源如何处理？
2. 能否导出并导入 full libghostty surface？若需上游补丁，接口、版本、资源所有权和 WHY.md 如何组织？
3. detached 时谁回应 DSR/DA 等查询？attached 时如何禁止 GUI 与 daemon 重复回复？GUI 接收的是语义输入还是已编码终端字节，如何保持模式一致？
4. 能否在事件序号 S 上生成一致快照，排队 S 后的输出，RESTORE_END 后无缝 LIVE，且不阻塞 PTY 读取？
5. 恢复时如何禁止剪贴板、通知、URL 打开、查询应答等副作用；新客户端终端能力与旧会话约定是否一致？

验收覆盖 vim/less/tmux、备用屏幕、256/真彩、宽字符/组合字符、resize、粘贴/鼠标/焦点模式与终端查询。验证应对比终端状态与交互结果，不能只检查导出的文本。

### P0 原型和明确决策

已初始化并审计 Ghostty 精确 pin `81598385d608dda4946093d6d6a234d204a2f225`。该版本的 `include/ghostty/vt/snapshot.h` 已提供快照编码和解码 API，支持未完成 VT/UTF-8 continuation；`include/ghostty.h` 没有对应 full surface 导入入口。`terminal/session-state/probe.c` 的实际运行进一步确认图像资源未恢复，故 P0 仍未通过。详细证据与待补接口见 [会话运行时 ADR](../architecture/session-runtime.md)，原型与运行方式见 [探针说明](../../terminal/session-state/README.md)。后续引擎原型改动仍在工程 `tmp/` 的隔离源码副本中进行；正式改动只提交补丁和 WHY.md。

交付 `docs/architecture/session-runtime.md` 的 ADR 章节、`terminal/session-state/` 的可重复原型及其运行说明，记录实际 pin/补丁哈希、依赖、命令、结果、内存/延迟数据和缺失项。验证顺序：

1. 真实 PTY 输出进入不链接窗口系统的 state 引擎，detached 时完成查询应答；用测试程序统计 DSR/DA/OSC 等每次查询恰有一份应答。
2. 在任意输入分块边界取快照，包括 CSI/OSC 未完成、UTF-8 半字符和进行中的多段资源传输。若不能序列化解析器状态，须证明可在有界时间得到安全边界，否则该路线失败；不能无限等程序补完序列。
3. 在真实 full libghostty surface 导入快照，比较网格、属性、主/备用屏幕、scrollback、模式、光标、解析器待续状态、资源及摘要，再输入下一批字节验证未来行为一致。
4. 将运行中的 vim/less/tmux detach 后继续输出、resize、查询，再创建新 surface 恢复。GUI SIGKILL、恢复中断、慢客户端、两次 attach 竞争均要覆盖。
5. 验证 live/replay/import 各模式下的应答和宿主动作隔离，以及中文 IME、粘贴、鼠标、焦点、配色查询的行为；比较结构化状态和交互记录，像素作为渲染证据补充。

通过条件是同一方案全部满足准确恢复、单一应答 owner、有界资源、无 UI 依赖及既定平台引擎分档。导出文本、ANSI 重画屏幕或局部字段相同不能单独通过。若需补丁，P0 必须给出能运行的最小 export/import、barrier 和副作用隔离接口，不能将三个关键能力推迟到 P4 才验证。

优先选择“权威无 UI 状态 + full surface 导入及受控增量”的路线。若不成立，ADR 列出引擎补丁的实际代价和其他路线；改成自绘、改成共享引擎状态的渲染协议或只承诺进程保活都改变已定范围/铁律，必须明确调整设计后再推进，不能作为隐藏降级。P0 未通过时 P1 仅做与恢复策略无关的 backend/生命周期拆分。

### 恢复事务与尺寸顺序

- worker 在事件边界 S 取得一致快照，快照固定会话尺寸、能力配置、解析器状态和终端语义状态；GUI 的选区、搜索、IME 组合串不属于会话，不跨 attach 继承。
- 快照必须不可变或 COW；序列化交给有界任务，不长时间占住 worker。捕获锁持有时间和 COW 额外内存必须测量。S 后的输出、尺寸、配色/能力变更按同一 session event sequence 排序并排队，不能只给 OUTPUT 编号。
- RESTORE_BEGIN 包含 instance、generation、S、快照格式、总长度上限和配置标识；CHUNK 检查偏移/总量，END 校验完整性。格式与当前引擎 revision/补丁集、快照/资源格式和能力契约精确匹配；标识跨 OS/CPU 定义，不能以平台二进制 SHA 相同为条件；不读取不兼容快照；禁止序列化指针、锁、fd 等进程资源。
- GUI 新建副本，禁用输入和宿主副作用，导入成功后 READY(S)。worker 开始顺序发送增量并指定 LIVE_BEGIN(L)；GUI 应用至 L 并跨过引擎 barrier 后发送 READY(L)，worker 返回 LIVE_READY(generation, L) 后才允许新的交互。L 后输出在同一有序流继续发送，双方不能在 READY 等待期间互相停止读写。S 与 L 相同时也用 attachment 状态区分两次 READY。
- 恢复先使用原尺寸，LIVE 后才提交当前窗口的 resize。worker 在事件序列中应用权威状态 resize、backend resize，并回传实际结果；平台尺寸变更不能与 PTY 外部写入形成事务，应记录实际观察顺序，不承诺操作系统原子性。失败中止该 attachment 并明确会话状态，不能让 GUI 单方面相信新尺寸已生效。
- detached 时保持最后成功尺寸，不自行改成默认 80×24。重新附着不得先 resize 后导入旧尺寸快照。快照超时、增量超限、校验失败或 GUI 断开仅撤销恢复和释放预算；会话继续在权威状态可用的前提下运行。

若快照路线不可行，先提交 ADR 给出真实限制与替代成本。完整事件回放只能作为明确受限方案：从起点保存所有相关事件、隔离副作用、保留尺寸/应答历史并验证。尾部原始输出不允许冒充恢复方案。不因 P0 未通过而发布“可准确重连”的声明。

第一版不将终端输出写磁盘；快照与历史在内存中受限保存。磁盘持久化、敏感输出保护和重启恢复需独立设计。

## SSH 传输与远端会话边界

远端持久会话由远端 sessiond 持有该主机的 PTY、shell 与权威终端状态。
客户端拥有 libssh2 session/channel，SSH 网络链路是 attachment 的传输载体，
断线后建立新连接接回原 session。此前「本机 daemon 持有 SSH shell，SSH 断线
即终态」只适用于普通 SSH 模式，不能作为本次持久会话的实现或退路。

SSH 从 P2 进入主线。channel 不申请 PTY；管理流与每条数据流各有一个 exec
代理，stdout 只放协议，stderr 独立、有界、脱敏。首版不依赖 subsystem 配置、
TCP 端口转发或额外公网端口。远端禁 exec、缺代理或不兼容都显式失败。

所有终端 channel 由客户端单个 SSH I/O owner 调度，读写/取消和保活公平、有界；
同一个 libssh2 session 不跨线程并发调用、不跨进程传裸指针或 socket。SFTP/exec
文件面板继续用已有独立 SSH 连接，不随终端 attachment 迁移或自动重放任务。
复用 SSH 端口是入口契约，不代表 SFTP 必须与终端复用同一 TCP 连接。

认证、hostkey 交互绑定连接 generation 和有效期；断连/取消使晚到回复失效。
私钥只经平台安全存储解锁并用于客户端认证，临时材料按既有契约擦除、禁日志。
不把认证秘密放入代理命令行、环境、协议快照或远端磁盘。远端只知道认证后的
OS 用户，通过私有端点身份和短期 token 管理会话，未知 hostkey 不自动接受。

断线判定、旧代租约、会话身份、CREATE nonce、UNKNOWN 输入结果与 S/L 恢复
事务统一见远端架构。恢复目标是同一原 shell/PTY；远端 sessiond 已死或记录不存在
必须明确报告，不能以重新开一个 SSH shell 冒充恢复。首版仍不承诺宿主重启恢复。

## 实施顺序与每阶段交付

| 阶段 | 内容 | 进入下一阶段的条件 |
|---|---|---|
| P0 | 恢复与查询响应原型、引擎边界 ADR、能力矩阵草案 | snapshot/import 和单一响应 owner 有可运行证据；不可行则明确调整产品范围 |
| P1 | 契约与目录重构：types、backend、session/attachment、平台边界 | 原有行为回归全绿；没有临时双 API；ABI/绑定同步 |
| P2 | codec、私有 endpoint/身份/启动锁、SSH exec 流/代理与租约、客户端平台接缝 | 协议/fuzz、真 sshd 双向流与认证取消、并发启动/静默断网撤销、失败清理通过 |
| P3 | 共用 sessiond/runtime：本地 IPC 与 Linux 远端代理、原生 PTY/状态机/背压/回收 | 两种入口的生命周期同测；SSH/代理退出与断网不杀会话，输入/resize/退出/无 display 验证 |
| P4 | terminal-state 与 SSH 恢复事务、barrier/副作用隔离 | 真断网后同一 PTY、TUI/查询/resize/持续输出恢复与不重复输入通过 |
| P5 | Apple 本地/远端会话发现与恢复 UI、本机 helper/远端用户安装、第二种原生客户端 | 本地 IPC 与 macOS → Linux 网络恢复两条产品链路；认证/版本/安装失败可见 |
| P6 | 其余原生客户端与 macOS/Windows 会话宿主扩展 | 客户端/宿主分开验收；Windows ConPTY/Job 和 renderer 各自取证，移动端不假定后台常驻 |
| P7 | 远端部署、网络/身份/平台故障矩阵收口 | 黑洞网络/高 RTT/丢确认/并发重连/登录清理/多架构安装与普通 SSH/SFTP 回归通过 |
| P8 | 清理旧 owner/重复队列、文档收口、性能校准 | 所有宣称能力有证据，旧路径无残留，文档与产品一致 |

P1/P2 可以拆成小提交；P0 未通过前可以做可复用的 backend 拆分，但不固定错误的恢复协议或对外承诺。每切片单一职责、闸门绿后按仓库约定提交 main，不将目录搬迁与行为修改混在同一提交。

### 阶段依赖与提交切片

P0 → P1 → P2 → P3 → P4 → P5 改为 macOS 客户端 → SSH → Linux 远端 sessiond 的首条主线，并在 P5 加入第二种原生客户端证明跨平台接缝。P6/P7 基于稳定契约扩端和收口，P8 在两者完成后执行；SSH 不再后置到 P7。本机/远端入口须共用逻辑并在 P3/P5 同期验收；本机 daemon 常驻与 Windows ConPTY 不是连接 Linux 的前置条件；各客户端的渲染/输入、各宿主的后台生命周期仍须独立原生证据。

| 切片 | 修改范围 | 必留证据 |
|---|---|---|
| P0-a/b | 钉版本引擎审计；状态恢复/应答原型与 ADR | 真实 PTY 和 full surface；失败项与替代成本 |
| P1-a | types 与公共头依赖、错误返回契约 | 头文件独立编译、ABI/Apple 引用双向校验 |
| P1-b | 抽取 PTY/SSH backend，行为不变 | 登录 shell/控制终端/有界写/排空/SSH 回归 |
| P1-c | session/attachment 所有权与进程内路径 | detach 不终止、terminate 有界、未知状态、取消释放 |
| P2-a/b | codec/字段表；endpoint/身份/启动；SSH exec 流与代理/租约 | 黄金编码/fuzz；并发启动/身份失败；真 sshd 无 PTY 双向流与静默断网 |
| P3-a/b | 共用本地/远端 daemon 与 PTY worker；限额/回收 | SSH 退出/代理死亡不杀会话、孤儿 fd、短写/慢消费者、资源压力 |
| P4-a/b | 权威 state 接线；SSH 断网完整恢复事务 | S/L 边界、重复查询、半序列、恢复取消、模式一致及同一原 PTY |
| P5-a/b | Apple 本地/远端会话发现/控制；本机 helper/Linux 用户安装与第二种原生客户端 | 实际断网重连/崩溃重开、关闭/结束菜单、跨 OS/CPU 格式与安装路径 |
| P6-a/b | 其他客户端；macOS/Windows 及本地宿主 | 分角色原生 runner 日志、Job 隔离、无 display、渲染/输入与安装后发现 |
| P7-a/b | 远端部署/身份/网络故障收口；普通 SSH/SFTP 独立性 | 真 sshd、拒绝/未知/取消/晚到回复、黑洞网络、丢确认、租约与部署策略 |
| P8 | 移除过渡路径、文档/性能/能力收口 | 下述完成审计全项通过 |

每一切片同步改调用者并保持可构建，不靠临时兼容包装让旧调用者留存。发布构建只链接其需要的 client/runtime 目标；用依赖图或链接结果证明 GUI 的 daemon 路径不意外拥有 backend，会话库不反向依赖 terminal/宿主。

迁移中允许各端按阶段更新，但最终不保留 legacy 标记、兼容 shim、双版本协议和“失败后偷偷退回进程内 shell”。移动端进程内模式是明确能力，不是 desktop daemon 故障的隐式降级。

## 验证与工程质量

- 重构前记录 `scripts/gate.sh`、ABI、Apple 绑定、transport 和平台构建基线；预存失败与环境 SKIP 单列。
- codec 单测验证黄金字节、短读写、粘包拆包、整数溢出、最大帧、未知字段；fuzzer 加 ASan/UBSan，持续解析不可信输入。
- 生命周期集成测试用真实 SSH/代理/sessiond/PTY：GUI SIGKILL、代理死亡、TCP reset、无 FIN 的静默丢包与网络恢复；证明同一 shell PID/启动标识与 PTY，另验 detach/terminate、启动失败/并发、过期 endpoint、输出排空和租约清理。
- 压力测试覆盖持续大输出、永不读输入的程序、慢 attachment、多会话公平性、反复 attach/detach，检查 RSS/fd/线程/句柄泄漏。
- 恢复测试验证屏幕状态、交互模式、查询次数和外部副作用；建立 S 边界并发输出与恢复期再次断网、输入/CREATE 已执行但确认丢失、旧租约 BUSY 和迟到回调测试，不能只有 echo 或 GUI SIGKILL。
- Windows 必须跑原生 CI/测试；交叉编译不等于运行验证。Linux 无 display 的 daemon 测试不得依赖 GUI 库。
- 按模块运行 TSan；与 sanitizer 不兼容的依赖隔离标注，不把未覆盖范围宣称为线程安全。
- 新增闸门按 AGENTS.md 附有针对性的变异验证：破坏 detach 语义、长度验证、恢复序号等应能产生对应失败。
- 错误包含 instance/session/request 标识；日志不含终端输出、输入、环境内容或密钥。低频可观测字段包括队列深度、断连原因、退出原因。
- 性能以当前进程内路径为基线记录吞吐、输入延迟分位数、空闲 CPU、单会话内存；P0/P3 记录后设可验证预算，不预填没有证据的目标数字。

## 文档与规则同步

实施时同步 `AGENTS.md` 的目录、C11+平台后端约束、API/线程契约；补充 architecture/session-runtime.md 与 reference/session-ipc.md；更新 capability-matrix.md、roadmap.md、各端绑定与 helper 打包说明。

不因用户允许破坏兼容而删除行为回归验证：登录 shell、控制终端、cwd、未知状态、有界写入、EOF 排空等现有正确性要求继续成立。完成标准是新架构接管这些责任，而不是绕过旧测试。

## 计划完成与实施完成的审计

已完成的 P1 前置切片：错误类型抽取至 `ghosterm/types.h`，SSH/SFTP/Transport 子头不再反向包含聚合头，公开头独立编译与无环依赖纳入 `ctest public_headers`。三组变异验证反向依赖、隐藏类型依赖及子头环路均能使检查变红。此切片不改变函数签名、符号名或错误枚举值，现有 Apple 绑定仍由原 module map 消费聚合头；不等于 P1 的 backend/session/attachment 拆分已完成。

该切片当时核心闸门 8/8、57 个公开符号与 Apple 55 项绑定引用对拍通过。初次 Apple 测试因缺完整 Xcode/XCTest 失败；后续补齐 Xcode 27.1 与 Metal Toolchain 27A266a，Apple 绑定与真实 SSH/SFTP、full libghostty 构建、终端 23 项及补丁闸门 9/9 通过。认证测试已改用无私钥材料的合成 PEM 头，不再读取开发者 `~/.ssh/id_rsa`。Keychain 需在桌面登录会话验证：该会话下宿主 257 项为 0 失败、1 项既有开发态资源跳过，认证变异 11/11 通过。生产 full surface snapshot 导入仍未实现，下文实验原型不进入生产构建。

桌面全测还暴露了跨 PTY 继承 master/wakeup fd 引起的关闭卡死。`f41c2b3` 为 transport fd 设置 CLOEXEC、exec 前隔离非标准描述符，创建失败时先关闭 master 再收尸。新增 `pty_inheritance` 验证真 exec 与两个 PTY 并存时的关闭，两个独立副本变异目标红/API 对照绿/回滚复绿；当前核心闸门 9/9、57 个公开符号与 Apple 引用对拍通过。清理宿主构建缓存并重链该核心后，桌面 257 项全测通过了原先卡死的标签关闭路径。

P1-b 已抽取私有 `core/src/io/backend.h` 操作表、`io/pty_posix.c` 与 `io/ssh_shell.c`。当前 `transport.c` 仅负责公开参数检查、backend 分派、类型与 EOF 状态；PTY 的子进程/waiter/fd/输入尾队列不再与 SSH 分支混在同一对象。此阶段刻意保持既有生命周期：close 仍释放 backend 并结束进程，尚未交付 detach；session/attachment 及其队列所有权属于 P1-c，不能将模块拆分视为 daemon 已接管。

后端拆分的现有回归已验证到变异：`mutation-check-transport.sh` 12/12、`mutation-check-login-shell.sh` 11/11（目标红、对照绿、回滚复绿）。脚本落点随代码移动，UNKNOWN 的变异留在门面，其余 PTY 变异进入 `io/pty_posix.c`；双文件恢复校验和工程 `tmp/` 参数均已实跑。

P0 增加真实 POSIX PTY 的最小 TUI 探针：在半截 CSI 捕获状态，副本停收期间由权威实例完成查询应答，再重放实际读取的有界增量；比对恢复状态，且子进程检查控制终端、前台进程组及 51×17 resize。新增三组定向变异验证增量丢失、重复应答和未传尺寸，与原四组合计七组。该探针不经过 session/attachment、IPC 或 full surface，不替代外部 TUI、并发捕获、图像及 GUI 重连验证，P0 仍未通过。

P0 full surface 隔离原型已能在新 surface 构造时导入 VT 快照：先建立最终 Terminal 地址，再补标准只读 parser continuation，renderer/IO 线程随后启动。ground 与六类 continuation 的规范状态对拍、查询/剪贴板/通知回调抑制、bracketed-paste 输入、绘制调用及销毁通过；三组变异目标红、同库对照绿、回滚复绿。补丁位于 `patches/libghostty/8159838/experimental/`，不进生产构建，未固定恢复 ABI。另有四组分配变异证明 VT sticky 语义错误可观测、callback 解码预算耗尽时输出为空且分配回收；原生页面 mmap 不在该预算内。图像、动画 owner、全量副作用、事件序号/barrier、完整内存限额及 GUI 重连仍待实现，不能据此通过 P0。

后续联合探针已增加真实 PTY、系统 Vim 与独立 tmux server 的实读快照/增量恢复；full surface 三组引擎变异和五组夹具变异均通过。资源 I/O 探针另确认：Kitty 临时文件被权威实例消费后，原始输出重放无法在副本重建图像，且不会设置语义失败标记；规范快照相同也掩盖这一差异。从权威读取像素后提供给副本的单图像受控对照通过，四组定向变异有牙。该证据要求 P0 补齐引擎侧资源结果/失败事件与有序消费，而非只给 snapshot 添加图片字段；当前仍无生产资源协议或 parser hook。

0011/0012 后续补充 relative 父链的无 UI codec 与 full Metal 构造时资源图恢复，包含父节点后置、内部 ID、多层链及删父节点后的回收。App tick 后的对拍发现 GUI 会提前重排恢复网格，实验补丁已隔离这一行为：GUI 仅经 backend 提出 resize 请求，权威接受后的生产有序回传仍待实现。该证据为实际资源图/规范状态对拍，不是图像像素验收；loading/animation、checkpoint 绑定、页面预算和生产恢复事务仍未完成。

0013 增加仅作用于当前实验 surface 的连续序号 output/resize 入口及 applied sequence 查询：实际应用后发布序号，拒绝乱序/重复/非法尺寸且保留可重试状态，有序模式下绕过序号的原始输出会使副本显式失效。逐步 canonical/资源图对拍与 bracketed-paste 输入字节验证提供局部状态 barrier 证据；没有 attachment generation、checkpoint 序号绑定、输入请求标记或生产 IPC，仍不能通过 P0。

0014 补充 surface 侧 bound checkpoint 描述，将 canonical/资源图及 generation/起始序号/源额度用独立 Python SHA256 对拍；错误组合拒绝、合法蓝色资源重算摘要后接受。从指定 S=40 继续应用事件，旧 generation、旧实验入口及 bound S=0 的原始绕过有明确拒绝/失效证据。该 generation 尚非 daemon attachment 生命周期，摘要也不证明并发原子捕获或来源认证；输入元数据、loading/动画、总内存预算与生产恢复事务仍未实现，P0 保持未通过。

0015 增加在编码点保存的输入 generation/sequence/linefeed，覆盖普通键、延迟按键序列、粘贴分片、鼠标和原始绑定；编码后推进模式的确定性探针观察旧字节与旧标记，IO drain 不追补较新序号或重读 linefeed。非法/缺失标记与回调显式失效，回复来源拒绝而用户 CSI 绑定仍可写出。十五组 native 变异加两组上游单测变异护元数据、分片、拒绝、只读释放及 72 字节内部消息预算。仍没有 worker/IPC INPUT 校验、原子捕获、全量宿主授权桥接或 GUI 重连，P0 不因此完成。

0016 将 bound GUI reset 从本地直接改 VT，改为低频权威动作请求；拒绝/缺失回调保持旧状态，收到有序 reset 事件后才清图像/模式并推进序号。七类原生用例与七组变异覆盖 pending、同步回调、拒绝、旧 generation、continuation 及请求不得混入输入流。其他状态动作（含 clear_screen）���������真实 worker/IPC、原子捕获和剩余 P0 缺口仍未完成。

0017 清屏权威事件原型已通过十类 native 用例、四套无 UI 夹具、103 项页面删除回归，以及十组定向变异和最终回滚复验。该过程发现并修正恢复后共享历史页的 eraseActive 断言，另独立验证活动图片删除与真正历史图片保留。仍无生产 worker/IPC 或完整恢复事务。

0018 分块图像快照原型已有运行证据：GTIR0003 保留各 screen 的 LoadingImage 和已有静态资源图，支持原始/zlib/PNG、初始 display/quiet/应答/隐式 ID 与半截 APC；无 UI 基线和含隐式 ID 的十一类 full surface 基线通过，十一组 VT/四组 native 变异及最终回滚复验均通过。动画帧 loading/pending/animation、原子捕获、总预算与生产恢复事务仍未交付。

0019 严格拒绝加载记录的零图片 ID。十三类正确 CRC 的畸形记录在已有图片/placement 后验证错误、零 allocator 调用和目标状态不变；五组变异及回滚复验、full surface direct 对照通过。该切片不新增恢复能力或生产接口。

0020 动画 checkpoint 原型已有运行证据：GTIR0004 保存帧/播放状态/循环/权威时刻，修正完整动画导入的存储计费，GUI renderer 改为消费有序权威 tick。无 UI 与十类原生用例、八处分配失败回收、十一组 VT/五组 native 变异及最终回滚复验通过。帧 loading/pending、原子捕获、总预算及生产恢复仍缺失。

0021 GTIR0005 已有未完成动画帧的恢复证据：捕获有效引用后重绑新缓存 generation，失效引用保持失效，支持按编号/编辑/合成/背景/quiet/PNG/zlib/continuation。二十八类无 UI、九处分配失败、十四组 VT 变异及十五类 native/六组原生变异均通过最终回滚。仍无异步 pending、并发捕获、总预算或生产恢复。

0022 GTIR0006 已有 pending 像素恢复与权威生产者身份的隔离证据：不把预留长度当作像素；十六类无 UI、九类原生 surface 用例通过，真实 PNG 解码线程可在捕获/取消后返回结果。五组无 UI、四组原生变异均完成目标红/对照绿与最终回滚复验，刻意分离权威 token 与 GUI 缓存 generation。尚未交付跨 screen 的异步生产者路由、分配失败/畸形 pending 记录专项验证、原子捕获、总预算或生产 worker/IPC，P0 仍未通过。

0023 已补齐 pending 的跨 screen 归属：完成/取消按全局 authority origin 定位原 screen，并保留活动 screen 与另一屏同 ID 图片；取消显式使用原屏 pin pool。重复 origin 的双屏记录在正确 CRC 下以零 allocator 调用拒绝。九类无 UI、七类原生基线通过，三组无 UI/两组原生变异均有目标红、独立对照绿及最终回滚复验；0022 的十六类用例回归通过。pending 专项 OOM/其余畸形记录、真实生产者调度、原子捕获、总预算和生产 worker/IPC 仍未完成。

pending-failure 已新增十五类无 UI 失败路径基线：双屏 pending/placement/帧 loading 的八个导入分配点逐一 OOM 后，可在原目标重试并续传帧，销毁回收全部 allocator 分配；非活动屏像素提交 OOM 保持 pending 并可重试。十二类正确 CRC 的畸形 pending 记录在已有双屏资源的目标上以零 allocator 调用拒绝，canonical/资源不变。四组定向变异已分别命中预检、临时 slot 回收、OOM 错报与无分配取消，独立对照与最终回滚全量基线复验通过。该证据不包含原生 surface 的 OOM、OS page/mmap 预算或生产恢复，P0 仍未通过。

0024 已实现可选原生 OS page provider：创建/快照恢复、历史页、备用屏和克隆继承同一宿主供页策略，allocator 不序列化。mmap 探针的八类基线通过，恢复十六个页面分配点逐一 OOM 后映射归零，克隆拒绝/计费测试通过；默认 full surface 七类回归通过。六组定向变异均有目标红、独立对照绿、SHA256 复原和最终回滚基线；克隆定向测试最终复绿。一般分配/资源/快照副本的共享额度、full surface 自定义预算接线、resize 失败事务与并发捕获仍缺失，P0 仍未通过。

共享 POSIX budget 原型已把一般分配、OS 页面及显式临时缓冲区纳入全局/owner 双层额度，按实际映射页与瞬时对齐映射预留。故障按 scope 保持，feed/resize 前后检查并停止失效后的输入。十二类基线、十一组定向变异与最终回滚通过；八线程争抢四份额度恰好四份获准，新建会话拒绝不杀已有会话。编码器内部临时分配尚未转到 scratch scope，full surface、真实 worker/IPC、原子捕获及性能校准仍缺失，P0 未通过。

0025 已接通快照任务 allocator 与成对发布：canonical/continuation、资源 wire 和压缩页检查全部使用 scratch scope；source/scratch 必须同账本不同 scope，两份完整输出通过合计 wire 上限后才发布，失败全回收。十四类基线及压缩活动页定向测试通过；source 额度冻结时不发生新源分配，失败后仍可输入。十组定向变异、独立对照、SHA256 复原与最终回滚全量基线均通过；默认 full surface 七类回归及核心闸门通过。捕获仍同步独占源；增长余量预留、COW/并发原子捕获、GUI 预算与生产 worker/IPC 尚缺，P0 未通过。

活动 owner 的峰值额度预留已接入：committed = 已接纳 grant + 按需 scope 实际 live，任务准入只取未承诺容量。owner 在 grant 内分配不再重复争抢全局容量；free 保留整份 grant，close 后归还，扩额/缩额与总上限调整受同一账本锁保护。checkpoint 要求 source/task 都先接纳。十类基线、十二组定向变异、独立对照和最终回滚通过；实际终端在检查点存活时 resize/输入通过，八线程四份 grant 恰好接纳四份。旧共享 budget 十一组变异及十四类快照回归通过，核心闸门全绿。并发原子/COW 捕获、GUI 预算、生产 worker/IPC 和性能校准仍未完成，P0 未通过。

0026 不可变捕获原型已分离 owner 复制与后台编码：完整元数据、continuation、页面/资源独立持有，压缩块在后台解压，资源 CRC 延后完成。源在 encoder 回调屏障期间继续输出、重置并销毁后，结果仍与边界前同步编码逐字节一致。十七类基线通过，含双屏同 ID 图片、16 MiB 图像、两阶段额度回收及取消；十一组定向变异、独立对照及最终回滚复验通过，默认原生七类回归和核心闸门全绿。owner 时间与映射峰值有记录，但未作生产性能校准。真实 worker 的 S 事件边界、任务调度/取消、GUI 预算与 IPC 恢复仍缺失，P0 未通过。

Core 新增私有 `session/state-owner` 状态提交边界：在初始化线程独占调用引擎适配回调，成功应用后才推进 64 位序号；拒绝递归及跨线程进入，应用失败或序号耗尽保持故障。冻结在相同 owner 边界返回独立对象与当前 S，冻结任务失败不推进序号，允许重试。该模块不依赖终端引擎，不引入公开 ABI；目前仅使用测试状态适配器验证，尚未连接真实 worker/backend 或 0026 冻结接口。两项 CTest、四组目标红/独立对照绿变异及 SHA256 回滚通过；核心闸门 11/11、57 符号与快速构建通过。此切片只建立 worker 将使用的提交原语，不等于真实 PTY 的 S 边界或 P1-c 已完成。

Core 私有 session worker 已连接真实 PTY backend 与 state-owner：工厂、读写、resize、状态提交、捕获和释放均在同一 worker 线程。固定 16 条/64 KiB 命令队列包含执行中的命令；逐轮限制命令与读工作量，10 ms 有界读取；满队列 stop、创建期间 stop、取消回调和状态故障停止排空有证据。四类基线通过，实际子进程确认 91×31 resize，捕获序号与状态事件数一致且旧副本不随后续输出变化。七组定向变异、独立对照及 SHA256 回滚复验通过，核心闸门 15/15、57 符号与快速构建通过。当前适配器为测试累积器，未连接 libghostty 0026、共享预算、回复路由、attachment/IPC 或公开 API；生产 S 边界和 P0/P1-c 仍未完成。

worker/libVT 组合原型已把真实 PTY 输出与 resize 接到带共享预算的引擎实例，并在 worker 的实际 S 边界取得 0026 冻结对象；终端侧新增 frozen-budget，组合位于 daemon/probes，保持 Core 与引擎隔离。真实 encoder 分配屏障期间，源继续输出/resize 并销毁后，编码与 S 的同步参考逐字节相同；已知子进程前缀另建独立期望，阻止 source feed 同时漏掉的假绿。五类基线覆盖半截 CSI/UTF-8、resize-before-capture、图片 RGBA 与 scope/额度失败；八组变异、独立对照与 SHA256 回滚通过，核心闸门 15/15、57 符号及快速构建通过。该组合仍是探针，没有生产回复路由、generation/attachment、编码任务调度取消、IPC 或 GUI 预算；P0/P1-c 尚未完成。

权威应答现已通过 worker 专用 sink 路由：只在 owner apply 窗口暂存、退出引擎后写 backend，再发布输出；独立 64 KiB 上限，不占用户命令队列。超额保持故障（parser 忽略错误也停止）、跨线程/越界调用与 backend 写失败有显式证据。十三组 Core worker 变异及回滚通过。真实 libVT/PTY query 基线校验六字节 DSR 回答且无额外输入，丢失/重复回复两组变异与已有八组合计十组；六类基线最终回滚通过，核心闸门 20/20、57 符号和快速构建通过。reply 固定元数据尚未计入生产全局准入；session/attachment、宿主授权、编码任务取消、IPC/GUI 恢复仍缺失，P0 未通过。

有界编码任务执行器已接到真实 libVT 探针：固定一个线程、1–16 槽，所有阶段和未 forget 的终态均占槽；取消与领取共锁排序，执行中取消等待 encode 返回再释放输入/结果，TAKEN 必须消费者 release 后才能释放/关闭。八类 Core 基线、十二组变异与回滚通过。真实 encoder 的 allocator 屏障内取消保持冻结对象/额度，源可继续输出并销毁；放行后不交付结果、任务 scope 关闭、映射归零。七类 native 基线与十一组变异最终回滚通过，核心闸门 28/28、57 符号及快速构建通过。尚无 attachment owner 的 detach/generation 接线、唯一全局池/通知、S/L 增量恢复事务或 IPC/GUI 预算，P0 未通过。

attachment 控制器已接入进程内恢复任务：独占活动代、不可复用 generation、每代一次 checkpoint 接管；detach 先撤销成员身份，再取消未借出的任务，不持有/停止 backend。旧代借出结果等待原 lease 释放，退休任务 reap 后返回 context 且不影响新代；池满拒绝不接管输入，可回收重试。八类 Core 基线与十二组变异、SHA256 回滚通过。真实 PTY/libVT 在 encoder 屏障内 detach/新代 attach 后继续 resize/输出，旧恢复结果不交付新代且额度归零；七类 native 基线与十一组变异最终回滚通过，核心闸门 36/36、57 符号及快速构建通过。current 仅为成员校验，非 LIVE/输入授权；公开生命周期、manager、S/L 增量事务、IPC/GUI 接线仍缺失，P0/P1-c 未完成。

有界恢复流与两阶段闸门已接到真实 VT 组合探针：owner 在 S 启动队列，worker 每个成功状态事件（含 resize）直接入固定 arena/事件槽；超额只撤销 attachment 并取消恢复，权威继续。READY(S) 固定 L，先发 S+1..L 再 LIVE_BEGIN(L)，WAIT_L 不停发后续输出，READY(L) 后才满足 generation/序号范围的输入闸门，S=L 仍分两次确认。流七类基线/十三组变异、ASan/UBSan 七类，attachment 十类/十三组及 worker 十三组变异通过；核心闸门 45/45、57 符号与快速构建通过。真实 PTY/VT 导入 continuation/资源并按队列重放，与权威最后状态逐字节一致；九类基线、十四组变异最终回滚通过。修正了 resize 同尺寸目标与图片标记抢先拦截的变异夹具，保证目标断言真正被命中。当前仍是同步 VT/逻辑 marker，full surface barrier、IPC writer、其余事件源、全局准入及公开生命周期未完成，P0 仍未通过。

恢复队列现已接到真实 full Metal surface：主线程只构造/绘制与输入，独立线程消费增量；取出事件时观察旧 engine sequence，应用后查询实际 applied sequence，并用实际 S/L 发两次 READY。LIVE 后 stamped 输入校验 generation/sequence 与字节（不向已关闭的测试 PTY 投递）。七个完整场景及一个导入对照、十一组 full-surface 变异与回滚通过；query-delta 检查快照后 DSR 源独占应答，GUI 不重复产生回写/宿主副作用。新增显式像素格 resize；固定 10×20 测试口径与 source/GUI 一致，原零格 VT-only 契约不变。共享预算十二类/十一组及 VT 十类/十四组变异回归通过，核心闸门 45/45、57 符号与快速构建通过。pin/实验链已在 macOS arm64 + 完整 Xcode 干净重建；这仍是同进程状态/输入证据，非像素验收。GUI 全局预算、其他事件源、持续输出的 IPC/GUI 完整恢复与公开生命周期仍缺，P0 未通过。

恢复用 full surface 现在在宿主提供的 CPU/原生页预算内运行：provider 必须整套
齐备，缺页供给器或零额度时构造直接失败；构造后任何预算失败 sticky 生效，序号
发布、快照、增量应用与输入全部停摆。共享 Metal Device 与共享字体 fallback 缓存
归 App/SharedGridSet 所有，不计入每 surface 预算，否则跨 allocator 释放会 SIGSEGV；
释放 surface 后须继续 tick app 预算才归还。十五类 full surface 基线（含 import-only
与 gui-default/gui-default-utf8 两类默认 allocator 对照、五类预算故障）、十一组
full surface 变异、十一组共享预算变异、六组引擎预算变异与最终回滚通过；字体归属
变异目标以信号中止而 UTF-8 对照保持绿。VT 十类/十四组变异、核心闸门 45/45、
57 符号、快速构建与补丁闸门 9/9 回归通过。该预算不覆盖 App/共享字体/系统/GPU
内存，也没有 GUI 全局准入、IPC writer、生产 manager 或崩溃重连，P0 未通过。

验证顺序第 4 步起步：`tui-less`/`tui-vim` 跑真实 less/vim——先在主屏滚出 100 行
scrollback，再进副屏整屏绘制，detach 之后继续 resize 重绘并投递按键（`G`、
`iHELLO<Esc>`），最后恢复进全新的 Metal surface，与权威最后状态逐字节一致。
程序输出不是固定字节串，镜像 oracle 改用独立事件日志重放，并断言副屏已激活、
`SCROLL001` 仍在 checkpoint 里。这是第一组同时带真实 scrollback、副屏与程序
自绘的用例，由它逼出 0028：恢复页按逻辑行数精确分配、没有 reflow 余量，detach
期间的 resize 会把页拆开，文字相同而 canonical 字节不等；补足标准容量后恢复与
原生页行为一致。证据：VT 十二类/十五组变异，full surface 十七类/十二组 full
surface 变异、十一组共享预算变异、七组引擎变异（新增 `restored page without
reflow headroom`，tui-less 红、continuation 绿）与最终回滚通过；上游
`zig build test` 的 snapshot/terminal 过滤集通过；核心闸门 45/45、57 符号、
`scripts/build-core.sh` 与补丁闸门 9/9 通过。

恢复中断已补上：核心 `interrupt` 用例证明 detach 发生在 DRAINING 中途时整代
作废——队列清空、相位落 DETACHED，取事件/live_begin/READY(L)/输入一律 STALE，
下一代从新的 S 从头重来、不继承残留；`gui-interrupt` 在真实 Metal surface 上
跑同一条路，断言半恢复的 canonical 不等于权威最后状态、之后敲键的字节被闸门
就地丢弃而不是缓存重放、权威槽位立刻可被新一代占用。两层各自有牙：生产变异
让 `gt__restore_detach` 只清队列不落相位（`interrupt` 红、`flow` 绿），夹具
变异把 detach 推迟到增量排空之后（`gui-interrupt` 红、`continuation` 绿，证明
中断确实在恢复中途）。证据：核心八类流基线/十四组变异（八类 ASan/UBSan 干净），
full surface 十八类基线/十三组变异、十一组共享预算变异、七组引擎变异与最终
回滚通过；核心闸门 46/46、57 符号、`scripts/build-core.sh` 与补丁闸门 9/9 通过。

恢复期间权威继续跑也补上了：`gui-live` 不再在重放前停掉 worker，而是在
READY(S) 之后让它再做一次 resize——权威线程在消费线程排空增量的同时继续
push。L 冻在 READY(S) 那一刻的 last 上、不跟着 last 往前跑，队列里越过 L 的
事件在 READY(L) 之后沿同一条流送达并上屏；随后由恢复线程停掉权威，surface
与权威停机时的状态逐字节一致，收尾契约（finished/无多余回复）仍成立。活着的
程序什么时候写 PTY 不归恢复侧决定，所以停机前后各排空一轮。两组夹具变异有牙：
把 emit 挪到 READY(S) 之前（事件被算进 L，恢复中途再没有东西越过 L）、越过 L
的事件只出队不上屏，两者都是 `gui-live` 红、`continuation` 绿。证据：full
surface 十九类基线/十五组变异、十一组共享预算变异、七组引擎变异与最终回滚
通过；核心闸门 46/46、57 符号、`scripts/build-core.sh` 与补丁闸门 9/9 通过。

慢客户端也补上了：`gui-slow` 把恢复流的事件上限收到 8，屏幕吃进 S 和第一条
增量之后消费线程就不再取事件，而权威还在提交 resize。有界队列撑满时作废的是
这一代而不是会话——相位落 FAILED/LIMIT、队列清空，取事件/live_begin/READY(L)
全部返回 LIMIT，`current` 不再承认这一代，半恢复的屏幕拒绝输入且 legacy 写与
副作用为零，槽位立即可被新一代占用；权威在队列满之后继续推进（基线里权威做到
27 个事件而客户端停在 2），停机后收尾契约照常成立。夹具变异让这一代在撑满后
照常往下走到 LIVE，`gui-slow` 红、`continuation` 绿——拒绝来自核心，不是探针
自己收手。证据：full surface 二十类基线/十六组变异、十一组共享预算变异、七组
引擎变异与最终回滚通过；核心闸门 46/46、57 符号、`scripts/build-core.sh` 与
补丁闸门 9/9 通过。

两次 attach 竞争在 GUI 层也补上了：`gui-race` 从 `ghostty_surface_new` 导入
快照一直到 READY(L) 之后敲键，全程有另一个线程不停抢同一个会话。在位的一代
不会被顶掉——抢的那一方每次都只拿到 BUSY 且不写出 generation，拿不到快照
（`take` 返回 INVALID）、开不了输入闸门、从队列里取事件只得到 STALE；在位这
一代的恢复照常走到 LIVE，surface 与权威最终状态逐字节一致，输入照常送达。
恢复结束后抢注方仍是 BUSY，只有在位的一代 detach 之后它才接得上。夹具变异让
抢注方改用在位那一代的号，它立刻就能从同一条流里取到东西，`gui-race` 红、
`continuation` 绿。核心侧两线程同时 attach（一个 OK、一个 BUSY）由
`core/tests/test_attachment.c` 的 `race` 覆盖，这里补的是 GUI 恢复全程的窗口。
证据：full surface 二十一类基线/十七组变异、十一组共享预算变异、七组引擎变异
与最终回滚通过；核心闸门 46/46、57 符号、`scripts/build-core.sh` 与补丁闸门
9/9 通过。

2026-10-19 补上复用器的能力探测：`query-caps` 用确定性 `/bin/sh` 脚本复刻
tmux/screen 开机时问的那串查询——DA1（`CSI c`）、DA2（`CSI > c`）、XTVERSION
（`CSI > q`）与 DECRQM（`CSI ? 2004 $p`），每一问都逐字校验答复，并在收完后
确认没有多余字节。不直接跑 tmux 是因为本机只有 Homebrew 版，拿它当闸门会让
闸门依赖机器；`terminal/session-state` 那组独立 socket 的 tmux 夹具仍按
PATH/`GHOSTERM_P0_TMUX` 可选运行，这里补的是 worker/full surface 这条链。

这一步暴露并修掉一个真缺口：XTVERSION 与 DECRQM 由 libghostty-vt 自带答复，
设备属性却要宿主装 `GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES` 回调，不装就是
静默不答——复用器会停在 DA1 上一直等。权威侧现在装上这个回调，答
`CSI ? 62;1;22 c` 与 `CSI > 1;10;0 c`。DECRQM 问的是 detach 之后才补完的
2004，所以「detach 期间继续查询」也一并覆盖。恢复出来的副本对这四类查询全程
不答（surface 宿主回写计数为 0），否则程序会收到两份答复。

夹具变异两组：拆掉设备属性回调（静默不答）、把固件号改一位（答了但答错），
`query-caps` 均红而 `query` 绿。证据：worker/libVT 十七组变异、full surface
二十二类基线/十七组变异、十一组共享预算变异、七组引擎变异与最终回滚通过；
核心闸门 46/46、57 符号、`scripts/build-core.sh` 与补丁闸门 9/9 通过。

同一步剩下的只有 GUI SIGKILL。它不能在同进程里做，而且现在可以把话说准：
`gt__attachment_attach` 只在 `active` 为假时放行，而 `active` 只由显式
`gt__attachment_detach` 或恢复流自身失败（有界队列溢出）清掉——没有任何基于
存活性的回收。于是空闲会话上的 GUI SIGKILL 会把槽位永久占住：权威不产事件，
队列就永远不会溢出，新客户端只会一直拿到 BUSY。所以 IPC 层「传输断开即
detach」不是优化，是 P0 的硬需求，必须和 IPC 一起落地并验收。P0 未通过。

2026-10-19 起做第 5 步「应答和宿主动作隔离」的第一刀：`isolation`。
`query-caps` 覆盖的是有回复的查询，`isolation` 补的是有副作用、没有回复的
宿主动作。脚本在 detach 前后各发一次 OSC 9 桌面通知与配色查询
`CSI ? 996 n`，权威装 `GHOSTTY_TERMINAL_OPT_DESKTOP_NOTIFICATION` 与
`GHOSTTY_TERMINAL_OPT_COLOR_SCHEME` 回调并计数：两次通知（按 body 长度核对）、
两次配色询问，答复逐字为 `CSI ? 997;1 n`，收完确认没有多余字节。

关键是 detach 之后那一份：它落进增量流，被原样重放给恢复出来的真实 Metal
surface。副本的 `host_write` 与宿主动作回调全程为 0。这个零此前是空断言——
没有任何用例发过会触发宿主动作的序列；现在它有了内容，并且不是「字节没到」：
同一段字节里的 `SECOND` 确实出现在副本最终 canonical 中，与权威逐字节相等。
机制来自 0004：副本模式下 `processOutputLocked` 把输出交给独立的
`replica_stream`，`colorSchemeReport`/`focusGained`/`sizeReport` 一律提前返回。

变异三组：配色回调不答、答成 light（`isolation` 红、`query` 绿），以及把
detach 后那次 OSC 9 从脚本里删掉——增量流里没有宿主动作，「副本一次都没触发」
就无从谈起，该路径同样红。证据：worker/libVT 十四类基线/十九组变异、full
surface 二十三类基线/十八组变异、十一组共享预算变异、七组引擎变异与最终回滚
通过。

第 5 步剩下的：中文 IME、粘贴、鼠标、焦点在恢复链路上的行为。
`docs/reference/capability-matrix.md` 里已有的 IME/焦点证据来自 macOS GUI 层的
`TerminalIMETests`/`WindowFocusTests`，不是恢复链路，不能顶替。P0 未通过。

2026-10-19 同一步的第二刀：`clipboard`。剪贴板是唯一一类「宿主必须当场答复」
的动作——回调是同步的，写不回复等于拒绝，读不回复等于交给程序一个空剪贴板，
所以它既不是纯动作也不是纯查询，得单独验。脚本在 detach 前后各做一轮写加读：
写 `HELLO` 与 `SECONDCLIP`，权威断言收到的是解完 base64 的原文（协议层编码归
引擎，宿主不该再碰）；两次读都答 `PASTE`，程序收到的
`OSC 52;c;UEFTVEU= ST` 共 17 字节逐字校验，收完确认没有多余字节。

detach 后那一轮同样落进增量流，被重放给恢复出来的真实 Metal surface，副本的
剪贴板动作计数全程为 0。这条比通知更要紧：副本若也执行一次写，用户的系统
剪贴板会被真的覆写。

变异两组：把读答复的内容换成等长的别的字符串（答复仍是 17 字节，逐字对不上，
`clipboard` 红而 `query` 绿），以及把 detach 后那次写从脚本里删掉——增量流里
没有剪贴板动作，副本的零就无从谈起。后一组的红落在权威侧的「写了两次」断言
上，它证明的是「这条增量确实发了」，副本的零不是因为字节压根没到；零本身由
full surface 探针的 `surface_effects`/`surface_legacy_writes` 断言。

证据：worker/libVT 十五类基线、二十组变异目标红同库对照绿并回滚复绿；
full surface 二十四类基线、十类完整恢复、十九组 full surface 加十一组共享预算
加七组引擎预算变异通过；恢复增量流变异、`build-core.sh`、`gate.sh`（46 项测试
全通过、57 个公共符号对拍一致）、`gate-ghostty-patch.sh` 均通过。P0 未通过。

2026-10-19 同一步的第三刀：`input`，方向反过来——前两刀问「副本不许做什么」，
这刀问用户在恢复出来的窗口里真正打字点鼠标时，字节有没有按**被恢复的终端
状态**编对。脚本在 detach 前开 `?1000h`/`?1006h`，两个鼠标模式随快照回来，
副本的编码因此不是 GUI 自己挑的默认值。恢复后在同一个真实 Metal surface 上
逐步验：中文 IME 组合中（`composing=true`）一个字节都不出去；提交时走按键
路径，出去的是原始 UTF-8 六字节、不带 bracketed paste 包裹；粘贴是否包 `?2004`
由恢复出来的状态决定；鼠标左上角按下得 `CSI <0;1;1 M`、抬起得 `CSI <0;1;1 m`。
全程 `surface_legacy_writes` 与宿主动作计数为 0——输入只经 p0 通道，副本不直接
写权威 PTY。

变异两组：把组合中的按键翻成已提交（那一步的零变成六字节），把左键换成右键
（SGR 第一个参数变 2），均 `input` 红、`continuation` 绿。

**这刀同时查出一个缺口，当时没有修，留给下一刀：焦点上报在副本上被丢弃。**
鼠标报告是 `write_*` 消息，经 `queueIo` 被 0015 改道进 p0 输入通道；焦点报告走
的是 `io.focusGained`，而 0004 在副本模式下让它整个提前返回。于是程序开了
`?1004h`、用户切到恢复出来的窗口，`CSI I`/`CSI O` 收不到。焦点是用户输入，应
当和鼠标一样改道而不是丢弃。

证据：worker/libVT 十六类基线、二十组变异目标红同库对照绿并回滚复绿；
full surface 二十五类基线、十一类完整恢复、二十一组 full surface 加十一组共享
预算加七组引擎预算变异通过；恢复增量流变异、`build-core.sh`、`gate.sh`（46 项
测试全通过、57 个公共符号对拍一致）、`gate-ghostty-patch.sh` 均通过。
第 5 步只剩焦点这一项。P0 未通过。

2026-10-19 同一步的第四刀：补上上一刀记下的焦点缺口。新增实验补丁
`0029-replica-focus-input`，把 0004 对 `focusGained` 的「整条丢掉」改成「改
道」——在同一次持锁里既读 `focus_event` 模式又取 stamp（两者必须来自同一刀，
否则模式和代号可能分属不同状态），副本模式下把编好的 `CSI I`/`CSI O` 包成
`ReplicaWrite` 走 p0 输入通道交回权威，并跳过 `backend.focusGained`（那才是宿
主动作，副本不该做）。非副本路径一字未动。

探针侧 `input` 的 detach 前缀与子脚本都加上 `?1004h`，让焦点模式随快照回到副
本；恢复后在同一个真实 Metal surface 上多验三步：切走得 `CSI O`、重复设同一个
值必须零字节、切回得 `CSI I`。顺序是先切走再切回，因为 surface 构造出来默认
就是聚焦的，直接设 `true` 是空操作。

变异两组：fixture 侧把重复设焦点那一次改成相反值（该为零的一步冒出 `CSI I`，
证明「状态没变不许报」不是摆设），引擎侧把 0029 的副本分支改回丢弃（焦点模式
开着、状态真变了，字节却到不了权威），均 `input` 红、`continuation` 绿并回滚
复绿。

证据：worker/libVT 十六类基线、二十组变异；full surface 二十五类基线、十一类
完整恢复、二十二组 full surface 加十一组共享预算加八组引擎预算变异通过；恢复
增量流变异、`build-core.sh`、`gate.sh`、`gate-ghostty-patch.sh` 均通过。
第 5 步枚举的行为至此全部有恢复链路上的证据：live/replay/import 三种模式下的
应答与宿主动作归属（`query`/`query-delta`/`isolation`/`clipboard`），中文 IME、
粘贴、鼠标、焦点（`input`），配色查询（`isolation`）。仍欠「像素作为渲染证据
补充」这一句：现有像素证据是 GUI 层的 `PixelRenderTests`，不是恢复出来的副本，
恢复链路上的比对仍停在结构化状态与逐字节交互记录。P0 未通过。

2026-10-19 补上第 5 步的恢复像素证据：`pixels` 的真实 PTY 在 detach 前输出
truecolor 红底白字 `FIRST`，detach 后输出绿底 `SECOND`，分别进入快照和增量。
新 Metal surface 导入时抓自己的 NSView，必须有红色与白色且绿色为零；到 LIVE
后再抓一帧，必须保留红色并新增绿色。源端隐藏光标，白色像素不能由光标冒充。
canonical/resources 逐字节相等、S/L 屏障和 stamped 输入检查照常执行；像素只是
补充，不替代状态验证，不声称字形质量。PNG 留在工程 tmp 下供检查。

新增变异让 CGContext 不画图，抓到全零位图：`pixels` 红、`continuation` 绿，
回滚后复绿。运行环境为 macOS Darwin 25.6.0 arm64、Xcode 27.1（27A9275），
基于 `d8008b2` 的本切片。`python3 scripts/probe-session-worker-surface.py --mutations`
通过二十六类 full-surface 基线、二十三组 surface、十一组共享预算、八组引擎变异
与最终回滚复验；`bash scripts/gate.sh`（46 项测试、57 个公共符号）及
`bash scripts/build-core.sh` 通过。日志为 `tmp/worker-pixels-verification.log` 与
`tmp/worker-pixels-gate.log`。第 5 步至此有恢复链路上的渲染证据，但第 4 步的 GUI SIGKILL、
GUI 全局准入与生产 IPC/manager 仍缺，P0 未通过。

2026-10-19 起补 GUI SIGKILL 所需的连接 owner：私有
`core/src/ipc/attachment-peer` 接管 Unix stream，观测 HUP/EOF/系统错误就关闭
socket 并撤销本代、取消未借出的恢复任务；借出快照留给 owner 停用后释放，旧
peer 不得撤销新一代。receive 必须在空闲和输出背压时定期运行，不能靠有输出
才清理连接。读写 NONBLOCK、每次至多 1000 ms、EINTR 不重置单调 deadline，
短写显式报长度；HUP 后未接受输入不再投递。未固定恢复 wire schema。

十三类 `attachment_peer_*` 基线与十五组变异覆盖这条接缝：独立空闲客户端
SIGKILL 后可立即重占 attachment，原真实 PTY 仍能输入并输出 `SURVIVED`；编码中
取消与借出快照的寿命均保留。这只是连接死亡的基础测试，客户端尚不是 GUI，
没有跨进程快照/增量/full-surface 重建、生产 listener/manager 或身份/token
校验，因此第 4 步的 GUI SIGKILL 仍欠，P0 未通过。

运行证据：基于 `502e806` 的本切片，Darwin 25.6.0 arm64、Apple clang 21.0.0。
`python3 scripts/mutation-check-attachment-peer.py` 十三类基线、十五组目标红/对照
绿及 SHA256 回滚复绿；`bash scripts/gate.sh` 59/59、57 个公共符号，
`bash scripts/build-core.sh` 通过。关闭 libssh2 的独立 ASan/UBSan 构建中，
`ctest --test-dir tmp/attachment-peer-asan -R '^attachment_peer_' --output-on-failure`
十三类通过。Zig 0.16.0 的 `cc -target x86_64-linux-gnu -std=c11 -Wall -Wextra -Werror`
对新 peer 源码交叉编译通过，仅编译，不声称 Linux 运行支持。日志分别在
`tmp/attachment-peer-{mutations,gate,build,asan-test,linux-compile}.log`。

2026-10-19 首个真实 GUI SIGKILL 跨进程恢复原型：
`session-process-owner.c` 的无 UI 权威保留 worker/PTY，posix_spawn/exec 启动的
Metal GUI 只继承专用 socketpair，没有共享 attachment/terminal 指针。第一
GUI 已导入、完成 S/L 并抓到红底白字后，权威 SIGKILL 它，waitpid 校验信号；
此时 last 不变、队列没满，peer 确实因死亡回收槽位。旧连接销毁不碰新代。

同一 PTY 随后在没有 GUI 时继续 resize、输出绿底 `SECOND`，逐字核对 DSR
唯一回复并确认无多余字节。新代捕获更晚的 S，启动第二个全新 GUI，经有界
分块快照和 S 后的 resize 走完两次 READY；输入携 g/applied sequence，通过
socket 回到权威。真实 shell 逐字核对 `USER` 的十六字节 bracketed paste 后
才输出 `THIRD`；这条 LIVE 输出继续上屏并以蓝色像素验证，最终 canonical/resources 与权威
停机状态逐字节相等，legacy 回写/宿主动作均为零，红/绿/白像素也实测存在。

新增十一类 codec 基线及十组进程/九组 codec 变异，另回归二十六类既有 surface
基线。实验 `GTP0` 帧仅走继承式 socketpair，不作为生产 IPC v1；尚无 listener、
manager、身份/token、发现列表、真实应用恢复入口与 helper 打包。接收缓冲与
App/共享字体/系统/GPU 仍未纳入 GUI 全局准入，P0 未通过。它补上第 4 步受控
PTY 上 GUI SIGKILL 的跨进程证据，不把 P5 的产品验收一并宣称完成。

运行环境为 Darwin 25.6.0 arm64、Xcode 27.1（27A9275）、Apple clang 21.0.0，
基于 `d52d4ee` 的本切片，从既定 pin/完整实验补丁链干净重建。
`python3 scripts/probe-session-process.py --mutations` 通过两类进程、十一类 codec、
十组进程与九组 codec 变异、SHA256 回滚及二十六类 legacy surface 基线；
codec 的十一类 ASan/UBSan 基线通过。最终蓝色像素采样为 1973，且已人工查看
`tmp/worker-surface/process-final.png`。`otool -L` 确认 server 只依赖 VT dylib
与 libSystem，不链接窗口系统。`bash scripts/gate.sh` 59/59、57 个公共符号及
`bash scripts/build-core.sh` 通过。日志：`tmp/process-verification.log`、
`tmp/process-wire-asan.log`、`tmp/process-server-dependencies.log` 与
`tmp/process-core-gate.log`。未运行 Linux/其他平台的新进程探针。

2026-10-19 继续补 GUI 资源准入：新增 `terminal/session-state/gui-budget`，
同一 GUI 进程的所有恢复 lease 共用 ledger。CPU、原生页、wire 接收缓冲按成组
预留准入，失败回滚全部 scope，计数与字节上限独立；故障 lease 在所有分配
释放后可归还额度。原接收 snapshot/expected 的 malloc 改走 wire provider。

真实 Metal 进程用例同时建两份 surface，总 320 MiB、每份 64+64+32 MiB，
第三份在构造引擎前失败；释放后重试成功。辅助 surface wire 耗尽后拒绝序号
发布与增量，主 surface 照常恢复/输入/绘制。独立八类预算基线与十四组变异，
另加三组原生接线变异。`python3 scripts/probe-session-process.py --mutations`
完成两类进程、八类预算、十一类 codec、十三组进程/九组 codec/十四组预算变异、
SHA256 回滚与二十六类既有 surface 基线。八类预算 ASan/UBSan 通过；新 budget
与 probe 以 Zig 0.16.0 对 Linux x86_64 严格 C11 交叉编译通过，仅编译无运行。
`bash scripts/gate.sh` 59/59、57 公共符号及快速构建通过。
环境 Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0，基于 `7dcf80e`。
证据日志为 `tmp/gui-budget-{verification,asan,linux-compile,core-gate}.log`。

该共享池尚不覆盖 App/共享字体/系统/GPU、截图和固定栈/帧缓冲；不同 GUI
进程也未共用准入，仍不能声称全局内存有界。生产 manager/listener/认证协议与
产品恢复入口仍欠，P0 未通过。

2026-10-19 补跨进程快照内容校验：实验 SNAPSHOT/EXPECT 元数据附带发送端
canonical/resources 各自的 SHA-256，GUI 在导入或最终状态对拍前核对。此前 GUI
本地计算的引擎 checkpoint digest 只能绑定收到的内容，无法核对发送端原文。
新增两组摘要翻位变异分别使目标在导入前红、同库 legacy 对照绿，回滚后复绿。
它只检查传输损坏，不是认证/MAC，不替代生产 token/构建标识/完整性终止契约。

基于 `1c9ee87`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；
`python3 scripts/probe-session-process.py --mutations` 完成两类进程、八类预算、
十一类 codec、十五组进程/九组 codec/十四组预算变异、SHA256 回滚与二十六类
surface 基线。`bash scripts/gate.sh` 59/59、57 公共符号及快速构建通过。
日志 `tmp/process-integrity-verification.log` 与 `tmp/process-integrity-gate.log`。
未新增 Linux 运行证据。完整资源预算、生产 daemon/协议及产品入口仍欠，P0 未通过。

2026-10-19 新增跨进程 `crossing`：READY(S) 已固定 L 后，权威 worker
继续提交 resize；先排到 L 并发送 LIVE_BEGIN(L)，等待 READY(L) 时继续发送
L 后事件。客户端等待 LIVE_OK 时持续应用增量，LIVE_OK 仍确认原 L；其后输入
stamp 使用新的实际 applied sequence。实测 S=4、L=5、final=7，最终完整状态
与像素验证通过。漏发越界事件或客户端等确认时不消费增量，均 crossing 红、
roundtrip 对照绿，回滚后复绿。只验证受控 resize 跨界，不代表持续大流量
双向背压下的生产 writer 已验收。

基于 `f0a1b21`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0，
`python3 scripts/probe-session-process.py --mutations` 三类进程、八类预算、
十一类 codec、十七组进程/九组 codec/十四组预算变异、SHA256 回滚及二十六类
surface 基线通过。核心闸门 59/59、57 公共符号及快速构建通过。
证据 `tmp/process-crossing-{verification,gate}.log`。P0 未通过。

2026-10-19 修复实验 blob 传输的时间预算：原先每帧重新获得 5 秒，分块虽
各自不过期，整个快照仍可能长时间占用连接。现在单 blob 全部帧共用一份单调
deadline，header/body 同样共享；发送/接收部分超时必须终止流，不继续解析。
slow-send/slow-receive 每回调最多等待 30 ms 后返回 WAIT，每 body 延迟 2.7 秒，
两帧总时间超过 5 秒就失败。恢复逐帧 deadline 的两组变异目标红、blobs 对照绿。
该限制仅针对单 blob，不冒充整代恢复总超时或生产取消清理。

基于 `8df6f5c`、Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；最终
`python3 scripts/probe-session-process.py --mutations` 三类进程、八类预算、
十三类 codec、十七组进程/十一组 codec/十四组预算变异、SHA256 回滚与二十六类
surface 基线通过。十三类 codec ASan/UBSan 通过；核心闸门 59/59、57 公共符号
及快速构建通过。证据 `tmp/process-deadline-verification-final.log`、
`tmp/process-deadline-asan.log`、`tmp/process-deadline-gate.log`。
早一轮在运行中调整测试，末尾源码哈希检查明确报错，该轮不作为通过证据；
最终冻结源码完整重跑已通过。P0 和生产 daemon 仍未完成。

2026-10-19 补快照的引擎构建绑定：发送端/客户端由构建脚本编入同一份
实际 VT dylib/full archive、pin、目标、实验补丁链与快照契约的 SHA-256 指纹。
SNAPSHOT/EXPECT 元数据扩为 136 字节，客户端先匹配构建，再分配和接收内容，
不做不同构建的兼容读取。发送指纹翻位使目标在匹配处失败，同库 legacy
对照绿，回滚复绿。严格产物摘要不承诺跨机器重建一致；它不是认证或 MAC，
不替代生产 handshake/token/升级清理流程。

基于 `c9c204a`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；
`python3 scripts/probe-session-process.py --mutations` 完成三类进程、八类预算、
十三类 codec、十八组进程/十一组 codec/十四组预算变异、SHA256 回滚与二十六类
surface 基线。核心闸门 59/59、57 公共符号及快速构建通过。
日志 `tmp/process-build-identity-{verification,gate}.log`，指纹记录
`tmp/worker-surface/process-engine-build.txt`。生产 daemon 与完整资源预算仍欠，
P0 未通过。

2026-10-19 开始补性能证据：进程探针按阶段记录 owner 不可变捕获、后台编码、
快照本地发送接受、到 GUI 绘制确认的耗时；记录 capture/encoder scope 与权威
ledger 的峰值、wire 长度，并用 macOS getrusage 记录 authority RSS 峰值和累计
CPU。观测不设阈值，时钟失败打印 -1，系统取样失败明确报 unavailable。

基于 `34f40b1`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0，最终
回滚基线四次捕获（roundtrip、kill 两代、crossing）的样本如下：

| 指标 | 本次小负载样本 |
|---|---|
| owner 捕获 | 0.303–0.525 ms |
| 后台编码 | 0.321–0.945 ms |
| 本地 socket 接受快照 | 0.017–0.029 ms |
| 发送到绘制确认 | 755.721–1672.835 ms，含主动等帧/辅助 surface 检查，不是输入延迟 |
| canonical / resources | 1202 或 1292 / 96 bytes |
| capture / encoder 计费峰值 | 507904 或 524288 / 49152 bytes |
| 权威 ledger live / committed 峰值 | 2326528 或 2342912 / 268435456 bytes |
| authority 峰值 RSS | 7405568 或 7520256 bytes（不含 GUI） |

这是小 FIRST/SECOND/THIRD 负载，未覆盖长 scrollback/资源压力，不能推出生产
限额或 RSS 上限。没有旧路径对比、输入延迟分位数、GUI RSS 与空闲 CPU，故性能
校准仍未完成。完整进程/codec/预算 43 组变异及 26 类 surface 回滚复验通过，
核心闸门 59/59、57 公共符号及快速构建通过。日志
`tmp/process-metrics-verification.log`、`tmp/process-metrics-gate.log`。
P0 与生产 daemon 仍未完成。

2026-10-19 扩大恢复负载到千行历史：新增跨进程 `history`，真实 PTY/awk
生成 1000 行，清可见屏后继续原交互；捕获时实际读到 977 行 scrollback。
canonical 74601 bytes、resources 96 bytes，多帧传输后导入真实 Metal 两个
surface，全量状态、预算隔离、原 shell 输入和最终像素均通过。删掉历史生成
的变异使 history 在捕获检查处红、roundtrip 对照绿，回滚复绿。输出观察缓冲
固定扩为 128 KiB，是测试日志开销，不充当生产 scrollback。

最终回滚样本：owner 捕获 0.422 ms、后台编码 3.263 ms、本地发送接受
93.342 ms、发送到绘制确认 864.693 ms（含等帧/辅助 surface 检查）；capture
峰值 933888 bytes、encoder 峰值 245760 bytes、权威 ledger live 峰值
2998272 bytes、committed 峰值 268435456 bytes；authority RSS 峰值
8323072 bytes。仍不包含 GUI RSS/系统/GPU，不代表最大历史/图像或延迟分位数。

基于 `f54dabb`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；
`python3 scripts/probe-session-process.py --mutations` 四类进程、八类预算、
十三类 codec、十九组进程/十一组 codec/十四组预算变异、SHA256 回滚及二十六类
surface 基线通过；核心闸门 59/59、57 公共符号及快速构建通过。
证据 `tmp/process-history-{verification,gate}.log`。旧路径性能对比、完整预算和
生产 daemon 仍欠，P0 未通过。

2026-10-19 修复编码失败的 attachment 槽位泄漏：任务 FAILED 被 reap 后原实现
只删除任务却仍 active，后续 take 无结果、新 attach 永远 BUSY。现在回收失败
任务时仅撤销仍在位的对应 generation 并清空恢复流，retired 保留原错误。
旧代失败晚到回收不碰新代及其任务。owner 仍须定期 reap；不是自动 watchdog。

当前代失败后重试、新代存活时回收旧失败，两类真实 executor 测试都能随后完成
新快照消费。十一类 attachment 基线、十五组变异及 SHA256 回滚，十一类
ASan/UBSan 基线通过；peer 十三类基线/十五组变异回归通过。核心 gate 60/60、
57 公共符号与快速构建通过。基于 `ea59ff5`，Darwin 25.6.0 arm64、Apple clang
21.0.0。日志 `tmp/attachment-failure-{mutations,asan,peer-regression,gate}.log`。
未改引擎/宿主接线，未重复 Metal 全套；生产错误呈现与重试入口仍欠，P0 未通过。

2026-10-19 补跨进程恢复的标题/cwd 显式语义证据：所有真实 PTY 用例在
FIRST 前设置 OSC 2 标题与 OSC 7 cwd，detached 期间改成另一组值。owner 在
每次捕获与最后停机快照前从引擎 TITLE/PWD API 逐字核对预期；初始 GUI 与
接回 GUI 的完整 canonical 对拍因此不再依赖“源和副本都是空值”的弱证据。
OSC 7 保留原 URI，不声称已完成路径解析或产品目录 UI。

四组删除变异覆盖初始/更新两阶段、标题/cwd 两字段，目标红、独立同库对照
绿，回滚复绿。基于 `f0ef57f`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang
21.0.0；最终 `python3 scripts/probe-session-process.py --mutations` 四类进程、
八类预算、十三类 codec、二十三组进程/十一组 codec/十四组预算变异、SHA256
回滚及二十六类 surface 基线通过；核心 gate 60/60、57 符号与快速构建通过。
证据 `tmp/process-metadata-verification-final.log`、`tmp/process-metadata-gate.log`。
会话 runtime 的 OSC 133/7501 摘要、完整预算与生产 daemon 仍欠，P0 未通过。

2026-10-19 开始补 daemon 自己持有的 OSC 7501 摘要：新增纯 C 私有
`session/program-status` owner 表，接受引擎已解码/规范化的报告，复制字符串
与 state/kind/progress，支持原位更新、slash 子树清除、空 id 清空、独立深拷贝。
条数/持有字符串容量显式有界，拒绝不改变旧表。字段最长 4096 字节、条数最多
1024、持有字符串最多 1 MiB；对象/数组开销、更新临时记录与整表捕获副本仍须
纳入未来 manager 准入，不能声称字符串限额已等同全局内存限额。

六类基线和十二组隔离变异覆盖复制/别名寿命、深拷贝、子树边界、限额回滚与
未知进度。ASan/UBSan 六类通过，Linux x86_64 严格 C11 交叉编译通过（未运行）。
基于 `b48b5fc`，Darwin 25.6.0 arm64、Apple clang 21.0.0；核心 gate 66/66、
57 公共符号及快速构建通过。日志 `tmp/session-program-status-{mutations,asan,
gate,linux}.log`（构建日志另为 `tmp/program-status-build.log`）。

该提交是摘要存储基础，尚未接 VT 回调/worker 捕获/IPC S/L，Apple UI 模型未
迁移，OSC 133 命令摘要、文本显示清洗、生产 daemon 与完整预算仍欠，P0 未通过。

2026-10-19 将程序表接入 headless VT/worker 捕获：进程权威安装 PROGRAM_STATUS
回调，真实 PTY 分初始、detached、恢复后输入三阶段报告 working/25%、
blocked/permission/未知进度、done，解码 message 逐字核对。回调有界复制 span，
拒绝时形成 owner sticky 错误；同一 freeze 回调复制摘要与 VT 状态，在同一 S
发布，编码线程读取独立副本。源已变 done 时借出的 blocked 副本仍不变。
成功结果释放及取消/编码无结果的 input cleanup 均归还摘要；权威表随会话清理。

移除回调、移除 detached 状态、遗漏摘要捕获三组变异目标红/legacy 对照绿。
基于 `1577701`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；最终
`python3 scripts/probe-session-process.py --mutations` 四类进程、八类预算、
十三类 codec、二十六组进程/十一组 codec/十四组预算变异、SHA256 回滚及二十六类
surface 基线通过；核心 gate 66/66、57 符号及快速构建通过。日志
`tmp/process-summary-owner-verification-final.log`、`tmp/process-summary-owner-gate.log`。
首轮因运行中补清理路径而被源码哈希检查拒绝，不作最终通过证据。

摘要仍未 wire 编码/客户端导入，表与副本的有界 malloc 尚未接总预算，OSC 133
命令生命周期和产品 UI 未迁移。该切片仅完成服务端权威/捕获寿命接线，P0 未通过。

2026-10-19 将捕获摘要传到新 GUI：新增实验 GPS1 编码，显式字段/长度，
最多 32 条、65536 字符串字节；事务解码拒绝 clear、重复 id、未知值、NUL、
越界/截断与尾随数据。SNAPSHOT/EXPECT 元数据扩为 176 字节，带摘要长度和
SHA-256；SUMMARY blob 紧随资源，三份内容总和保持 16 MiB 上限。客户端 wire
缓冲使用 admitted scope，READY(S) 前解码并核对 working/blocked 状态，表不
保留 wire 指针。最终 EXPECT 的 done 是诊断对拍，不作为 LIVE 摘要推送。

摘要损坏/遗漏导入两组进程变异、八组 codec 变异及 15 类摘要基线（含独立字节
oracle）通过；摘要 codec 的 15 类 ASan/UBSan 通过。基于 `9fbefab`，Darwin
25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；最终
`python3 scripts/probe-session-process.py --mutations` 四类进程、八类预算、
十三类帧 codec、15 类摘要 codec、二十八组进程/十一组帧/十四组预算/八组摘要
变异、SHA256 回滚及二十六类 surface 基线通过；核心 gate 66/66、57 符号及
快速构建通过。日志 `tmp/process-summary-wire-verification-final.log`、
`tmp/summary-wire-asan.log`、`tmp/process-summary-wire-gate.log`。首轮修改中的
源码被哈希闸门拒绝，不计最终通过。

解码表/深拷贝与服务器序列化栈缓冲仍未计入总 ledger，实时摘要事件、OSC 133、
产品 UI 与生产 daemon 尚未完成，P0 未通过。

2026-10-19 补实时程序摘要：owner 在成功 apply 后把触发变更的输出与当时
编码的摘要作为同序号的有界 attachment 队列项入队，发送线程不回读变化中的
权威表。实验 writer 发送标记输出、同 g/sequence 的摘要 metadata/digest 与
blob；客户端同一 I/O 消费步骤应用摘要后再推进流 cursor。无变更的输出/resize
仍走原路径。引擎自己的 applied sequence 与摘要不是通用 UI 原子发布机制。

真实 shell 接受输入后产生 done/THIRD；客户端必须在导入最终 EXPECT 摘要前
确认实时表已是 done/-1/Done，避免最终 oracle 掩盖实时路径遗漏。禁用摘要
配对、损坏 generation、损坏 digest 三组变异目标红/legacy 对照绿。
基于 `8b1c02f`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；
`python3 scripts/probe-session-process.py --mutations` 四类进程、八类预算、十三类
帧/15 类摘要 codec、三十一组进程/十一组帧/十四组预算/八组摘要变异、SHA256
回滚及二十六类 surface 基线通过；核心 gate 66/66、57 符号及快速构建通过。
日志 `tmp/process-summary-live-{verification,gate}.log`。

单记录实时摘要已有证据，但最大摘要配对项可能超过现有队列字节上限，生产错误
路由与该规模测试仍欠；编码栈/记录表未纳入总 ledger。OSC 133、产品 UI、通用
高流量 writer 和生产 daemon 尚未完成，P0 未通过。

2026-10-19 修复合法大摘要与旧 64 KiB 恢复队列容量不匹配：实验容量按
8 + 16 KiB + PSW_MAX 配成 82576 字节，owner event 允许核心 LIMIT 返回，
不因 attachment 超额直接触发进程断言。单条容量仍固定，累计积压仍按 LIMIT
撤销该代，不扩成无界流。生产 writer 的错误交付/断连回收尚待接线。

新增实际 C attachment 规模测试：32 条记录用满 65536 持有字符串字节，GPS1
66056 bytes，加完整 16 KiB 输出配对后为 82448 bytes；入队和独立解码通过。
第二条同规模项必须 LIMIT、撤权、清空字节/事件；下一代仍可接收同规模项。
旧容量/不撤权/不清空三组变异目标红、独立对照绿及 SHA256 回滚通过。

基于 `04d3d83`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；最终
`python3 scripts/probe-session-process.py --mutations` 四类进程、两类规模队列、
八类预算、十三类帧/15 类摘要 codec、31 进程/11 帧/14 预算/8 摘要/3 规模变异
及二十六类 surface 回滚基线通过；两类规模测试使用完整相关 C 源码的
ASan/UBSan 构建通过。核心 gate 66/66、57 符号及快速构建通过。日志
`tmp/process-summary-scale-verification-final.log`、`tmp/summary-scale-asan.log`、
`tmp/process-summary-scale-gate.log`。首轮的独立对照误用上轮变异二进制，已修脚本
重建对照并完整重跑，首轮不作通过证据。

此处证明核心容量/累计背压，未声称真实 Metal 最大摘要或完整生产重连已验收。
固定栈/记录表/queue arena 的统一 ledger、OSC 133 与生产 daemon 仍欠，P0 未通过。

2026-10-20 开始补 OSC 133 命令摘要：新增私有纯 C owner 模型
`session/command-status`，有界复制命令/错误文本，分离 running、可选退出码、
可选耗时与起点有效位。零时间点/零耗时有效，负退出码保留；没有起点、缺
时钟、时钟倒退、只收到结束时不编造成功/耗时，重复结束不沿用旧 duration。
output_start 重置旧结果，prompt/input/未来 kind 不清上一条命令。非法输入
保留旧值，独立值副本不含借用指针；结构不直接作为 wire 格式。

基于 `0af7c49`，Darwin 25.6.0 arm64、Apple clang 21.0.0；六类基线、十三组
定向变异及 SHA256 回滚、六类 ASan/UBSan 通过；核心 gate 72/72、57 公共符号
与快速构建通过。Linux x86_64 严格 C11 交叉编译通过，仅编译无运行。日志
`tmp/session-command-status-{mutations,asan,gate,linux}.log`。
尚未接真实 VT 回调/worker 捕获/IPC，Apple LastCommand 未迁移，prompt_start
对 working/blocked 程序表的清理联动未实现，生产 daemon 与 P0 仍未完成。

2026-10-20 将 OSC 133 接入 headless VT 权威与捕获：真实 PTY 初始发
output_start/命令文本、detached 后 command_end/7、恢复输入后下一条 start/end
不带退出码。回调有界复制引擎解码文本，保留可选退出码，附带单调时间有效位
交 C 模型。owner 同次 freeze 按值捕获命令与程序表/VT 状态，在同一 S 发布；
后台编码线程读取副本。owner 已变下一条命令/未知退出码时，借出的旧命令
仍为 ghosterm test/7/有效耗时，首次运行中快照也明确检查。

回调缺失、真实退出码序列缺失、捕获值遗漏三组变异均目标红/legacy 对照绿。
基于 `d488770`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；
`python3 scripts/probe-session-process.py --mutations` 四类进程、两类规模队列、
八类预算、十三类帧/15 类摘要 codec、34 进程/11 帧/14 预算/8 摘要/3 规模变异、
SHA256 回滚及二十六类 surface 基线通过；核心 gate 72/72、57 公共符号及快速
构建通过。日志 `tmp/process-command-owner-{verification,gate}.log`。
命令摘要尚未 wire 编码/客户端导入，prompt_start 的程序状态清理联动、真实
error 文本、UI 迁移与生产 daemon 仍欠，P0 未通过。

2026-10-20 将命令捕获快照传到客户端：新增 GCS1 显式编码，运行中、退出码
与耗时有效位分离，负 i32/大端 u64 与文本长度有独立字节 oracle。daemon 的
单调起点不进入 wire。拒绝未知 bit、保留字节、状态矛盾、越界/NUL/截断/尾随
数据；失败不改旧值。快照元数据扩为 216 字节，带命令长度和 SHA-256，所有
blob 总和仍限 16 MiB；客户端 wire 缓冲计费，READY(S) 前导入并核对运行中或
已结束/退出码 7。最终 EXPECT 导入下一条未知退出码命令，仅是诊断而非实时事件。

基于 `2988d29`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；13 类
命令 codec 基线/ASan/UBSan、八组 codec 与两组新增进程变异通过。完整
`python3 scripts/probe-session-process.py --mutations` 四类进程、两类规模、八类
预算、十三类帧/15 类程序表/13 类命令 codec，36 进程/11 帧/14 预算/8 程序表/
3 规模/8 命令变异（共 80）、SHA256 回滚及二十六类 surface 基线通过。
核心 gate 72/72、57 公共符号及快速构建通过。日志
`tmp/process-command-wire-{verification,gate}.log`、`tmp/command-wire-asan.log`。
命令 LIVE 更新、prompt_start 联动、真实 error 字段、产品 UI 和生产 daemon
仍欠，完整预算未完成，P0 未通过。

2026-10-20 补实时命令摘要：程序 dirty 或命令 start/end dirty 任一变更，
owner 将输出、程序表和命令值作为一个同序号队列项编码；writer 不回读权威
当前值。paired header 两个 u64 长度，更新 metadata 88 字节，两份摘要有
独立长度/SHA-256。客户端均导入后推进流 cursor，仍不声称通用 UI 原子发布。

真实 shell 的 OSC 7501 done/THIRD 与后续仅 OSC 133 下一条 start/end/FOURTH
用 fixture gate 分开；owner 观察 THIRD 才投递门控换行，确保不同 read/apply。
客户端必须在最终 EXPECT 导入前确认下一条已停止、退出码未知、耗时有效。
移除 command dirty 和损坏实时命令 hash 两组变异目标红/legacy 对照绿。

最大配对容量固定增为 90804 bytes，最大实际程序表/输出/4096+4096 命令错误
组合为 90676 bytes，最大规模/累计背压的完整相关 C ASan/UBSan 通过。
基于 `42cdc82`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；完整
`python3 scripts/probe-session-process.py --mutations` 四类进程、两类规模、八类
预算、十三类帧/15 类程序表/13 类命令 codec，38 进程/11 帧/14 预算/8 程序表/
3 规模/8 命令变异（共 82）、SHA256 回滚及二十六类 surface 基线通过。
核心 gate 72/72、57 公共符号及快速构建通过。日志
`tmp/process-command-live-{verification,gate}.log`、`tmp/command-live-scale-asan.log`。
prompt_start 联动、真实错误报告、统一预算/产品 UI/生产 daemon 仍欠，P0 未通过。

2026-10-20 补 prompt_start 程序表清理：私有 C 存储只在语义 kind=1 时移除
working/blocked，保留 done/error/idle（包括已删除父记录下的独立子记录）；
归还字符串字节预算，既有捕获副本不变，input_start 与其他 kind 不清理。
真实 PTY 依次写五条状态、OSC 133 B/A，权威回调核对 B 保留五条、A 剩三条；
随后显式清 temp 子树。中间状态仅在权威回调观测，不声称 GUI 显示过三条。
清理段另设 PROMPT 门控，再发仅 OSC 133 的 next command/FOURTH，保持
命令 dirty 独立性验证，避免同批程序更新掩盖命令遗漏。

基于 `bff32f9`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
程序存储七类基线、十六组定向变异和 SHA256 回滚通过；七类 ASan/UBSan 通过。
完整 process 套件四类进程及原有 codec/预算/规模基线、40 进程/11 帧/14 预算/
8 程序表/3 规模/8 命令变异（共 84）、二十六类 surface 基线及 SHA256 回滚通过。
核心 gate 73/73、57 公共符号对拍及快速构建通过。日志
`tmp/program-prompt-{mutations,gate}.log`、`tmp/process-program-prompt-verification.log`。
仅完成 P0 私有存储及探针接线；真实错误报告、统一预算、产品 UI、生产 listener/
manager/auth/writer 尚欠，P0 与生产 daemon 未完成。

2026-10-20 增加真实 OSC 133 错误文本链路证据：shell 在 detached 阶段发
`D;7;err=detached failure`，冻结的命令与新 GUI 快照必须保留退出码 7 及该文本；
随后在无程序表变化的门控段发 `D;err=unknown exit failure`，LIVE 客户端在
最终 EXPECT 之前必须已有该文本，退出码仍未知。错误文本不被当成退出码，
后续 owner 值也不能覆盖旧捕获值。该文本是 shell 不可信内容，未接产品显示。

基于 `05374d4`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；
完整 process 套件四类进程、原有帧/codec/预算/规模基线及 42 进程/11 帧/
14 预算/8 程序表/3 规模/8 命令变异（共 86）、26 类 surface 基线与 SHA256
回滚通过。新增丢弃 callback error 与 shell 漏报 LIVE error 两变异目标红、
legacy 对照绿；旧的漏报退出码变异保留 error 参数，确保只检验退出码。
核心 gate 73/73、57 公共符号及快速构建通过，日志
`tmp/process-command-error-{verification,gate}.log`。
这只覆盖 shell 错误文本，PTY/引擎/资源故障的生产报告、全局预算及产品 UI/
listener/manager/auth/writer 尚欠；P0 与生产 daemon 未完成。

2026-10-20 将 GUI 解码摘要纳入已有 wire 预算：私有程序存储增加可选分配域，
结构、固定条目数组和每条字符串全部经该域分配/按原长度释放，副本继承分配域；
默认入口保留 libc 行为，公共 ABI 不变。更新先分配新字符串再释放旧值，故
预算需包含瞬时 old+new；额度不足返回 NO_MEMORY 且旧值不变。分配器 context
必须活到所有原表与副本释放后，由调用方串行访问。

GPS1 新增带分配域的事务解码入口。GUI checkpoint 与 LIVE 解码均使用已准入
的 wire scope，接收缓冲、旧摘要和新摘要可同时计费；每次成功解码必须观测到
scope live 增长，关闭摘要后才能释放 lease。绕开该域的变异目标红、legacy 绿。
固定解析栈、命令 POD、owner 默认分配与其他宿主对象尚未因此纳入总 ledger。
首轮进程基线因旧的“快照释放后 wire 为零”断言失败；现改为精确核对持久摘要
余量，关闭摘要后必须归零，并以遗漏释放变异验证该检查。随后完整重跑。

基于 `33c98fb`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0；
程序存储八类基线、21 组变异、SHA256 回滚与 ASan/UBSan 通过。故障注入覆盖
两步创建、四步复制、替换额度耗尽，原状态保留且每次失败无净分配；精确释放
尺寸与独立副本回收亦核对。完整 process 套件 44 进程/11 帧/14 预算/8 程序表/
3 规模/8 命令变异（共 88）、四类进程及 26 类 surface 基线、SHA256 回滚通过。
核心 gate 74/74、57 公共符号及快速构建通过，日志
`tmp/program-allocator-{mutations,gate}.log`、`tmp/process-program-allocator-verification.log`。
统一资源总预算、低层故障报告及生产 daemon/产品 UI 尚欠，P0 未通过。

2026-10-20 将权威程序表和捕获摘要接入 daemon 现有预算：原表在 source
scope 分配，冻结副本显式选 encoder scope，后者活到任务结果释放，不能因
capture 中间态释放而提前关闭。source 在引擎释放后仍持有原表，最终原表释放
后才关闭；每轮 encoder 关闭会验证副本和快照分配均已归还。

私有 C 新增指定目标分配域的 copy 入口，默认 copy 仍继承源域；显式 NULL
选 libc。无效回调返回 INVALID，目标额度或分配失败返回 NO_MEMORY、*out=NULL，
源值与源额度不变。原表关闭不影响独立目标副本。真实 owner 核对原表占用 source
scratch、副本增加 encoder live 且不增加 source live；绕过 source、让副本继续
占 source、遗漏原表释放三变异各自目标红/legacy 绿。

基于 `7c73674`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
九类程序存储基线、23 变异与 SHA256 回滚及 ASan/UBSan 通过；目标域四步分配
故障与额度拒绝均核对回收。首轮 process 在第 23 项后因旧复制调用的变异锚点
失效而退出；更新锚点并逐一预检 47 个定位唯一后，重新执行完整套件。完整 process 四类进程、47 进程/11 帧/14 预算/
8 程序表/3 规模/8 命令变异（共 91）、26 类 surface 基线与 SHA256 回滚通过。
核心 gate 75/75、57 公共符号及快速构建通过，日志
`tmp/program-domains-{mutations,gate}.log`、`tmp/process-program-domains-verification.log`。
固定栈、队列/任务/worker 等对象的总预算和低层故障报告、生产 daemon/产品 UI
仍欠；这不等于已完成进程总 ledger 或 P0。

2026-10-20 将 attachment 控制器、恢复字节环和事件数组接入同一私有分配域。
新增通用 `base/allocator.h`，程序表的原私有类型作为别名保留，公共 ABI 不变。
默认构造仍用 libc；预算构造覆盖三块内存，创建任一步失败均回收已分配部分。
字节环/事件上限仍独立于预算，detach、溢出撤销及新 generation 复用保留固定
数组和已占额度，BUSY close 不归还，真正关闭按原分配尺寸归还全部额度。

真实 daemon 探针在 worker 启动前以 source scope 创建 attachment，核对用量
增加；结束时先关闭 attachment 再关闭 source。权威摘要创建的检查改为相对
增量，避免已计费队列掩盖“摘要绕过预算”故障。新增整个队列绕过预算的变异，
旧的摘要绕过预算变异仍必须失败。固定栈、任务池/worker/peer 等分配仍欠总预算。

基于 `70208e6`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
restore 九类基线/19 变异、attachment 十二类基线/18 变异、程序表九类/23 变异
及 SHA256 回滚通过。新分配用例的完整 restore/attachment/task 实现 ASan/UBSan
通过。失败注入覆盖两步 ring/三步 controller 初始化、精确释放尺寸、额度拒绝、
BUSY 保留、溢出撤销与跨代复用不追加分配。
完整 process 48 进程/11 帧/14 预算/8 程序表/3 规模/8 命令变异（共 92）、
四类进程及 26 类 surface 基线、SHA256 回滚通过。核心 gate 77/77、57 公共
符号及快速构建通过。日志 `tmp/queue-budget-{restore-mutations,attachment-mutations,program-mutations,gate}.log`、
`tmp/process-queue-budget-verification.log`。总 ledger、低层故障报告及生产
listener/manager/auth/writer、产品 UI 尚欠；P0 与生产 daemon 未完成。

2026-10-20 将任务池与 worker 的堆分配接入 source scope：私有 budget 构造
覆盖任务池（含 16 个固定槽位）、worker（含固定请求队列与 64 KiB 回复缓冲）
及每次复制的 input/resize payload。默认构造仍用 libc，公共 ABI 不变。回调
context 活到 close/join，worker 的 submit 与完成回收跨线程，分配器须线程安全。

任务池 BUSY close 保留全部额度，实际 close 在 join 后按原尺寸释放。worker
请求额度不足返回 NO_MEMORY，不入队、不接收输入所有权、不调完成回调；取消
与完成按原 payload 长度释放，join 归还 worker 对象。构造额度拒绝不启动线程。
探针在 source 中创建两者；worker 的相对计费按 scratch 类比较，避免引擎其他
分配掩盖 worker 绕过预算。两个新增 process 变异各自目标红/legacy 绿。

基于 `deb5d32`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
worker 十类基线/17 变异，任务池九类基线/14 变异与 SHA256 回滚通过。新用例
验证无效回调、构造额度拒绝、输入拒绝无回调、后续满额接受、取消回收、借出结果
时 BUSY 保留及最终精确释放。worker/state-owner/PTY 与 task 的相应用例
ASan/UBSan 通过；独立 worker 编译最初缺 Darwin 宏和辅助符号，补齐
`-D_DARWIN_C_SOURCE` 并链接 core 静态库后通过，其他 core 辅助对象未全量插桩。
完整 process 50 进程/11 帧/14 预算/8 程序表/3 规模/8 命令变异（共 94）、
四类进程及 26 类 surface 基线、SHA256 回滚通过。核心 gate 79/79、57 公共
符号及快速构建通过。日志 `tmp/runtime-heap-{build,worker-mutations,task-mutations,gate}.log`、
`tmp/process-runtime-heap-verification.log`。OS 线程栈、peer、backend/宿主对象等
仍欠总 ledger；低层故障报告、生产 listener/manager/auth/writer 与产品 UI 未完成，
P0 未通过。

2026-10-20 将 IPC peer 对象纳入 source 预算：私有 adopt-with-allocator
入口覆盖单个 peer 对象，默认入口仍用 libc、公共 ABI 不变。无效回调返回
INVALID_ARG；分配/额度失败沿既有契约返回 INTERNAL、out=NULL，不改 fd 标志/
所有权或 attachment generation。成功接管后，HUP/EOF 关闭 fd 并撤销旧代，
对象及额度保留到显式 close，旧对象 close 不得撤销已经建立的新代。

真实 IPC owner 的专用 adapter 转发到 source allocator，独立记录 peer
请求字节，避免 worker 并发释放载荷影响“peer 是否入账”的相对检查。所有
peer 回调仍在单一 I/O owner 上；旧/新 peer 可短暂共存，最终 peer 请求额度
及 source scope 都须清零。adapter 统计是请求尺寸，source 预算仍按映射页计费。

基于 `a467a6f`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
十四类 peer 基线/17 变异、SHA256 回滚通过；新增对象绕过预算和错误释放尺寸
两变异目标红/flow 对照绿。新增用例覆盖无效回调、分配/额度拒绝、socket 标志
与 generation 不变、HUP 后对象保留和新代隔离；peer/attachment/task/restore
实现的相应用例 ASan/UBSan 通过（链接的其他 core 辅助对象未全量插桩）。
完整 process 51 进程/11 帧/14 预算/8 程序表/3 规模/8 命令变异（共 95）、
四类进程及 26 类 surface 基线与 SHA256 回滚通过。核心 gate 80/80、57 公共
符号及快速构建通过。日志 `tmp/peer-budget-{mutations,gate}.log`、
`tmp/process-peer-budget-verification.log`。OS 线程栈/socket buffer、backend 和
宿主固定对象等仍欠总 ledger；低层故障报告与生产 listener/manager/auth/writer、
产品 UI 未完成，P0 未通过。

2026-10-20 增加显式 POSIX 线程栈/guard 属性与准入：私有 thread-options
头校验正数页倍数、组合溢出，再用 pthread 属性创建线程，失败不回退 OS 默认。
任务池/worker 新私有 options 构造入口保留原 allocator/default 入口，公共 ABI
不变；属性创建/销毁不接管外部分配器，线程容量由宿主在启动前预留到 join 后。

真实 daemon 探针为 worker/encoder 各预留 1 MiB+两页，source 从 64 MiB 减为
61.9375 MiB（本机页 16 KiB），四个原有 scope 加两个线程容量仍最多承诺
256 MiB。线程 scope 的 committed 有值、live 为零：这是 OS 线程容量预留，
不是伪造一次 allocator 分配。在 open_state/encode 内核对当前栈局部变量范围、
Darwin 实际报告栈大小和 Mach 查询到的不可访问 guard；实际栈加请求 guard
必须装得进预留。worker join 与 task close/join 后才分别关闭预留 scope。

本机请求 1048576、实际报告 1060864、guard 映射 16384、每线程预留 1081344
bytes。额外 12288 不当成跨平台常量；未来 OS 舍入超出预留必须让探针失败。
独立 tmp 探针观察 join 后原栈映射不再覆盖原地址，仅是当前宿主证据，非其他
libpthread/cache 行为证明。TLS、pthread 内部对象与 backend/GUI 线程仍需计费。

基于 `c903ae3`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
线程三类基线/六变异与 SHA256 回滚通过，分别护 stack/guard 属性、真实 guard
映射、创建时丢弃属性、零 guard 与组合溢出；native 栈/guard 映射断言仅在
Darwin 执行，其他 POSIX 只验证属性/创建。三类 ASan/UBSan 通过，使用
`ASAN_OPTIONS=detect_stack_use_after_return=0` 让局部地址观测真实栈。worker
十基线/17 变异、task 九基线/14 变异及回滚保持通过。
完整 process 55 进程/11 帧/14 预算/8 程序表/3 规模/8 命令变异（共 99）、
四类进程及 26 类 surface 基线、SHA256 回滚通过。新增 worker/encoder 忽略
属性与未预留容量四变异各自目标红/legacy 绿。核心 gate 83/83、57 公共符号
及快速构建通过。日志 `tmp/thread-budget-{build,options-mutations,worker-mutations,task-mutations,gate}.log`、
`tmp/process-thread-budget-verification.log`。总进程 ledger、backend/宿主对象及
低层故障报告、生产 listener/manager/auth/writer 与产品 UI 未完成，P0 未通过。

2026-10-20 将 PTY 后端自有堆分配接入 source：私有 allocator 构造覆盖
句柄、可执行路径、登录 argv[0]、argv/环境指针数组、TERM 字符串及输入尾队列。
最大对齐的分配头保存分配域和原尺寸，释放精确归还（含头部），父进程临时数据
在 fork 后回收，子进程不调用新分配器，继续原有 async-signal-safe 操作。默认
libc 路径保留 realloc，预算路径扩容先新后旧，峰值同时计旧/新两份。

启动任一步失败完整回收；macOS 登录 argv[0] 分配失败明确报错，不静默以普通
shell 继续。尾队列上限 4 MiB 用减法检查避免加法溢出，额度失败保留旧队列。
这里保证的是队列替换事务性；write 报错前仍可能已送出部分前缀，不承诺整个
写请求原子回滚，也不允许盲目重试。真实 daemon adapter 转发 source allocator
并单独计后端请求字节，worker join 后该计数必须归零。

基于 `b72369f`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
四类预算基线/八变异与 SHA256 回滚通过：逐步启动失败、额度拒绝、临时数组/
对象回收、尾队列扩容 OOM 保留与后续成功、超上限无分配、两段共 2 MiB 的
真实 PTY/cat 字节顺序。分配器绕过/尺寸错/复制丢内容/OOM 丢旧队列/上限失效/
临时数组或对象泄漏/登录 OOM 降级八变异各目标红、独立对照绿；最后一项仅
Darwin 执行，其他 POSIX 不冒称已验 macOS 登录规则。
四类 ASan/UBSan 通过（PTY 实现插桩，链接的其他 core 辅助对象非全量插桩）。
原有登录 shell 11 项检查、transport 12 项检查、PTY inheritance 两变异及回滚
通过；登录变异释放也同步使用分配头约定，保持变异可编译且有指向性。
完整 process 56 进程/11 帧/14 预算/8 程序表/3 规模/8 命令变异（共 100）、
四类进程及 26 类 surface 基线与 SHA256 回滚通过。核心 gate 87/87、57 公共
符号及快速构建通过。日志 `tmp/pty-budget-{build,mutations,inheritance-mutations,login-mutations,transport-mutations,gate}.log`、
`tmp/process-pty-budget-verification.log`。waiter 的 OS 栈、libc passwd/TLS/事件
队列内部对象及其他宿主资源仍欠总账；低层故障报告、生产 daemon/产品 UI
未完成，P0 未通过。

2026-10-20 将 PTY waiter 的栈/guard 纳入准入：私有 options 构造保留原
default/allocator 入口，显式属性在任何自有分配与 fork 之前完成 pthread 校验。
创建线程仍使用同一设置；OS 线程创建失败继续原有 kill/reap/关 fd/释放句柄路径。
新增原生 C 私有线程查询返回借用的 waiter ID，调用方不得 join/接管它，非 PTY
明确返回 UNSUPPORTED；失败输出清零。无公开 ABI/平台绑定变更。

headless owner 查询真实 waiter，核对独立线程 ID、Darwin 栈范围/guard 映射
及预留容量；worker/encoder 的当前线程观测仍保留局部地址检查。三条线程各
预留 1081344 bytes，本机 waiter 请求/报告/guard 分别 1048576/1060864/16384。
source 相应从 61.9375 MiB 调为 60.90625 MiB（三条各 1 MiB+两页），加其余
scope 的总承诺仍为 256 MiB；线程 scope 只计容量 committed，不伪造 live。
backend close 已 join waiter，worker join 后才关闭 waiter 栈预留，encoder
预留继续到任务池实际 close/join。

基于 `b0b165b`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
五类 PTY 预算基线/12 变异及 SHA256 回滚通过，新增真实 waiter 栈、非法属性
在 fork 前拒绝、错误线程 ID 和非 PTY 假支持断言；栈映射与忽略属性变异仅
Darwin 执行，不外推 Linux 的原生映射证据。五类 ASan/UBSan 通过，PTY 实现
插桩，其他链接 core 辅助对象非全量插桩；使用关闭 fake stack 的原有配置。
PTY inheritance 两变异及回滚保持通过。
首轮脚本扩展误加旧变异元组字段导致解包失败，修正后重跑；复核又将完整 pthread
属性预校验提前到 fork 前，并停止未完成的首轮 process 后完整重跑，不计首轮
为通过。最终 process 四类进程、58 进程/11 帧/14 预算/8 程序表/3 规模/
8 命令变异（共 102）、26 类 surface 基线与 SHA256 回滚通过。新增 waiter
忽略属性/缺少准入两变异各自目标红、legacy 对照绿。核心 gate 88/88、57 公共
符号及快速构建通过。日志 `tmp/waiter-stack-{pty-mutations,inheritance-mutations,gate}.log`、
`tmp/process-waiter-stack-verification.log`。TLS/libc 内部对象、其他宿主资源的
账目及低层故障报告、生产 listener/manager/auth/writer 与产品 UI 尚欠，P0 未通过。

2026-10-20 增加 worker 可查询终态：固定 POD 保存 STOPPED/EOF/ERROR、原始
状态码、失败阶段、最后成功权威 apply 序号及退出观测。初始化前序号未知不当 0，
初始化后的合法序号 0 保持已知。结果在 backend/state/排队请求清理后、finished
通知前加锁发布一次，后续查询只拷值；PENDING/INVALID 清输出，READY 后到 join
前不再变化。finished 仍可早于 start 返回，使用回调内查询者必须先安全发布句柄。
记录不含终端字节、命令或环境文本，最后 apply 序号不是 GUI 已应用 ACK，也不
证明发生 apply 失败后的当前引擎状态还能恢复。

失败阶段区分 state open/backend open/read/input/resize/apply/reply/capture，
保留具体 GT 错误，不统一涂成 IO。合法 freeze 失败只回该任务，空 frozen 的
协议违约则致命；两种行为分别断言。已有 backend factory 只返回指针，其失败
仍沿旧契约报 IO，未伪造更细原因。close 的 void 契约亦尚无独立错误报告。

新增私有 observe_exit 与 legacy exited() 分离。PTY waiter 保存真实 CODE 或
SIGNAL 及原值；worker EOF 前只消费显式观测，不用旧的 128+signal 或 SSH 的
占位 0 推断退出成功。观测 NOT_FOUND=未知、缺回调=UNSUPPORTED、非法成功
记录=PROTOCOL，各由 termination_status 明示；EOF 字节流正常结束与程序是否
成功是两回事。正常退出 0/7、真实 SIGTERM=15、未知和缺观测分别验证。现有
SSH shell 的 legacy exited 仍填 0，未改公共行为，新观测缺失明确 UNSUPPORTED；
不声称 SSH 退出语义已经迁移完成。

基于 `9fc23e2`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
17 类 real-PTY/fault-adapter 基线、18 变异与 SHA256 回滚通过，覆盖失败阶段、
回调前发布/重入查询、已知/未知/未支持退出观测、信号与退出码分离、局部/致命
捕获失败、query pending 与终态不变。故障码由真实 PTY 外的适配器注入，未冒充
内核真实 read/write 失效。17 类 worker/state-owner/PTY 实现 ASan/UBSan 通过，
其他 core 辅助对象非全量插桩；原 worker 十类/17 变异与回滚通过。
复核发现 legacy exited 语义不足后，停止未完成的首轮 process，补独立观测与
信号/未知/未支持测试后完整重跑，不计首轮为通过。
最终 core gate 105/105、57 公共符号及快速构建通过；process 四类进程、59 进程/
11 帧/14 预算/8 程序表/3 规模/8 命令变异（共 103）、26 类 surface 基线及
SHA256 回滚通过。host 在 finished 后、join 前复制结果并在 join 后核对最终
权威序号；漏收结果变异目标红/legacy 绿。日志
`tmp/worker-result-{build,baselines,mutations,legacy-mutations,gate}.log`、
`tmp/process-worker-result-verification.log`。结果尚未编入 IPC/产品状态，整套
故障通知/关闭事务、总 ledger、生产 listener/manager/auth/writer 与 UI 仍欠，
P0 与生产 daemon 未完成。

2026-10-20 将 worker 终态接入 P0 IPC：新增固定 56-byte GWR1、PW_RESULT，
显式大端编码 generation/权威序号/结束类型/失败阶段/GT 状态/退出观测与已知位，
不传 native padding 或终端文本。拒绝 pending kind、未知 flags/枚举/保留位、
隐藏序号/退出码、状态矛盾、零 generation、截断/尾随；失败输出清零，编码
容量不足不写正文。初始化失败不能伪装已知序号，CODE 0 与未知/未支持仍分开。
实验 build contract 改为 process-terminal-result-1，旧实验构建不能混接。

owner 完成 worker/资源清理并取到终态，先排空所有已成功应用的增量，再发一条
结果。客户端校验 generation、frame sequence、payload sequence 与本地 cursor
一致，只接受一条终态；之后拒收 output/resize，进入本地 ended 输入门禁。
最终 EXPECT 只做诊断对拍，不把已结束会话改回 LIVE。原生窗口 title 属性随
终态更新，未将此当作窗口文字像素验收。终端字节仍在 IO 消费线程直达 surface。

新增 read-error 场景：在真实 PTY 已完成 FOURTH 之后，以适配器注入 read error，
不发送 stop；真实 worker 结束/关闭后产生 READ/IO 报告。新 Metal GUI 在 EXPECT
之前收到 ERROR，保留最后有效网格、更新结束属性，并用实际 surface text API
验证 LATE 不再进入输入缓冲。故意移除门禁时先清空旧缓冲，避免旧输入 metadata
检查掩盖门禁故障。此处是故障适配器，不声称复现内核真实 read 失败。

基于 `8016f40`，Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0。
GWR1 十一基线/十一变异、SHA256 回滚与 ASan/UBSan 通过，含独立字节 oracle、
u64 最大值、CODE 0、SIGNAL、未知/未支持观测和事务失败。完整 process 五类
进程（含 read-error）、66 进程/11 帧/14 预算/8 程序表/3 规模/8 命令/11 终态
编码变异（共 121）、26 类 surface 基线及 SHA256 回滚通过。新增遗漏结果、
错代、重复结果、结束后输出、晚输入、窗口 title 未接和读错误折叠 EOF 七项
均目标红、独立对照绿。核心 gate 105/105、57 公共符号及快速构建通过。日志
`tmp/result-wire-mutations.log`、`tmp/result-ipc-gate.log`、
`tmp/process-result-ipc-verification.log`。
当前原生场景只覆盖 LIVE 后主动停止与读故障，EOF/信号/未知/未支持由 codec/
core 用例验证；启动前失败、apply 破坏状态后的诊断、恢复期终态、生产 listener/
manager/auth/writer、产品 UI 和完整总 ledger 尚欠。尚未新增 controller ENDED
相位；收到结果前的在途输入仍须依赖 worker STOPPED 拒绝，生产协议需明确回复，
不能因这次本地门禁便声称竞态消失。P0 与生产 daemon 未完成。

2026-10-20 的结束输入封锁（基于 `e9aac97`）：私有 controller 新增会话级
只读锁存及 ENDED 相位。已知最终序号的 end 立即撤销输入许可，保持当前连接
资格与已接受尾部；恢复尚未完成或最终事件尚未入队时返回 WAIT，输入仍封锁。
最终事件齐全且恢复至 LIVE 后再进入 ENDED，继续排空。重复相同终态幂等，
旧代、不同终态、越过最终序号的快照/事件及迟到握手均不得改写终态或丢尾部。
锁存跨 detach/reattach 保留；保存状态的只读重接由 C fixture 验证，尚非完整
已结束 GUI 的重连证据。此入口只接受已知最终序号，未知不能伪填零。

P0 在 worker join 后、发送末尾数据及结果前封锁 controller；探针专用
PW_PRE_END_INPUT 同步让尚未知结束的 GUI 用真实引擎编码 INFLIGHT，再以合法
代号及已应用序号发送。当前 peer 仍有效，输入被明确回复 PW_INPUT_REJECTED
（16 字节：generation 8、GT_ERR_CANCELLED 4、PW_INPUT_ENDED 4），不提交给
已结束 worker。随后旧尾部及 GWR1 仍正常到达；收到结果后的本地 LATE 门禁
继续验证。同步帧只是实验时序控制，不是生产协议设计。

Darwin 25.6.0 arm64、Xcode 27.1、Apple clang 21.0.0 下，核心 gate 108/108、
57 公共符号通过；restore-stream 10 基线/25 变异、attachment 14 基线/25 变异
均目标红、独立对照绿、SHA256 回滚复绿。三项新增结束用例经 ASan/UBSan
通过。日志 `tmp/ended-gate.log`、`tmp/ended-restore-mutations.log`、
`tmp/ended-attachment-mutations.log`。恢复途中 end 的延迟完成有核心证据，
原生五类场景及 68 进程/11 帧/14 GUI 预算/8 程序表/3 规模/8 命令/11 终态
编码变异（共 123）、26 类旧 surface 基线、SHA256 回滚全部通过，日志
`tmp/process-ended-verification.log`。原生 GUI 场景仍仅验证 LIVE 后终态；
恢复期故障、启动前未知序号失败、生产
listener/manager/auth/writer、产品 UI 与完整资源账本仍欠，P0 未完成。

2026-10-20 的恢复期终态原生证据（基于 `adb4f60`）：新增 `recovery-error`。
快照 S 已冻结并发出、controller 尚在 SNAPSHOT 时，探针向真实 PTY 注入
待处理命令输入使其推进至 FOURTH，再用既有 read 故障适配器结束 worker。
在处理 READY(S) 前查询 READY 结果并调用 end，必须返回 WAIT、锁存最终序号
与只读状态，保留原快照和增量。不是操作系统自行产生 read 错误的证据。

真实 Metal GUI 导入旧快照后继续 READY(S)/S..L/READY(L)，最终序号已入队，
所以 L 就等于终态序号；不再要求 LIVE 后额外产生数据，仍要求 cursor 精确等于
权威最终序号。握手完成不能重开输入；end 随后完成为 ENDED，相同代号/有效
应用序号的真实引擎编码 INFLIGHT 被明确拒绝，再发送 GWR1。GUI 在 EXPECT
之前核对程序/命令摘要、错误结果和最后网格，最终蓝色像素及本地 LATE 门禁
仍通过。控制器在恢复中结束、界面完成恢复后收到终态；尚非失败时中止恢复、
初始化前未知序号错误或已损坏 apply 状态的诊断方案。

`python3 scripts/probe-session-process.py --case recovery-error` 通过，
S=4、L=8、final=8，日志 `tmp/process-recovery-first.log`。同一 Darwin 25.6.0
arm64 / Xcode 27.1 / Apple clang 21.0.0 环境下，完整六类原生场景、70 进程/
11 帧/14 GUI 预算/8 程序表/3 规模/8 命令/11 终态编码变异（共 125）、
26 类旧 surface 基线及 SHA256 回滚通过；新增省略锁存、篡改最终序号两项
均 recovery-error 目标红、roundtrip 对照绿。核心 gate 108/108、57 公共符号
对拍通过。日志 `tmp/process-recovery-verification.log`、`tmp/recovery-end-gate.log`。
生产 listener/manager/auth/writer、产品 UI 与完整资源账本仍欠，P0 未完成。

2026-10-20 的 apply 失效清理（基于 `4f19fb6`）：worker 的私有配置新增可选
`state_invalidated(state, error, last_sequence)`。仅 sticky owner failure 触发，
backend 关闭后、close_state 之前同线程调用；传精确 owner 错误与最后成功序号，
通知时结果仍 PENDING。普通 backend/read/input 错误和可恢复的 capture OOM
不误标状态损坏，close_state 仍执行一次。回调必须有界、不 join、不导出状态；
最后成功序号不能证明当前引擎尚有效。公开 ABI 与 GWR1 字节格式未改。

P0 适配器接收通知后禁止最后快照导出/元数据读取。新增 `apply-error`：在
FOURTH 已成功发布后，通过故障 read 适配器喂入清屏及 POISON 字节；apply
在实际引擎 feed 成功后故意返回 IO，不发布事件、不推进成功序号。为构造独立
oracle，探针在破坏前保留规范/资源快照；失效后不从引擎重导出，销毁并报告
ERROR/APPLY/IO。GUI 仅排空有效事件，对齐最后成功序号并保留蓝色网格，在
EXPECT 之前得到结果与摘要，LATE 输入仍拒绝。该 oracle 是试验夹具，不是
生产自动回滚或通用 apply 恢复方案，也不证明上游错误必定在同一点发生。

独立 `--case apply-error` 通过；worker-result 17 基线/23 变异、SHA256 回滚
通过，新增遗漏通知、丢失错误、错误推进序号、误报健康状态及销毁后通知五项
均目标红/独立对照绿；apply/capture-null/capture/read 四例 ASan/UBSan 通过。
日志 `tmp/process-invalid-first.log`、`tmp/state-invalidated-mutations.log`。
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
108/108、57 公共符号对拍通过；完整七类原生场景、73 进程/11 帧/14 GUI
预算/8 程序表/3 规模/8 命令/11 终态编码变异（共 128）、26 类旧 surface
基线及 SHA256 回滚通过。漏接通知、损坏状态仍导出、部分失败谎报成功三项
均 apply-error 目标红、read-error 对照绿。日志 `tmp/state-invalidated-gate.log`、
`tmp/process-invalid-verification.log`。
启动前未知序号、reply flush 失败时权威序号与已发布序号的差异、生产故障诊断/
listener/manager/auth/writer、产品 UI 与完整资源账本仍待完成，P0 未通过。

2026-10-20 的 reply 失败发布修复（基于 `9772e27`）：成功 apply 已推进
权威序号，reply flush 失败不能再提前返回并吞掉该事件。worker 保留真实
REPLY 错误，在 flush 尝试之后仍发布成功事件和 output，然后结束；apply
本身失败时不发布，已发送 reply 不重复重试。输出回调以成功序号是否推进
为准，不能以最终 rc==OK 为准。通知结果中的序号现在仍能对应已发布流，
不把 reply I/O 错误当成引擎失效。没有改变公开 ABI 或 GWR1 格式。

新增真实 Metal GUI `reply-error`：FOURTH 后故障适配器提供 DSR 状态查询，
权威引擎实际产生 CSI 0 n，backend 的这次 reply write 明确返回 IO；原来
成功发送的查询响应不受影响。成功解析的查询仍作为最后事件发布，S=4、
L=5、final=9，controller 可完成 ENDED、排空到最后序号再发 REPLY/IO。
GUI 应用查询时不产生第二份应答，保留最终网格；本地晚输入与在途输入均
继续拒绝。该故障由适配器注入，不声称复现真实内核写失败或已投递前缀。

独立 `--case reply-error` 通过，日志 `tmp/process-reply-first.log`。
worker-result 17 基线/26 变异及 SHA256 回滚通过，新增事件丢失、字节丢失、
重复发布三项均 reply 目标红、独立对照绿；既有 session-worker 10 类基线与
全部定向变异回滚复绿。reply/apply/read/capture-null 四例 ASan/UBSan 通过。
日志 `tmp/reply-publication-mutations.log`、`tmp/reply-worker-mutations.log`。
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
108/108、57 公共符号对拍通过；session-worker 共 17 项变异回滚复绿。
完整八类原生场景、75 进程/11 帧/14 GUI 预算/8 程序表/3 规模/8 命令/
11 终态编码变异（共 130）、26 类旧 surface 基线与 SHA256 回滚通过；
遗漏最后查询、丢弃失败 reply 对应事件两项均 reply-error 目标红、read-error
对照绿。日志 `tmp/reply-publication-gate.log`、`tmp/process-reply-verification.log`。
启动前未知序号错误、生产诊断/监听器/manager/auth/writer、产品 UI 及完整
资源账本仍待完成，P0 未通过。

2026-10-20 的启动失败未知序号控制器契约（基于 `172f237`）：新增私有
`gt__attachment_end_unknown`，调用方仅能在 worker 结果 `has_sequence=false`
时使用，不能从数值零推断未知。只允许 capture/submission 之前，锁存会话
只读与 `has_terminal_sequence=false`，保留当前 peer 资格用于错误投递。
没有虚构数据流或 S/L：内部 phase 保持 CAPTURING，拒绝 begin/push 和后续
已知 end（包括零）；未知锁存跨 detach/reattach 保留，重复调用幂等。已有
已知零不能降为未知，已开始快照也不能改走该入口。已知结束接口现在明确
置 `has_terminal_sequence=true`，查询 POD 同时返回该判别位。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
109/109、57 公共符号对拍通过；attachment 15 类基线/33 项变异、SHA256
回滚复绿，其中八项新增变异均 unknown-end 目标红、nojob 对照绿。
unknown-end/ended/end-wait 经 ASan/UBSan 通过，原生 recovery-error 重新编译
运行通过（S=4、L=8、final=8）。日志 `tmp/unknown-end-gate.log`、
`tmp/unknown-end-mutations.log`、`tmp/unknown-end-native.log`。
新增 unknown-end 核心用例验证上述边界、旧代拒绝和重接；这只是控制器契约，
尚未实现无 surface 的启动错误 GUI/IPC 分支，也没有从 CAPTURING 假报 LIVE。
启动错误详情仍由 worker result 保存，上层须显式发送错误并结束恢复等待。
生产监听器/manager/auth/writer、产品 UI 与完整资源账本仍欠，P0 未通过。

2026-10-20 的无快照启动失败原生分支（基于 `29d7c40`）：新增
`start-error`，真实 worker 的 open_state 返回 IO，未建立 state/backend，
结果为 ERROR/STATE_OPEN/IO、has_sequence=false。join 后调用未知结束入口，
保持 peer 资格，在继承 socketpair 上直接发送 GWR1/PW_RESULT；帧序号零只是
无数据流的占位值，payload 判别位仍未知，未发送 SNAPSHOT/READY/LIVE。

GUI 的 I/O 线程在首帧分支验证终态后确认，不等待快照或构造 terminal surface。
主线程显示启动失败窗口及未知最终序号文本，断言窗口属性、surface/budget
为空且终端预算池无 reservation，随后退出并让 peer 回收本代。窗口属性是
UI 接线证据，不是像素验收。此场景的 bootstrap worker/controller 使用默认
分配器，尚未纳入完整资源账本；没有生产 listener、发现、身份认证或产品 UI。
独立 `--case start-error` 原生编译运行通过，日志 `tmp/process-start-first.log`。

脚本新增 `--startup-mutations` 定向运行模式，以 start-error 为目标、普通
roundtrip 为对照；也将启动场景与变异纳入完整 --mutations 的后续运行范围。
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --startup-mutations` 的三项变异均
start-error 目标红、roundtrip 对照绿，SHA256 回滚及两类原生基线复绿；
日志 `tmp/process-start-mutations.log`。核心 gate 109/109、57 公共符号对拍
通过，日志 `tmp/start-error-gate.log`。现有 75 个进程变异锚点逐项唯一；
本切片未重跑完整进程变异集，不将脚本新增的完整运行范围算作通过证据。
完整 daemon 与 P0 仍未完成。

2026-10-20 的启动失败资源预算（基于 `b7811eb`）：start-error 的 executor、
controller/restore arena、worker 和 peer 改用同一 source 分配域；各对象创建后
必须观察到对应 live charge。总准入 4 MiB，其中 source 1 MiB，worker 与
executor 各预留 1 MiB 加两页容量（guard 及对齐余量）。实际失败 worker 的栈参数在
open_state 线程内验证；executor 在本场景没有任务，保留配置额度至 pool join，
不将此表述为本场景实测了其线程栈区域。

worker join 后 source 必须回到 controller/executor 基线，再释放其栈额度；
peer 关闭后回到同一基线，controller/pool 释放后 source 与栈 scope 全部关闭，
最终 live/committed 归零且映射回收对称，输出 bootstrap peak 指标。无需
backend、waiter 或 VT 额度。本次仅覆盖该失败场景中显式纳入的对象与线程容量，
不包含主线程栈、pthread/libc 内部开销、GUI/AppKit/renderer 或进程 RSS 总量；
生产全局准入、完整资源账本与 daemon 仍未完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，定向
`--startup-mutations` 十项均 start-error 目标红、roundtrip 对照绿，SHA256
回滚与两类原生基线复绿；七项新变异覆盖四类分配域绕过、worker 栈未预留、
peer 不释放及栈额度泄漏。实测 bootstrap peak=147456、committed peak=3211264
字节，最终 live=0、committed=0。核心 gate 109/109、57 公共符号对拍通过。
日志 `tmp/process-start-budget.log`、`tmp/start-budget-gate.log`。现有 75 个
进程变异锚点保持唯一，本次未重跑完整进程变异集。

2026-10-20 的 GUI I/O 线程容量（基于 `00b72a3`）：共享 GUI pool 增加独立
opaque runtime reservation，与 surface 的 general/pages/wire 额度竞争同一
总预算。reserve 拒绝时清空输出并关闭失败 scope，unreserve 归还容量；租约
存在时 pool close 返回 BUSY。计数随 pool 快照返回，API 使用方必须先停止并
join 借用线程再释放；租约不是 OS 分配器，不自动探测线程是否还活着。

独立 runtime 用例在 12 页总量下先预留 7 页，证明 6 页 surface 拒绝；归还后
surface 成功，再证明反向 runtime 准入拒绝；失败不影响原租约，pool 保持可
回收。P0 GUI 客户端为 process_consume 预留 1 MiB 加两页容量，线程明确设置
1 MiB stack/一页 guard，并在线程内核验实际栈尺寸及租约。总策略变为原有
320 MiB surface 容量加该 I/O 额度，两 surface 的准入观测包含此额度。普通
恢复与无快照启动失败都经 join 后归还；SIGKILL 仍由操作系统回收进程资源，
本切片不声称走过正常 unreserve。

新增 `--runtime-mutations`：五项 C 预算变异以 runtime 为目标、valid 为对照；
默认线程栈回退以 start-error 为目标、旧 surface 原型为对照。Darwin 25.6.0
arm64 / Xcode 27.1 / Apple clang 21.0.0 下，六项均目标红、独立对照绿，
SHA256 回滚、九类 GUI 预算基线及 start-error/roundtrip 原生复验通过，日志
`tmp/gui-runtime-verification.log`。runtime/valid/rollback/race 四例 ASan/UBSan
通过；核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/gui-runtime-gate.log`。
现有 75 个进程变异锚点保持唯一，本次未重跑完整进程变异集。

pool/租约 descriptor 的 libc 元数据、主线程、AppKit/renderer 和其他线程/系统
内部开销仍不在此容量额度内；尚非完整资源账本或生产 GUI/daemon 接线，P0
未通过。

2026-10-20 的整合复验（基于 `155653c`）：此前启动失败、预算和 GUI runtime
切片的定向证据现已在同一版本合跑。`python3 scripts/probe-session-process.py
--mutations` 完成九类原生场景（roundtrip/kill/crossing/history/read-error/
recovery-error/apply-error/reply-error/start-error）、86 进程/11 帧/19 GUI 预算/
8 程序表/3 规模/8 命令/11 终态编码变异，共 146 项；所有目标红、独立对照绿，
SHA256 回滚、九类 GUI 预算基线和 26 类旧 surface 基线复绿。进程退出码为 0。
日志 `tmp/process-integrated-verification.log`。`bash scripts/gate.sh` 109/109、
57 公共符号对拍及 GATE PASS，日志 `tmp/process-integrated-gate.log`。

实际环境复核为 Darwin 25.6.0 arm64，macOS 26.6.2（25G83），Xcode 27.1
（27A9275）、Apple clang 21.0.0；脚本显式选择
`/Applications/Xcode.app/Contents/Developer`，不依赖全局 xcode-select 的 CLT
选择。上游 pin 仍为 `81598385d608dda4946093d6d6a234d204a2f225`。
本轮无实现改动，未新增平台支持或性能承诺；生产 listener/manager/auth/writer、
产品 UI、完整资源账本和 P0 逐项验收仍未完成。

2026-10-20 的跨进程中文组合/提交（基于 `df7819c`）：新增 `ime` 场景，
独立 Metal GUI 在恢复到 LIVE 后调用 preedit 与 composing key，观测引擎
ime_point 宽度为正且 input buffer 仍空；清除后宽度归零，再以非 composing
key 提交“中文”。此时输入精确为六字节原始 UTF-8，没有 bracketed-paste
包裹。随后同一 surface 的 USER 粘贴带原有包裹，合计 22 字节经带 generation/
applied-sequence 的 INPUT 帧送到真实 worker/PTY。shell 用独立的六字节读取及
字节相等检查确认中文，再验证后续粘贴，程序继续输出并完成最终网格对拍、
终态与晚输入拒绝。普通 roundtrip 不走中文分支，继续验证既有输入路径。

这是引擎 IME API 接到跨进程数据面的证据，不是操作系统候选窗、键盘布局或
NSTextInputClient 的端到端验证；没有以一次 surface_text 伪造 IME 提交。
系统输入法完整路径、其他交互逐项证据与 P0 资源/生产缺口仍待收口。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --ime-mutations` 四项均 ime 目标红、
roundtrip 对照绿：遗漏 preedit、组合提前提交、遗漏清除、提交误走粘贴。
SHA256 回滚及两类原生基线复绿，本轮 ime 的 S=4、L=5、final=9；
日志 `tmp/process-ime-verification.log`。核心 gate 109/109、57 公共符号对拍
通过，日志 `tmp/process-ime-gate.log`。现有 75 个进程变异锚点保持唯一；
本次未重跑加入 IME 后的完整进程变异集。

2026-10-20 的 GUI 元数据分配域（基于 `9136041`）：pool、surface 租约和
runtime 租约 descriptor 统一经可选 `p0_gui_metadata_allocator` 分配/按精确原始
尺寸释放，构造时清零；NULL 域明确选用原有 libc 行为。分配失败清空输出、
保留既有对象，容量准入失败也归还已分配 descriptor。新 metadata 用例逐个
拒绝 pool/runtime/surface 分配，验证 NO_MEMORY、BUSY、完整释放以及禁用
容量下的失败清理；域上下文必须活到 pool close 后，共享时回调须支持并发。

原生客户端接入独立 mmap 元数据域：包含对齐头及页舍入，每次映射前检查
4 MiB 上限，释放核对原始长度；pool close 后要求映射归零并销毁锁。没有把
VT 聚合头引入 full libghostty GUI，保持元数据接口为纯 C POD。这个 4 MiB
是独立于 surface/runtime 预算的额外上限，并非生产全局统一 ledger；主线程
中的分配域控制字段、其他框架/系统开销及其余资源仍待收口。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--metadata-mutations` 五项均 metadata 目标红、valid 对照绿，覆盖 pool/租约
绕过分配域、错误释放尺寸、失败准入泄漏与错误 pool 释放尺寸；SHA256 回滚、
十类 GUI 预算基线及 start-error/roundtrip 原生复验通过。实测两条路径元数据
映射峰值分别 32768/81920 字节，最终 live=0。日志 `tmp/gui-metadata-verification.log`。
metadata/runtime/rollback/race 四例 ASan/UBSan 通过。核心 gate 109/109、57
公共符号对拍通过，日志 `tmp/gui-metadata-gate.log`。现有 75 个进程、14 个旧
GUI 及六个 runtime 变异锚点逐项唯一；本次未重跑完整进程变异集。
P0 与生产 daemon 未完成。

2026-10-20 的 GUI 元数据统一容量准入（基于 `8db4849`）：私有 limits 新增
metadata。非零时要求显式提供能约束实际开销的元数据域，并在 pool 构造阶段
从同一 total 预留全部 metadata 上限；否则拒绝，不能悄悄回落到无界 libc。
域本身继续核对映射实际字节，pool 则按完整额度保守承诺，不重复把这些实际
字节再加到 committed。metadata=0 明确不包含元数据，保留旧探针对照语义。
构造额度不足返回 LIMIT、清空输出并释放 descriptor；pool 关闭时归还元数据
承诺再销毁预算，最后宿主释放 pool 的映射。

combined 用例以 12 页总量预留 7 页 metadata，验证 6 页 surface 和 6 页 runtime
都因剩余 5 页而拒绝；runtime 5 页可成功并与 metadata 合计 12 页，归还后
回到 7 页。metadata 大于 total 的构造失败可完整清理。原生策略 total 明确为
320 MiB surface + I/O 栈容量 + 4 MiB metadata；启动失败和正常恢复在 pool
关闭前保留 4 MiB promise，关闭后实际 metadata 映射归零。这是同一 GUI pool
的容量准入，尚非跨进程/生产 manager 全局账本；live 的引擎统计与元数据实际
映射统计仍分项记录。其他框架、线程/系统开销和 P0 余项仍未完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--combined-mutations` 四项均 combined 目标红、valid 对照绿，覆盖遗漏 metadata
预留、误接受无界 libc、额度不释放及额度拒绝谎报成功；SHA256 回滚、十一类
GUI 预算基线与 start-error/roundtrip 原生复验通过，日志
`tmp/gui-combined-verification.log`。combined/metadata/runtime/rollback 四例
ASan/UBSan 通过；核心 gate 109/109、57 公共符号对拍通过，日志
`tmp/gui-combined-gate.log`。现有 75 个进程、14 个旧 GUI、六个 runtime、
五个 metadata 变异锚点保持唯一。本次未重跑完整进程变异集。

2026-10-20 的混合并发准入（基于 `5b1b118`）：新增 combined-race，在同一
12 页 pool 中先承诺 6 页 metadata；两个真实 pthread 同时请求 6 页 surface
和 6 页 runtime。32 轮交替创建顺序，每轮必须恰有一个成功、一个 LIMIT，
输出句柄与结果一致，committed=12 页、对象计数匹配，pool 仍 BUSY；释放
赢家后精确回到 metadata 的 6 页及一个 pool descriptor。无数据分配的容量
竞争不能谎报两份成功，也不能因为部分申请/回滚造成两方都失败。

每轮随后强制 surface 占满再拒绝 runtime，反向再做一次，检查失败输出清空、
已有容量保持、失败 descriptor 回收及最终 pool 释放。不依赖随机竞争赢家来
覆盖两条拒绝清理路径。元数据 fixture 的调用在该单 pool 的 admission 锁内
串行化，主线程只在 pthread join 后观测计数；这不证明共享域跨 pool 的并发
回调安全，也不替代生产全局 manager 的并发准入。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--combined-mutations` 九项均目标红、独立对照绿；五项新增变异覆盖遗漏
metadata 承诺、runtime/surface 不预留以及两条拒绝路径泄漏 descriptor。
surface 不预留使用 invalid 对照，其余使用 valid 对照。SHA256 回滚、十二类
GUI 预算基线及 start-error/roundtrip 原生复验通过；日志
`tmp/gui-combined-race.log`。combined-race/combined/race 三例 ASan/UBSan 通过，
核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/gui-combined-race-gate.log`。
本次未重跑完整进程变异集。P0 与生产 daemon 仍未完成。

2026-10-20 的真实 VT 启动配额失败（基于 `0f18605`）：新增 state-oom。
open_state 在程序表建立后即把部分状态交给 worker 清理；VT 创建/初始 resize
失败返回 IO，不能再 CHECK 退出整个权威进程。尚未完成配置的状态由 close_state
只销毁 terminal/程序表，不读取最终元数据或导出快照。只有配置完成后才置
state_ready，普通 LIVE 终态仍保留原有最终快照与摘要路径。

场景在控制器、executor 和 worker 已分配后收紧 source 至当前 live+64 KiB，
实际调用 VT 构造，必须观察到 engine_attempted、一次 partial_cleanup、
SCOPE_LIMIT/denied、无残留 terminal/程序表，以及 join 后回到原控制器基线。
不是用工厂直接返回 IO 代替真实引擎分配拒绝。worker 结果仍为未知最终序号
ERROR/STATE_OPEN/IO，不伪造 S=0 或进入恢复握手。

source 故障是 sticky，不能从该域再分配错误投递 peer。场景在失败之前从
4 MiB 总量中额外预留 64 KiB 健康诊断域，仅该 peer 使用；释放走同一域。
GUI 收到通用 STATE_OPEN/IO 并确认，没有创建 terminal surface；此处没有新增
wire 配额原因字段或把 IO 当成专用 NO_MEMORY。诊断域和 source/线程额度在
关闭后全部归零。普通 start-error 和正常 roundtrip 保留原分配路径。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--state-oom-mutations` 五项均 state-oom 目标红、普通 start-error 对照绿，
覆盖错误释放域、部分程序表泄漏、从故障 source 分配诊断 peer、丢弃部分清理
上下文及用直接 IO 冒充真实引擎失败。SHA256 回滚和 state-oom/start-error/
roundtrip 原生复验通过，日志 `tmp/process-state-oom-verification.log`。
本轮实际 peak=212992、committed peak=3276800 字节，最终 live/committed=0。
核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/state-oom-gate.log`。
现有 75 个进程变异锚点保持唯一；本次未重跑完整进程变异集。仍未覆盖所有
setter/backend 启动失败原因或生产低内存诊断，P0 与生产 daemon 未完成。

2026-10-20 的第二轮整合复验（基于 `188d859`）：
`python3 scripts/probe-session-process.py --mutations` 在同一版本完成十一类
原生场景（原九类加 ime/state-oom），95 进程/11 帧/33 GUI 预算/8 程序表/
3 规模/8 命令/11 终态编码变异，共 169 项；所有目标红、独立对照绿，
SHA256 回滚、十二类 GUI 预算基线和 26 类旧 surface 基线复绿，退出码 0。
包括真实 VT 配额拒绝后的部分状态清理与健康诊断 peer、跨进程中文提交、
元数据/surface/runtime 统一容量及混合并发拒绝清理，均已从单独定向证据
进入本轮完整回归。日志 `tmp/process-latest-integrated.log`。
`bash scripts/gate.sh` 109/109、57 公共符号对拍及 GATE PASS，日志
`tmp/latest-integrated-gate.log`。

环境仍为 Darwin 25.6.0 arm64 / Xcode 27.1（27A9275）/ Apple clang 21.0.0，
上游 pin `81598385d608dda4946093d6d6a234d204a2f225`；脚本显式使用 Xcode
Developer 目录。本轮未修改实现，未新增平台或性能声明。系统输入法完整路径、
其他交互逐项证据、所有启动失败原因、完整资源账本及生产 listener/manager/
auth/writer/产品 UI 仍未完成，不能由完整脚本绿色推导 P0 已通过。

2026-10-20 的 backend 启动配额失败与已知零（基于 `9d61bca`）：新增
backend-oom。启动 state 先完成真实 VT 初始化，随后在 backend factory 内把
source 上限收紧到当前 live，实际调用 PTY 构造；首个 backend 分配被拒绝，
必须有 denied、无 backend 对象/字节。按当前 PTY 实现，拒绝发生在 pipe/fork
之前，不需要 waiter 栈。worker 结果为 ERROR/BACKEND_OPEN/IO，
has_sequence=true、sequence=0；与 state-oom 的未知序号明确区分。

清理按 state_ready/backend_started 的生命周期判别：backend 从未成功启动
时，只销毁已建立的 VT/程序表，不要求程序已经输出最终摘要，不导出不适用
的最终快照。源额度故障后仍由独立健康诊断域建立 peer。控制器 end(0) 返回
WAIT 并保留 known=true/terminal_sequence=0、只读和连接资格；尚无快照或
READY/LIVE，不能为了发送错误伪造恢复完成。GWR1 的已知位随终态发送。

GUI 在无 surface 的启动失败分支按 BACKEND_OPEN 检查已知零，并显示该阶段
与“known: 0”；STATE_OPEN 保持 unknown。两条路径都不创建 terminal surface，
IO 线程 join 和 pool/元数据释放仍按既有顺序完成。backend 场景 bootstrap
总额度 8 MiB，source 4 MiB、两线程栈和 64 KiB 诊断承诺均显式纳入；不是
增加生产全局准入或完整进程 RSS 保证。生产 daemon 和 P0 余项仍未完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--backend-oom-mutations` 四项均 backend-oom 目标红、state-oom 对照绿，
覆盖已知零误走未知入口、遗漏已初始化状态清理、PTY 拒绝被工厂直接 NULL
冒充和 wire 已知位丢失。SHA256 回滚及 backend-oom/state-oom/roundtrip 原生
复验通过，日志 `tmp/process-backend-oom-verification.log`。本轮实际 peak=
1966080、committed peak=6422528 字节，最终 live/committed=0。核心 gate
109/109、57 公共符号对拍通过，日志 `tmp/backend-oom-gate.log`。
现有 75 个进程与五个 state-oom 变异锚点保持唯一；本次未重跑完整进程变异集。
仍不覆盖所有配置 setter、PTY 创建失败点或生产启动错误 UI，P0 未通过。

2026-10-20 的跨进程鼠标/焦点报告（基于 `13461d8`）：新增 pointer。
真实 PTY 在首个快照前开启 DEC 1000/1006/1004，GUI 仅从恢复状态取得这些
模式；LIVE 后调用鼠标/焦点引擎 API，逐步验证 SGR 左键按下/释放各 9 字节、
CSI O/I 各 3 字节，重复设置 false 不新增输入，空移鼠标也不产生报告。
24 字节报告再接原有 16 字节 USER 粘贴，整体携带同代/同一已应用序号，经
INPUT 到 worker 和真实 PTY。shell 独立读取并逐字节比较报告，再验证粘贴，
继续产生输出；最终网格、摘要、单一查询应答及结束后输入拒绝仍通过。

这是恢复模式与跨进程编码数据面的证据；API 事件在主线程产生，字节在 I/O
线程和 worker 之间传送，不进入 UI 事件队列。尚非真实 AppKit 点击/窗口焦点
切换的端到端宿主路由验证；不以此宣布原生事件入口或 P0 完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--pointer-mutations` 四项均 pointer 目标红、roundtrip 对照绿：遗漏按下、
释放误作按下、遗漏聚焦和重复失焦误作状态翻转。SHA256 回滚及两类原生
基线复绿；本轮回滚 pointer 观测 S=5、L=6、final=10（依实际 PTY 分块推进，
不是固定序号期望）。日志 `tmp/process-pointer-verification.log`。
核心 gate 109/109、57 公共符号对拍通过，日志 `tmp/process-pointer-gate.log`。
现有 75 个进程变异锚点保持唯一；本次未重跑完整进程变异集。

2026-10-20 的有序配色命令基础（基于 `50a663f`）：私有 worker 新增
COLOR_SCHEME 命令、dark=0/1 的固定 POD 和 GT_STATE_COLOR_SCHEME 事件（kind=4，
不与实验输出摘要 bundle 的 kind=3 混用）。支持能力由 config 显式声明，默认
false；未声明时 admission 返回 GT_WORKER_UNSUPPORTED，不分配、不排队、
不修改状态；这是私有队列结果域，未来公开入口仍须映射为 GT_ERR_UNSUPPORTED。
支持时复制 POD，经原有有界 FIFO 在 state owner 上 apply，成功后发布同一
权威序号，后排 capture 必须看到该配置；不写为用户输入、不触发 backend resize。
适配器 apply 失败保留原错误、停止 worker 并触发状态失效通知，不伪报成功。

四类核心用例使用真实 PTY 和配置状态模型，验证 FIFO 中“配置→捕获”、调用方
复用 POD 后值不变、unsupported/invalid 不产生副作用、失败结果保留。模型不是
VT/GUI 配色接线；现有适配器仍默认未支持。实际引擎 scheme callback、快照中
配置元数据、全 surface 的有序配置 barrier、查询应答与渲染观察仍须完成，
不能用这些模型测试声称配色同步或 P0 已通过。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，核心 gate
113/113、57 公共符号对拍通过；四基线/八项配置变异均目标红、独立对照绿、
SHA256 回滚复绿，两套旧 worker 变异（17 与 26 项）也全部回滚复绿。
flow/unsupported/failure/invalid 经 ASan/UBSan 通过；现有原生 roundtrip 重新
编译运行通过。日志 `tmp/worker-scheme-gate.log`、`tmp/worker-scheme-mutations.log`、
`tmp/worker-scheme-old-worker.log`、`tmp/worker-scheme-old-result.log`、
`tmp/worker-scheme-native.log`。公开 ABI 和 GWR1 未改，未重跑完整进程变异集。
P0 与生产 daemon 仍未完成。

2026-10-20 的真实 headless VT 配色适配（基于 `8ceecba`）：新增独立
`daemon/probes/session-scheme-owner.c`，只链接 VT/core，不链接 AppKit/Metal。
`otool -L` 实测该二进制及 VT dylib 的依赖仅 VT/libSystem，没有窗口/渲染框架。
在该适配器声明配色能力，state owner 应用配置后，VT 的 COLOR_SCHEME 查询
回调读取同一 owner 的配置。真实 PTY 依次发出三次 CSI ?996n，并独立比对
light/dark/light 的 CSI ?997;2n / ;1n / ;2n；每次检查多余字节为空，查询和
应答计数均为三。主线程只提交配置/capture/input；回调和 apply 在 worker 上，
有序配置后的 capture 记录外部配置值与同一序号。阶段使用唯一序数 marker，
不能拿首次 light 的输出冒充第三次应答。

capture 此处仅是独立的外部配色 POD，不是完整 VT 规范快照或 GUI 恢复载荷。
当前主进程原型的 VT/GUI 适配器仍未启用配色能力；需把配置元数据并入一致
checkpoint，接通 full surface 有序配置 barrier，再验证并发查询/渲染。此
独立适配器的成功不授权 GUI 绕过 worker，也不表示配色跨进程恢复已完成。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`--scheme-owner-mutations` 四项均目标明确退出 1、roundtrip 对照退出 0，覆盖
查询忽略新配置、未声明能力、未接真实回调、捕获遗漏外部配置；SHA256 回滚、
真实 VT 基线和原生 roundtrip 复绿，日志 `tmp/scheme-owner-verification.log`。
C 适配器/worker/PTY 的 ASan/UBSan 链接探针通过，VT dylib 保持原构建。对象与
VT/PTY 通过 source 4 MiB 域，worker/waiter 栈按显式 options 预留，总额度
8 MiB；实际 peak=1933312 字节，最终 live/committed=0。本轮仅实际核验 worker
栈尺寸，不把 waiter options 等同于新做了线程区域测量。
核心 gate 113/113、57 公共符号对拍通过，日志 `tmp/scheme-owner-gate.log`。
未重跑完整进程变异集；完整资源账本、生产 daemon 和 P0 余项仍待完成。

2026-10-20 的完整进程集成复验（基于 `ea21f82`）：在 Darwin 25.6.0 arm64、
Xcode 27.1 / Apple clang 21.0.0、Ghostty pin
`81598385d608dda4946093d6d6a234d204a2f225` 下，实际运行
`python3 scripts/probe-session-process.py --mutations`，退出 0。
181 项变异为 103 process + 11 wire + 33 GUI-budget + 8 summary-wire +
3 summary-queue + 8 command-wire + 11 result-wire + 4 scheme-owner；目标红、
对照绿及 SHA256 回滚通过。回滚后 roundtrip、kill、crossing、history、
read-error、recovery-error、apply-error、reply-error、start-error、ime、
state-oom、backend-oom、pointer 十三类原生场景与 26 类旧 surface 基线通过。
完整日志 `tmp/process-integrated-181.log`；临时日志不入库。

同轮 `bash scripts/gate.sh` 退出 0，113/113 测试与 57 个公共符号对拍通过，
日志 `tmp/process-integrated-181-gate.log`。未新增断言、未改公开 ABI。
本轮整合了最近定向验证的切片，不扩大其证据范围：独立 headless VT 配色
适配器尚未接入主进程快照/增量或 GUI；GUI 实际应用配置后的序号 barrier、
系统输入法与原生鼠标/焦点宿主路由、完整资源账本及生产 daemon 仍待完成。
P0 保持未通过。

2026-10-20 的 full surface 配置应用屏障（基于 `05d11bb`）：实验补丁
0030 将配色事件分为 begin（仅准入）和主线程 apply（仅提交带序号配置消息）。
待应用期间保持旧 applied sequence，后续有序 output/resize 拒绝且不消费序号，
新编码输入通过 stamp.blocked 丢弃、不排队重放；焦点 I/O 编码路径及宿主动作
也遵守阻塞。已在此前编码的输入保留原 stamp。Termio 在持锁校验精确配置消息、
主题和预算后，实际更新终端默认颜色等配置，最后才发布该事件序号。

绑定副本的普通无序配置重载会使流失效，不能在事件流外修改权威状态；错误
标签或派生失败同样不允许伪造成功。没有取消待应用事件的接口，调用方超时须
作废连接。上游在配置没有主题条件时会跳过 conditional state 更新，实验标签
消息因此显式携带权威主题。未修改 vendor 或公开 core ABI，补丁及理由随代码
存于 `patches/libghostty/8159838/experimental/`。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，实际运行
`python3 scripts/probe-session-worker-surface.py --scheme-barrier`，退出 0。
真实 full Metal surface 验证 dark→light→dark→dark（同主题也推进事件）、I/O
配置主题、终端实际背景色 0x123456、待应用期间的序号/输入/输出/resize 隔离、
错误代次/序号拒绝、配置后查询仍不从副本出站，以及无序重载使流失效。
七项隔离变异覆盖提前发布、遗漏发布、后续事件越过屏障、输入 stamp 丢标记、
配置消息丢序号、无序重载被接受及实际背景色未更新；均明确目标退出 1、普通
continuation 对照退出 0，SHA256 回滚与最终两条原生基线复绿。
日志 `tmp/scheme-barrier-verification.log`。

同一最终引擎构建经进程探针的 copy_sources/build/probe/legacy 入口，重跑
十三类原生进程场景与 26 类旧 surface 基线通过，仓库源文件哈希未变；日志
`tmp/scheme-barrier-compat.log`，临时驱动 `tmp/verify-scheme-barrier-compat.py`。
核心 gate 113/113、57 个公共符号对拍通过，日志 `tmp/scheme-barrier-final-gate.log`。
本轮未重跑 181 项完整进程变异集，不能把独立七项变异视为其新增已集成覆盖。

确认只覆盖 I/O 配置及终端状态实际应用，不承诺 renderer 已完成绘制；该探针
也不证明系统事件入口、任意字体/配置的权威事件模型或完整资源账本。主进程
IPC 配色能力仍禁用：一致快照中的外部配置、增量载荷与 GUI 主线程调度尚待
接线。生产 daemon 与 P0 其余要求仍未完成，P0 保持未通过。

2026-10-20 的跨进程配色快照与增量接线（基于 `541c53b`）：新增 scheme
原生场景。私有 worker 适配器仅在该场景声明 COLOR_SCHEME 能力，owner 应用
主题与固定实验配色档的默认颜色；实际 VT COLOR_SCHEME 回调仍是唯一查询应答
owner。capture 在同一 worker 边界复制 dark 值并冻结完整 VT，编码与传输使用
捕获值，不读取后来变化的 live 配置。实验 snapshot/EXPECT 元数据在该档扩为
224 字节，新增 PW_SCHEME 为 8 字节 0/1 的有序增量；旧档仍严格接受 216 字节。
这不是生产 IPC v1 或能力协商，双方仍通过构建指纹核验当前实验契约。

0031 实验补丁在 bound checkpoint 构造参数中携带可选主题，并把域标记及主题
值加入 generation/S/资源/规范快照的同一摘要。只翻转主题而沿用摘要必须拒绝；
未绑定元数据和缺少显式设置标记的 dark 值也拒绝。surface 从该主题初始化条件
状态与 I/O 配置，不能继承新 GUI 进程的全局外观。显式主题的条件派生失败不走
上游日志后回落。未修改 vendor、公开 core ABI 或生产平台绑定。

GUI 的 I/O 线程直接接收并应用输出；配色增量先通过 0030 begin 保留旧序号，再
用固定大小的低频请求通知主线程派生配置。I/O 等待真正的 applied cursor 后才
消费下一增量，并检查实际 I/O 主题与终端默认背景；终端字节不进入 UI 队列。
实验配色档为 dark 背景 0x123456 / light 背景 0xabcdef、白色前景、非闪烁光标
默认值与 pin 的默认 palette。光标策略显式与源 VT 一致；首次整表对拍发现普通
配置重载会带入 GUI 的不同光标默认值，未用掩码或放宽比较规避差异。

真实 PTY 先验证 light 查询；捕获 dark 快照后，light 增量进入 S→L，READY(S)
冻结边界后再发 dark 增量，LIVE 后再发 light。GUI 构造前强制全局 light，验证
恢复的 dark 不受它污染；三条增量按 light/dark/light 次序实际应用。PTY 在用户
输入后再次逐字核验 dark 查询，并确认没有多余应答。结束时原捕获值保持 dark、
owner 已为 light；GUI 的完整 canonical/resources 与最终权威状态逐字一致。
保留原有终态、输入拒绝、程序摘要、命令状态与像素断言。应用确认仍不是绘制
完成确认，也未新增默认背景色的像素验收。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --scheme-mutations` 退出 0：十一项
C/宿主/codec 及两项隔离引擎变异均 scheme 目标明确退出 1、roundtrip 对照退出
0；覆盖捕获缺失、误传 live 值、构造标记、非法元数据/增量、错误 kind、查询
忽略配置、权威颜色漏应用、GUI 颜色错误、主线程不接请求、codec 拒绝新 kind、
构造 I/O 主题落回全局和跳过主题摘要校验。SHA256 回滚与两类原生基线复绿，
日志 `tmp/process-scheme-verification.log`。

同一最终构建另跑十四类原生进程场景、26 类旧 surface 基线通过；旧的“权威
重复光标应答”变异限定到原光标分支后，roundtrip 红、legacy continuation 绿，
SHA256 回滚与复验通过。75 个旧进程变异锚点逐项唯一，仓库源文件哈希未变。
日志 `tmp/process-scheme-compat.log`，临时驱动
`tmp/verify-process-scheme-compat.py`。核心 gate 113/113、57 公共符号通过，
日志 `tmp/process-scheme-gate.log`。完整进程脚本已纳入新场景和十三项变异，配置
总数变为 194；本轮未重跑完整集合，不把声明的计数当作已完成的集成证据。

生产设置 UI→daemon 命令、任意主题/字体/配置的权威模型、原生事件入口、绘制
完成与性能验收、完整资源账本及 listener/manager/auth/writer/打包仍待完成。
当前命令由原型驱动提交；固定实验配色档的成功不代表产品配色接线或生产
session runtime 已交付，P0 保持未通过。

2026-10-20 的跨进程配色背景像素验收（基于 `a39db2a`）：在 scheme
原生场景增加六次真实 Metal 视图采集，分别为初始 dark 快照、S→L 的 light、
L 后的 dark、可输入 LIVE 的 dark、LIVE 后的 light 以及 light 终态。主线程
在相应配置实际应用后观察帧，后续配置仍须由同一主线程接收；像素观察没有
改变协议 I/O applied 的含义，也没有把它变成绘制完成确认。

从自己的 NSView 采集并转为 RGBA，不依赖屏幕录制权限或既有截图文件。每帧
要求期望背景 RGB（dark=0x123456、light=0xabcdef，各通道容差 5）覆盖超过
半幅画面，另一种背景颜色少于 64 像素；后者单独拦截局部残留，不能只凭主体
颜色正确放行。六次采集计数与当前阶段绑定，最终必须实际执行六次。
本轮 1600×1200 帧中，前四帧匹配背景均为 1908988 像素，后两帧为
1899513，另一背景颜色均为零。临时 PNG 位于
`tmp/worker-surface/process-scheme-{0..5}-*.png`，不入库。

`python3 scripts/probe-session-process.py --scheme-pixel-mutations` 在
Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下退出 0。四项隔离
变异均 scheme 明确退出 1、roundtrip 对照退出 0：采集丢失内容、复用初始
bitmap、读取缓冲混入 256 个旧色像素，以及只让实际 renderer 的 RenderState
在 light 时仍使用 dark 背景。最后一项不改权威或 GUI I/O 状态，证明仅对拍
canonical 和配置值看不见的错误也会红。混色项仍满足主体新色比例，但旧色
计数 256，单独触发残留断言。SHA256 回滚与 scheme/roundtrip 原生复验通过，
日志 `tmp/process-scheme-pixel-verification.log`。

同一最终构建的十四类原生进程场景与 26 类旧 surface 基线通过，75 个旧进程
变异锚点逐项唯一、仓库源文件哈希未变；日志
`tmp/process-scheme-pixel-compat.log`，临时驱动
`tmp/verify-scheme-pixel-compat.py`。核心 gate 113/113、57 个公共符号对拍
通过，日志 `tmp/process-scheme-pixel-gate.log`。完整进程变异脚本已纳入四项
像素变异，配置总数为 198，本轮未重跑完整集合。

证据只覆盖固定实验配色档的默认背景及已有前景/SGR 图像；不覆盖任意主题、
字形/抗锯齿或通用 renderer 绘制完成屏障。截图诊断的 bitmap 分配不是生产
资源 ledger 的证据。生产设置 UI→daemon、原生事件入口、性能验收、完整资源
账本与 listener/manager/auth/writer/打包仍待完成；P0 保持未通过。

2026-10-20 的完整配色集成复验（基于 `dab0da0`）：实际运行
`python3 scripts/probe-session-process.py --mutations`，退出 0。环境为
Darwin 25.6.0 arm64、Xcode 27.1 (27A9275)、Apple clang 21.0.0，Ghostty pin
`81598385d608dda4946093d6d6a234d204a2f225`。198 项变异为 103 process +
11 wire + 33 GUI-budget + 8 summary-wire + 3 summary-queue + 8 command-wire +
11 result-wire + 4 scheme-owner + 13 process-scheme + 4 scheme-pixel；目标红、
对照绿及 SHA256 回滚通过。最后重新构建，roundtrip、kill、crossing、history、
read-error、recovery-error、apply-error、reply-error、start-error、ime、
state-oom、backend-oom、pointer、scheme 十四类原生场景与 26 类旧 surface
基线通过。完整日志 `tmp/process-integrated-198.log`，临时日志不入库。

同轮 `bash scripts/gate.sh` 退出 0，113/113 测试与 57 个公共符号对拍通过，
日志 `tmp/process-integrated-198-gate.log`。仓库工作树在验证后保持干净，运行中
未改被监测源码。本轮没有新增断言或改公开 ABI，整合验证最近配色与渲染切片。

P0 仍未通过。当前真实 TUI 原生场景只有 vim/less，不能推导 tmux 已验；本机
已只读确认 `/opt/homebrew/bin/tmux` 为 3.6a，但尚无 tmux 恢复场景证据。
系统事件入口、任意主题/配置、通用绘制完成与性能验收、完整资源账本，以及
生产 listener/manager/auth/writer/设置入口/打包仍待完成。完整脚本绿色不改变
这些交付边界。

2026-10-20 的真实 tmux 跨进程恢复（基于 `50ebb63`）：新增 process/tmux
场景，使用 PATH 中的 tmux（本轮 3.6a）。Python 测试控制器在工程 tmp 下建立
独立前台 server、显式配置及专属 socket；PTY 中运行真实 tmux client，pane
运行实际交互脚本。客户端用 -N 禁止偷偷另起 server，缺少 tmux 明确失败，不
跳过或回落到裸 shell。该 server 是测试应用负载，不是 Ghosterm 生产 daemon。

第一代 GUI 导入备用屏幕的 FIRST 画面并完成 READY 后，被真实 SIGKILL；连接
死亡释放旧代，权威 worker、同一 PTY 与 tmux 继续运行。detached 时 resize 和
输入推动 pane 产生 SECOND/VERIFIED；第二代 GUI 经原有快照、增量和 S/L 屏障
恢复，并把实际编码的 bracketed USER 输入送回 pane。pane 逐字核验 7 字节
DSR 与 16 字节粘贴，且在两次交互分别实测 stty 尺寸 31×91、32×92（有界等待
异步 SIGWINCH）。原有红/绿/白及最终蓝色像素断言仍通过。

capture/close 要求权威处于真实 alternate screen、标题为 Ghosterm tmux；两代
GUI 的 canonical/resources 均完整逐字对拍。真实应用没有发送程序摘要或命令
报告，故 PSW 保持空、命令保持 unknown/empty，未伪造已运行/已完成/退出码 0。
旧的有报告场景继续原断言；只扩展测试预期，不分叉恢复算法或渲染接缝。
外层 VT 查询沿用同一 worker reply sink，GUI legacy 写及宿主动作计数为零；
本轮观察到 3 次回调、34 字节外层应答，pane 的 DSR 由 tmux 自身处理。

测试只清理自己的 server：明确 kill-server 并 wait/有界 terminate/kill，随后
核验遗留 socket 的设备/inode、类型、uid 及不可连接状态再 unlink；不会直接
删除替换节点或活端点。配置和环境变量恢复，socket/config 最终归零。四个清理
基线覆盖本代失效 socket、普通文件、替换 inode 和活端点；不把此测试 fixture
当作生产 endpoint 身份/启动锁方案。

Darwin 25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0 下，
`python3 scripts/probe-session-process.py --tmux-mutations` 退出 0。三项清理
变异均目标红、本代正常清理对照绿；六项 C 变异均 tmux 明确退出 1、roundtrip
退出 0，覆盖 pane 查询/粘贴预期错误、backend resize 假成功、未知命令伪造成功
零、权威备用屏幕丢失和 detached 输入遗漏。编译失败不计作目标红。SHA256
回滚、四条清理基线及两类原生场景复绿，日志 `tmp/process-tmux-verification.log`。

同一最终构建的十五类原生进程场景、26 类旧 surface 基线通过，75 个旧进程
变异锚点逐项唯一、仓库源文件哈希未变。otool -L 验权威和 VT dylib 均只依赖
VT/libSystem，无 UI 框架。日志 `tmp/process-tmux-compat.log`，临时驱动
`tmp/verify-tmux-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-tmux-gate.log`。新场景与九项变异已纳入完整脚本，配置总数 207；
本轮未重跑完整集合，最近一次完整 198 项证据仍基于 `dab0da0`。

P0 仍未通过。tmux 应用进程的资源不属于已测 runtime 配额；完整资源账本、
系统事件入口、任意主题/配置、性能与各平台原生验收，以及生产 listener/
manager/auth/writer/设置入口/打包仍待完成。没有改公开 ABI、vendor 或生产绑定。

2026-10-20 的真实 vim/less 跨进程恢复（基于 `860ba1b`）：新增 process/less
与 process/vim，运行系统 `/usr/bin/less`、`/usr/bin/vim`，不再只依赖同进程
worker-surface 的 TUI 证据。本轮为 less 668、Vim 9.1 patches 1–1752，Darwin
25.6.0 arm64 / Xcode 27.1 / Apple clang 21.0.0；引擎 pin 未变。

两场景均在工程 tmp 建立 200 行受控文件；less 禁用用户 LESS 过滤/历史设置，
vim 用 -u NONE/-i NONE/-n 并禁标题修改，测试文件独占创建、finally 删除，环境
变量恢复。真实应用先显示首页，第一代 Metal GUI 导入备用屏幕后被 SIGKILL。
同一 PTY/worker 在 detached 期间 resize，less 收到 G 后显示末页，vim 收到
iHELLO 后保持插入状态；随后在新 generation 上捕获并恢复到第二代 GUI。

LIVE 后通过真实 surface key API 编码事件：less 收到 g 返回首页，vim 接受
USER 文本并产生新的输出。按协商模式传输有界编码字节（1–128），没有把所有
TUI 强行当作固定 bracketed paste 载荷；旧场景继续原字节断言。成功必须等待
提交输入之后的新程序输出，不能拿第一代 GUI 之前的首页标记冒充后续响应。
故意确认成功却不转发输入，会被新的 since-offset 观测拦住。

两代 GUI 都按原快照/有序增量/S-L 协议恢复，初始及最终完整 canonical/resources
逐字对拍；权威要求 alternate screen，未观察到的标题/cwd、程序摘要与命令保持
empty/unknown，不伪造退出码 0 或时长。设备属性由权威回调应答，GUI 的 legacy
写和宿主动作仍为零。三次 TUI 图像采集要求真实白色内容非空，未声称字形逐像素
一致；两程序的终态、迟到输入拒绝和分配域释放沿用既有验收。

`python3 scripts/probe-session-process.py --tui-mutations` 退出 0。八项变异
均目标明确退出 1、对应 vim/less/tmux 对照退出 0：detached 导航/插入丢失、
恢复后的错误导航/文本、空白帧、权威备用屏幕丢失、输入 ACK 后实际未转发，
以及将未观察命令伪造为成功零。SHA256 回滚及 less/vim/roundtrip 原生基线
复绿，日志 `tmp/process-real-tui-verification.log`。

同一最终构建的十七类原生进程场景与 26 类旧 surface 基线通过，75 个旧进程
变异锚点逐项唯一、源文件哈希未变；权威与 VT dylib 的 otool -L 仍仅 VT/
libSystem。日志 `tmp/process-real-tui-compat.log`，临时驱动
`tmp/verify-real-tui-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-real-tui-gate.log`。完整脚本已纳入两场景与八项变异，配置总数 215；
本轮未重跑完整集合，最近完整 198 项证据仍基于 `dab0da0`。

这些是 macOS arm64 的键盘 API/结构化状态与真实程序交互证据，不替代系统
NSTextInputClient/原生鼠标焦点事件入口、任意主题/字体、通用绘制完成或性能
验收。截图/应用内存不是完整 runtime ledger 的证明。生产 listener/manager/
auth/writer/设置入口/打包与各端收口仍待完成，P0 保持未通过。公开 core ABI、
vendor 和生产平台绑定未改。

2026-10-20 的 AppKit 鼠标跨进程恢复切片（基于 `f8523d7`）：新增
process/native-pointer 与实验 `session-appkit-host.inc`。合成 NSEvent 经
`NSApp sendEvent:`、真实 NSWindow/NSView 分发，宿主进行 view 坐标转换及 y
翻转，并在按下后检查实际 firstResponder。恢复前禁输入，LIVE 后原始按下/
释放报告连同已有焦点 API 的 O/I 报告经 stamped INPUT、IPC、同一权威 worker
进入原 PTY，shell 逐字检查 24 字节报告，再接收 16 字节 bracketed paste。
终端输出仍由 I/O 线程直接 feed surface，不进入宿主事件队列。

五项定向变异覆盖恢复期放行、y 翻转丢失、释放变成按下、按下丢失，以及点击
后 firstResponder 错交给窗口；每项要求 native-pointer 明确退出 1、旧 pointer
对照退出 0，最终 SHA256 回滚及两个原生基线复绿。NSWindow 已会在鼠标分发中
设置 firstResponder，故没有把删除宿主重复设置当成有效故障。

本轮实际通过定向函数 `native_pointer_mutations`（临时驱动调用脚本同名函数，
安装同一 INT/TERM handler），日志 `tmp/process-native-pointer-targeted.log`。
完整入口 `--native-pointer-mutations` 已纳入五项变异，完整集合配置数 220；
本轮未重跑完整集合，最近完整 198 项证据仍基于 `dab0da0`。
十八类原生、26 类旧 surface、75 个旧变异锚点唯一性、源哈希与无 UI 权威
依赖对拍见 `tmp/process-native-pointer-compat.log`，驱动
`tmp/verify-native-pointer-compat.py`。核心 gate 113/113、57 公共符号通过，
日志 `tmp/process-native-pointer-gate.log`。公开 ABI、vendor 与生产端绑定未改。

范围仅为 macOS arm64 实验宿主的合成鼠标事件。探针尝试 AppKit 激活后仍未取得
系统 key window，已删除激活逻辑；没有用伪造 isKeyWindow/通知冒充 OS 焦点
证据。窗口 key/resign、系统物理输入、NSTextInputClient、生产 Swift 宿主、
任意主题、通用绘制完成与完整预算仍待核验。生产 daemon listener/manager/
auth/发现列表/恢复 UI/helper 打包未交付，P0 保持未通过。

2026-10-20 的原生文本输入恢复切片（基于 `d845f40`）：实验 AppKit 宿主实现
NSTextInputClient，并由 keyDown 调 interpretKeyEvents。新 process/native-ime
在 LIVE 前经 NSWindow 分发合成按键、模拟组字和提交，要求全部被宿主拒绝，
按键解释器未运行，既不缓存 marked text 也不产生终端输入。LIVE 后真实
firstResponder 接收合成 x 按键，必须经过解释器文本回调，而非只靠普通按键
fallback 碰巧编码出同一字节。

随后通过 NSTextInputClient 模拟中文组字，验证 marked/selected 的 UTF-16
range、引擎 preedit 宽度非零、且没有额外输入。取消组字必须清空标记与引擎
宽度；候选窗锚点按引擎坐标转换到 window/screen 并与独立期望比较。引擎 imePoint.y 是单元格底边，转换不再减行高；独立变异护此约定。再次用
NSAttributedString 组字及提交中文，要求清空组合态、按键入语义输出精确
`x中文` 七字节，无 bracketed paste 包裹。该载荷与后续 16 字节 USER 粘贴经
stamped INPUT/IPC/权威 worker 到达原 PTY，shell 独立逐字核验，沿用完整状态、
终态、迟到输入拒绝及分配域释放检查。高频输出仍直接走 I/O 线程到 surface。

`python3 scripts/probe-session-process.py --native-ime-mutations` 通过十一项
定向变异：解释器绕过、预编辑遗漏、组字泄漏输入、取消未清标记、提交误走
粘贴、提交未结束组合、候选窗 y 翻转丢失、多减一行高度，以及 keyDown/setMarkedText/
insertText 三个恢复期保护分别失效。每项目标退出 1、原 ime API 对照退出 0，
SHA256 回滚后两场景复绿，日志 `tmp/process-native-ime-verification.log`。
之前五项原生鼠标变异也复验通过；只读鼠标锚点改为精确匹配 mouseDown，避免
新增文本入口后误匹配其他保护，日志 `tmp/process-native-ime-targeted.log`。

最终十九类原生与 26 类旧 surface 基线通过，75 个旧进程变异锚点仍逐项唯一，
源哈希未变；权威与 VT dylib 无 AppKit/Foundation/Metal 依赖。日志
`tmp/process-native-ime-compat.log`、驱动 `tmp/verify-native-ime-compat.py`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-native-ime-gate.log`。
完整脚本已纳入新场景与十一项变异，配置总数 231；本轮没有重跑完整集合，最近
完整 198 项记录仍基于 `dab0da0`。公开 core ABI、vendor 与生产端代码未改。

这是 macOS arm64 合成普通按键与模拟 NSTextInputClient 回调的实验宿主证据，
不是物理键盘、系统中文输入法选字、系统 key-window、生产 Swift 宿主或设置
入口的恢复验收。NSString/AppKit/候选窗等内存不构成完整 runtime ledger
证明；任意主题、通用绘制完成、跨进程总预算仍待核验。生产 daemon listener/
manager/auth/发现列表/恢复 UI/helper 打包未交付，P0 保持未通过。

2026-10-20 的完整进程原型集成复验（基于 `aa60729`）：
`python3 scripts/probe-session-process.py --mutations` 退出 0，十九类原生场景、
231 项变异和 26 类旧 surface 最终基线通过。不是将不同构建的定向结果相加：
同一命令按序运行全部组，完成 SHA256 回滚后重新构建，并再次运行十九类原生
与所有旧 surface；所监控源文件及补丁的起跑哈希在 finally 中逐项保持一致。
日志 `tmp/process-integrated-231.log` 中实际有 231 条 target red/control green
记录，与分项计数一致。此前基于 `dab0da0` 的完整 198 项记录保留为历史证据。

本次覆盖 roundtrip/kill/crossing/history、read/recovery/apply/reply/start
错误、state/backend OOM、IME API、鼠标/焦点 API、固定配色档、tmux、less、
vim、AppKit 合成鼠标和原生文本输入。最终真实 PTY/Metal GUI 的结构化状态、
有序输入、查询 owner、终态与资源释放检查继续通过；配色六个时点的背景像素、
TUI 非空画面及旧 surface 的资源/continuation 等基线同一运行通过。

计数口径：103 process = 原有 75 + startup 10 + runtime 6 + IME API 4 +
backend OOM 4 + pointer API 4；33 budget = GUI-budget 14 + state OOM 5 +
combined 9 + metadata 5。其余为 wire 11、summary-wire 8、summary-queue 3、
command-wire 8、result-wire 11、scheme-owner 4、process-scheme 13、
scheme-pixel 4、tmux 9、real-TUI 8、native-pointer 5、native-IME 11。
预算的 19 项没有同时算进 103 process，避免重复或漏计。

同一源树的 `bash scripts/gate.sh` 退出 0，113/113、57 公共符号与错误入口
闸门通过，日志 `tmp/process-integrated-231-gate.log`。最终 `otool -L` 另验
process-server 与 VT dylib 仅依赖 VT/libSystem，没有 AppKit/Foundation/Metal，
日志 `tmp/process-integrated-231-linkage.log`。本轮没有改实现或新增闸门。

环境 macOS 26.6.2 (25G83)、Darwin 25.6.0 arm64、Xcode 27.1 (27A9275)、
Apple clang 21.0.0 (clang-2100.3.34.2)；Ghostty pin 仍为
`81598385d608dda4946093d6d6a234d204a2f225`。实际链接产物和补丁共同生成的
engine build 身份为 `680e3ca7eac6efb3f4cbb209f8eef22736b3cb789634468014211fe13aa0113a`。
源/补丁/日志 SHA256 与环境记录在 `tmp/process-integrated-231-manifest.json`，
清单 SHA256 为 `9568ab969e66e0f0387741af59325bf88bf9f562ecb9050aa2601da1ad8209c7`；
可按上述基准提交、钉版本和命令重新运行完整验证，临时日志不入库。

完整集合通过仍不等于 P0 通过或生产支持：连接仍是继承的私有 socketpair，
AppKit 证据仍是合成事件和模拟文本回调，系统输入法选字/系统窗口焦点及生产
Swift 接线没有新增证据；任意主题、通用绘制完成、框架/GPU/全局资源预算和
最终性能验收未完成。listener/manager/auth/writer、发现/恢复 UI、helper
打包以及后续各平台/SSH 迁移仍待交付，能力矩阵继续标 ❌。

2026-10-20 的主线程容量准入切片（基于 `6b4e3e7`）：审计发现进程原型已预留
worker/encoder/waiter/GUI I/O 栈，但权威主线程与 GUI 主线程仍在账本外。
新增私有 Darwin `session-main-stack.h`，仅在实际主线程查询 pthread 栈容量，
检查当前局部变量位于该栈，并经 mach_vm_region 核验其下方完整连续、不可访问
的保护区；查询失败、边界不一致、溢出或合计超过 128 MiB 明确拒绝，不回落到
猜测值。Mach object port 查询后释放。此接口不进入 portable core 或公开 ABI。

本机实际栈为 8,372,224 字节，连续 VM guard 为 58,736,640 字节，合计
67,108,864 字节（64 MiB）。整份虚拟容量计入承诺，不把 guard 当成新增 RSS，
也不只计算一页保护区。OS 已创建主栈；原型在启动 worker/I/O 和接纳 surface
之前预留既有容量，不修改栈大小或保护区映射。server/process_client 等已有
主栈局部对象被这份容量覆盖，不再另算一份对象字节。

权威 normal 总限额从 256 MiB 的原覆盖范围加上实测 main capacity；startup/
state/backend 失败场景同样加入独立 main scope，保持原 source/线程限额不变。
GUI 总量加同一实测容量，通过既有 pool reserve 与 surface/runtime 共享准入，
在进程 pool 中只预留一次，不随 surface 重复计费；
I/O join 只归还 I/O 份额，主线程份额保留至 bootstrap/pool 最终关闭。资源
账本最终回到零不表示 OS 主栈已消失，只表示已无活动会话/恢复任务、准入域
正在销毁，随后进程退出；没有把常驻栈从 RSS 中假删。

独立 measurement/admission/thread 三类基线验证实际 RLIMIT_STACK、完整 guard
计费、容量与承诺一致、错误承诺拒绝、非主线程拒绝且输出清零。
`python3 scripts/probe-session-process.py --main-stack-mutations` 退出 0，十一项
变异目标红/对照绿、SHA256 回滚及 roundtrip/start-error/state-oom/backend-oom
复绿。覆盖 owner/GUI 未预留、漏释放、漏 guard、全局容量漏项，以及测量漏
保护区、准入忽略 committed、工作线程误判为主线程。正常路径要求明确退出 1，
独立 helper 检查要求非零；日志 `tmp/process-main-stack-verification.log`。
owner 未预留变异用“只初始化 scope、不承诺容量”，避免未初始化对象崩溃冒充
有效的准入断言。SIGINT/TERM 沿用既有 handler/finally 恢复和起跑 SHA 校验。

最终十九类原生、26 类旧 surface、75 个旧进程变异锚点唯一性、源哈希及无 UI
权威/VT 链接复验通过，日志 `tmp/process-main-stack-compat.log`，驱动
`tmp/verify-main-stack-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-main-stack-gate.log`。完整集合已纳入十一项，配置总数 242；本轮
未重跑完整 242，最近完整 231 项仍基于 `aa60729`。引擎 pin、上游补丁、公开
core ABI 与生产端绑定未改。

这是当前 macOS arm64 进程原型的 bootstrap 虚拟容量准入证据。Framework/GPU/
TLS/其他系统线程、全局资源上限、任意主题、通用绘制完成、系统输入法与窗口
焦点及生产 Swift 接线仍有缺口；不能据此声称完整 runtime ledger 或 P0 通过。
生产 listener/manager/auth/writer、发现/恢复 UI 与打包仍待交付。

2026-10-20 的 full surface 线程容量切片（基于 `bc0785c`）：审计发现 renderer
和 I/O 使用 Zig 0.16.0 的 std.Thread.spawn 默认栈（每个 16 MiB），原
surface CPU/page allocator 不覆盖这两份 pthread 栈。

新增实验补丁 `0032-surface-thread-observation.patch` 与 WHY：提供工具链默认
栈大小、Darwin 活 surface 的 renderer/I/O 线程 handle 只读查询。handle 是
当前进程内借用值，只在 surface_free 前使用，不序列化，不参与输入授权，
非 Darwin 查询分支返回 false，其他端尚无编译/运行证据；线程创建及 join
实现没有改变。正式脚本从干净 pin 依次应用全部补丁后重建通过。

GUI pool 的每 surface lease 新增可选 threads 容量域，在构造引擎前与三个
数据域一起准入；线程容量拒绝时回滚三个数据域、失败 scope 和 descriptor，
不占 surface 槽。threads=0 显式排除线程预算，不创建空域。非零容量在
surface_free 已 join renderer/I/O、且延期分配释放完成后才随 lease 归还；
主线程及宿主 I/O 的进程级 reservation 仍独立保留，不随 surface 释放。

宿主按查询值为两个线程各预留 stack + 2 OS pages（Darwin 栈报告额外空间与
完整 VM guard），当前合计 33,619,968 字节/Surface。初始及两次 sibling
构造均通过真实 pthread_get_stacksize_np/stackaddr 和 mach_vm_region 检查，
每线程报告栈 16,789,504、guard 16,384、承诺 16,809,984 字节；两角色必须
是不同非主线程。空对象、无效角色及空输出参数明确拒绝。两个实际 surface
的完整状态、第三个拒绝、隔离故障、释放与重用继续通过；全局额度只增加
新承诺，不扩大原 general/pages/wire 的单 surface 限额。

新增 threads 预算基线覆盖先预留 8 页、第二份数据域已满 14 页时在线程域
拒绝、重复拒绝不泄漏、释放及整数溢出。共享预算基线从 12 增至 13，日志
按实际用例数输出。`python3 scripts/probe-session-process.py --engine-thread-mutations`
退出 0：十一组目标红/对照绿、源码 SHA256 回滚及最终基线复绿，日志
`tmp/process-engine-threads-verification.log`。其中两组真的修改临时引擎并重建：
让 I/O 查询返回 renderer（句柄重复），以及将实际 renderer 栈翻倍至 32 MiB
（真实 pthread 栈超过承诺）；对应旧 surface continuation 对照仍绿。其他组
护未预留、拒绝回滚、释放、溢出、少算线程/guard、池漏项及观测漏报。

新增拒绝路径使两个旧替换锚点出现歧义，已收窄到原三个数据域的回滚路径；
14 GUI + 9 combined 旧变异重新通过，源哈希未变，日志
`tmp/process-engine-threads-budget-compat.log`。最终十九类原生、26 类旧 surface、
75 个旧进程锚点唯一性与无 UI 权威/VT 链接复验通过，日志
`tmp/process-engine-threads-compat.log`、驱动 `tmp/verify-engine-threads-compat.py`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-engine-threads-gate.log`。
完整集合配置数为 253，本轮未重跑完整集合，最近完整 231 仍基于 `aa60729`。
Ghostty pin、生产平台绑定和公开 core ABI 未改，新增补丁仅在实验路径应用。

以上是当前 macOS arm64 原型的线程虚拟容量证据，不是 RSS 或完整 runtime
ledger。std.Thread 的入口 Args 使用 libc allocator 且持有至线程结束，TLS、
搜索/其他框架线程、App/共享字体/GPU 与跨进程总预算仍未覆盖；系统输入法/
窗口焦点、任意主题、通用绘制完成和生产 daemon 链路仍待交付，P0 未通过。

2026-10-20 的线程入口帧预算与初始化失败清理切片（基于 `6a2bfc4`）：新增实验
补丁 `0033-surface-thread-args.patch`。Zig 0.16.0 POSIX spawn 忽略配置中的
allocator，旧 Args 走 libc 且保留至线程退出。预算型 Darwin surface 现在用
专用 pthread 启动器，入口 Frame 由可选 p0_thread_allocator 分配；省略时用
已有宿主 general allocator，分配拒绝直接失败，不回落 libc。Thread allocator
单独出现、缺少完整预算及 host/checkpoint 约束时拒绝创建。

Thread 包装保存标准 std.Thread 或预算 pthread handle，不构造 std.Thread 的
私有 Impl。未启用预算的路径继续用 std.Thread；预算路径保持原默认栈/guard、
maybeAttachSignalStack、命名与 join。renderer/I/O 的 stack 容量政策未降低，
非 Darwin 预算线程路径明确报 UnsupportedThreadAllocator，其他平台仍无原生
编译/运行证据。新的只读 allocator descriptor 查询仅供当前进程核验实际选择，
不序列化指针；所有回调和 allocator descriptor 要活到 surface_free。

完成 IO 对象初始化后，失败清理责任从临时局部副本转交已发布的 Surface；
初始化后续失败时先 stop/join 已启动线程，再按 renderer、两个线程对象、
terminal 的顺序销毁，避免运行线程继续访问已释放状态、notifier 或 display
link。第一帧拒绝时没有线程启动；第二帧拒绝时已启动 renderer 被停止/join，
其入口帧归还。健康路径各持有两个入口帧，surface_free 后实际 free 两次。

原生探针专用 frame provider 将请求转交同一 general 预算，并独立限制存活
入口帧数为 0/1/2；记录真实 alloc/free/denial。每个正常恢复均验证首个/第二个
请求拒绝时构造失败、所有帧及临时 lease 回收、原 surface 仍健康，随后两个
正常 sibling 的构造/拒绝/释放重用继续通过。省略可选 provider 时另创建真实
surface，逐线程核验 allocator descriptor 确实为其 general 域。

独立 Zig 四例覆盖成功线程、选定 allocator、分配拒绝不运行入口、无效栈参数
引起启动失败后的回收，以及内核 sigaltstack 保留已配置的备用信号栈。
`python3 scripts/probe-session-process.py --thread-args-mutations` 退出 0：
八项目标红/对照绿、SHA256 回滚及最终单元/原生基线复绿，日志
`tmp/process-thread-args-verification.log`。三组单元变异护正常 free、启动失败
errdefer、信号栈初始化；五组临时引擎重建变异护 libc 绕过、指定域忽略、默认
域选错、缺预算接受 thread-only allocator，以及分配失败悄悄回落 libc。
单元目标明确退出 1/对照 0，原生目标退出 1/旧 continuation 对照 0；编译失败
不算目标红。另跑 `--engine-thread-mutations`，原十一项栈准入变异复绿，包括
新预算 pthread 路径下真实 renderer 栈翻倍，日志
`tmp/process-thread-args-stack-compat.log`；旧物理栈变异锚点同步到预算启动器。

最终十九类原生、26 类旧 surface、75 个旧进程锚点唯一性、源哈希及无 UI
权威/VT 链接检查通过，日志 `tmp/process-thread-args-compat.log`，驱动
`tmp/verify-thread-args-compat.py`。核心 gate 113/113、57 公共符号通过，日志
`tmp/process-thread-args-gate.log`。完整集合配置数 261，本轮未重跑完整集合，
最近完整 231 仍基于 `aa60729`。Ghostty pin、公开 core ABI 与生产绑定未改。

这里只关闭了预算型 renderer/I/O 的入口帧分配缺口。TLS/备用信号栈、libpthread
内部、搜索/其他框架线程、App/共享字体/GPU 和跨进程总预算仍未完成；系统
输入法/窗口焦点、任意主题、通用绘制完成及生产 listener/manager/auth/writer、
发现/恢复 UI/helper 打包仍待交付，P0 保持未通过。

2026-10-20 的宿主拥有备用信号栈切片（基于 `70c5f3b`）：实验补丁
`0034-owned-signal-stack.patch` 把预算型 renderer/I/O 实际注册的缓冲区放进
入口 Frame，大小仍取 std.options.signal_stack_size。保留正常 pthread 栈和
VM guard，不关闭、缩小或取消已配置的备用信号栈；配置显式为 null 则不注册。
每线程注册前保留旧状态，工作返回后先恢复旧注册，再归还 Frame。

Darwin 真实 C 探针发现：新 pthread 查询返回 SS_DISABLE、size=0，但把这份
状态直接传回 sigaltstack 会报 ENOMEM；即使 SS_DISABLE，仍需满足
MINSIGSTKSZ（本机 32768）。恢复禁用状态时只补足最小 size，保留禁用语义；
已有活动栈则保留其原地址和长度。检查 free 时是否仍注册还必须看 DISABLE
标志，不能只看非零 size。临时 C 探针在工程 tmp 中执行，没有改 OS 配置或
用户应用的信号状态。

五类独立 Zig 基线验证分配/启动/释放、内核登记地址位于实际分配帧内、free
之前已恢复注册，以及嵌套活动栈恢复。期望大小直接取 std 配置，不复用待测
helper 派生常量，避免两边一起缩小而假绿。Frame 的信号区按 16 字节对齐；
错误路径沿用入口帧 allocator 和先 join 后回收的初始化事务。

原生 roundtrip 记录配置为 262144 字节/线程，两帧实际逻辑请求峰值 524352
字节，进入既有 general 域；这是请求量，不是 RSS，也不等于 provider 的
页舍入映射量。正常 Root/sibling 在真实构造后分别核对两帧至少覆盖配置字节，
首/次请求拒绝与最终 free 计数继续通过。新增私有只读大小接口不传输地址，
公开 core ABI 与生产绑定未改。

`python3 scripts/probe-session-process.py --signal-stack-mutations` 退出 0，
六组目标红/对照绿，源码 SHA256 回滚及最终基线复绿，日志
`tmp/process-signal-stack-verification.log`。五组单元变异覆盖派生大小减半、
注册 TLS 替代堆内存、释放前漏恢复、错误禁用活动旧栈、Darwin 禁用大小未
归一化；最后一组必须确实报告 SizeTooSmall，其他异常不能冒充有效失败。
原生变异真实重建半尺寸缓冲区，Frame 请求量检查变红，旧 continuation 仍绿。
编译失败不计入目标红。

`--thread-args-mutations` 的原八组复绿，信号设置变异锚点同步到新注册路径，
日志 `tmp/process-signal-stack-args-compat.log`。最终十九类原生、26 类旧 surface、
75 旧进程锚点唯一性、源哈希及无 UI 权威/VT 链接复验通过，日志
`tmp/process-signal-stack-compat.log`，驱动 `tmp/verify-signal-stack-compat.py`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-signal-stack-gate.log`。
完整集合配置数 267，本轮未重跑完整集合，最近完整 231 仍基于 `aa60729`。
Ghostty pin 未变，补丁只在实验路径应用，其他平台没有新增运行证据。

这只证明当前注册信号缓冲区的宿主所有权、计费与恢复；不证明 TLS 分配消失
或 RSS 下降。标准库链接的未使用 TLS 数组仍可能随映像 TLS 分配，其他 TLS、
libpthread、搜索/框架线程、App/字体/GPU、跨进程总预算仍未完成。系统输入法/
窗口焦点、任意主题、通用绘制完成及生产 listener/manager/auth/writer、会话
发现/恢复 UI/helper 打包仍待交付，P0 保持未通过。

2026-10-20 的搜索线程预算与失败回滚切片（基于 `1553f54`）：实验补丁
`0035-budgeted-search-thread.patch` 将预算型 surface 的惰性 search 线程接入
既有宿主线程 allocator；未预算路径继续使用标准线程。每个 surface 在构造
前预留 renderer/I/O/search 三份栈与 guard，当前共 50,429,952 字节；搜索
停止后入口帧与状态归还，容量保留到 surface lease 释放，供下一次搜索使用。
这份容量与实际存活线程数分别核验，空闲时不虚报已启动第三个线程。

另修复搜索启动失败只销毁 state 却保留 Some 的旧路径：错误退出同时撤销槽位，
避免之后查询、重试或销毁触碰未初始化的 thread。只读 presence 查询先验空槽，
不会为检测陈旧槽而读取无效句柄。角色 3 的句柄只在当前搜索存活时可借用，
不能跨 end_search 保存或序列化，所有查询由 surface main owner 发起。

新增原生 search 场景，先拒绝第三个入口帧分配并验空槽，再允许重试两轮；
真实 search_total 低频回调必须报告 FIRST 的匹配，校验实际 pthread、独立
句柄、宿主 allocator、栈/guard。第一轮停止须归还第三个帧，第二轮保持搜索
活跃后 SIGKILL GUI；同一 PTY 与新代 GUI 恢复后再次搜索，最终完整状态与
资源对拍。正常销毁必须回收活跃搜索；最终入口帧 attempts=5、freed=4、
denied=1、final_live=0，峰值请求 786,528 字节。终端输出仍由 I/O 直送 surface。

`python3 scripts/probe-session-process.py --search-mutations` 七项定向变异覆盖
少预留第三份容量、搜索绕过 allocator、失败残留槽、失败泄漏状态、忽略搜索
文本、停止遗弃线程、搜索角色冒用 I/O 句柄；每项目标红且独立对照绿，源码
SHA256 回滚后基线复绿。编译失败不计目标红，日志
`tmp/process-search-verification.log`。原十一项 engine-thread 变异复验通过，
日志 `tmp/process-search-engine-compat.log`。最终二十类原生、26 类旧 surface、
五类入口帧单元、75 旧进程变异锚点唯一性及源哈希/无 UI 链接复验通过，驱动
`tmp/verify-search-compat.py`，日志 `tmp/process-search-compat.log`。
核心 gate 113/113、57 公共符号通过，日志 `tmp/process-search-gate.log`。
完整集合配置数 274，本轮未重跑完整集合，最近完整 231 仍基于 `aa60729`。

Ghostty pin 未变；补丁只在实验路径应用，未改公开 core ABI 或生产绑定。
这只补齐当前搜索线程的预算与清理证据；其他 TLS、libpthread/框架线程、
App/字体/GPU 与跨进程总账本仍未完成。系统输入法、通用绘制屏障和生产
listener/manager/auth/writer、会话发现/恢复 UI/helper 打包仍待交付，P0 未通过。

2026-10-20 的 App 共享分配域切片（基于 `5850f27`）：实验补丁
`0036-app-allocator.patch` 为 runtime config 增加可选宿主 allocator，覆盖
CoreApp、embedded App、克隆配置与经 App allocator 分配的共享字体容器。
创建、错误回滚与 app_free 使用同一域；未提供时仍用默认分配器。只读
allocator 查询返回进程内描述符借用，不跨 IPC 或序列化。

GUI 进程在同一预算池中另预留 64 MiB App 域，先完成主线程与 App 准入再
创建引擎 App，surface 不再先于这份准入持有一个隐形全局 App。runtime
reservation 可选择使用其有界 allocator；活跃分配阻止 unreserve，分配超额
是局部粘性故障，旁边的 surface 域保持健康。App 销毁后才能撤销容量及关闭
总池。原始 config 与全局初始化目前仍在该域之外，不能宣称全部引擎内存
已经计费。两个 detached 预热线程、CoreText/Metal 内部、其余 TLS/框架/
GPU 资源与跨进程总账本仍须收口。

正常创建前分别拒绝前四次分配，要求创建明确失败、已分配对象归还、空域可
重用；成功后核验 runtime App 对象确实来自第二次宿主分配，CoreApp 的真实
allocator 与宿主描述符相同，配置副本有实际计费。正常回收完整 App 后宿主
分配归零；启动失败也必须销毁 App 后才能关闭池。私有 runtime-allocator
基线另验页取整、容量计费、活跃对象 BUSY、超额故障隔离和回收后关闭。

`python3 scripts/probe-session-process.py --app-budget-mutations` 十项目标红/
对照绿、SHA256 回滚和原生基线复绿，日志 `tmp/process-app-verification.log`。
变异分别破坏 runtime allocator、活跃内存检测、故障可见性、BUSY 口径、
scope 关闭、总池 App 容量、CoreApp/embedded App/克隆配置的分配域与 App
对象释放；引擎变异必须编译成功并由原生检查明确失败，不能用崩溃冒充。
旧 GUI 14、combined 9、metadata 5、main-stack 11 项变异另复绿；最终二十类
原生、26 类旧 surface、14 类 GUI 预算与五类入口帧单元、75 旧进程锚点唯一性、
源哈希/无 UI 链接复验通过，驱动 `tmp/verify-app-compat.py`，日志
`tmp/process-app-compat.log`。核心 gate 113/113 与 57 公共符号通过，日志
`tmp/process-app-gate.log`。完整变异配置数 284，本轮未运行完整集合；最近完整
231 仍基于 `aa60729`。Ghostty pin、公开 core ABI 和生产绑定未变，其他平台
没有新增运行证据。P0 仍未通过，生产 daemon 与 P1–P8 仍未交付。

2026-10-20 的 App 预热线程所有权切片（基于 `f91ab06`）：实验补丁
`0037-owned-app-warmups.patch` 将预算型 App 的字体与 Metal 预热改为两个
受拥有的 joinable 线程，实际预热工作继续执行。完成 App 配置克隆后才启动，
使用已有预算线程入口与宿主信号缓冲；单独提供 thread allocator，省略时
继承 App allocator。App 域缺失却设置线程 allocator/observer 会明确失败，
不静默回落。未预算路径保持上游标准预热线程行为。

第二次启动失败必须先 join 第一次已启动工作，然后销毁配置/runtime/core；
正常 App.deinit 在释放共享状态前 join。观察分三个阶段：worker 开始、
worker 工作完成、owner join 完成，避免将完成工作冒充释放线程生命周期。
GUI 在同一池中另预留两份栈/guard，当前共 33,619,968 字节，持有至 App
销毁与 join 完成。实际线程内测栈、VM guard 和 sigaltstack，校验两个 role
都开始并完成且由 main owner join，两个入口帧最终均归还。帧/信号缓冲使用
App 的有界数据域，栈容量单独计费，不重复宣称为实际 RSS。

正常 App 创建前分别拒绝第一次和第二次线程入口分配，核验明确失败、已
启动线程归还、App 对象与配置全部回收，预算域随后可重试。保留前四次 App
分配拒绝测试。独立 warmup 测试验证两项实际工作、第二次失败后的 joined
回滚、活跃工作阻止 join 返回；连同导入的五项线程入口测试共八项。

`python3 scripts/probe-session-process.py --warmup-mutations` 十项目标红/
对照绿、SHA256 回滚与正常恢复/启动失败/搜索基线复绿，日志
`tmp/process-warmup-verification.log`。三项单元变异破坏实际工作、失败回滚
和 join 等待；七项原生变异改回标准线程、忽略提供的 allocator、真实栈翻倍、
移除 App 域前置检查、漏工作完成报告、App 销毁漏 join 与容量减半。引擎
变异必须编译成功并由检查明确失败，编译错误/崩溃不能充数。

旧 App 预算十项、main-stack 十一项与 engine-thread 十一项变异复验通过，
最终二十类原生、26 类旧 surface、14 类 GUI 预算、八项 warmup/入口单元、
75 旧进程锚点唯一性、源哈希及权威/VT 无 UI 链接复验通过，驱动
`tmp/verify-warmup-compat.py`，日志 `tmp/process-warmup-compat.log`。
核心 gate 113/113 与 57 公共符号通过，日志 `tmp/process-warmup-gate.log`。
完整变异配置数 294，本轮未运行完整集合，最近完整 231 仍基于 `aa60729`。
Ghostty pin、公开 core ABI、生产平台绑定未变，其他平台无新增运行证据。

原始配置、全局初始化、CoreText/Metal 框架内部、libpthread/TLS 与其他线程/
GPU/跨进程总账本仍须收口；尚无生产 listener/manager/auth/writer、公开
session API、会话发现/恢复 UI/helper 打包。P0 保持未通过，P1–P8 未交付。

2026-10-20 的预热变异隔离修正（基于 `401d6f0`）：完整整合复验期间回查
验证器，发现 warmup 组最后一个 C-only 容量变异跳过 full engine build；
前一个“App 销毁漏 join”变异虽然恢复了源码，静态库仍可能保留该变异。
原容量检查确实在目标容量断言处变红，但不能把这当作只有一个故障的隔离
验证。现在每个原生变异前都从当前源码重建引擎，含 C-only 变异，确保链接
的是回滚后的正常库。此修正不改引擎、协议或预算策略，也不增加断言。

`python3 scripts/probe-session-process.py --warmup-mutations` 修正后退出 0，
十项目标红/独立对照绿、SHA256 回滚及 roundtrip/start-error/search 基线
复绿，日志 `tmp/process-warmup-isolation.log`。八项 warmup/线程入口单元
继续通过。核心 gate 113/113 与 57 公共符号通过，日志
`tmp/process-warmup-isolation-gate.log`。

修正前完整整合进程已停止，日志保留在
`tmp/process-integrated-294-pre-isolation.log`，不能记作完整 294 项通过。
配置仍为 294；最近已经完成的完整集成仍是基于 `aa60729` 的 231 项。
修正后的完整集合需重新运行，不能拼接不同轮次的定向结果代替同次通过。
P0 保持未通过，生产 daemon 与后续交付范围不变。

2026-10-20 的资源与线程完整集成复验（基于 `1cde0fa`）：
`python3 scripts/probe-session-process.py --mutations` 退出 0，同次通过二十类
原生场景、294 项变异及 26 类旧 surface 最终基线。每组源码 SHA256 回滚后
重新构建，最终再次运行全部原生和旧 surface；62 个源码/补丁哈希与起跑
记录一致。日志 `tmp/process-integrated-294.log` 实际含 294 条 target red/
control green，不以不同轮次的定向结果拼接。此前完整 231 项保留为历史记录。

新增整合部分为主线程容量 11、surface 线程容量 11、入口帧 8、备用信号栈 6、
搜索线程 7、App 共享预算 10、App 预热所有权 10，合计 63 项；与此前 231 项
合为 294。其余分项口径沿用本日完整 231 记录，其中 process 103、GUI-budget
33 不重复计入彼此。原生场景新增 search，最终 GUI 预算基线为 14 类。

本轮包含 `1cde0fa` 的验证器隔离修正：C-only 预热容量变异前同样重建引擎，
不能链接上一项已回滚源码但尚未重建的故障静态库。修正前中断的整合日志
不作为完整通过证据。真实 PTY、Metal GUI、两代恢复、查询 owner、TUI、
合成 AppKit 输入、固定配色像素、错误终态与资源回收均在当前补丁链下复验。

同一代码树 `bash scripts/gate.sh` 退出 0，113/113、57 公共符号及错误入口
检查通过，日志 `tmp/process-integrated-294-gate.log`。`otool -L` 另验权威
进程与 VT dylib 不链接 AppKit/Foundation/Metal 等 UI 框架，日志
`tmp/process-integrated-294-linkage.log`。Ghostty pin 为
`81598385d608dda4946093d6d6a234d204a2f225`，实验补丁至 0037；本轮未改实现或增加断言。
环境 macOS 26.6.2 (25G83)、Darwin 25.6 arm64、Xcode 27.1 (27A9275)、
Apple clang 21.0.0；实际 engine build 身份
`79f2a5b785d109a3e4959f25552beade47a6665e4ce309e2574410a4e8642037`。
源码/补丁/日志 SHA256 与环境见 `tmp/process-integrated-294-manifest.json`，
清单 SHA256 为 `69af4e58fccf1f6fc0e545649a73d2258e0060a0e5a391180b86847d2ee9fdef`。

完整通过仍不代表 P0 或生产 daemon 完成。历史双限额与截断摘要尚未在真实
权威/副本链路交付；原始配置/全局初始化、框架/GPU/其余系统资源总账本、
系统输入法/窗口焦点、任意主题、通用绘制完成与最终性能验收仍未收口。
连接仍是私有继承 socketpair；生产 listener/manager/auth/writer、公开
session API、发现/恢复 UI/helper 打包及 P1–P8 仍待交付，能力矩阵保持 ❌。

2026-10-20 的宿主隔离场景完成标记修正（基于 `24cffb8`）：兼容复验抓到
isolation 偶发少一份配色应答。旧等待标记 SECOND 会先匹配 OSC 通知正文
SECONDNOTE，于是主线程可能在第二次配色查询完成前就停止 worker。现在
使用独立 ISOLATION_DONE，只有子进程读完并验证应答后才输出；在第二条
通知与查询之间刻意分离 0.2 秒，验证不依赖 PTY 恰好合并两段输出。

`tmp/check-isolation-phase.py` 的真实 full surface 场景通过；改回旧标记后，
应答次数/字节断言明确变红，query-delta 独立对照仍绿，SHA256 回滚后两场景
复绿，日志 `tmp/isolation-phase-verification.log`。核心 gate 113/113 与
57 公共符号通过，日志 `tmp/isolation-phase-gate.log`。本切片只修验证器同步，
不增加生产能力，也不将历史完整复验当作“从未存在竞态”的证明。P0 未通过。

2026-10-20 的限额历史淘汰通知接缝（基于 `fa44fb0`）：实验补丁
`0038-history-pruned-notify.patch` 在 PageList 字节回收确认删除历史页后、
Limits.enforce 确认删除历史页后，通知注册的源 owner。C 入口
`ghostty_terminal_p0_set_history_observer` 可注册/注销借用上下文与回调；
无效 terminal 明确返回错误。回调只能记标记，不分配、不重入、不派发 UI。
没有提供精确逻辑行丢弃数量，也不把页级近似限额宣传为精确行上限。

primary reset 保留通知；clone/import 的独立 PageList 默认无通知，函数与
上下文指针不入快照。九类 C 基线分别覆盖独立字节限额、独立行数限额、两者
同时配置、不淘汰对照、清屏/reset 不误报、同块淘汰后 reset、调低策略立即
淘汰、导入后继续输出以及注销。检查实际保留文本：最旧内容已消失、最新内容
仍在，状态无处理错误。导入后未来输出的完整 canonical 仍与源相同。
三个新 PageList 用例具名执行并通过，所选 VT 模块共 36/36 测试。

`python3 scripts/probe-session-history.py --mutations` 退出 0，八项目标红/
独立对照绿，编译与执行分开，编译失败和信号崩溃不能冒充断言变红。变异
覆盖缺少字节/行通知、忽略注册、reset 误报/丢注册、clone 继承源通知、忽略
注销、恢复页缺增长余量；最后一项专门验证既有 0028 对未来淘汰一致性的作用。
SHA256 回滚后全部基线复绿，日志 `tmp/history-pruned-verification.log`。
验证器使用自己的隔离引擎/缓存，注入仅编译测试产物的命名构建步骤，避免
编译失败混入单元测试红；不修改 vendor 或产品构建。标记预检退出 2，活动
变异中的 SIGINT/SIGTERM 均退出并恢复三份起跑 SHA256，记录
`tmp/history-harness-check.log` 与 `tmp/history-harness-check.json`。

`python3 scripts/probe-session-process.py --case history` 原生构建/场景通过，
日志 `tmp/history-pruned-native-first.log`。随后二十类原生、26 类旧 surface、
入口帧/预热单元、75 个旧进程变异锚点唯一性及源码哈希/无 UI 链接复验通过，
驱动 `tmp/verify-history-pruned-compat.py`，日志 `tmp/history-pruned-compat.log`。
期间修复的 isolation 完成标记竞态见前一切片，不用一次重试掩盖回归。
核心 gate 113/113、57 公共符号通过，日志 `tmp/history-pruned-gate.log`。
完整进程变异集合已串行调用本验证器，配置数为 302，本轮未跑完整集合；最近
完整 294 仍基于 `1cde0fa`。Ghostty pin、公开 core ABI 与生产平台绑定未改。

这里只交付真实淘汰的引擎诊断接缝及验证；权威会话尚未注册它、显式双限额
策略/冻结 S 上的截断摘要/有序 LIVE 更新和产品 UI 尚未接线。零历史策略和
其他平台无新增运行证据。系统输入法/焦点、任意主题、通用绘制完成、其余
资源总账本与生产 listener/manager/auth/writer、发现/恢复 UI/helper 打包
仍未完成。P0 保持未通过，不能将可选回调默认空解释成“历史从未截断”。

2026-10-20 的限额历史会话接线（基于 `c5aafcc`，实验 P0）：新增
`history-limit` 场景。权威 VT 在 PTY 启动前显式设置 1 MiB 字节/100 物理行
双限额，读取引擎 getter 核对两项实际配置后才开始处理输出。上限沿用上游
页级近似语义，不声称精确保留 100 行；独立字节/行淘汰证据仍见 0038 验证器。
通知只将会话原子标记置为已截断，真实终端字节仍由 worker 直接送 surface。

私有 32 字节 HST/v1 控制记录区分未知、未截断、已截断；未知不猜限额，已知
策略要求两项正值。编码为固定大端、有保留字节校验，非法输入不修改输出。
不是公开协议或 `ghosterm.h` ABI。协议构建身份升级为
`process-history-restore-1`，不声称兼容旧实验 GUI。

冻结 S 同时捕获策略与截断状态。每条恢复事件在入队时复制同序号的控制状态，
出队后以相同 generation/sequence 发送历史帧；不从最新全局状态拼造旧事件。
客户端拒绝错代、错序、非法策略及状态倒退。真实场景在 S 后触发 1200 行
输出，验证旧快照仍为未截断、LIVE 已截断；已绘制 GUI 被 SIGKILL 后，同一
PTY 与新代 GUI 保留截断状态，并继续验证真实输入、权威回复、程序/命令摘要
和最终 canonical/resources 相等。

P0 AppKit 窗口通过原子控制状态在主线程更新 subtitle，分别显示未知、空提示
和已截断；第一代验证三次状态变化，第二代验证未知到已截断。这是 NSWindow
属性断言，尚无标题栏文字像素或可访问性证据，也不是生产 Swift UI。

`history-state` 的 18 项定向变异全部目标红/独立对照绿，回滚后协议基线及
原生 history-limit/roundtrip 复绿；覆盖协议 header/长度/窄化/策略/字节序/
事务性、遗漏通知、错误的两项实际限额、实时状态替换冻结状态、旧状态污染
事件、错代/错序、未知折叠、隐藏截断和不发布 LIVE UI。编译失败不能算红。
驱动 `tmp/run-history-state.py`，日志 `tmp/history-state-verification.log`；正式
入口为 `python3 scripts/probe-session-process.py --history-state-mutations`。
变异标记预检退出 2，活动变异中的 SIGINT/SIGTERM 均恢复三份起跑 SHA256，
日志 `tmp/history-state-harness.log`。变异过程暴露并补齐“两项限额互相掩盖”
的检查缺口：仅观察发生截断不能证明行数策略正确，必须读取实际配置核对。


本切片另通过 21 类原生场景与 26 类旧 surface、75 个旧进程变异锚点唯一性、
实际入口帧/预热用例及源码哈希；权威/VT 的 `otool -L` 不含 UI 框架，日志
`tmp/history-state-compat.log`。配色组十三项变异全绿；另验证两处重定向的旧
进程变异（初始标题与恢复队列预算）及十一项 wire 变异，目标红/独立对照绿，
回滚后协议基线复绿，日志 `tmp/history-state-retargets.log`。合计本轮复验十八项
新变异和二十六项相关旧变异，不是整套 320 项。核心 gate 113/113、57 个
公共符号通过，日志 `tmp/history-state-gate.log`。环境、源码/补丁和日志 SHA256
汇总见 `tmp/history-state-manifest.json`。

完整进程集合已加入本组，配置数为 320；本轮未跑完整集合。最近完整 294
仍基于 `1cde0fa`，不能以分项验证替代新版本整套集成。零历史/无限历史策略、
产品配置/恢复 UI、系统 IME/焦点、任意主题、通用绘制屏障、其余资源总账本
及生产 listener/manager/auth/writer、发现与 helper 打包仍未交付。P0 未通过。

2026-10-20 的完整进程集成复验（基于 `9331e42`）：
`python3 scripts/probe-session-process.py --mutations` 退出 0，同次通过 21 类
原生场景、320 项变异及 26 类旧 surface 最终基线。日志逐项包含 320 条唯一
目标红/独立对照绿记录，各组回滚后基线复绿。新增八项历史淘汰和十八项会话
历史状态验证已与原有恢复、输入、资源、线程和配色检查串行执行，不能仅把
此前的分项结果相加当作本次证据。67 份起跑源码/补丁 SHA256 与终态完全相同。

日志 `tmp/process-integrated-320.log`，核心 gate 113/113、57 个公共符号通过，
日志 `tmp/process-integrated-320-gate.log`。终态权威进程/VT 的 `otool -L`
无 AppKit/Foundation/Metal 等 UI 框架依赖，记录
`tmp/process-integrated-320-linkage.log`。期间在无 Zig 编译器活动的纯 C 变异
阶段轮换并删除整份闲置构建缓存，保留安装的引擎库及源码；之后的引擎变异
从新缓存重建，记录 `tmp/process-integrated-320-cache-rotation.json`。

Ghostty pin `81598385d608dda4946093d6d6a234d204a2f225`；终态实际引擎构建身份
`50dc6faa586df08aed77a7a953f222d6be7cd87dd41cf351e9ba83ed0a1e647d`。环境、分项通过记录与源码/日志 SHA256 见
`tmp/process-integrated-320-manifest.json`，清单 SHA256 为
`7a802985d51ceffc8bb02c739d2803171d1cfb49f273c2cb47c03b6e4b73c966`。本次替代旧 `1cde0fa`/294 作为最新完整集成记录；旧记录保留其历史范围。

同轮只读配置审计复现：正常文件加载背景 `abcdef`，超长行和目录路径却都
留下默认 `282c34` 且诊断计数为零，日志 `tmp/config-load-audit/results.log`。
因此不能仅凭诊断为空判断配置加载成功。原始 Config 对象/默认 arena/加载
暂存仍在全局 allocator，尚未进入 GUI 额度。另以真实额度耗尽验证独立配置
预算域变为故障时，App/surface 预算域仍健康且可继续分配、最终全部回收，
日志 `tmp/config-domain-audit.log`；这是预算域证据，不代表已绘制 surface 的
配置失败交互已经验证。配置构造/克隆/有界加载候选仍只在临时目录，未交付。

本轮不增加生产 daemon 能力。P0 仍未通过；原始配置、全局初始化、框架/GPU/
其余 TLS 与总预算边界、真实系统 IME/焦点、任意主题与通用绘制屏障仍需收口。
生产 listener/manager/auth/writer、公开生命周期、发现/恢复 UI、helper 打包
及 P1–P8 的其余交付仍未完成；其他平台没有新增原生运行证据。

2026-10-20 的原始配置独立预算切片（基于 `4adb76c`）：实验补丁
`0039-config-budget.patch` 为 C Config 提供宿主构造器和独立目标域克隆。
对象、默认 arena、配置克隆与初始文件暂存使用对应 allocator；释放先保存
arena 的 owner，再 deinit 和归还对象。普通 C 创建仍默认使用全局域。
借用描述符须存活到对应全部配置/克隆销毁；P0 显式克隆可选择另一域，并已验
源配置及其域释放后仍能读取克隆的动态标题字符串。

P0 严格读取初始配置至多 1 MiB，额外一字节用于区分恰好达标与越界。
当前 Zig Reader 达到 limit 即返回 StreamTooLong，首轮正向边界测试暴露了
这一包含边界问题；现已验证恰好 1 MiB 成功、超出失败。物理行至多 4094
字节，首行 BOM 单独处理；空文件、BOM 空文件及末行无换行均有正向用例。
目录/读取失败/长度越界/解析诊断有显式 false；P0 finalize 同样显式报告结果。
这只覆盖初始文件及这些检查，环境映射/路径展开的上游回退仍需后续处理。

GUI 池为根配置预留独立 8 MiB，配色更新另借同池临时 8 MiB 事务；失败自动
销毁部分配置并归还额度。根配置在 App 销毁后、池关闭前释放，不再先于池
准入创建全局占位配置。已有 Metal surface 绘制后，真实一页配置额度耗尽和
超长配置读取失败均发生并回收；根配置/App/surface 域仍健康，随后真实输入、
权威回复与最终 canonical/resources 对拍通过。配色更新继续走原有有序事件
和实际应用屏障，六个时点的背景像素检查保持通过。GUI 元数据峰值本轮观察
为 147456 字节，仍在原有 4 MiB 准入内；额度预留不是 RSS 声明。

19 组配置 API 基线、28 项新变异全部通过；每项目标红/独立对照绿，单元负例
必须以自身 FAIL 断言和退出 1 拒绝，编译失败/信号崩溃不算红。覆盖选域、
克隆、部分构造失败、回收、文件/行/BOM 包含边界、显式错误、空参数拒绝和
GUI 事务/实际故障演练接线。正式入口为
`python3 scripts/probe-session-process.py --config-mutations`；本轮使用相同
函数的缓存驱动 `tmp/config-budget-candidate/run-mutations.py`，日志
`tmp/config-budget-mutations-28.log`。变异开始前检查标记，逐项 SHA256 回滚；
每项均重新确认引擎产物，确保纯 C 变异不会复用上一项被还原的引擎。

活动冷缓存编译中的 SIGINT/SIGTERM 均验证后代终止和四份起跑源码 SHA256
恢复，标记预检退出 2，日志 `tmp/config-budget-harness.log`。这项检查先实际
发现了原构建函数直接调用 subprocess.run 绕过受控 runner 的残留进程；
`tmp/config-budget-harness-unmanaged-build.log` 保留失败，修复后将 runner
显式传入构建函数，编译与链接由独立进程组管理。

另复验 40 项旧变异（App 额度/预热各十、runtime 六、metadata 五、combined
九）、21 类原生和 26 类旧 surface，日志 `tmp/config-budget-compat.log`；
另单独复验引擎线程总额度变异，目标红/独立对照绿与回滚基线均通过，日志
`tmp/config-budget-thread-total.log`。合计复验 41 项相关旧变异。
249 处旧源码锚点按各自预期次数检查，随新配置额度更新了两处总量变异位置。
干净钉版本副本的完整补丁链可应用，配置 API/实现/头文件逐字节匹配已编译
基线，日志 `tmp/config-budget-patch-chain.log`。核心 gate 113/113、57 公共
符号通过，日志 `tmp/config-budget-gate.log`；权威/VT 无 UI 框架链接，记录
`tmp/config-budget-linkage.log`。源码、补丁和日志 SHA256 汇总见
`tmp/config-budget-manifest.json`。

完整集合已配置为 348，本轮未跑全集；最近完整 320 仍基于 `9331e42`。
这里没有生产配置入口或 daemon 能力增量。全局初始化、环境映射/路径展开、
框架/GPU/其余 TLS 和总预算边界，真实系统 IME/焦点、任意主题与通用绘制
屏障，以及生产 listener/manager/auth/writer、公开生命周期、发现/恢复 UI
和 helper 打包仍未完成。P0 保持未通过，其他平台没有新增运行证据。


### 2026-10-20：配置环境副本归属与路径展开错误（P0 未通过）

实验补丁 0040 将配置处理的环境映射副本移入调用方 allocator：包括工作目录、
浅/深色主题、普通配置路径、主题位置及默认 XDG 路径。工作目录和主题的
`~/` 展开失败向上传递，不再仅记日志后返回未展开路径；普通配置路径保留
诊断语义，P0 严格加载器拒绝诊断。全局环境初始化/驻留不在本切片范围。

`python3 scripts/probe-session-config-env.py --mutations` 从干净 pin 副本应用
完整补丁链，向隔离配置模块注入六条定向单元；所选依赖测试一共 83 项通过。
五项变异逐一证明指定 allocator、展开错误与工作目录/两档主题调用点有牙，
均为目标断言红、独立对照绿，SHA256 回滚后全绿。编译失败与信号不能计为
断言红。日志 `tmp/config-env-final-verifier.log`。标记预检返回 2，INT/TERM
实际中断已标记的编译进程组后，后代退出且源码 SHA256 复原；日志
`tmp/config-env-signals.log`。

另复验原生 `roundtrip`、`scheme`（含初始配置 API 的 19 组基线），日志
`tmp/config-env-native-{roundtrip,scheme}.log`；核心闸门 113 项/57 公共符号
通过，日志 `tmp/config-env-gate.log`。没有改原生宿主目录或公共 Ghosterm ABI。
本轮没有跑完整进程变异全集；最近完整 320 项证据仍基于 `9331e42`。
生产 daemon 仍未交付，P0 仍未通过；全局初始化、框架/GPU/其余 TLS、完整
总预算、真实系统 IME/焦点、任意主题/通用绘制屏障与生产生命周期仍待完成。



### 2026-10-20：全局引擎的宿主 allocator 与单次生命周期（P0 未通过）

实验补丁 0041 新增 `ghostty_p0_init` / `ghostty_p0_deinit`，全局初始化可
显式使用宿主 allocator，结束时释放由该 allocator 申请的全局/I/O 对象。
宿主 descriptor 必须活到全部 App/config/surface 销毁、线程 join 和全局
释放之后；释放不是仍有 borrower 时可调用的 BUSY 探针。初始化尝试失败或
成功释放后均不可重试；NULL 参数拒绝不消费尝试，legacy 与 owned 初始化
也不能相互覆盖。释放后不得再调用依赖全局状态的 API。非 C main/tool 路径
保留原 allocator 选择。

`python3 scripts/probe-session-global-owner.py --mutations` 从干净钉版本
应用完整链，通过六类基线：冷态释放拒绝、成功/配置 borrower/全局回收、
首/次分配拒绝、真实容量不足、legacy 隔离。九项定向变异均为目标断言红、
独立对照绿，SHA256 回滚后六类复绿；编译失败和信号不能计作断言红。日志
`tmp/global-owner-verifier.log`。该脚本与 process/worker-surface 共用构建
路径，必须串行运行。另实测已有变异标记时预检返回 2；SIGINT/TERM 中断
实际冷缓存编译器，后代清空且三份源码 SHA256 复原，日志
`tmp/global-owner-harness.log`。

另复验 21 类原生、26 类旧 surface 和 19 组配置 API，日志
`tmp/global-owner-compat.log`；权威/VT 无 UI 框架链接，日志
`tmp/global-owner-linkage.log`。核心 gate 113 项/57 公共符号通过，日志
`tmp/global-owner-gate.log`。没有改宿主目录或公共 Ghosterm ABI，也没有重跑
完整进程变异全集；最近完整 320 项仍基于 `9331e42`。

该入口尚未接入 P0 GUI 的统一 pool。全局 I/O 工作线程的原生栈/guard，
借用的 argv/environ、signal/locale 副作用，以及第三方库与系统框架内部
分配仍未纳入完整账本；不能把这个 allocator 入口当作总预算已完成。
产品 daemon、公开生命周期/发现/恢复 UI、全局准入及其余 P0 条件仍待交付，
P0 保持未通过。下一步需要补全实际 GUI 准入接线与全局 I/O 线程容量策略。



### 2026-10-20：全局引擎在 GUI 同池准入与有序释放（P0 未通过）

进程原型现在先建立统一 pool，再以独立 8 MiB 租约调用 owned 全局初始化；
初始配置和 App 也在该准入之后创建。已有 surface、App/配置及其线程清理后
才结束全局状态，最后释放该域。旧 surface 模式保留既有一次初始化路径。
实验补丁 0042 提供实际 allocator 身份观测，owned 期间核验与租约一致，
冷态/legacy/释放后为零；该值不跨 IPC，不延长寿命。

私有 global owner 管理初始化失败回收与 close 的 BUSY 保留。五类租约基线
覆盖无效参数、成功、池满拒绝后释放额度重试、真实分配失败，以及尚有 raw
allocator 借用块时保留域、归还后再次 close。测试不授权带着存活的 Ghostty
borrower 调用全局 deinit。实际容量契约另外同时准入两块 surface 和一份
临时配置，所有进程级域一直持有；多一页必须拒绝，释放后承诺恢复。该测试
不复制 total 算式作为期望值，能检出漏算全局 8 MiB。

`python3 scripts/probe-session-process.py --global-owner-mutations` 的十五项
新变异通过目标断言红/独立对照绿，SHA256 回滚后 roundtrip/scheme 复绿；
日志 `tmp/global-integration-mutations-final.log`。一次冷态 panic 曾被脚本
拒绝计作有效红，目标改为释放后的身份断言后整组通过。预检遗留标记返回 2，
SIGINT/TERM 实际终止冷缓存编译器及后代，四份私有源 SHA256 恢复；日志
`tmp/global-integration-harness.log`。

另复验旧配置 28 项、旧全局生命周期 9 项和两条保留全局额度的旧总量变异，
日志 `tmp/global-integration-old-config.log`、`tmp/global-integration-old-global.log`
及 `tmp/global-integration-totals.log`。261 处工作源文字锚点按预期次数检查，
日志 `tmp/global-integration-anchors-final.log`。最终 21 类原生、26 类旧 surface、
19 组配置 API 与五类租约通过，日志 `tmp/global-integration-compat.log`；
元数据峰值 180224 字节，仍在 4 MiB 预留内。权威/VT 无 UI 框架链接，记录
`tmp/global-integration-linkage.log`；核心 gate 113 项/57 公共符号通过，记录
`tmp/global-integration-gate.log`。

进程变异集合现配置为 363，本轮没有跑全集；最近完整 320 仍基于 `9331e42`。
全局 I/O 工作线程的原生栈/guard、std.Thread 的 C allocator 入口参数帧、
框架/GPU/其余 TLS 与总预算仍未收口；真实系统输入法/焦点、任意主题/通用
绘制屏障和生产 listener/manager/auth/writer、公开生命周期/恢复 UI 与打包
仍待完成。P0 未通过，不能把全局 Zig allocator 接线当作完整 GUI 内存限额。



### 2026-10-20：全局 I/O 双限额、原生容量与宿主入口帧（P0 未通过）

全局 owned I/O 现在固定最多两条工作线程、请求栈 16 MiB。每槽实际观测为
16789504 字节栈和 16384 字节连续不可访问 guard，分别由 16809984 字节承诺
覆盖；两槽合计 33619968 字节在 GUI 同一 pool 中于初始化前预留。入口参数
和 256 KiB 备用信号缓冲走既有全局 8 MiB allocator 域，退出前恢复信号注册，
不再落到 std.Thread 的 C allocator/TLS 分支。

实验 0043 与 `patches/zig/0.16.0/0001-owned-io-threads.patch` 配套：构建准备
先从所选 Zig 读取固定 SHA256 的 Threaded 源及空测试 stub，再只在 tmp/ 的
隔离副本应用补丁。不改安装的 Zig 或 vendor，保留原任务/普通 I/O 实现，
四处线程创建改经 owned registry；legacy 分支仍可使用标准线程。
`prepare-session-io.py` 拒绝版本/三份输入哈希漂移、修改过的输出和符号链接
逃逸，并在生成源旁保留 MIT 许可证。守卫破坏后拒绝写入、独立控制仍绿，
日志 `tmp/owned-io-generator-final.log`。

Threaded.wait_group 只覆盖 worker 函数，结束时外层帧还可能未释放；全局
清理先结束 Threaded，再 join 全部实际 pthread，最后释放 general/native
两个租约。关闭竞争用例刻意阻塞退出回调，去掉真实 join、只伪造计数仍被
断言抓到。future/group 的完成通知也早于 busy_count 递减，连续复用原型曾
触发瞬时 unavailable；验证预热加入有界空闲屏障，普通 I/O 饱和语义不变。

`python3 scripts/probe-session-process.py --global-io-mutations` 的 27 项
定向变异均为目标断言红/独立对照绿；四条 async/concurrent/group 创建路径、
取消、真实帧拒绝/重试、400 次复用、关闭竞争与空闲模式九类基线及回滚通过，
日志 `tmp/owned-io-mutations-final.log`。独立 C 观测还检查实际 pthread、
allocator 身份、信号缓冲位于所选分配范围，以及释放时已解除注册。
标记预检返回 2，SIGINT/TERM 中断实际冷缓存编译器后后代清空、七份私有源
SHA256 复原，日志 `tmp/owned-io-harness.log`。

另复验旧全局 9 项、旧 owner 15 项、旧配置 28 项及两条保留 native I/O
额度的旧总量变异，日志 `tmp/owned-io-old-global.log`、
`tmp/owned-io-regressions.log`、`tmp/owned-io-totals.log`。最终 21 类原生、
26 类旧 surface、19 组配置 API、五类租约和九类 I/O 基线通过，日志
`tmp/owned-io-compat.log`；元数据峰值 196608 字节，仍在 4 MiB 承诺内。
权威/VT 无 UI 框架链接，日志 `tmp/owned-io-linkage.log`；核心 gate
113 项/57 公共符号通过，日志 `tmp/owned-io-gate.log`。

完整进程变异现配置为 390，本轮未跑全集；最近完整 320 仍基于 `9331e42`。
框架/GPU/其余系统及 TLS 开销、完整资源账本与性能校准仍未完成；真实系统
输入法/焦点、任意主题/通用绘制屏障和生产 listener/manager/auth/writer、
公开会话生命周期、恢复 UI 与 helper 打包仍待交付。P0 仍未通过，不把本机
P0 I/O 的证据扩展到其他平台或完整 daemon 能力。




### 2026-10-20：真实 PTY → VT 的直接/worker 性能基线（P0 未通过）

命令 `python3 scripts/probe-session-performance.py --rounds 3 --mutations`。
每个模式使用全新进程、同一真实 raw PTY 子程序和未修改的钉版本 VT；三轮
交替先后顺序。每轮核验 4 MiB 输出和 128 次输入往返的每个字节，再与独立
分块生成的 VT 快照逐字节比较。输入延迟从发送/提交到输出实际进入 VT 并被
观测；每次发送前错开 1–7 ms，避免只测刚读完的有利窗口。分位数为 nearest
rank。空闲期间 direct 也以同样 10 ms 有界读等待，不以主线程纯 sleep 冒充基线。

本机 macOS arm64、Zig 0.16.0、Ghostty `81598385d608dda4946093d6d6a234d204a2f225`；
probe `-O2`、Core Debug、VT ReleaseFast。三轮观测区间：

| 路径 | MiB/s | 输入 p50 ms | 输入 p99 ms | 空闲 CPU % 单核 | 峰值 RSS 字节 |
|---|---:|---:|---:|---:|---:|
| direct | 90.019–118.966 | 0.038–0.041 | 0.056–0.066 | 0.158–0.171 | 4177920.000–4194304.000 |
| worker | 74.398–82.800 | 6.049–6.064 | 10.563–10.845 | 0.159–0.173 | 4308992.000–4308992.000 |

worker 输入 p50 约 6 ms、p99 约 11 ms，与其每轮 10 ms backend read 等待吻合；
这是待处理的实测开销，不设未经校准的产品性能闸门。峰值 RSS 包含最终 oracle/
快照；空闲 CPU 仅计测量进程，不含 PTY 子进程。报告附平台、构建档、编译器和
实际二进制/库/输入 SHA256，位于 `tmp/session-performance/results.json`；完整
日志 `tmp/session-performance-final.log`。输出损坏、跳过 VT 应用、输入回显错误
三项变异均命中普通断言，同变异的独立 oracle 对照仍绿，私有源码 SHA256 回滚。
预检遗留标记返回 2；实际测量进程的 SIGINT/TERM 清理与源码复原另验，日志
`tmp/session-performance-harness.log`。核心 113 项/57 符号闸门通过，日志
`tmp/session-performance-gate.log`。

范围仅为当前后台 PTY/worker/VT 链路：未测现有 GUI full surface、生产 IPC、
渲染/显示延迟、多会话公平性或整应用内存；不能代替计划要求的最终进程内 GUI
对比、P3/P8 校准或完整资源账本。完整进程变异仍配置 390，本次未重跑；此三项
为独立性能探针变异，不并入该计数。P0 与生产 daemon 仍未完成。




### 2026-10-20：worker 命令通知唤醒 PTY 读取（P0 未通过）

worker 提交命令/停止时在同一队列锁内写自有非阻塞管道，owner 在取命令前
排空通知并在读取前检查剩余队列；已有命令时 read timeout 为 0。私有 backend
另有可选 `read_with_wake`：PTY 将借用的通知 fd 加入 poll，不消费或关闭它，
不改变 EOF/PTY 生命周期；同时就绪时先读一块输出，下一轮继续处理命令，防止
连续命令把输出饿住。其他 backend 未提供该接口时保持原有 10 ms 有界 read，
不宣称有唤醒能力；PTY 也保留该上限，继续驱动积压输入的 flush。

新管道两端 CLOEXEC/NONBLOCK，直到 join 才关闭；创建/属性设置/线程创建失败
均清理。每 worker 多两枚 fd，worker 结构仍由原 allocator 域计费，管道的
内核资源等仍须在完整资源账本中收口。跨线程提交只通知 worker，不调用 backend。

`python3 scripts/mutation-check-worker-wake.py` 的十二项变异均为目标断言红、
同变异独立对照绿；六类 C 基线包含通知早于实际 poll、stop、已有两条命令不再
等待、无唤醒接口、真实 PTY 不消费通知/持续通知下输出前进，以及仅剩一个 fd
槽位时启动失败且不关闭调用方 fd。八次循环关闭后 fd 数回归，最终 SHA256
回滚；日志 `tmp/worker-wake-mutations-final.log`。遗留标记预检返回 2，
SIGINT/TERM 中断实际编译器后后代退出、三份私有源码复原，日志
`tmp/worker-wake-harness-final.log`。既有 worker 十七项变异复绿，日志
`tmp/worker-wake-old-mutations.log`。

相同命令 `python3 scripts/probe-session-performance.py --rounds 3 --mutations`
最终三轮观测如下（环境/口径与上一条后台基线相同）：

| 路径 | MiB/s | 输入 p50 ms | 输入 p99 ms | 空闲 CPU % 单核 |
|---|---:|---:|---:|---:|
| direct | 101.158–180.424 | 0.027–0.033 | 0.060–0.066 | 0.108–0.170 |
| worker | 110.354–154.494 | 0.027–0.043 | 0.085–0.106 | 0.066–0.132 |

此前 `40d3710` 中 worker p50 为 6.049–6.064 ms、p99 为 10.563–10.845 ms。
本次不据三轮吞吐波动设产品 SLA。原始结果/输入和实际库 SHA256 已保留在
`tmp/worker-wake-performance-results.json`，日志 `tmp/worker-wake-performance-final.log`。
输出损坏/跳过 VT 应用/输入回显错误三项性能探针变异另全部复绿。

核心 gate 119 项、57 公共符号通过（`tmp/worker-wake-gate-final.log`）；最终
21 类原生恢复、26 类旧 surface、19 组配置 API、五类全局租约与九类 I/O
基线通过（`tmp/worker-wake-native-final.log`）。公开 `ghosterm.h` 与平台绑定
未变。此切片未重跑完整 390 项进程变异，也不将独立十二项变异并入该计数。
完整 GUI/IPC/渲染性能、系统/GPU/TLS/全局资源账本及生产 P0–P8 仍未交付。




### 2026-10-20：私有多会话注册表与有序回收（P1-c 基础，P0 未通过）

`core/src/session/manager.{h,c}` 新增不依赖引擎/UI/协议的固定容量注册表，
最多 64 个槽；创建中、实际 join 中和保留的终态记录均占槽。成功创建才接管
调用方 context，worker join 后才调用 disposer；创建失败清零返回 ID、不接管
context，并归还槽位。ID 在单个 manager 生命周期内单调分配、不复用；它不是
生产协议的全局/认证 ID。worker 仍独占 backend/state，注册表不泄漏裸指针。

按 ID 提交、查询、全表快照、显式 terminate、reap、forget 已有实现。列表
容量不足整份不写；终态错误、退出信息和 sequence 的已知位原样保留，不把
尚未就绪折叠为成功。reap 先缓存实际结果，在锁外真实 join/销毁 context，
终态记录继续占容量直到 forget。创建/reap 在途时 close 返回 BUSY、保持可用；
正常 close 先请求所有 worker 停止，再逐一 join/销毁，最后释放注册表。
它不根据客户端数量隐式结束会话，也没有建立新的 attachment/恢复协议。

六类 C 基线验证真实 PTY 连续交互、自然 EOF、显式结束、保留记录/旧 ID 隔离、
两种异步工厂失败及未知退出信息、精确分配域/拒绝回滚、八线程争抢两槽、
阻塞最后回调时并发 reap/close，以及创建 allocator 屏障内的容量与 close。
`python3 scripts/mutation-check-session-manager.py` 十九项变异均达到目标断言
红、同变异对照绿、SHA256 回滚及最终六类基线复绿；日志
`tmp/session-manager-mutations.log`。遗留标记返回 2，SIGINT/TERM 中断真实
变异编译器后退出并复原三份私有源码，日志 `tmp/session-manager-harness.log`。

manager/worker/state-owner/PTY 和测试五份 C 源分别以 ASan+UBSan、TSan 插桩，
各六类用例退出 0；其余静态 core 依赖未插桩，不扩展为全 Core 覆盖。二进制、
源/库 SHA256 与退出码见 `tmp/session-manager-sanitizers/results.json`，逐例日志
位于同目录。完整核心闸门 125 项、57 公共符号通过，日志
`tmp/session-manager-gate.log`；公开 ghosterm.h/平台绑定未变。

这只是可复用的私有注册表基础：尚未组合进生产 daemon/runtime 或公开 API；
attachment/快照任务 owner、TTL/发现元数据、统一原生资源准入、listener/auth、
有序 writer、产品恢复与打包仍待接线。本轮未重跑完整 390 项进程变异，十九项
为独立 manager 变异，不并入该计数。P0 未通过，生产 daemon 能力仍标 ❌。


### 2026-10-20：注册表组合进跨进程恢复原型（P0 未通过）

P0 权威进程的正常/启动失败路径改由私有 manager 创建、提交、查询、终止、
join 和回收 worker，不再持有裸 worker 句柄。固定一槽，第二次创建显式拒绝；
GUI SIGKILL 前后核对同一 session 仍运行，新 attachment generation 不结束原 PTY。
终态保留到协议交付完毕，随后 forget/close。回收回调只释放 worker context 的
持有关系，探针组合根仍持有只读保存状态；不宣称已交付生产 session 对象。

十项新组合变异及十项 startup/六项改锚回归均目标红、同变异对照绿、SHA256
回滚；拆开 attachment 与 manager 分配计数，避免新增分配掩盖旧预算断言。
遗留标记退出 2、实际编译器 SIGINT/TERM 后代退出及源码复原已验。
日志位于 tmp/manager-composition-{mutations,regressions,native}-final.log 与
harness.log；21 类原生、26 类旧 surface、19 组配置、五类全局租约、九类 I/O
基线通过。复工时同组原生基线另重跑，见 tmp/manager-composition-native-resume.log。
核心 125 项/57 符号闸门通过。完整进程变异现配置 400 项，本次没有完整重跑；
最近完整集仍是 9331e42 的 320 项。此切片可复用到本地/远端同一 runtime，
生产 listener/代理/SSH 流/租约/公开 API/产品接线与完整 P0 仍未交付。


### 2026-10-20：本地与远端共用消息封装（P2 基础）

用户补充本地 daemon 同样必交付，已将 P3/P5 改为两种入口同测、同一个服务/
runtime/管理/恢复逻辑；远端仅增加 SSH 连接代理。新增 ipc/wire 是两条路径共用
的 24-byte GTSD/v1 封装，纯 C11、调用方固定缓冲、无网络/引擎依赖。拆包/粘包
精确返回消费前缀，帧头校验和独立限额在 payload 复制前完成，错误与半包 EOF
锁存；下一帧和借用生命周期由 I/O owner 显式推进。尚无消息 dispatcher/字段
schema，type/request ID 的语义校验仍待接线，不能把封装合法当成已认证或可执行。

八类 C 基线、十二组隔离变异目标红/同变异独立对照绿、SHA256 回滚通过；过期
标记退出 2，SIGINT/TERM 中断实际编译器后后代退出/源码恢复已验。八类基线
ASan/UBSan 通过。核心 gate 133 项/57 公共符号通过；日志分别位于
 tmp/ipc-wire-{mutations,harness,sanitizers,gate}.log。
协议说明见 session-ipc.md；生产本地 listener/身份、非阻塞 writer、SSH 流/代理、
租约、fuzz 与端到端恢复仍未交付，P0/P2 保持未通过。


### 2026-10-20：本地与远端共用短写续送（P2 基础）

新增 ipc/writer 单帧游标，复用 GTSD 编码，无 OS/libssh2/线程/动态分配依赖。
帧头复制、payload 有限期借用，advance 按实际写入数量推进，零进展不丢后缀；
pending 不能被替换，完成释放借用，错误计数/取消锁存失败。半帧取消必须由
I/O owner 关闭底层流，不能在同一流开启新帧；完整帧 deadline 与读取/租约公平
调度仍属后续 socket/SSH owner，不能因游标正确就宣称网络写有界。

五类 C 基线、八组目标红/同变异独立对照绿变异、SHA256 回滚、ASan/UBSan
通过；遗留标记与实际编译器 INT/TERM 后代清理复原已验。日志在工程
 tmp/ipc-writer-{mutations,harness,sanitizers,gate}.log。核心 gate 138 项/57
公共符号通过。仍无 listener/身份/SSH 代理、完整消息 schema 或生产恢复；
本地与远端同一服务的交付目标不变，P0/P2 保持未通过。


### 2026-10-20：本地与远端共用 Unix 私有端点（P2 基础）

新增 ipc/unix-endpoint，供本地客户端与远端 SSH 代理接入同一服务；无 UI/引擎/
会话依赖。目录逐级 openat/fstatat，拒绝 symlink/点路径/可写祖先，leaf 必须
当前用户 0700。startup 与 daemon lifetime 用独立非阻塞 flock，锁节点 0600/
单硬链接且永不 unlink。listen 仅在寿命锁下回收确认 refused 且 inode 不变的
本用户 socket；live/积压返回 BUSY，非 socket/外来节点拒绝，close 保留替代节点。
真实 accept/connect 核验 kernel peer UID；socket/fd 交付为 NONBLOCK/CLOEXEC，
connect 使用单调 deadline。固定对象由调用方 allocator 计费、失败回收。

十四类真实 socket/锁/fork-exec 基线与 ASan/UBSan、二十组目标红/同变异独立
对照绿变异、遗留标记拒绝、实际编译器 INT/TERM 后代退出/源码 SHA256 复原通过。
阻塞锁变异由测试父进程结束子进程后明确断言红。CTest 最初传 core/../tmp 而
触发预期的点路径拒绝，已改为构建端规范路径；生产规则未放宽。核心 gate 152
项/57 符号通过，日志 tmp/unix-endpoint-{mutations,gate}-final.log、harness.log、
sanitizers.log（后两项同 unix-endpoint 前缀）。本机 macOS arm64 证据，不替代
Linux/多用户/登录策略或 Windows named pipe。完整契约见 session-endpoints.md。

此处已实现可监听的低层端点，不代表生产服务循环已组合。接下来仍需协议/连接
限额、instance/token/握手、SSH 代理、租约、进程启动及 session/runtime 接线；
本地与远端同一服务的交付目标不变，P0/P2 保持未通过。


### 2026-10-20：共用有界帧连接与真实 socket 组合（P2 基础）

connection 通过无 OS/引擎依赖的 stream 函数表组合 decoder/writer，每 step 一次
read/一次 write、各最多 4096 字节；背压时不跳过读取。一次调用方预算分配拥有
两份独立有界 payload 缓冲，send 复制后才 ACCEPTED，pending 返回 BUSY；接收帧
释放不会破坏回复。整帧单调 deadline 不因头/body/零星进展重置，时间零合法，
IO 前后检查、时钟失败/倒退、超时、取消与非法 IO 锁存并恰好关闭一次。

socket-stream 采用实际 NONBLOCK/CLOEXEC Unix 流，读前 poll(0) 优先拒绝 HUP
后的未接受字节，对齐 attachment-peer；SIGPIPE 只用 socket/send 局部保护。
END 是连接终态，不是 shell 退出；无 session/worker 调用，不自动重放未知输入。

十六类基线包含真实命名端点帧往返、1 MiB 发送/背压与反向读取、EOF/HUP 和
SIGPIPE；确定性 fake stream 另护时钟/分块/错误。二十五组变异均普通目标断言
红、同变异独立对照绿、SHA256 回滚，十六类 ASan/UBSan 与遗留标记/实际编译器
INT/TERM 清理/十一份源码复原通过。Darwin 有两处独立 SIGPIPE 保护，变异须一起
破坏才暴露信号。核心 gate 168 项/57 符号通过；日志位于工程
 tmp/ipc-connection-{mutations,sanitizers,gate,harness}-final.log。

这里只组合组件，尚无服务级多连接调度、协议身份/instance/token、租约、会话
命令处理、SSH channel/代理或生产恢复。当前 macOS arm64 证据不推导 Linux/
Windows 支持。P0/P2 未通过，本地与远端同一服务的完整交付目标不变。


### 2026-10-20：共同 HELLO/BIND、单次授权与预约移交（P2 组件）

本地 daemon 与远端 sessiond 继续共用相同服务/runtime 目标。新增私有
`ipc/handshake` 与 `ipc/tokens`，统一固定大端字段、角色/状态/请求 ID、契约指纹/
能力/instance 拒绝、challenge ACK 和完整握手 deadline。token 固定容量分配，
绑定可信 principal/issuer/session/generation，单次消费、精确过期、定向撤销、
计数器耗尽、随机源失败与时钟倒退均显式处理；过期/关闭回调释放尚未移交的
预约。消费后责任转到握手，take 再交给数据 owner；失败只释放旧 generation，
不停止 session/PTY。握手成功后仍须走 S/L 屏障。实际生产引擎指纹生成、feature
注册和管理 owner 撤销调用尚未组合。详细字段与 API 责任见 session-ipc.md。

macOS arm64 十一类 token、十八类握手基线，含真实 socketpair/attachment、OS
随机源；三十四组变异目标红/同变异对照绿及 SHA256 回滚通过。无 token 服务的
时钟用例独立验证握手层，不把另一层保护当作本层断言有牙。连接新增已用缓冲与
销毁前清除，为十七类基线/三十组变异。四十六类 ASan/UBSan、完整 gate 198
项/57 公共符号与 build-core 通过。
遗留标记拒绝、实际编译器 INT/TERM 后代清理与 bootstrap 二十四份/connection
十三份源文件 hash 回滚已验。证据日志在工程 tmp/ipc-bootstrap-{mutations,gate,
harness}.log 及 tmp/ipc-connection-bootstrap-mutations.log。

尚无生产多连接服务、管理命令/会话 runtime 组合、SSH 代理、租约、Linux/
Windows 原生验证或真实断网恢复。本轮只完成共用协议组件，不视为 P2/P3 或整个
daemon 交付，原 P0 资源与恢复验收闸门继续未通过。

### 2026-10-20：共用服务 owner、管理请求与首轮 Linux 原生证据（P2 组件）

`ipc/server` 以同一固定容量 owner 组合 connection/HELLO/BIND 与 manager，
本地/SSH 不分叉协议。`ipc/control` 已有 LIST/INFO/TERMINATE/PING 和 64 字节
固定记录；未知结果不变成功，回复空间不足不执行命令。真实私有 Unix 端点/
PTY 证明正常断连、独立客户端 SIGKILL、关闭服务 owner 均不终止借用的 manager；
token 过期/issuer 撤销、数据代次/租约与故障单独释放。worker join/保留期、
CREATE/恢复数据 dispatcher、可执行产品与 SSH 代理仍未组合。详细字段和责任
统一在 [service 参考](session-service.md)，不把观测 data callback 当作恢复实现。

macOS arm64、Ubuntu 24.04 aarch64 各完整 gate 222 项/57 公共符号及 build-core
通过；service 24 类/37 组变异、macOS service 24 类 ASan/UBSan 与 Linux
service/握手/token/连接 70 类 ASan/UBSan 通过。Linux 另有旧 bootstrap 34、
connection 30、Unix endpoint 20 组变异及 SHA256 回滚。GCC 排版诊断已修复，
包括旧代码/测试，以及原 bootstrap 的一条变异注入，编译失败不计有效目标红。
SSH e2e 使用 internal-sftp 修复 macOS 程序路径在 Linux 不存在的问题；首次
诊断时已明确结束那条等待中的测试，不把中断运行算通过。原共享文件系统的
socket chmod EINVAL、过长 socket 路径与不可信祖先目录也保留为失败证据；
随后在工程 tmp 下的原生 tmpfs、短路径及 umask 077 重跑。237 份源码 SHA256
对照将临时源码副本与主工作区关联；两平台实际编译器 INT/TERM 清理、service
五份源码回滚通过。日志/运行环境/VM 清理见 service 参考。

上述是两个 arm64 的核心组件证据，不代表 x86_64/Windows、实际 SSH sessiond
链路或各端 GUI。生产本地与远端 daemon 必须共用的目标不变，P0/P2/P3 及完整
恢复验收继续未完成。

### 2026-10-20：CREATE 参数、提交与 nonce 查询（P2/P3 组件）

本地与远端共用 `session/create-spec`、`session/creation` 和管理 CREATE/
QUERY_CREATE。最大 8192 字节、明确 POSIX cwd/executable、shell/direct、尺寸、
TERM 与环境 policy；严格 UTF-8/NUL/数量/重复键/保留字节，POSIX 像素溢出
明确拒绝。固定容量结果表用 principal/nonce 去重，相同参数重放原结果、变更
冲突，精确 TTL 后 UNKNOWN；客户端未知时不得自动再 CREATE。manager 注册
是提交点，异步启动/终态通过 INFO 区分，不把准备或 worker admission 当 shell
已就绪。独立请求/job 的回调与字符串活到 manager join/dispose 后，过期/关闭
结果表不结束已提交 PTY；无流量 tick 清除过期原始参数。字段、状态码、所有权
与限制统一见[创建协议](session-create.md)。

macOS arm64 与 Ubuntu 24.04 aarch64 核心 gate 242 项/57 公共符号、build-core
通过。创建十二类+wire 八类基线、四十组变异验证普通目标断言红/同变异对照绿、
源码/依赖 archive hash 复原；旧 service 三十七组回归通过。两平台各四十四类
creation/service ASan/UBSan 通过；遗留标记退出 2、实际编译器 INT/TERM
清理和十份源码 SHA256 回滚已验。真实私有端点的 CREATE 回复被阻塞后断连，
新连接 QUERY 取回原 ID 且仅一次准备；实际 PTY 的 cwd/env/特殊 argv、异步
工厂错误与独立生存期另有证据。非法回复枚举测试只破坏该字段，避免被另一条
session ID 检查遮盖，编译/信号失败不计目标红。

Linux 延用工程 tmp 下原生 tmpfs/短路径/umask 077，与主工作区复制源码逐文件
校验。日志位于 tmp/creation-{gate,mutations-final,service-regression,sanitizers,
harness}.log 和 tmp/creation-linux-{final,mutations,sanitizers,harness}.log。
源码副本/归档的 SHA256、完整命令与 VM 恢复停机记录另保留于工程 tmp。

该结果不是完整 daemon：生产可执行组合根、规范化宿主基础环境、VT/恢复
数据通道、ATTACH 管理、公开 client ABI、SSH 代理及 GUI 仍未交付；生产
runtime 还需在保留 ID 后、worker 启动前注册 session/attachment 的所有权，
不能用 nonce 结果缓存当会话对象表。这里用
manager 直接驱动测试输入，不声称已实现 wire INPUT 或 SSH 断网恢复。
P0/P2/P3 的完整验收继续未完成。

### 2026-10-20：共用启动前 ID 绑定

manager 在锁内预留唯一 ID，锁外调用元数据绑定，再启动 worker/发布；creation
的 prepare 与必填 bind_id 收到同一个可信 principal。绑定失败不分配 worker，
返回保留原因的 PREPARE_FAILED，清理部分 context/槽位且 ID 不复用；同 nonce
仍重放原拒绝。工厂与回调启动前即可确定对象身份，不依赖结果缓存事后查找。
详细契约见[创建协议](session-create.md)。这是 runtime 对象表的接缝，实际
session/attachment 表与生产 daemon 组合仍待接入；不将它写成恢复已完成。

macOS arm64 / Ubuntu 24.04 aarch64 各 gate 243 项、57 公共符号与 build-core
通过；创建 21 类基线/49 组变异、manager 19 组及 service 37 组回归通过，
相关 51 类 ASan/UBSan 通过。绑定中的独立观察线程检查 BUSY/列表隐藏/close
保护；误持锁在内部有界等待中触发普通 FAIL，不能靠 watchdog 代替目标断言。
两平台遗留标记退出 2、实际编译器 INT/TERM 后代清理与十二份源码 SHA256
复原通过。受私有签名变动影响的原生进程探针重新编译，roundtrip、kill、
start-error、crossing 通过；复用已钉版本的引擎产物，未重跑完整 P0 变异集合。
日志见 tmp/registration-{gate,mutations,manager-regression,server-regression,
sanitizers,harness,native}.log 与 tmp/registration-linux*.log。Linux 使用工程
tmp 下原生 tmpfs，与主工作区逐文件核对源码；VM 用后恢复停机。公开 ABI/
平台绑定/引擎 pin 未变，P0–P8 完整验收继续未完成。

### 2026-10-21：共用宿主 runtime 与对象引用

`session/runtime` 已组合 manager/creation、启动前 ID 绑定、真实 POSIX PTY、
注入的状态工厂和每会话 attachment。引用独立于 worker/job/nonce 结果缓存；
join 保留对象，旧代 encoder 可安全跨 join 清理，forget 需终态/零引用/空闲
控制器。关闭服务不停止 runtime；显式 runtime 关闭有引用/活动任务则 BUSY，
可继续使用，条件满足才 stop/join/free。完整契约与边界见
[宿主 runtime](session-host-runtime.md)。

macOS arm64 / Ubuntu 24.04 aarch64 各 gate 252 项、57 公共符号与 build-core
通过；九类 runtime 基线/25 组变异普通目标红/独立对照绿。默认 direct 路径的 14 个宿主分配点
逐一拒绝并精确退款；真实 PTY 的 cwd/env/argv、状态/backend/apply 失败、并发
引用和旧代任务跨 join 另有证据。共用 HELLO/CREATE/新连接 QUERY 经 socketpair
组合服务/runtime；关闭管理连接和服务后原 PTY 继续执行。旧创建 49 组与 peer
17 组变异回归通过。两平台各 38 类 runtime/attachment/peer ASan+UBSan 通过；
此前 sanitizer 过滤器选到未构建的 peer 测试所致 Not Run 记录保留，补构建后
完整重跑通过，不把首次失败计作通过。两平台实际编译器 INT/TERM 后代清理、
五份源码 SHA256 回滚及标记预检已验。日志见 tmp/runtime-{gate,mutations,
creation-regression,peer-regression,sanitizers-final,harness}.log 与
tmp/runtime-linux*.log。Linux 工程 tmp 下的原生 tmpfs 与主工作区逐文件核对，
VM 用后恢复停机。公开 ABI/平台绑定/引擎 pin 未变。

这里的状态工厂是 C 模型测试，生产 VT 组合、终态快照保存/恢复、ATTACH/data
dispatcher、worker 执行时的输入授权、SSH 代理、客户端与可执行部署仍未交付；
P0–P8 完整验收继续未完成。

### 2026-10-21：ATTACH、token 与 runtime 归属组合

`ipc/attachment-owner` 固定预约表持有 runtime 引用，组合单次 token store 与
共用 server。可信 principal/issuer 由管理 owner 注入；同一 owner 只供一个
活服务使用，构造失败归还 claim，配置从 owner 派生 token/binding，不维护
两份授权事实。ATTACH(36/37) 固定 8/88 字节，DATA DETACH(48) 关闭本连接。
管理断连只撤销未消费 token；数据握手/连接接管消费后的预约，释放精确 scope，
迟到旧代不影响新代。熵/时钟/容量失败回滚，BIND 仍不授权输入。服务 scratch
排入独立 TX 后清除，owner/token store 退款前清零。字段/限制见
[ATTACH 契约](session-attach.md)。

macOS arm64 / Ubuntu 24.04 aarch64 各 gate 264 项/57 公共符号与 build-core
通过；十二类新基线/41 组变异、相关 82 类 ASan/UBSan 通过。已有 runtime
25、creation 49、service 37 组变异回归通过；后加服务 scratch 清除再验证新
41 组和完整核心闸门。真实 socketpair/PTY 覆盖回复未读、issuer 消费前后断开、
重放/身份/旧代/精确到期与空闲 tick、数据租约、DETACH、构造/熵/时钟/容量；
独立状态模型与 data 观测器不冒充真实 VT/恢复。两平台标记预检、实际编译器
INT/TERM 后代清理和七份源码 SHA256 回滚通过。Linux 源码在工程 tmp 下原生
tmpfs 逐文件对照，使用后恢复 VM 停机；源码快照、产物 SHA 与日志位于
tmp/attach-{source-proof,mutations-final,gate-final,runtime-regression,
creation-regression,server-regression,sanitizers,harness}.log 与
tmp/attach-linux*.log。公开 ABI/平台绑定/引擎 pin 未变。

生产 VT/终态快照、完整 data dispatcher、worker 执行时输入授权、公开 client、
SSH stdio 代理、可执行启动/安装与各端恢复入口仍未完成；P0–P8 继续未交付。

### 2026-10-21：USER worker 执行许可与结果三态

私有 USER 提交在同一有界队列复制 stamp/bytes，worker 写入/resize/apply 前
核验 attachment generation、LIVE/只读、L..dequeued 范围与递增 command ID。
当前代被拒绝的 ID 仍消耗；旧代不能改新代计数，新代重置，禁止回绕。策略
拒绝只完成该请求，backend/state 错误仍保留真实终态。completion 明确区分
NOT_EXECUTED/OPERATION_OK/OUTCOME_UNKNOWN，即使两种错误码相同也不混淆；
OK 不表示 shell 执行。许可是线性化点，不撤回已提交 backend 的在途字节。
原裸提交直接改名 `submit_internal`，只供可信宿主/探针，客户端不得回落。
没有旧名别名或公开 ABI 变化；详细线程/引用/重试契约见
[USER 命令](session-user-commands.md)。复制 payload 在完成/取消前清除，worker
在 join 后清除再退款。

macOS arm64 / Ubuntu 24.04 aarch64 各 gate 273 项/57 公共符号与 build-core
通过；六类 worker 与三类真实 PTY/runtime 基线、34 组隔离变异通过，普通
目标 FAIL/独立对照与 SHA256 回滚齐全。相关 115 类 ASan/UBSan 通过；相同
AUTH 状态分别验证策略拒绝未执行和 backend 已尝试未知，拒绝不能误杀 session。
旧 worker 17、manager 19、creation 49、runtime 25 组变异回归通过。两平台
实际编译器 INT/TERM 后代清理、十四份源码复原及标记预检通过。

受私有 API 改名影响的原型 C 源重新编译/链接，复用已钉引擎产物的 21 类
原生进程与 26 类旧 surface 基线通过；没有重跑完整 P0 变异集，也不代表
原型已转换成 USER wire 或 P0 通过。未修改 pin 的 VT 另重建，可信 internal
路径的 direct/worker 三轮性能探针及原三项变异通过；原始数据/编译与库 SHA
另保留于 tmp/user-command-performance-final.log 和探针 results.json，不作为
USER/wire/GUI 的端到端 SLA。日志见 tmp/user-command-{mutations,
gate,worker-regression,manager-regression,creation-regression,runtime-regression,
sanitizers,harness,native}.log 与 tmp/user-command-linux*.log。源码快照/原生
tmpfs 对照、产物 SHA256、完整命令及 VM 恢复停机记录保留在工程 tmp。

INPUT/RESIZE 的 wire/result 邮箱、真实 VT/终态快照/data dispatcher、公开
client ABI、SSH stdio 代理、可执行启动/安装与各端恢复仍未交付；P0–P8 继续
未完成。

本轮审阅另确认 POSIX `make_winsize` 把 u32 像素强转为 unsigned short；
后续独立修复与验证见下一项。

### 2026-10-21：共享 POSIX 尺寸能力准入

CREATE、直接 PTY open/resize 与共用 runtime 的 USER resize 采用同一私有
尺寸检查：像素宽/高大于 65535 明确拒绝，resize 零列/行 INVALID，open
保留 80×24 默认单元；零像素与 65535 均可精确传入内核。USER 能力拒绝在
复制/排队前返回，不分配、不回调、不消耗 ID，不改变 state/PTY 或停止会话。
直接 open 仍以既有 NULL/诊断报告失败，没有扩展公开 ABI 或承诺 Windows
ConPTY 的同一像素限制。详见 [USER 契约](session-user-commands.md)。

新增三类真实 PTY 测试由 exec 子进程读取内核 winsize，覆盖默认/最大值、
四种越界和零单元拒绝后的原尺寸；一类 runtime 测试拒绝后用同 ID 成功 resize，
原 shell 继续输入。13 组独立尺寸变异只接受普通 FAIL 退出 1，并有同变异独立
绿对照；编译错误不算红。CREATE 的原尺寸变异随共用检查移动，仍维护原有
语义测试，不将两个校验点同时移除后漏掉验证。

macOS arm64 / Ubuntu 24.04 aarch64 的四类基线/13 项尺寸变异、原 CREATE
49 项与 USER 34 项变异、相关 119 项 ASan/UBSan 均通过；两端核心闸门各
277 项、57 公共符号与快速构建通过。新脚本的遗留标记退出 2、实际编译器
INT/TERM 后代清理和六份源码 SHA256 回滚均通过。Linux 在原生 tmpfs 编译/
运行，两端 231 份源文件/脚本 SHA256 对照与库/测试产物散列另保存于工程
tmp/dimensions-{source-manifest,native-proof}.json；日志见
tmp/dimensions-{gate-final,mutations,creation,user,sanitizers,harness}.log 与
tmp/dimensions-linux-*.log。旧 PTY transport 的 12 项变异/对照及回滚亦绿，
记录在 tmp/dimensions-transport.log；其旧阻塞用例保留原超时判据，不计入新
尺寸变异的普通 FAIL 证据。未改平台代码或引擎 pin；本轮不重新宣称 GUI/
SSH/P0 证据，INPUT/RESIZE wire/result dispatcher 仍待交付。

### 2026-10-21：共用 USER wire 与有界异步结果

`ipc/user-wire` 为 INPUT/RESIZE/COLOR 与 REJECT/RESULT 提供固定大端 schema，
不发送 C struct；严格校验方向、长度、保留字、stamp 与结果三态矛盾。
`ipc/user-dispatcher` 组合现有 runtime/预约 owner，RX 在 worker 复制后释放；
每次至多一收一发，回调只复制 POD。固定 peer/命令槽保存完成结果，不发布
排队成功 ACK，不持有 connection 或走 UI 队列。worker 队列满显式 REJECT，
邮箱满关闭该 DATA 连接；断连不停止 PTY，未知命令不重发。

server 新增配对 data_drop 退休钩子：先撤销本代、再通知 dispatcher、最后
销毁 connection。旧回调在完成最后一次邮箱访问前保留槽与 runtime 引用，
close 返回 BUSY；定期 reap 回收，旧 scope 不碰新代。四类 wire 与七类真实
socketpair/PTY 基线、37 项新隔离变异通过；提前释放引用的变异必须由普通
BUSY 断言抓到，崩溃不算证据。构造校验用例先释放 server claim，避免被
另一失败遮蔽。旧 server 变异的单行锚点随退休结构移动，原语义/对照未减弱。

macOS arm64 / Ubuntu 24.04 aarch64 各 288 项完整闸门、57 个公开符号、快速
构建与相关 107 项 ASan/UBSan 通过；macOS 新数据路径 7 项 TSan 通过。
两端旧 ATTACH 41、server 37 变异亦通过。新脚本的残留标记退出 2、实际
编译器 INT/TERM 子进程清理、十份源码 SHA256 回滚已验；237 份源码/脚本
在原生 Linux tmpfs 与工作区对照，日志/产物散列及归档保留于
tmp/user-data-{gate,mutations,attach-regression,server-regression,
sanitizers-final,tsan-final,harness}.log 与 tmp/user-data-linux-*.log。

详见 [USER 契约](session-user-commands.md)。S/L 消费者仍是 C 模型，未交付
完整恢复 router、生产 VT/终态快照、SSH 代理、公开客户端、启动/安装或 GUI
接回；没有将单独 USER 子处理器或以上组件测试标作 P0/完整 daemon 通过。

### 2026-10-21：终态冻结对象与 runtime 保留

恢复路由前置审阅发现 runtime 没有保存 worker 退出后的 state：原 close_state
会销毁源对象，已退出会话无法再用它捕获快照。现增加私有 final_capture：
backend 关闭后、state 关闭/队列清理/result 发布前调用一次，健康 EOF/stop
冻结独立对象；错误或未建立 state 不导出。普通冻结失败与进程结果分别保存，
NULL 成功契约失效则保留 CAPTURE/owner 错误。显式 stop 仅保存已观察前缀，
不宣称排空了未读 PTY 尾部。

creation 转发原 context/status/capture；runtime 保存 frozen 并在 worker
result 真实发布后开放借用。query 不消费对象、join 不释放对象，异步 encoder
须持 session 引用；零引用/已 join/控制器空闲后 forget 或宿主 close 才调用
工厂 release_frozen。工厂与引擎分配域继续存活，不能依赖已关闭 state。

十类 worker、四类真实 PTY runtime 与原 invalid 构造基线、20 项新隔离变异
通过；包括后台 close_state 阻塞时禁止提前发布、覆盖源 model 后的独立内容、
多引用/重复查询/最后释放、失败与已知序号 0。两端各 302 项完整核心闸门、
57 公共符号/快速构建与相关 130 项 ASan/UBSan，macOS 15 项 TSan 通过；
旧 runtime 25 和 creation 49 项变异亦绿。新脚本普通 FAIL/独立绿对照、
残留标记退出 2、实际编译器 INT/TERM 后代清理、十份源码 SHA256 回滚已验。
239 份源/脚本在 native Linux tmpfs 与工作区对照，源码归档、产物 SHA256 和
日志在 tmp/final-state-{gate,mutations,runtime-regression,creation-regression,
sanitizers,tsan,harness}.log 与 tmp/final-state-linux-*.log。

详见 [runtime 终态契约](session-host-runtime.md#终态冻结与保留)。当前状态适配
仍为 C 模型；生产 VT frozen/终态编码与 DATA 快照/增量/READY/终态/心跳
router、SSH 代理、公开 client、启动安装及 GUI 恢复仍未交付，P0–P8 未完成。

### 2026-10-21：共用 DATA 恢复路由

`ipc/data-wire/router` 通过同一个非 UI owner/connection writer 组合原 worker/
attachment/task 与 USER 子处理器：捕获 S、后台编码、BEGIN/CHUNK/END、
READY(S)、S+1..L、LIVE_BEGIN(L)、WAIT_L 后续输出、READY(L)、有序终态。
OUTPUT/RESIZE/COLOR 全部有同一 sequence，native 尺寸/配色转成固定大端，
不发送 C struct；终端 bytes 不进 UI 队列。心跳只为有效入帧续 server 租约，
不重置整个恢复期限；错误收尾也有独立固定期限。

每代一个稳定捕获/task context 持有 runtime 引用，旧代清理按精确 task ID；
捕获排队或编码在途时断连，close 仍 BUSY，不能提前释放。CAPTURE 在 worker
退出清理阶段取消时等终态 result 发布，再借用已保存 frozen 恢复，不重开
shell、不重复活跃捕获。SESSION_END 分开表示可导入状态和 NO_SNAPSHOT/
state_status；未知序号、已知零及健康退出但冻结失败保持独立。

失败先撤销 generation，再由 server 的 data_revoked 有界发送 ERROR/等待
确认；途中旧请求/心跳不能执行、延长期限或抢掉错误回复。编码器的空/超限/
错误部分 blob 都释放。构造先做元数据限额拒绝，再调用 allocator；内层另一次
拒绝不能遮蔽上层校验。blob/引擎来源/线程的其他预算仍须生产宿主另行准入。

五类 wire、一类精确任务回收与十四类真实 socketpair/PTY 路由基线、47 项新
隔离变异通过；涵盖 S<L<final、WAIT_L 持续输出、USER 前后闸门、新代只读
终态接回、捕获/编码/终态发布三种在途阻塞、错误确认/期限及固定资源回收。
大小期望独立于实现常量；分块超限使用零偏移，避免被总长度检查遮蔽。
仅接受普通 FAIL/exit 1、同变异独立绿对照，编译/信号/外部 watchdog 不算红。

macOS arm64 / Ubuntu 24.04 aarch64 各 322 项完整闸门、57 个公共符号与快速
构建通过；两端相关 162 项 ASan/UBSan、macOS 15 项 TSan 通过。旧 server
37、ATTACH 41 与 controller 全部变异/回滚复绿。新脚本残留标记退出 2、
实际编译器 INT/TERM 后代清理、十六份源码 SHA256 回滚通过；245 份源码/
脚本在原生 Linux tmpfs 与工作区对照。日志/产物散列及源码归档保存在工程
`tmp/data-router-{gate,mutations,server-regression,attach-regression,
controller-regression,sanitizers-final,tsan-final,harness}.log` 和
`tmp/data-router-linux-*.log`。

协议与边界见 [DATA 契约](session-ipc.md)。snapshot 编码器/接收消费者仍为
C 模型，无生产 VT 编码/兼容校验/GUI barrier、SSH 代理、公开 client ABI、
可执行 daemon/安装或跨 OS/CPU 导入；本切片没有通过 P0 或完成整个 daemon。

2026-10-17 已补全计划、建立 P0 VT 探针并恢复核心基线；尚未实施 session runtime/daemon。初次闸门因缺 `cmake` 退出（127），补齐 CMake/pkgconf 后发现并修复可选 SSH 后端既有构建与错误契约问题（`5203fca`）。最新 `scripts/gate.sh` 9/9、57 个公开符号对拍及 `scripts/build-core.sh` 通过；关闭 libssh2 的独立构建与四项非 SSH 测试通过，不能将此表述为无后端时 SSH e2e 通过。最初的 P0 探针仅有三组局部验证及四组定向变异；后续 full surface、真实 PTY/TUI 与资源原型进展见上。资源探针默认仍明确返回 3（P0 未完成），整套恢复事务仍待验证。

代码重构完成必须逐条填证据，以下各项当前均未完成：

- [ ] P0 ADR 与可重复原型证明 export/import、唯一应答来源、输入模式一致及有界快照；记录 pin、补丁和运行结果。
- [ ] P1 新 ABI/各端已有绑定同步；内部头不被宿主引用；无旧生命周期 API、重复 owner 或静默降级路径。
- [ ] P2 协议字段/状态表及编码样例齐全；身份、token、并发启动、半包/溢出/超时/fuzz 有运行证据。
- [ ] P3 同一 sessiond 的本地 IPC 与 Linux 远端 SSH/代理/PTY 生命周期、限额、排空和压力通过；detach/断网/代理退出不杀会话，资源泄漏、取消和 owner 可验证。
- [ ] P4 真实 TUI 恢复、宽字符/半序列/图像资源、查询次数、宿主副作用、尺寸与 S/L 序号测试通过。
- [ ] P5 本地 IPC 与 macOS 客户端 → Linux sessiond 均可发现/接回；本机 helper、远端用户安装、真断网/GUI SIGKILL 与第二种原生客户端有证据；关闭/结束、UNKNOWN/认证/版本错误可见，签名档位诚实标注。
- [ ] P6 其他客户端与 macOS/Windows/本地宿主分开原生验收；Windows ConPTY/named pipe/Job 与 GUI 引擎各自取证，未验证仍标待定。
- [ ] P7 真 sshd 认证/hostkey/取消/秘密材料、静默断网/租约/重连/丢确认、多架构部署/登录清理策略通过；不重复输入，SFTP/exec 独立回归通过。
- [ ] `scripts/gate.sh`、`scripts/build-core.sh`、`scripts/check-abi.sh`、`scripts/check-abi-apple.sh`，以及修改端对应的构建与原有平台闸门通过；环境缺失/SKIP 单列，不能算已通过。
- [ ] 所有新增断言具备 AGENTS.md 要求的变异验证，故意破坏 detach、应答隔离、恢复边界、长度检查、背压和身份验证分别使目标测试失败，回滚复绿且 hash 一致。
- [ ] AGENTS.md 目录与平台规则、架构总览、session-runtime ADR、session-ipc 字段表、roadmap、能力矩阵、helper 打包说明和许可声明同步；旧路径检索与链接检查完成。
- [ ] 基于同环境旧路径的吞吐、输入延迟分位数、空闲 CPU、峰值 RSS 与单会话开销完成对比；P0/P3 设定的预算通过，无无证据性能承诺。

验收记录注明 OS/架构、工具链、commit、测试命令、结果与证据位置；失败和未覆盖项保留，不用总测试数代替上述逐项证据。测试临时产物统一位于工程 `tmp/`，长期可审阅结论写入相应架构/协议文档。


### 2026-10-21：Linux 预编译包与 macOS 客户端用户态部署

已接入固定清单打包、实际 GPL 源归档、非阻塞 SFTP 安装、全表 SHA256、精确契约
探测及本机 renameat 原子发布。远端不编译、不安装系统依赖、不提权、不新增端口；
失败不影响已有 release/运行中 daemon，确认丢失保留 publication_unknown。
实测 macOS arm64→Ubuntu aarch64/glibc 2.39，当前包最低 glibc 2.34。
完整边界、重现入口与未完成项见 [Linux 部署](session-linux-deploy.md)。
macOS/Linux 核心 350 项、57 ABI；部署 12 组、启动/代理/发布 42 组变异通过。
已部署 Linux helper 经真实 SSH 完成原 PID 恢复；自动网络重连、公开 ABI、原生
产品入口和完整 P0 仍未交付。当前严格不兼容，协议演进继续留作后续议题。


### 2026-10-21：本地与 SSH 共用自动恢复协调器

私有 session-client 组合现有 bootstrap/control/attachment，以相同 provider 接缝
接本地 Unix stream 与远端 SSH exec。固定总恢复期限、退避、原 instance/session、
CREATE 不明只 QUERY 原 nonce、旧代 ATTACH_BUSY 等待、应用屏障和禁止输入重放
已接入。17 类基线、19 组变异验证，细节见 [协调器](session-client.md)。
真实 macOS→Linux 网络夹具注入 RST 与静默丢包，保留旧上游直到租约回收；全新
SSH 认证自动恢复同 session/PID、更高代数，真实 VT 重导入并继续输入/接收终态。
终态可明确 unknown，不能强制折叠成退出码 0 或伪称已知为 7。
生产认证 provider 的取消/总预算、公开 ABI、原生 Surface/UI 与完整 P0 继续实施。

该切片补充：macOS/Linux 核心 367、daemon 418、57 ABI；协调器 19 组变异与
真实网络夹具 3 组变异通过，后者保护实际 RST/静默丢包/旧上游租约证据。
macOS ASan/UBSan、Linux ASan/UBSan/LSan 与变异脚本信号/标记/SHA256 复原通过。


### 2026-10-21：非阻塞 SSH 认证连接器

已补共用 C 连接器：TCP/握手/主机密钥/方式探测/userauth 分步可取消，总期限
覆盖确认等待。已解析地址与 hostkey 身份分开，原生异步 DNS 尚待接线。
真实测试修正了 socket 改回阻塞导致 libssh2 读超时失效的问题；补充旧同步路径
失败握手不得冒充已核验、材料替换/释放擦除及独立 ssh-keygen 指纹对拍。
Mac/Linux 核心 374、57 ABI、18 组变异与 Mac ASan/UBSan、Linux ASan/UBSan/LSan
通过；provider、公开 ABI 和原生入口继续实施。见 [SSH 认证连接器](session-ssh-connector.md)。


### 2026-10-21：原生请求准入与非阻塞 SSH provider

共用 C provider 接住异步 DNS/凭据/hostkey UI 的请求 ID 与迟到结果，连接器
实际驱动 TCP/SSH、认证与专用双通道 owner，替换了真实网络夹具的阻塞认证
worker。认证总期限覆盖原生等待，取消只通知一次；平台临时路径深复制，
旧请求/重复完成/回调重入拒绝，材料与流有明确所有权。
Mac/Linux 核心 385、daemon 436、57 ABI，11 类基线、19 组变异、Mac ASan/UBSan、
Linux ASan/UBSan/LSan 与无后端明确 UNSUPPORTED 通过。
真实 Mac→Linux RST/静默丢包及 3 组夹具变异已用新 provider 重跑通过；原
PID 继续输入/终态，旧上游留存至租约回收。
详见 [SSH provider](session-ssh-provider.md)。公开 ABI、原生异步 DNS/Keychain、
Surface 重导入和产品入口继续实施，不将测试环回地址当作 DNS 产品能力。

### 2026-10-21：原生空 Surface 与后台导入接缝

full libghostty 新增只读空副本，IO owner 一次性导入 canonical/continuation/资源，
实际 applied 与 LIVE 后才开放当代输入。每次 attachment 创建新 Surface，宿主
后续交换同一逻辑 pane/tab 的视图，不在已有 Terminal 上替换内存对象。
九类原生基线、九组有指向性的变异、真实 Metal 红/绿像素、原有 26 项 Surface
回归，以及标记/信号/后代清理/SHA256 回滚通过。证据与边界见
[Surface 后台导入](session-surface-import.md)。该切片尚未接入产品入口，也不代表
完整 P0；strict hash 变化要求重建 Linux payload，与当前不兼容路线一致。

### 2026-10-21：本地 stdio provider 共用恢复协调器

本地两条 socketpair 经同一个 session-proxy 接 sessiond，复用 SSH 的协议和
session-client。进程启动采用描述符隔离，stderr 有界排空，错误退出保留原因；
取消只回收自身代理，250ms 后升级仍不退出的代理，不结束 daemon/PTY。
重试先回收上一代、固定最多两个代理，异步 quiesce 后才允许释放 owner。
八类基线和十一组有指向性变异在 macOS/Linux 通过，macOS ASan/UBSan 通过；
真实本地代理恢复同 session/PID 并继续输入与终态已验证。
见 [本地 provider](session-local-provider.md)。公开 ABI/原生产品入口继续实施。

### 2026-10-21：公开 session C ABI 与 Apple 薄绑定

经 ghosterm.h 导出 opaque session，组合相同的本地/SSH provider 与恢复协调器；
提供 create/resume、分步驱动、实际应用屏障、INPUT/RESIZE/COLOR、异步认证准入、
终态与非阻塞 detach/回收。输入保留编码时的代数/实际应用序号，不能在排队后
重新盖成未来状态。Swift 绑定只搬运 POD/字符串/借用内存与 owner 生命周期。
Mac/Linux 核心 398、daemon 452、68 公共 ABI；Apple 66 项绑定+2 项原登记、
7 独立公开头、Mac ASan/UBSan/Linux ASan/UBSan/LSan 与 OFF 后端通过。
19 组 C ABI、6 组 Swift、49 组 attachment 变异通过；本地与真实 sshd 的生产
C ABI 已验证原 PID 恢复、resize/color、继续输入及结束后的 readonly 再恢复。
详见 [公开客户端 ABI](session-client-api.md)。尚未接 Surface/UI 产品入口、
原生异步 DNS/Keychain、公开部署 bootstrap/列表/TERMINATE，完整 P0 继续实施。

### 2026-10-21：原生异步解析、公开认证/安装与新 Linux payload

公开 setup ABI 包装共用连接器/安装器，Apple DNSServiceGetAddrInfo 不阻塞 UI，
地址与 hostkey 身份分离；认证 take 一次后由临时专用 SSH 安装，长连接再认证。
安装进度限频、跨队列取消、回调内禁止释放 SSH、失败保留 publication_unknown。
Mac/Linux 核心 400、daemon 454、76 ABI；14 C/11 原生变异及原 6 绑定变异、
sanitizer/OFF 后端/Debug-Release/标记-信号-SHA256 回滚通过。
新补丁集的 Linux VT 已重建；原生 Swift 从 Mac 上传并安装新 aarch64 payload，
安装后公开 session API 接回原 PID/更高代，继续输入与终态/只读再恢复通过。
见 [原生 setup](session-setup-api.md)。尚需产品 Keychain/凭据 UI 编排、catalog
随包、Surface/UI 交换；完整 P0 和 x86_64 远端仍不冒称已完成。


### 2026-10-21：原生 replica Surface 绑定与只读 vendor 构建

空副本通过独立配置继承字体并固定 Source 配色；容器字节只在 IO owner 导入，
INPUT 保留编码时 stamp，配色在 UI 只传标量并等待实际应用。真实键入发现并修复
导入前动态 Surface 的递归转发。SDK 改在 tmp/ 隔离生成、build/ 原子发布，全部
Apple 构建/打包/变异脚本消费同一位置，vendor 源码不改。
11 原生基线/13 引擎变异、26 旧 Surface、5 Swift/C 真实绑定测试/7 变异、
allocator-only 符号隔离变异及标记/信号/SHA256 回滚通过。
详见 [Surface 导入及原生绑定](session-surface-import.md)。这仍非产品 UI、完整
P0 或新 Linux 包的交付；后续继续同一 pane 的 Surface 交换、Keychain/安装流程、
helper/catalog 随包与 Linux 配套重建，不添加旧后端兼容。


### 2026-10-21：原生 Surface Transport 与真实 Linux 网络恢复

共用公开 session ABI 直连 full Surface；快照字节留在 IO，主线程只收创建 policy、
Surface 身份和状态。输入在进入 GCD 前限额，保留编码 stamp，断连立即撤销并清空，
UNKNOWN 单独提示且继续恢复。真实本地 PTY、Mac→Linux 原生 Surface 的同 PID/
更高代/新视图和旧代输入拒绝通过；真实 reset、静默丢包和旧 upstream 租约到期后
重新认证/导入通过。新 46 补丁集 Linux 库/预编译包已原生上传安装并实测。
详见 [原生 Transport](session-native-transport.md)。继续 AppModel/同 pane 接线、
会话列表/创建结果持久化/显式终止、Keychain/安装编排与随包；完整 P0 未完成。


### 2026-10-21：公开管理 API、跨进程创建查询与显式终止

公开 pending_nonce/query-only 模式，须绑定原 instance；has_admitted_create 在
CREATE flush 前即可持久化，未知/过期不自动创建。catalog-only 复用同一 provider/
management 心跳，不要 Surface、不开 DATA，nil/已知空、refresh 与断线撤销分开。
TERMINATE 与 detach 分开，保留 PENDING/KNOWN/UNKNOWN，接受不冒充进程退出，
未知终止不自动重发。Apple 薄绑定同步新字段/3 个入口，ABI 标记 BREAKING。
Mac/Linux 407 核心/461 daemon/79 ABI、Apple 77+2 对拍通过；31 C ABI、15 管理、
10 原生管理及原 19 协调器/6 绑定变异通过，标记/信号/SHA256 回滚通过。
Mac→Linux 46 补丁的已安装 helper 另经生产公开 API 查询 nonce、列会话、终止活
进程并观察结果通过。Linux 构建权限曾被现有启动信任校验拒绝，修正自有构建
产物权限并复绿，未降低校验。详见 [公开 API](session-client-api.md)。接下来继续
持久化/列表 UI、AppModel 默认入口、Keychain/安装编排与随包，完整 P0 未完成。
