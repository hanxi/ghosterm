# 共用 CREATE：参数、提交点与结果查询

状态：**管理协议已能创建真实 PTY；生产 daemon/客户端未交付**。日期：2026-10-20。
入口与 owner 见[会话服务](session-service.md)，帧/握手见[会话协议](session-ipc.md)。
本地与远端共用同一编码、creation ledger、manager 与 worker 启动路径。

## 提交和查询

客户端生成非零 128 位随机 nonce。nonce 与 instance、服务端可信 principal
一起定位一次创建意图，不是认证凭据；principal 从已核验流取得，不能自报。
server 构造时强制 creation ledger 属于相同 manager/instance。重连必须核验
原 instance，再查询 nonce，不能以别名或新的 instance 悄悄替换原会话。

一次 CREATE 先检查整个参数，再保留有界记录、准备元数据；manager 预留唯一
ID，绑定 runtime 对象后才启动 worker，随后发布注册表。**注册表发布是提交点**。
ACCEPTED 表示有可查询的 session ID，
不表示 shell 就绪、命令已执行或引擎已经可以恢复。PTY/引擎工厂仍在 session
worker 上执行；异步工厂失败通过 INFO 的终态/stage/status 表达，nonce 查询
仍返回原来的创建结果。

同一 principal/nonce 和相同原始 payload 返回原结果，不再调用准备或启动；
不同参数返回 CONFLICT，不改已提交会话。重放不会刷新原记录的保留时限。
不同 principal 查询为 UNKNOWN，不泄露其他作用域的 session ID。

回复丢失时客户端执行 QUERY_CREATE：已提交可取回原 ID，未提交/已过期均可能
UNKNOWN。**UNKNOWN 不证明未创建，禁止自动补发 CREATE（包括沿用同一 nonce）**，
也不自动换 nonce 创建替代 shell。客户端应显示结果未知并允许发现/人工处理。
首版没有无限期 exactly-once、跨实例或跨版本去重承诺。

ledger 有 1–64 个固定槽，保留期 1–86400000 ms。完整 payload/结果在到期时
清除；没有空间时 BUSY，不能覆盖尚未到期的记录。确定的准备/分配/启动拒绝也
保留结果，避免回复丢失后再次执行准备。容量不足、非法请求不启动 worker。
时钟倒退使 ledger 永久失败，已提交的 session 不因这些错误被终止。

## 请求 schema

整数使用大端，CREATE 最大 8192 字节，禁止发送 C struct 内存布局。
字段顺序是固定 48 字节前缀，然后 executable、cwd、term，再依序 argv、env。
每个字符串为 u32 字节长度+UTF-8 字节，不带 NUL；拒绝内嵌 NUL、非法 UTF-8、
截断、额外字节、重复环境键和数量/总长度越界。argv 允许空字符串。

| 偏移 | 字节 | 字段 |
|---|---|---|
| 0 | 16 | client creation nonce，非零 |
| 16 | 4 | command：0 SHELL、1 DIRECT |
| 20 | 2 | columns，非零 |
| 22 | 2 | rows，非零 |
| 24 | 4 | pixel width |
| 28 | 4 | pixel height |
| 32 | 4 | environment policy：1 HOST_BASE；其他显式 UNSUPPORTED |
| 36 | 4 | argc，0–32，不包含 executable/argv[0] |
| 40 | 4 | envc，0–64 |
| 44 | 4 | reserved=0 |
| 48 | 变长 | executable、cwd、term 的三个长度前缀字符串 |
| 后续 | 变长 | argc 个参数，再 envc 个 `KEY=value` 字符串 |

当前宿主路径规则为 POSIX：executable/cwd 必须明确、绝对，term 非空且至多
128 字节。SHELL/DIRECT 分别映射现有 PTY 的 shell/direct 策略，不凭 executable
是否给出猜测。Windows 宿主路径/ConPTY 尚未实现，Windows 客户端连接 POSIX
宿主时仍传宿主路径，不能拿本机路径替代。

HOST_BASE 明确表示宿主基础环境+本次 overrides。KEY 为 ASCII 标识符，区分
大小写；重复键拒绝，TERM 只由 term 字段提供。正式 daemon 组合根仍需构建
稳定的宿主基础环境，并按本次请求处理动态值（例如 SSH_AUTH_SOCK），不能将
首个 GUI 的临时 cwd/env 永久缓存。该 launcher/环境组合尚未交付。

`create-spec` 不做 shell 拼接、不访问文件系统。POSIX PTY 选项转换保留 argv/
cwd/env 与明确尺寸，像素尺寸超过 winsize 可表达范围时 UNSUPPORTED，不静默
截断。语法正确但未知 command/environment policy 可返回明确不支持；畸形
请求关闭当前管理连接，不消耗 nonce 记录，不执行创建。

## 消息和结果

