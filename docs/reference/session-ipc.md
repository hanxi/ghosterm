# 会话协议：本地 IPC 与 SSH 共用封装

状态：**共用封装/管理、USER 与 DATA 恢复路由已有私有 C 实现；生产服务未交付**。日期：2026-10-21。
部署与恢复契约见[远端会话架构](../architecture/remote-session.md)，实施范围见
[daemon 计划](plan-session-daemon.md)。本文不是一个已可连接的 daemon 接口。

## 共同入口

同一 sessiond、注册表、session/attachment、恢复事务和协议处理器供两条路径使用：

- 本地客户端 → 私有 Unix socket / named pipe → sessiond。
- 远端客户端 → SSH exec 流 → 连接代理 → 同一类私有端点 → sessiond。

代理仅转发连接；不复制 daemon 的控制面、PTY owner 或恢复逻辑。两种传输都可
任意拆包、粘包和短写，不能依赖一次 read/SSH packet 等于一条协议消息。
[Unix 私有端点](session-endpoints.md) 已有目录/两把锁/监听/连接/内核 peer UID
基础与真实 socket 证据；尚未组合协议处理器、session runtime 或 SSH 代理。

## 编码选型与版本兼容

当前使用自定义二进制 GTSD/v1，不使用 Protobuf。帧头、管理/输入消息的固定宽度
字段由共享 C 编解码器显式序列化；可变字节字段有独立长度及额度。终端快照使用
独立的 [GTVC 容器](session-checkpoint.md)，其内部引擎状态/资源仍有自己的格式
契约。SSH 和本地 IPC 只替换字节流，不各自定义消息格式。

首版不承诺向后兼容旧后端，也不保留旧 ABI/协议适配层。HELLO 同时核验 wire
版本、跨 OS/CPU 的协议/引擎契约指纹及必需能力；同样写着 GTSD/v1 不代表引擎
快照必然兼容。不同平台的二进制 SHA256 用于安装完整性，不能代替兼容契约。

遇到不兼容版本明确拒绝连接，保留旧 daemon、原 PTY 和会话，不隐式升级、迁移
或清空。升级保留旧版本产物，使用匹配版本入口管理活跃会话；后续若引入兼容
窗口，须另定义消息演进规则、版本协商、快照迁移与跨版本对拍，不能仅因换成
Protobuf 或增加 capability 位就宣称旧后端兼容。

### 后续议题：协议与版本兼容（暂不实施）

当前开发期指纹包含相关编解码实现文件的 hash，适合拦截候选契约漂移；新增
实现也可能改变指纹，因此它不是“客户端与后端可独立升级”的长期版本策略。
当前明确按最快的严格不兼容方案实现，不加旧后端适配、版本协商或兼容窗口。
以下仅记录后续讨论方向，不作为本次 Linux SSH 全流程的前置条件。

后续若需要客户端/后端独立升级，再讨论将以下兼容域分开：

| 兼容域 | 计划规则 | 不兼容时 |
|---|---|---|
| 管理/传输协议 | 定义主/次版本、稳定 schema 标识、能力协商、必需/可选字段和未知字段规则；兼容新增尽量不改变主版本 | 缺少某项能力明确报不支持；破坏性协议变化才拒绝整条连接 |
| 终端快照/资源 | 独立声明引擎、状态与资源格式兼容标识；只有实际证明互通的版本才可导入 | 保留旧 daemon/会话，由匹配版本入口接回，不自动迁移或清空 |
| 安装产物 | 版本目录、逐文件 SHA256、固定库路径与原子激活 | 校验/探针失败回滚安装，不修改正在运行的实例 |

现有 v1 消息使用严格长度，不能在尾部随意加字段或把未知字段默认忽略。
可扩展编码须先定义明确的新 schema/协商入口，再实现双向跨版本测试；新增
消息优先按 capability 启用。是否继续用手写 C codec 或采用 Protobuf 是编码
实现选择，不能代替上述兼容策略。若决定采用长期稳定格式，再明确兼容标识如何避免被内部重构或非协议改动
影响；当前不提前实现这套策略。

