# USER 命令：执行许可与投递结果

状态：**私有 C runtime/worker、USER wire、结果邮箱与共用 DATA 路由已实现；
生产入口未交付**。日期：2026-10-21。本地 IPC 与远端 SSH 使用
相同的 USER 提交路径；它不依赖 UI 或引擎类型。

## 提交与执行

`runtime_submit_user` → `manager_submit_user` → `worker_submit_user` 复制命令
bytes 与 stamp，进入同一固定队列：最多 16 条在途请求、64 KiB 复制 payload。
stamp 为 generation、command_id、applied_sequence 三个 u64，不借用调用方栈。
当前 USER 支持 INPUT/RESIZE/COLOR_SCHEME；CAPTURE 只允许可信宿主操作。

缺少策略明确 UNSUPPORTED；空 stamp、零 generation/command ID、非法 payload
或缺少 completion 拒绝入队。拒绝入队不接管数据、不调用 completion。队列满/
分配失败也不消耗 command ID。同步 ACCEPTED **只表示已经排队**，不是已执行。

当前 POSIX runtime 在排队前核验 RESIZE 的宿主尺寸能力：零列/行返回
INVALID；像素宽/高超过 65535 返回 UNSUPPORTED，零像素表示未指定。
拒绝时不分配 payload、不回调、不消耗 command ID，也不改变 PTY/state 或
终止 worker。CREATE 与直接 POSIX backend 的 open/resize 使用同一检查；
backend open 仍允许零列/行采用 80×24 默认值，CREATE 则要求明确正值。
直接 open 的既有指针接口以 NULL/诊断报告拒绝，resize 返回具体 GT 错误。
这不是对 Windows ConPTY 或任意平台像素范围的统一限制。

worker 从队列取出请求后、任何 write/resize/apply 之前调用 runtime 策略。
creation 的 context 包装也转发策略与 stamp，不能丢到另一个 context 或回落
内部接口。attachment 在自己的锁内核验当前 generation、会话只读位、LIVE
阶段、L≤applied_sequence≤dequeued 和严格递增的 command ID。

对仍在位的 generation，每个处理过的非零递增 ID 都消耗，包括恢复中或序号
不合格而被拒绝的请求；重复/倒序返回 PROTOCOL。新的 generation 重置 ID，
旧 generation 不得消耗/重置新代计数。UINT64_MAX 可作为最后一个 ID，不能
回绕重用。过时代次、未完成恢复、只读或序号不合格返回 AUTH，拒绝只影响
该请求，不触碰 backend/state、不把 session 变成失败。
重复 ID 的拒绝只证明这次提交未执行，不能据此推断原提交未执行。

许可检查在 worker 上与 detach/恢复状态共享锁，是执行的线性化点；不持有
这把锁进行 I/O。撤销 generation 后仍未越过许可点的排队请求会被拒绝；许可
已通过的在途操作及 backend 已接受的尾队列可以继续完成，不能撤回已发送
字节。静默断网在服务得知 EOF/租约到期前也不等于已撤销。这不是“物理断网
后绝不再有一个字节”的承诺。

客户端的应用序号仍须来自真实引擎 barrier/stamp。C 范围检查不能证明 GUI
真的导入/应用，也不能消除 GUI 编码之后并发发生的模式切换；真实 VT/GUI
语义仍受 P0 验收约束。

## 结果三态

USER completion 在 worker 上收到借用的 POD：原 stamp、GT status 和 delivery。
调用方须在回调内复制结果，保留 completion context/独立 runtime 引用到回调
完成，即使原数据连接已经死亡；不能把回调或 terminal bytes 送往 UI 事件队列。

| delivery | 含义 |
|---|---|
| NOT_EXECUTED | 本请求未触碰 backend/state：策略拒绝或执行前取消 |
| OPERATION_OK | backend/state 接受了操作；不表示 shell 程序读到、命令执行或执行成功 |
| OUTCOME_UNKNOWN | 已尝试操作但返回错误，可能已有前缀/状态副作用；不能当作未执行 |

相同 GT_ERR_AUTH 可以是策略拒绝的 NOT_EXECUTED，也可以是 backend 出错的
OUTCOME_UNKNOWN；不能只看错误码分类。backend/state 错误仍使 worker 按原
stage/status 进入真实终态，策略拒绝则保持会话运行。停止前尚未开始的排队
请求完成为 CANCELLED/NOT_EXECUTED。UNKNOWN 或确认丢失时不自动重发，包括
换 generation/command ID 后补发。NOT_EXECUTED 也不积攒恢复前键入供自动补发。

完成/取消前清除复制的 payload，join 后清除 worker 自有内存，再按原 charge
退款；不承诺擦除调用方或 OS/backend 内部副本。

## 可信内部操作

原私有裸提交已直接改名为 `worker_submit_internal` / `manager_submit_internal`，
没有保留旧名字或兼容包装。它们供可信 C 宿主操作/测试驱动使用，绕过 USER
策略；网络输入和平台客户端不得使用，更不能在 USER 拒绝后回落。自动终端
查询应答仍由 worker 的独立 reply staging 处理，不依赖客户端 LIVE 资格。

这些都是 core 私有接口，公开 `ghosterm.h` 和各端绑定未变。生产客户端入口
仍须通过统一 C ABI/Transport，不能从平台 UI include 私有头。

## 共用 wire 与结果邮箱