| ID / 消息 | 方向 | payload |
|---|---|---|
| 32 CREATE | 客户端→服务 | 上述 48 字节前缀+字符串，最多 8192 |
| 33 CREATE_REPLY | 服务→客户端 | 固定 32 字节结果 |
| 34 QUERY_CREATE | 客户端→服务 | 非零 nonce，恰好 16 字节 |
| 35 QUERY_CREATE_REPLY | 服务→客户端 | 固定 32 字节结果 |

请求沿用管理连接的严格递增非零 request_id，回复沿用对应 ID。结果布局：
nonce 0:16、status 16:4、detail 20:4、session 24:8。detail 使用 GT 状态码
0–9，零表示没有额外诊断，**不能绕过 status 当作请求成功**。只有 ACCEPTED
可有非零 session，且 detail 必须为零；其他状态的 session 必须为零。

| status | 含义 |
|---|---|
| 0 ACCEPTED | manager 已登记；向 INFO 查询当前启动/终态 |
| 1 UNKNOWN | 没有当前保留记录；不推定没执行 |
| 2 INVALID | 无效调用/请求；畸形 wire 不回此结果而关闭连接 |
| 3 CONFLICT | 相同作用域 nonce 已用于不同参数 |
| 4 BUSY | ledger 或 manager 容量不足 |
| 5 UNSUPPORTED | 未配置创建工厂或参数能力不支持 |
| 6 NO_MEMORY | creation 请求/job 分配失败 |
| 7 PREPARE_FAILED | 准备失败，detail 保留原因 |
| 8 START_FAILED | worker 未能被 manager 接管/启动 |
| 9 CLOCK | ledger 时钟失败 |
| 10 INTERNAL | 内部错误（包括无效工厂配置） |

未配置 creation ledger 时 CREATE/QUERY_CREATE 明确 UNSUPPORTED。配置后要求
RX 至少容纳 8192 字节；回复空间在执行创建之前准入。成功创建不等于已 ATTACH，
更不开放 terminal INPUT；恢复与 S/L 屏障仍是独立事务。

## 工厂与对象寿命

`session/creation` 固定 ledger 走宿主 allocator；每次新的有效启动另分配一个
独立、不可变的 spec/字符串/job。allocator 必须线程安全，context 活到所有
job 回收；该域不自动涵盖 worker、backend、引擎或线程容量，需由组合根另配。
prepare 只在控制 owner 上做有界元数据准备，不能调用引擎/PTY 工厂、等待或
重入 ledger；worker 工厂才做真正启动。这里限制工作数量，不作 OS 线程创建
具有硬实时上限的承诺。

prepare 与必填的 bind_id 都收到同一服务端可信 principal。bind_id 在 manager
锁外、任何 worker 回调之前运行，只绑定 session ID 与对象元数据；此时 ID
尚未对 LIST 发布，INFO/输入/terminate/reap/forget 返回 BUSY，close 也不能
销毁正在创建的 manager。绑定失败保留原原因为 PREPARE_FAILED，不分配 worker；
预留槽位释放、ID 不复用。它是启动前的临时绑定，不能当作创建已经提交。

prepare 输出的 context 在准备/绑定/无效配置/worker 启动失败时立即 dispose，
在 manager 接管成功后仅在 join 后 dispose，一次且仅一次。dispose 必须接受
NULL/部分准备的 context，并清理可能存在的临时绑定。spec 可以被工厂借用
直到 dispose 完成；job 随后清除并精确退款。
open_state/backend 和 output/event/finished 的 context 经薄包装转发，state
ops/close_state/state_invalidated 保留原 state context。关闭/过期 ledger 只
影响结果缓存，不释放 manager 持有的请求、停止 PTY 或丢掉原回调。

## 证据与未交付范围

当前基线包括 codec/最大消息、真实 PTY 的 cwd/env/特殊 argv、回调 owner 与
生存期、nonce 重放/冲突/跨身份、精确过期/擦除、分配与工厂拒绝、异步启动
错误；真实命名端点另覆盖 CREATE 回复被阻塞后断连、新连接 QUERY 取回同一
ID、只准备一次，以及畸形请求不启动。测试直接用 manager 驱动后续 PTY 输入，
不冒充已完成 wire INPUT/终端状态恢复。

变异命令：`python3 scripts/mutation-check-creation.py`。macOS arm64 / Ubuntu
24.04 aarch64 各完整 gate 243 项/57 公共符号与 build-core 通过；十三类创建、
八类 wire、四十九组变异及旧 manager 十九组/service 三十七组回归通过。
相关五十一类 ASan/UBSan 通过；两平台实际编译器 INT/TERM 后代清理和十二份
源码 SHA256 复原通过。四类受私有签名变化影响的原生进程场景另重新编译/通过。
详细日志/源码关联见
[本轮计划记录](plan-session-daemon.md)。当前仍无可安装 daemon、生产
VT 组合、公开 client ABI、SSH 代理或 GUI 恢复入口；P0/P2/P3 未完成。正式
启动前绑定已组合进[共用宿主 runtime](session-host-runtime.md) 的对象表与独立
引用；仍需真实 VT/恢复与生产入口，不能依靠 GUI 或结果缓存代替对象所有权。