若后续决定实现兼容，验收至少包括新客户端→旧后端、旧客户端→新后端、缺少可选功能、未知可选
字段、缺少必需能力、破坏性版本拒绝、快照不兼容及有活跃会话时升级/回滚。
这些验收暂不实施；当前代码仍严格匹配，不宣称已经向后兼容。

## 固定帧头

所有整数用大端编码；禁止直接发送 C struct 的内存布局。

| 偏移 | 长度 | 字段 | 当前约束 |
|---|---|---|---|
| 0 | 4 | magic | ASCII `GTSD` |
| 4 | 2 | version | 1；不兼容版本拒绝 |
| 6 | 2 | type | 非零 u16 消息 ID；由后续协议处理器校验支持集 |
| 8 | 4 | flags | 保留，必须为零 |
| 12 | 4 | payload_length | 不超过 1 MiB，且不超过连接配置的独立上限 |
| 16 | 8 | request_id | 由后续处理器按消息类型/方向校验；封装层只保存数值 |
| 24 | payload_length | payload | 原始字节，不在封装层作 UTF-8 转换 |

固定编码样例：type `0x1234`、长度 3、request_id `0x0102030405060708`：

```text
47 54 53 44 00 01 12 34 00 00 00 00 00 00 00 03
01 02 03 04 05 06 07 08
```

该 ID 仅是编码测试样例，不是已发布的消息。当前实现为
`core/src/ipc/wire.{h,c}`，不含 OS、引擎、UI、socket 或 libssh2 依赖。
它只负责封装；认证、未知消息拒绝、合法状态/方向、request ID、字段 schema
与恢复协议须由握手与后续命令处理器实施，不能因帧合法就执行会话操作。

## 流式解码与生命周期

调用者提供有界缓冲区，解码器不自行分配内存。先完整检查 24 字节帧头，超限或
非法帧在复制 payload 前拒绝。每次 feed 最多消费一帧，返回实际消费前缀长度，
粘连的下一帧继续由调用者持有；FRAME 保持到 next 显式释放。

解码器复制输入，调用者可复用 read 缓冲。frame 的 payload 借用解码器缓冲，
只在 next/重新初始化前有效；一个 I/O owner 串行调用。错误锁存到流结束，不能
在同一流扫描下一个 magic 继续。EOF 在完整帧边界为 END；半个头或 payload 为
ERROR；完整但未取走的帧仍可交付。超时/取消由连接 owner 使用单调 deadline
驱动，解析器不阻塞、不创建线程。写 owner 必须单独保留短写后缀和帧 deadline。

## 共用短写续送

`core/src/ipc/writer.{h,c}` 提供无 I/O、无分配的单帧发送游标，本地 socket 与
SSH channel owner 均可使用。begin 复制帧头、借用不可变 payload；view 只给出
当前未发送的连续区段，advance 仅按底层实际写入字节数推进。WAIT/EAGAIN 不
改变游标，未发送后缀不能被下一帧覆盖；完成帧后才释放 payload 借用并允许下一帧。

DONE 只说明字节交给底层流，不证明对端已接收或 shell 已执行。非法写入计数
锁存失败；取消/超时必须关闭底层连接，不能在半帧后换发新帧。调用方负责在
整个帧头与 payload 期间保持同一个单调 deadline，并持续服务读取/租约/取消；
本模块不执行 socket/libssh2 调用，也不证明真实网络的调度或写入时间已经有界。

五类 C 基线覆盖 1–27 字节写入预算、零进展、pending 替换拒绝、错误计数、
所有未完成边界取消和空帧；八组隔离变异均目标红/同变异对照绿、SHA256 回滚。
五类 ASan/UBSan、遗留标记退出 2、实际编译器 SIGINT/TERM 后代退出与源码复原
通过；核心 gate 138 项/57 符号通过，日志在工程
`tmp/ipc-writer-{mutations,harness,sanitizers,gate}.log`。命令为
`python3 scripts/mutation-check-ipc-writer.py`，当前证据为 macOS arm64。

## 共用连接步进与真实 socket 适配

