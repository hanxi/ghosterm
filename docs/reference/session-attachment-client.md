# 本地与远端共用的 attachment 客户端

状态：**私有 C 客户端与真实 VT 组合已实现；产品客户端未交付**。日期：2026-10-21。
实现为 `core/src/ipc/attachment-client.{h,c}`，消费[共用 DATA/USER 协议](session-ipc.md)。
本地 IPC 与 SSH 只提供同一种 framed connection，不各写恢复状态机。
当前由[共用引导组件](session-client-bootstrap.md)完成 HELLO/BIND、核验
instance/session/generation 并写完 BIND_ACK，再移交连接。共用管理客户端已接
真实 VT 组合；自动重连与生产 SSH 入口仍待接线。

## 所有权与接缝

一个非 UI I/O owner 串行调用 open/step/submit/info/close。open 成功才接管
连接，失败不关闭调用方连接。连接必须已建立、TX 空闲，RX 至少 65552、TX 至少
65560 字节；RX 可以已持有第一条 DATA 帧。它不持有 manager/backend/PTY，
关闭只撤销 attachment transport，不结束远端或本地原会话。

snapshot/event 直接调用 sink，终端字节不进 UI 事件队列。ACCEPTED 表示 sink
已经消费或复制所有借用字节；异步引擎须用自己已准入的存储。BUSY 接受零字节，
不能保留借用；FAILED 可能已改变状态，必须撤销副本。客户端不把 enqueue
当成应用完成，也不会在 handle 释放后接收异步回调。

sink.applied 轮询当前引擎真正连续应用到的 generation/sequence。snapshot 和
event 都带代次；旧代确认不推进状态或续期，未来代次、倒退、超过提交序号均
显式失败。known sequence 0 与 unknown 分开。sink.invalidated 每条连接最多
一次，关闭时撤销本代宿主动作/输入路由；异步引擎任务自己持有所需 context。

## 恢复、LIVE 和终态

1. 严格校验 SNAPSHOT_BEGIN 的代次/格式/总长后才分配一份有界缓冲；连续接收
   chunk，精确核对 offset、末端、S 和 total，拒绝重复 begin、缺块及尾部漂移。
2. 将完整快照交给 sink。sink 应独立验证[容器及引擎资源](session-checkpoint.md)、
   准入预算并隔离副作用；core 不链接引擎、不知道具体快照内容。接受后擦除
   core 自有快照缓冲，但继续等真正 applied(S)。
3. applied(S) 后才发送 READY_S。之后只接受从 S+1 开始连续的事件，LIVE_BEGIN
   的 L 必须等于当前已提交序号。收到 L 仍不解锁输入；等 applied>=L 再发送
   READY_L，期间继续接收 L 之后的事件。
4. READY_L 整帧写出后才进入 LIVE。只读快照进入 READONLY，始终不能提交输入。
   写出并不证明服务端已处理，但同一流保证后续 USER 不会越过 READY_L。
5. 收到正常 SESSION_END 立即锁住输入，精确核对 session 和最终提交序号；
   引擎应用完最后序号后才发布终态。没有可用快照的终态可在 begin 前报告，
   state_error、worker result 与序号可用性分别保留，不伪造空导入或 READY。

局部失败或连接死亡取消 transport、擦除缓冲、撤销副本。服务端 RESTORE_ERROR
先进入 CLOSING，保留具体错误并发送精确 ACK，继续驱动到对端关闭或独立期限。
不能在 ACK 刚写入内核后就立刻关闭：HUP 优先的端点可能丢弃尚未接受的 ACK。
错误收尾期间不执行应用操作，也不续恢复/应用/心跳期限。

## USER 与不确定性

submit 仅在 LIVE 且非只读时接受。客户端覆盖调用方 stamp，使用当前代次、
单调命令 ID 和引擎已确认序号；不能用“已收到/已提交”序号编码确认状态。
本地背压接受零字节且不消耗 ID。已 QUEUED 的输入已经复制到连接，不代表
服务端执行，更不代表 shell 命令完成。命令 ID 和事件序号都有溢出保护。

