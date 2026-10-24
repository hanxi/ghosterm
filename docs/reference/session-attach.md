# 共用 ATTACH：预约、授权和数据连接归属

状态：**私有 C 组件已组合；生产恢复入口未交付**。日期：2026-10-21。
本地 IPC 与远端 SSH 入口共用此协议、[服务 owner](session-service.md) 和
[宿主 runtime](session-host-runtime.md)。实现为 `ipc/attachment-owner` 与
control/server 的管理分派，不依赖 UI 或真实引擎。

## 所有权与时序

同一服务有一个 attachment owner，借用 runtime，拥有固定 1–256 个预约槽和
同容量 token store。每个待用/已消费的预约持有独立 runtime 引用；连接关闭
不会停止 manager/worker。owner 只能被一个活着的服务 claim，禁止两个从 1
开始分配 issuer ID 的服务共用它。构造失败必须归还 claim，关闭服务先退休
连接再 unclaim；有 claim 时关闭 owner 返回 BUSY。

服务的预约授权配置填写 attachments、manager/creation、instance；后三者须来自相同
runtime。token store 与 binding 回调由服务从 owner 派生，禁止再填第二套
handshake token/binding 字段。owner 或 data dispatcher 任一缺失时，ATTACH
明确 UNSUPPORTED；没有数据处理器时 DATA HELLO 也不假装支持。

1. 可信管理连接发送 session ID。服务从认证后的流取得 principal 和当前连接
   issuer，二者不能从 payload 自报。错误 principal/未知 session 均 NOT_FOUND。
2. 先处理 token 到期，再取得 runtime 引用并预约唯一 generation。同会话已有
   活动代、引用/预约容量不足返回 BUSY；失败不抢占旧代或停止会话。
3. 用 OS CSPRNG 发行单次 token，绑定 instance/principal/issuer/session/generation。
   熵/能力/时钟错误均显式返回，撤销本次 generation、释放引用；不留永久 BUSY。
4. 数据连接完成 HELLO/BIND/ACK 后接管这份预约。成功 BIND 只取得成员资格，
   仍为 CAPTURING，绝不跳过快照/增量的 S/L 屏障或开放输入。
5. 管理连接死亡只撤销它尚未消费的 token。已消费的预约由数据握手/数据连接
   owner 释放，不因原管理连接退出或原 token TTL 到期而重复释放。
6. 数据断连、握手失败/超时、数据租约过期或 DETACH 释放本代。回调逐项核对
   全部 scope 字段，重复或迟到的旧代回调不能撤销新代。独立 checkpoint task
   仍须持有自己的 runtime 引用，不能借用这份会随连接消失的引用。

ATTACH 回复丢失不能推定没有预约，也不能盲目 BIND 猜测 generation。客户端
可关闭原管理连接，使尚未消费的预约撤销，再重新连接/查询/预约；或等待有界
TTL。未确认输入不重发，ATTACH 不会自动 CREATE 新 shell。服务 tick 在没有
请求时也驱动到期；引用和活动代不会无限积累。

## 消息与字段

| 消息 | 方向 | request_id / payload |
|---|---|---|
| 36 ATTACH | 管理客户端→服务 | 严格递增非零 ID；恰好 8 字节非零 session:u64 |
| 37 ATTACH_REPLY | 服务→管理客户端 | 对应请求 ID；恰好 88 字节 |
| 48 DETACH | 已 BIND 数据客户端→服务 | ID=0、flags=0、空 payload；关闭本数据连接，无额外回复 |

整数大端，不映射 C struct。管理连接上的 DETACH、服务方向消息、长度/零 ID
等畸形请求按协议错误关闭连接。数据连接上的畸形 DETACH 同样关闭连接；它
没有路径去终止 session 或定位另一代。底层发送接受仅表示排队，不表示对端
已经处理或 shell 已执行。

| ATTACH_REPLY 偏移 | 字节 | 字段 |
|---|---|---|
| 0 | 4 | status |
| 4 | 4 | detail：GT 状态码 0–9 |
| 8 | 16 | daemon instance |
| 24 | 8 | 原请求 session，非零 |
| 32 | 8 | attachment generation |
| 40 | 4 | token TTL，毫秒 |
| 44 | 4 | reserved=0 |
| 48 | 40 | token：u64 发行计数器 + 32 字节秘密 |

status：0 OK、1 NOT_FOUND、2 BUSY、3 UNSUPPORTED、4 ENTROPY、5 CLOCK、6 INTERNAL。
只有 OK 才能携带 capability：detail=0，instance/generation/计数器/秘密非零，
TTL 为 1–60000。失败只回原 session 和错误，instance/generation/TTL/token
全零。未知 status/detail、非零保留字段、截断/额外字节或失败时泄漏 capability
均拒绝，解码输出清零。detail=0 不绕过 status 变成成功。

token 不写日志、文件或测试快照。控制处理器的临时结果、连接的 RX/TX，以及
owner/token store 的拥有内存在使用完或关闭时清除；不承诺擦除调用方、OS 或
SSH 内部副本。客户端保存的结果也要在消费/取消后清除。

## 当前证据与限制

真实 socketpair/PTY 覆盖管理→预约→数据 BIND→DETACH、回复未读就断连、
管理 issuer 消费前后断开、过期边界和空闲 tick、数据租约、跨身份/重放、
旧代迟到、容量/熵/时钟/构造失败、独占 claim 和固定 wire schema。服务关闭
后原 PTY 仍继续交互；状态工厂是独立 C 模型，data callback 只是观测器。

USER 的 worker 执行许可与结果三态已有[独立 runtime 组件](session-user-commands.md)，
尚未接入此数据流。此模块未交付真实 VT 快照发送、完整 data dispatcher、
公开客户端、SSH stdio 代理、生产 daemon 启动或安装。协议/owner 组合的成功
不等于真实网络恢复或 P0 完成。验证命令为
`python3 scripts/mutation-check-session-attach.py`，分平台证据见[实施计划](plan-session-daemon.md)。