`ipc/connection` 将 decoder/writer 组合为同一个非 UI I/O owner，通过私有 read /
write / 单调 clock / close 函数表使用底层流，不依赖 OS、引擎或 libssh2。一次
step 最多调用一次 read 和一次 write，各不超过 4096 字节；不等待、不重试循环，
有待发输出时仍先尝试读取。连接级 poll/调度、多个连接间公平性由服务循环承担。

一个调用方预算域的固定分配包含对象、scratch、接收与发送缓冲，各 payload
上限独立且不超过 1 MiB。send 在 ACCEPTED 前复制载荷，只容纳一个待发帧；
BUSY 接受零字节且保留上一帧。接收帧借用至 release/终态/销毁，复制发送与接收
存储独立，释放/复用接收帧不会覆盖未发出的回复。不能持有帧等待响应排空再释放，
否则会停止后续读取；协议处理器须有自己的有界准入/应答策略。

接收 deadline 从首个字节开始，发送从入队开始，帧头、body、零星进展共用同一个
deadline，WAIT 不续期。时间戳零合法，不作“未开始”哨兵；IO 前后检查时钟，
完成回调若已过期也不能当成功。时钟失败/倒退、超时、非法 IO 计数、截断 EOF、
错误与取消锁存终态，关闭流恰好一次、撤销帧借用。空闲接收不启动半包 deadline，
握手/租约/业务空闲计时仍须由协议 owner 实施。

END 只表示连接 EOF，不是 shell 已退出；TIMEOUT/取消同样不能折叠为会话退出。
ACCEPTED 仅表示本地连接队列接受，发送完成也不代表对端确认、worker 入队或应用
执行。断连时未确认命令仍为 UNKNOWN，不自动重发；本模块不停止任何 session。

`ipc/socket-stream` 是当前 Unix stream 实现：adopt 后 NONBLOCK/CLOEXEC，失败
不接管 fd；read 的 poll(0) 在 recv 前优先处理 HUP/错误，丢弃断连后未接受的
排队字节，与 attachment-peer 契约一致。SIGPIPE 通过平台可用的 socket/send
保护抑制；不安装进程全局忽略策略。成功的 connection_open 接管 close，构造
失败仍由调用方关闭 adapter；context 必须活到 connection 销毁。

macOS arm64 十七类 C 基线覆盖分块/复制、收发整帧 deadline、回调迟到/时钟
倒退、背压读取公平性、双缓冲、非法 IO、取消/一次关闭、分配回收、协议拒绝，
以及真实命名端点请求/响应、1 MiB 发送遇 socket 背压时反向接收、部分 EOF、
SIGPIPE、HUP 排队字节拒绝与缓冲清除。`python3 scripts/mutation-check-ipc-connection.py`
的三十组变异均目标断言红/同变异独立对照绿、SHA256 回滚；Darwin 同时具备
SO_NOSIGPIPE/MSG_NOSIGNAL，故信号变异须同时破坏两处才构成目标失效。

连接/授权/握手四十六类 ASan/UBSan、遗留标记拒绝和实际编译器 INT/TERM
清理通过，连接十三份源码 SHA256 复原。核心 gate 198 项/57 公共符号通过，
日志在工程 `tmp/ipc-bootstrap-{sanitizers,gate,harness}.log` 与
`tmp/ipc-connection-bootstrap-mutations.log`。这不是 Linux /
Windows 原生证据，也不是实际 SSH 流、服务循环、身份或完整 session 恢复证据。

## 当前证据与余项

八类 C 基线覆盖固定字节、所有单切分位置/逐字节/双帧粘连、非法头/锁存、独立
限额、所有截断 EOF、非法调用、空帧和含 NUL/非法 UTF-8 的二进制载荷。
`python3 scripts/mutation-check-ipc-wire.py` 在隔离副本做十二组变异，每组目标
断言红、同变异独立对照绿，回滚核对 SHA256；编译失败和信号退出不能冒充断言。
本机 macOS arm64 的八类 ASan/UBSan、遗留标记拒绝和 SIGINT/TERM 中断实际
编译器后的清理/复原已验，核心 gate 133 项/57 符号通过；日志保留在工程
`tmp/ipc-wire-{mutations,harness,sanitizers,gate}.log`，不是 Linux/Windows 运行证据。