固定 1–16 个 pending 记录核对结果的 kind、ID、代次与 applied stamp。重复、
未知或不匹配结果关闭 attachment。REJECT 是明确未入队；它不会触发自动重试，
客户端也不自动复用 ID。断线/取消/超时后，每个未确认操作恰好报告一次
OUTCOME_UNKNOWN，包括本地已排队但尚未完全写出的字节；不缓存重放键入。

## 容量与期限

固定对象包含命令表及 65560 字节 TX scratch，另只有一份不超过 64 MiB 的
快照缓冲；长度和配置先验证再分配。RX 属于既有 connection。event 的 BUSY
保留一个 RX 帧并停止接收更多事件，保持内存有界；过慢 sink 会被期限撤销。
所有自有终端字节/记录在复用或释放前擦除，allocator 接到精确原始大小。

restore deadline 从 open 开始且不随流量续期；应用确认、sink 背压、心跳应答、
每条 USER 和错误收尾都有独立单调 deadline。已过期的 callback 返回不能
重新使恢复成功。心跳只能检测活性，不证明 PTY 或程序存活；普通输出不能
代替 PONG，也不能续未确认 USER。配置上限为恢复十分钟、其余期限一分钟，
heartbeat 必须短于 peer timeout；这些是准入边界，产品默认值仍需网络校准。

## 构建与验证

`ghosterm_client_protocol` 是独立静态目标，仅含共用帧/连接/socket 适配、DATA/
USER、终态 codec 与客户端，不继承 libssh2、PTY/runtime 或引擎链接。
`ghosterm_core` 复用同一 object target；没有复制实现或额外公开 ABI。
终态 codec 从管理分派移到 `ipc/control-info.c`，wire 字段不变。

十二类基线包括链接边界检查；四十五项隔离变异要求普通 FAIL/exit 1、同变异独立
绿对照、起跑前 SHA256 回滚。依赖检查的变异故意使 codec 引用 manager，链接
边界必须红、同一二进制 readonly 仍绿。覆盖延迟引擎确认/短写、已知零、背压、
结果三态/重复/错配、独立 deadline、时钟与回调过期、只读/缺快照终态和分配失败。
INT/TERM 杀真实编译器后的后代清理、源码复原与遗留标记退出 2 另验。

macOS arm64 与 Ubuntu Linux arm64 的客户端十二类基线/四十五项变异、核心
334 项及 57 公共符号通过。macOS 另有 26 类 ASan/UBSan/TSan（含原 VT/容器
与新的共用客户端）；借用 VT 动态库未重编 TSan，不代表引擎内部无竞态。Linux 客户端另有十二类 ASan/UBSan 通过。
原服务端 37 项变异和 VT/容器 37 项变异回归通过。真实 VT 场景已移除手写
分块/READY/USER 消费流程，用本组件接原 HELLO/CREATE/ATTACH/BIND 服务，
验证 S<L、原 PTY detach 后查询/输入、最终 canonical/resources 一致及终态锁定。

```bash
export TMPDIR="$PWD/tmp"
bash scripts/gate.sh
ctest --test-dir core/build -R '^attachment_client_' --output-on-failure
python3 scripts/mutation-check-attachment-client.py
```

原始日志/哈希在工程 `tmp/attachment-client-{unit,mutations,harness,gate}.log`、
`tmp/attachment-client-proof.json`、`attachment-client-linux-*`、`attachment-client-{sanitizers,tsan}.log`。
Linux 构建使用独立源码副本和原生临时文件系统，不复用 macOS archive。
这仍不等于生产 GUI、公开 client ABI、跨 CPU/full surface、完整资源事件或 SSH
断网恢复已交付；P0–P8 不因此完成。