`ipc/user-wire` 使用 [GTSD/v1 帧头](session-ipc.md)，所有整数大端，flags 与
request_id 均为零；这里的 command ID 独立于管理 request ID。三类请求都有
24 字节 stamp：generation 0:8、command_id 8:8、applied_sequence 16:8。
这些消息仅属于 DATA 连接；发到管理连接也必须关闭，不作为未知管理命令。

| ID / 方向 | stamp 后 payload | 总长度 |
|---|---|---|
| 64 INPUT / 客户端→服务 | 二进制原文，不做 UTF-8 或 NUL 转换 | 25–65560 |
| 65 RESIZE / 客户端→服务 | cols 24:2、rows 26:2、px_width 28:4、px_height 32:4 | 36 |
| 66 COLOR / 客户端→服务 | dark 24:4，0 浅色、1 深色 | 28 |
| 67 REJECT / 服务→客户端 | 原请求 type 24:2、reserved 26:2=0、reason 28:4 | 32 |
| 68 RESULT / 服务→客户端 | 原请求 type 24:2、reserved 26:2=0、GT status 28:4、delivery 32:4、reserved 36:4=0 | 40 |

generation/command ID 非零，applied_sequence 可为零。长度必须精确，未知
方向/type、保留值、零单元和非法 color 拒绝；u32 像素原值交给宿主能力准入。
REJECT reason 固定为 1 BACKPRESSURE、2 STOPPED、3 INVALID、4 NO_MEMORY、
5 UNSUPPORTED、6 UNAVAILABLE。它是未入 worker 队列的证明，不消耗 ID，
不携带执行 status/delivery。C 解码结果必须先看 `rejected`，不能将其清零的
status 当作命令成功。RESULT 只来自真正的 worker completion，没有“排队
成功即 RESULT”的 ACK；status=OK 当且仅当 delivery=OPERATION_OK。
两类回复的 stamp 都回显原请求，并不发布新的权威状态序号或 GUI barrier。

`ipc/user-dispatcher` 由单一非 UI I/O owner 步进，构造时核对 runtime/预约
owner/instance。一次固定分配容纳 1–64 个连接槽，每槽预留 16 个命令记录，
配置可将每槽准入限为 1–16。每次最多收一条请求、发一条结果，发送先复制到
connection 独立缓冲；terminal bytes 只从 RX 复制进 worker，不进邮箱/UI 队列。
worker 回调只在互斥锁内复制 POD，结果按提交顺序发送，不持锁调用 runtime。
组合根须配置 connection RX 至少 65560、TX 至少 40，才能承载完整 schema；
USER 组件本身不拥有或重建 connection 的缓冲/限额。
邮箱满/连接槽满立即撤销该 DATA 连接，不能留着 RX frame 等待回复排空；
worker 队列满则显式 REJECT，不终止 session。输出进展不续 peer 的输入租约。

server 退休时先撤销 generation，再调用 `data_drop`，最后销毁 connection。
状态型 dispatcher 必须安装配对的 step/drop。drop 精确匹配完整可信 scope，
不调用 token store、不停止 worker；未完成回调仍占用槽和 runtime 引用。
回调最后一次邮箱访问完成后，I/O owner 的有界 reap 才能清空旧槽并释放引用。
server close 后也须继续 reap；dispatcher close 在活跃连接/在途回调存在时
返回 BUSY，绝不提前释放。回调不持有 connection，也不会给新代发送旧代结果。

这是 USER 子处理器；现已由[共用 DATA router](session-ipc.md) 组合快照、
READY(S/L)、增量、终态、心跳及错误收尾，在同一 writer 上调度。
单独安装 USER 处理器不会使 BIND 变成 LIVE。
当前真实 socket/PTY 测试的 S/L 消费者仍是 C 模型，不能当作生产 Ghostty
快照恢复、GUI 或 SSH 接线已交付。

## 证据与未交付

六类 worker 基线覆盖复制/拒绝/缺能力、队列与 allocator、取消和 backend
错误；三类 runtime 基线使用真实 PTY、C 状态模型及实际 checkpoint/task/S/L
流程，验证排队后撤销旧代、重放/序号/上限/新代重置和只读尾部未齐时拒绝。
命令拒绝不推进真实 shell；新代完成恢复后才可正常输入。

`pty_dimensions_*` 三类真实 PTY 基线由 exec 子进程读取内核 winsize，核验
默认值、65535 边界、溢出/零单元拒绝后尺寸不变；另有
`session_runtime_user-dimensions` 验证排队前拒绝与同 ID 随后成功 resize/输入。
`python3 scripts/mutation-check-pty-dimensions.py` 提供 13 组独立变异。

变异命令为 `python3 scripts/mutation-check-user-command.py`；同变异独立对照
须绿，普通 FAIL 退出 1 才算目标红，编译/信号/watchdog 不算证据。
另有 `ipc_user_wire_*` 四类 schema/边界测试与 `session_runtime_wire-*` 七类
真实 socket/PTY 用例，覆盖 BIND 非 LIVE、完整握手后输入/resize/color、
排队前拒绝/ID 保留、队列满、断连旧代在途/新代恢复、邮箱容量、故障三态和
生命周期/配置。`python3 scripts/mutation-check-user-data.py` 的 37 项变异
要求同一普通 FAIL/独立绿对照/SHA256 回滚口径。
DATA router 的生产 VT/客户端接线、公开客户端和 SSH/daemon 入口仍未交付；
这不是完整网络恢复或 P0 通过。分平台记录见
[实施计划](plan-session-daemon.md)。