HELLO/BIND 与部分管理消息的组件进展见下；完整会话命令/生产宿主调度、网络
租约、异步命令结果、生产 runtime/实际 SSH 流、fuzz 和端到端恢复尚未交付；P0/P2
均未通过。

## 共用 HELLO/BIND 引导事务

`ipc/handshake` 是本地与远端共用的服务端协议状态机，借用一个全新 connection，
由同一非 UI I/O owner 步进；不创建线程、监听器或 session。私有端点须先校验
peer UID，远端入口须先经过 SSH 认证。principal/issuer 是服务端可信身份，
不能从消息中的用户自报字段取得。当前测试通过真实 socketpair 和真正的
attachment controller；尚无生产 composition root 或 SSH channel 入口。

所有字段沿用大端编码。引导 ID 1–6 保留如下，其他消息在握手状态拒绝：

| ID / 消息 | 方向 | payload 字段（偏移:字节数） | request_id |
|---|---|---|---|
| 1 HELLO | 客户端→服务 | role 0:2、reserved 2:2=0、compatibility 4:32、required_features 36:8、expected_instance 44:16 | management 非零；data 为零 |
| 2 HELLO_REPLY | 服务→客户端 | status 0:4、role 4:2、reserved 6:2=0、compatibility 8:32、offered_features 40:8、instance 48:16、challenge 64:16 | 与 HELLO 相同 |
| 3 HELLO_ACK | 客户端→服务 | 原样回送 challenge 0:16 | 零（引导确认例外） |
| 4 BIND | data 客户端→服务 | instance 0:16、session 16:8、generation 24:8、token 32:40 | 零 |
| 5 BIND_REPLY | 服务→data 客户端 | status 0:4、session 4:8、generation 12:8、challenge 20:16 | 零 |
| 6 BIND_ACK | data 客户端→服务 | 原样回送新的 challenge 0:16 | 零 |

长度必须精确相符；session/generation 非零。role=1 management、2 data。
status=0 OK、1 INCOMPATIBLE、2 UNSUPPORTED、3 WRONG_INSTANCE、4 DENIED。
保留位、方向、状态、request_id 和 challenge 由握手校验，不只依赖封装层。
management 经 HELLO/ACK 后交接给后续管理 dispatcher；data 还须 BIND/ACK。

instance 为进程启动时从 OS 随机源取得的非零 128 位值，重启不得复用。data
HELLO 必须指定 instance，management 首次发现可用全零；指定后不匹配明确拒绝。
compatibility 是跨平台的协议/引擎快照兼容契约指纹，**不是平台可执行文件的
SHA256**。组合根提供非零 32 字节指纹和实际 features；required 不被 offered
覆盖即拒绝。当前尚未生成实际引擎指纹或分配生产 feature 位，测试中的指纹与
位集合只是夹具，不能据此宣称跨 OS 快照可用。

每条回复包含新生成的 128 位 challenge。发送完成后等待匹配 ACK；错误回复也
等待 ACK 或整个事务超时才关闭，避免 HUP 优先语义把尚未读取的拒绝消息丢弃。
challenge 不是新的身份认证方法。整个 HELLO/ACK/BIND/ACK/交接共用一个
1–60000 ms 单调 deadline，不因进度重置；首字节尚未来也会超时。时钟倒退/
失败、随机源失败、非法消息和断连均显式失败；已经消耗但尚未移交的 generation
只释放一次。交接前再次验证 generation，旧 owner 不能释放新的 attachment。

READY 必须经 `take` 才转移连接和 generation 责任，take 只能成功一次；成功
BIND **不等于 LIVE，也不允许输入**，恢复 S/L 屏障仍由后续 dispatcher 完成。
拒绝、超时、关闭握手或释放 attachment 均不终止 session/PTY。

## 单次 attachment token 与预约归属

