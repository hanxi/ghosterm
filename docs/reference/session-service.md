# 共用会话服务 owner 与管理协议

状态：**组件已接通真实私有端点和 manager；生产 sessiond 未交付**。日期：2026-10-20。
入口/部署见[远端会话架构](../architecture/remote-session.md)，帧与 HELLO/BIND
见[会话协议](session-ipc.md)。本地客户端和 SSH 代理使用同一个服务 owner，
没有按入口复制管理或 attachment 逻辑。

## 服务组合与责任

`core/src/ipc/server` 接管已认证流的 connection/handshake，借用一个 manager、
instance、时钟、token 存储和数据处理器。它不监听网络、创建 I/O 线程、直接操作 backend/引擎或连接 UI。
配置 creation ledger 后通过 manager 异步创建 session worker；具体启动仍在 worker。当前真实命名 Unix 端点的测试宿主在 accept 校验 UID 后，
将非阻塞 socket 适配器交给同一个 admit/step；SSH 代理仍未实现。

server 固定 1–64 个槽，allocator 承担服务、连接与握手分配。admit 返回明确
INVALID/FULL/CLOSED/IDS_EXHAUSTED/NO_MEMORY/CLOCK/INTERNAL；有效流即使被
拒绝也关闭一次，输出 ID 清零。非法函数表不被接管。stream context 必须活到
connected(id) 为假或 server_close 返回，不能因 admission 返回就复用其内存。
连接 ID 也是服务端 issuer 身份，单调发行、从不重用，耗尽拒绝。

每次 step 轮转起点，固定轮数服务所有槽；每条连接进行有界读写，握手交接可能
多一次有界步进。阻塞发送不阻止该流读取一帧或其他客户端处理请求。没有 poll
等待、IO 重试循环、worker join 或 UI 同步调用；宿主负责 readiness/定时调度。
只允许一个非 UI owner 调用，回调不得重入。连接表和 token 共用单调时钟域。

管理请求必须有严格递增的非零 request_id；引导握手有独立 ID 命名空间。
一个回复待发送时，下一帧可以进入独立 RX 缓冲，但暂不执行。没有无限命令队列，
也不在回复空间不足时先执行 TERMINATE。返回 ACCEPTED 或发送完回复仍不表示
shell 已退出，丢失回复不能推定操作未执行。

关闭/失败只释放连接、撤销该 issuer 尚未消费的 token，或释放已消费的本代
attachment；不调用 manager terminate/close/reap。管理连接断开不撤销已经转交
给数据 owner 的 generation。旧数据 owner 失效时仍只释放旧代，不碰新代。
token 在没有流量时也由服务 tick 驱动过期。全局时钟失败/倒退锁存失败、关闭
所有 peers、拒绝新 admission，manager/PTY 继续由独立生命周期 owner 管理。

## 管理消息

帧头仍是 GTSD/v1，整数为大端，flags=0。下表都是精确 payload 长度；session ID
非零。不传 C struct 的内存布局、指针或 size_t。

| ID / 消息 | 方向 | payload |
|---|---|---|
| 16 LIST | 客户端→服务 | 空 |
| 17 LIST_REPLY | 服务→客户端 | status:u32、count:u32、count 个 64 字节记录 |
| 18 INFO | 客户端→服务 | session:u64 |
| 19 INFO_REPLY | 服务→客户端 | status:u32、64 字节记录；失败时记录全零 |
| 20 TERMINATE | 客户端→服务 | session:u64 |
| 21 TERMINATE_REPLY | 服务→客户端 | status:u32、原 session:u64 |
| 22 PING | 客户端→服务 | 空 |
| 23 PING_REPLY | 服务→客户端 | 空 |
| 24 CONTROL_ERROR | 服务→客户端 | status:u32 |

回复沿用对应请求 ID。status=0 OK、1 NOT_FOUND、2 BUSY、3 UNSUPPORTED、
4 INTERNAL。NOT_FOUND 只表示这次查询没有记录，不等于成功退出或 shell 状态
已知。TERMINATE 返回 OK 表示幂等结束请求被接受，实际终态还须查询。

最多 64 条记录，LIST 最大 4104 字节，不分页、不静默截断。manager 错误以
status 返回；未知请求明确 CONTROL_ERROR/UNSUPPORTED，连接仍可处理 PING。
已知请求长度/字段非法、收到服务方向消息、重复/倒退 ID 均关闭当前连接，不
执行会话操作。CREATE/QUERY_CREATE、参数和结果保留已实现，见
[创建协议](session-create.md)。ATTACH 与数据 DETACH 的组合见
[预约与数据归属](session-attach.md)；FORGET 的 wire 管理消息仍未实现。

## 64 字节会话记录

| 偏移 | 字节 | 内容 |
|---|---|---|
| 0 | 8 | session ID |
| 8 | 4 | registry phase：1 WORKER、2 REAPING、3 RETAINED |
| 12 | 4 | flags：bit0 termination_requested、bit1 has_result、bit2 has_sequence、bit3 has_exit_code；其他必须零 |
| 16 | 4 | result kind：0 UNKNOWN、1 STOPPED、2 EOF、3 ERROR |
| 20 | 4 | failure stage：0 NONE、1 STATE_OPEN、2 BACKEND_OPEN、3 READ、4 INPUT、5 RESIZE、6 APPLY、7 REPLY、8 CAPTURE |
| 24 | 4 | result status，沿用当前 GT 状态码 0–9 |
| 28 | 4 | termination observation status，沿用当前 GT 状态码 0–9 |
| 32 | 8 | last authority sequence；无 bit2 时为零 |
| 40 | 4 | exit code；无 bit3 时为零 |
| 44 | 4 | termination kind：0 UNKNOWN、1 CODE、2 SIGNAL |
| 48 | 4 | termination code/signal；未观察到时为零 |
| 52 | 12 | reserved=0 |