管理 ATTACH(36/37)、数据 DETACH(48) 的实际字段和 runtime 引用归属见
[ATTACH 契约](session-attach.md)。它组合下面的单次 token/握手，不替代
VT 快照、S/L 恢复或 worker 输入执行授权。

`ipc/tokens` 是固定容量 1–256 槽的单 owner 存储，一次分配走宿主 allocator。
token=8 字节大端发行计数器+32 字节 OS 随机秘密；作用域绑定 instance、可信
principal、issuer、session、generation。发行计数器防重复，即使测试随机源故意
重复也不能重放；计数器耗尽显式报错，不回绕。token 不写日志、文件或测试快照。

发行只接管**新预约的 generation**，同一 session/generation 已有待用 token
即 BUSY；失败不接管预约。真实 attachment controller 仍须阻止已占用 generation
被再次预约。TTL 为 1–60000 ms，在恰好到期时失效。服务必须在无流量时也调用
expire；握手期间另会驱动 expire。各连接与 token 存储共用单调时钟域。

正确消费一次后清除存储项，预约责任交给握手，后者在 take 时再交给数据 owner。
错误作用域、未知/过期/重放 token 拒绝；作用域不匹配不会烧掉仍有效的 token。
过期、发行者撤销、指定 session/generation 撤销、存储关闭或时钟倒退均清除
对应项并回调释放预约，不能只删 token 留下永久 BUSY。时钟倒退使整个存储永久
失效。管理 owner 负责在退出时撤销它尚未消费的 token；已经移交的数据连接
由数据 owner 管理，不被 token 存储重复释放。回调不得重入存储。

随机源无弱替代：Linux 单次非阻塞 getrandom，Apple/FreeBSD/OpenBSD getentropy，错误清除
输出并返回；未提供实现的平台显式 UNSUPPORTED。已有 macOS/Linux arm64
组件运行证据，当前不代表其他平台原生验证。连接清除已消费 RX、已发送 TX、读 scratch，终止清除待用 payload，销毁
清除整块分配；token/握手对象在回收前也清除。此保证只涵盖这些拥有的副本，
不涵盖 OS、SSH、引擎或调用者缓冲。

macOS arm64 的 11 类 token、18 类握手基线覆盖真实 socket、OS 随机源、固定
字段、兼容性/能力/实例拒绝、授权单次性/过期/撤销、预约移交、旧代隔离、断连、
超时、时钟与熵故障。34 组 bootstrap 变异均目标断言红/同变异对照绿；隔离源码
夹具另验 UINT64_MAX 耗尽，无生产测试开关。连接增加清除验证后为 17 类/30 组
变异。命令为 `python3 scripts/mutation-check-ipc-bootstrap.py` 和原 connection
脚本。四十六类 ASan/UBSan、遗留标记/实际编译器 INT/TERM 清理及源码 SHA256 复原
通过，完整 gate 198 项/57 公共符号通过；当前仍无本地/SSH 生产服务、真实断网
或跨平台恢复的交付证据。

## 共用服务 owner 与管理消息

`ipc/server` 已将 HELLO/BIND、连接与 manager 组合为固定容量的有界服务轮次；
LIST/INFO/TERMINATE/PING 的字段、责任和证据统一见
[会话服务参考](session-service.md)。本地与远端仍使用同一个处理器。真实私有端点
与 PTY 的 macOS/Linux arm64 组件验收已通过，不能据此宣称生产 sessiond、
恢复数据 dispatcher、SSH 代理或跨平台 GUI 已交付。CREATE/QUERY_CREATE 的
字段、参数/工厂寿命与回复丢失语义见[创建协议](session-create.md)。

## 共用 DATA 恢复事务（2026-10-21）

`ipc/data-wire` 与 `ipc/data-router` 已组合现有 runtime、attachment/task 与
USER 子处理器；本地 socket/远端 SSH 的流入口使用同一状态机。当前为私有 C
组件：真实 socketpair/PTY 的编码器和客户端仍是 C 模型，没有生产 VT、SSH
代理、公开 client ABI 或 GUI 导入；不能据此声称 P0 或完整 daemon 可用。

以下帧 flags/request_id 均为零，整数大端。generation 非零；S/L 可以是已知
的 0。快照格式由非零 format 与 HELLO compatibility 一同约束，bytes 是编码器
产生的完整不可变 bundle；接收方仍须按该格式验证内容再导入，不能当 ANSI
回放。路由不制造引擎格式或替代引擎校验。

| ID / 消息 | 方向 | payload（偏移:字节数） | 长度 |
|---|---|---|---|
| 40 SNAPSHOT_BEGIN | 服务→DATA | generation 0:8、S 8:8、total 16:8、format 24:4、flags 28:4、reserved 32:8=0 | 40 |
| 41 SNAPSHOT_CHUNK | 服务→DATA | generation 0:8、offset 8:8、bytes 16:剩余 | 17–65552 |
| 42 SNAPSHOT_END | 服务→DATA | generation 0:8、S 8:8、total 16:8 | 24 |
| 43 READY_S | DATA→服务 | generation 0:8、S 8:8 | 16 |
| 44 STATE_EVENT | 服务→DATA | generation 0:8、sequence 8:8、kind 16:4、body_length 20:4、body 24:剩余 | 由 kind 固定 |
| 45 LIVE_BEGIN | 服务→DATA | generation 0:8、L 8:8 | 16 |
| 46 READY_L | DATA→服务 | generation 0:8、L 8:8 | 16 |
| 47 SESSION_END | 服务→DATA | generation 0:8、flags 8:4、state_status 12:4、管理 info 行 16:64 | 80 |
| 48 DETACH | DATA→服务 | 无，沿用已有连接释放规则 | 0 |
| 49 PING | DATA→服务 | generation 0:8、非零 nonce 8:8 | 16 |
| 50 PONG | 服务→DATA | 原 generation 0:8、原 nonce 8:8 | 16 |
| 51 RESTORE_ERROR | 服务→DATA | generation 0:8、非 OK GT status 8:4、stage 12:4 | 16 |
| 52 RESTORE_ERROR_ACK | DATA→服务 | 原 generation 0:8、原 status 8:4、原 stage 12:4 | 16 |

这些消息发到管理连接必须关闭，不能作为未知管理命令处理。服务方向帧、
错代、错误长度/保留字、非法阶段/顺序会关闭该 DATA 连接，不能操作其他会话。

BEGIN total 为 1–64 MiB；flags 目前只允许 bit 0 READONLY。CHUNK 为
1–65536 字节，offset+length 不得越过 64 MiB；客户端必须检查连续 offset、
协商的 format/预算、最终 total 和 END 的 S，完整验证后才确认。协议 decoder
只验证单帧，不代替客户端的有界组装与引擎导入。

EVENT kind=1 OUTPUT 的 body 为 1–16384 原始字节；kind=2 RESIZE 的 body
为 cols:2、rows:2、pixel width:4、pixel height:4（12 字节）；kind=4 COLOR
为 dark:4（0/1）。未知 kind、零单元、零事件序号和 body_length 不符拒绝。
绝不把 native `gt_state_size_t` 内存布局发送上网。每个成功权威事件都占同一
session sequence，客户端按 S+1 起严格连续应用；它不是只给 OUTPUT 编号。

正常 SESSION_END 的 flags=0、state_status=OK，必须有已知最终 sequence，
且有序事件已经发送到该序号。flags bit 0 NO_SNAPSHOT 表示无可导入状态，
此时 state_status 必须非 OK；进程 result/has_sequence 独立保留。启动失败的
未知序号、已知序号 0、健康退出但冻结失败不能折叠。NO_SNAPSHOT 可在没有
快照/READY 的情况下发送，仅供诊断/只读显示，不授权输入或假装空终端成功。

ERROR stage 为 1 CAPTURE、2 ENCODE、3 LIMIT、4 DEADLINE、5 REVOKED、
6 PROTOCOL；LIMIT 当前为保留阶段，无专门发出路径。任务失败报告 ENCODE，
无法取得更具体原因的撤销报告 REVOKED；这些描述恢复事务，不证明进程退出。

### 一条事务、一个 writer