没有 has_result 时 16–51 全零，has_sequence/has_exit_code 也必须为假。
已观察 CODE 必须与 has_exit_code/code 一致；SIGNAL 不伪造 shell 退出码；
NOT_FOUND/UNSUPPORTED 的 termination 保持 UNKNOWN。解码拒绝矛盾字段、
非法枚举、非零 reserved 或多/少字节，失败清除输出。

registry phase 是回收阶段，不能单凭 WORKER 宣称进程仍在运行：一个已结束但
尚未 join 的 worker 仍在该阶段，并携带 has_result。服务不代替生命周期 owner
做 join；自动回收、保留期与 FORGET 尚未组合，记录仍受固定容量限制。

## 数据处理器与超时

未注入真实数据处理器时 DATA HELLO 明确 UNSUPPORTED；有 token 存储也不假装
提供恢复。注入后服务器仍持有连接，每轮检查 generation，向回调借出 connection
与 scope。回调只做一次有界处理，不能保留/销毁这些对象，必须自行实施恢复
S/L 与输入屏障。IDLE 不续租；只有接受完整有效 peer 消息的 PROGRESS 才续租。
CLOSE/非法结果只关闭该 attachment。当前数据测试注入观测回调，不是已完成的
终端恢复 dispatcher。

握手 deadline 覆盖首字节等待；完整帧 deadline 由 connection 控制。交接后另有
1–60000 ms idle timeout，只有完整有效请求/数据处理器确认的 peer 进展刷新。
poll、部分帧、持续发送输出都不能续租。宿主必须持续 tick，无流量时同样生效。
实际网络上的双向心跳、租约值校准、静默丢包与重连仍未实现/验证。

## 当前证据与边界

`ipc_server_*` 通过真实命名端点、共同 handshake/connection、实际 manager 和
PTY 验证管理操作。正常断连/独立空闲客户端被 SIGKILL 后，原 PTY 可继续输入/
输出；service_close 也不会停止借用的 manager。64 条真实异步启动失败记录的
LIST 验证最大回复；保留 unknown、正常退出或信号各自语义。确定性发送阻塞
验证同流读取和另一客户端的公平性，不冒充真实 TCP 黑洞证据。

变异命令：`python3 scripts/mutation-check-ipc-server.py`。测试和日志均落在工程
`tmp/`。本轮 macOS arm64 与 Ubuntu 24.04 aarch64 各通过完整 gate 222 项/57 公共
符号及 build-core；24 类 service 基线/37 组变异（含发行 ID 耗尽夹具）通过。
本机 24 类 service ASan/UBSan，Linux 70 类 service/握手/token/连接 ASan/UBSan
通过。两平台遗留标记退出 2、实际编译器 INT/TERM 后代退出与 service 五份源码
SHA256 回滚也通过；Linux bootstrap/connection 另分别核对二十四/十三份源码。
Linux 另重跑 bootstrap 34、connection 30、Unix endpoint 20 组变异；均
要求目标断言红、同变异独立对照绿和 SHA256 回滚。GCC 会拒绝原 deadline 变异
注入的歧义排版，现已将注入改成花括号，编译失败不计作有效红证据。

Linux 在现有 Colima VM 中使用当前源码副本；237 个复制文件的 SHA256 与主
工作区逐一对照。副本在工程 tmp 下的原生 tmpfs，临时绑定到短路径 tmp/n；
创建目录使用 umask 077。共享目录的 socket chmod 实测 EINVAL，且长于 107
字节的 Unix socket 路径被显式拒绝，不能把这些失败改成放松生产权限或长度
校验。最初的共享目录运行不算通过。Linux 构建还修复几处既有 GCC
misleading-indentation，并将 SSH e2e 夹具改用 OpenSSH internal-sftp，避免
引用不存在的 macOS 可执行路径；该内置子系统的定义见
[OpenSSH sshd_config(5)](https://man.openbsd.org/sshd_config#Subsystem)。不修改
宿主/客户的 SSH 配置。

证据日志为工程 `tmp/ipc-server-{gate,mutations-final,sanitizers-final,harness-final}.log`、
`tmp/sessiond-linux-{native-validation,components-mutations,sanitizers,harness,source-proof}.log`，
源码归档与清单位于 `tmp/sessiond-linux-source{.tar.gz,-manifest.json}`。VM 验证后
恢复原停机状态。此处只证明上述两种 arm64 的核心组件，不代表 x86_64、Windows
或各平台图形客户端验证。
新增 CREATE 的证据见[创建协议](session-create.md)。当前仍无可安装的 sessiond
可执行产品、生产引擎组合、公开 client ABI、恢复 dispatcher、SSH 代理或 GUI
入口；P0/P2/P3 均未完成。

2026-10-21 的 USER 子处理器另增加可选 `data_drop`：只配 drop、不配 step
是非法构造；DATA 退休在 generation 撤销后、connection 销毁前调用一次，
状态型处理器保留异步 context 至自行 reap/close，server 不代替它 join。
只读观测器可不配 drop。USER wire/邮箱及其组件证据见
[USER 契约](session-user-commands.md)；完整恢复 router 仍未交付。