BIND 后先在 worker 的事件边界 S 冻结独立对象并开始 attachment 增量队列，
再交 checkpoint executor 编码。任务 context 持有 runtime 引用，直到实际
reap 才释放。已退出会话可以借用 runtime 保留的不可变终态；result 发布后
不会再产生 S+1，因而允许从该 final sequence 开始恢复。CAPTURE 在退出阶段
被取消时，先等终态发布，不重复提交活跃捕获或重新创建 PTY。

BEGIN→有序 CHUNK→END 走 connection 的复制发送缓冲；最后一块复制完成后
释放借用的编码结果，后台 executor 清理并按精确 task ID 回收。旧代任务不能
消费新代已完成的任务。客户端 READY_S 必须等 END 实际排出 writer；服务
固定当时的 L，发送 S+1..L 后才发 LIVE_BEGIN(L)。READY_L 前继续发送 L 后
输出，避免终端输出反过来阻塞恢复。只有 READY_L 后才有 LIVE 输入资格；
BIND、编码成功、dequeued 或发送成功都不等于 GUI 已应用。

USER 结果与恢复流共用单一 writer，I/O owner 每次最多处理一个入帧、发送
一个出帧，空闲时轮换 USER 完成与恢复输出。终端 bytes 不经过 UI 事件队列。
结果/事件顺序由各自契约校验，不把 USER RESULT 的回显序号当作应用 barrier。

PING/PONG 在捕获、编码、恢复和 LIVE 均可用。仅有效入帧更新 server 输入
租约，输出/部分字节/PONG 发送都不续期。只保留一项待入 TX 的 PONG，已复制
的 TX 帧另有独立所有权；客户端应等回应后再发下一次心跳。

### 失败、取消与资源界限

编码器必须使用调用方线程安全 allocator/额度，返回独立 blob；失败时留下
的部分结果也归 release 清理。空 blob、超 snapshot_limit、未知返回码转成
明确失败，不能拿部分数据进入 LIVE。每代最多一个 worker 捕获/encoder job；
旧代捕获/编码在途时关闭连接，context 与引用仍保留至回调结束和任务 reap。

路由固定 1–64 peer 槽、每 peer 1–16 USER 准入记录、单一事件 scratch 和
出帧 scratch；snapshot_limit 不超过 64 MiB，chunk 不超过 65536。非法配置
在调用 allocator 前拒绝。连接需 RX≥65560、TX≥max(16408,chunk_bytes+16)。
源引擎/冻结对象/线程/内核预算仍由宿主另行准入，这不是完整资源账本。

restore deadline 为整个捕获/编码/快照/S/L 事务共用的单调期限（1–600000 ms），
不因分块、PING 或部分进展重置。失败先撤销该 generation，再通过 server 的
`data_revoked` 路径发送一次 ERROR，等待匹配 ACK/DETACH 或错误期限
（1–60000 ms）；原 server idle/frame deadline 仍适用。撤销后不执行入帧中的
任何命令，途中已有请求/心跳最多丢弃一帧且不续期，不能抢掉错误回复。无响应
或断连仍可能导致回复未知；客户端不得自动重发未确认键入。

`data_step/data_drop/data_revoked` 必须配套安装，revoked hook 缺 step/drop
是非法配置。drop 只清理同一完整可信 scope，不结束 session，不回收在途
context；宿主在没有连接时也须调用 router reap。close 在活跃连接/捕获/任务/
USER 回调存在时返回 BUSY，调用方先关闭 server，再继续 reap 至可释放。

当前五类 wire、一类精确任务回收、十四类真实 PTY/router 基线及 47 项隔离
变异通过；数字上限使用独立期望值，分块超限测试不让总长度检查遮蔽目标。
只接受普通 FAIL/exit 1、同变异独立绿对照和 SHA256 回滚，编译/信号/外部
watchdog 不算红。命令：`python3 scripts/mutation-check-data-router.py`。
这些 C 模型的 S/L/终态证据不替代生产 Ghostty 兼容快照、真实 GUI barrier、
SSH 断网或跨 OS/CPU 导入验收；分平台结果见[实施计划](plan-session-daemon.md)。
