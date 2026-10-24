# 本地与远端共用的宿主 runtime

状态：**私有 C 组合已实现；生产 daemon/恢复入口未交付**。日期：2026-10-21。
实现为 `core/src/session/runtime.{h,c}`，不依赖 UI、SSH 或引擎头文件。
本地 IPC 和远端 SSH 代理使用同一个 [管理服务](session-service.md)，借用同一
runtime 的 manager/creation；连接入口不决定会话的所有权或回收策略。

## 创建和状态

runtime 持有 manager、creation ledger 和 session ID→对象表。新请求先建立
独立 session 对象、明确的 POSIX PTY 参数和 attachment 控制器，再通过
[启动前绑定](session-create.md) 登记可信 principal/唯一 ID；成功后才启动
worker。控制器分配失败、绑定拒绝或线程启动失败均撤销部分准备与计费，不留
一个可以被发现的假会话。manager 发布仍是 CREATE 的提交点。

引擎适配由组合根注入 `state.open/ops/close/invalidated/release_frozen`，open 在 worker 上
执行，收到不可变 spec、可信 principal/ID 和查询应答 sink。状态初始化成功后
才创建实际 PTY；所有引擎操作、PTY 读写和关闭仍由同一个 worker 串行执行。
成功的输出/resize/配色事件直接进入该 session 的有界 attachment 队列，不进
UI 事件队列。队列满只撤销该 attachment，不能停止权威会话。

默认使用一个线程安全 allocator 域覆盖 runtime/对象表、manager、ledger/job、
session 对象、attachment/队列、worker 和 POSIX PTY 的自有分配。线程栈/guard、
内核资源、引擎和借用的 checkpoint pool 仍需组合根单独准入；这不是完整资源
账本。allocator 不得重入 runtime/manager/attachment。

## 引用、join 与回收

`runtime_acquire` 按可信 principal/ID 取得独立引用，错误 principal 和未知 ID
都返回 NOT_FOUND，尚未发布返回 BUSY；每 session 最多 256 个引用。连接持有
引用；每个 checkpoint task 的 context 也须持有引用，直到任务真正 reap 并
处理完 context 后才 release。detach 和 encoder 完成均不能提前释放这份引用。
release 清空调用方指针，禁止复制引用所有权或在 release 后访问借用的控制器。

worker join/dispose 只清除对 creation spec 的借用并标记 joined，保留 session
对象和 attachment。结果缓存过期不会影响这份所有权。旧代 encoder/结果即使
跨越 worker join，仍可由其原引用安全释放/reap；旧代取消不撤销新代。

生命周期 owner 统一调用 runtime 的 reap/forget/close，不能直接调用借用
manager 的 create/reap/forget/close 或关闭 ledger。forget 只接受 manager 已
保留终态、零引用且无活动 attachment/任务的对象，然后释放控制器/对象和
manager 记录；新 ID 不复用。没有因客户端数量变为零而结束会话的逻辑。

显式宿主关闭先检查引用及控制器是否空闲：BUSY 时 runtime 继续可用，不停止
worker。调用方须先关闭服务/token owner、detach、释放已借结果并 reap 任务；
条件满足后才停止/join worker，依序释放 ledger、控制器与对象表。checkpoint
pool 及工厂/allocator context 活到 runtime 真正关闭以后。管理与生命周期由
同一控制 owner 串行调度；引用和 worker 数据路径允许并发。

## 终态冻结与保留

worker 的可选 `final_capture` sink 在 backend 关闭后、state 关闭及结果发布
之前调用一次。健康 EOF/显式 stop 通过同一 owner 冻结独立不可变对象；错误
或未建立 state 时只通知失败，不导出失效状态。普通冻结失败单独记录，不将
正常退出改成执行失败；成功却返回 NULL 属于适配器契约失效，成为 CAPTURE
错误并 invalidated。stop 保留的是已观察状态，不承诺补齐未读 PTY 尾部。

runtime 总是安装此 sink，creation 包装须转发原 context/status/capture。
session 持有 frozen 对象及独立 capture_status，不依赖已关闭的引擎 state。
`runtime_final` 先等真实 worker result 发布；即使 sink 已执行但 close_state
尚未返回，也只能返回 BUSY，并清空输出。OK 不等于有可恢复快照：调用方须
分别检查 worker result、has_sequence、capture_status 与 frozen。冻结失败
不能被当作成功空快照，已知终态序号 0 也不能折叠成未知。

借用的 frozen/sequence 在持有该 session 引用期间有效，join 不销毁它，查询
不消费它；并发/异步 encoder 各持独立引用。零引用、已 join、attachment/
任务空闲后，forget 才调用工厂 release_frozen，或在显式 runtime close 的
join 后释放。工厂 context/引擎预算域须继续存活，release 不借用旧 state，
不能重入 runtime 或转给 UI。生产适配器仍须为冻结对象单独准入预算。

## 当前证据与边界

九类 C 基线覆盖真实 PTY 的 cwd/env/特殊参数、detach/重连同一对象、结果过期
后继续交互、S/L 有序输出/resize/配色、旧代 encoder 跨 join、引用上限和关闭
拒绝、各分配点失败精确回收、异步状态/backend/apply 错误、并发引用，以及
共用 HELLO/CREATE/QUERY 服务经 socketpair 创建 PTY。管理连接关闭、重新查询
与关闭服务均不重建或停止原 PTY；这里不使用 tmux。

状态适配器测试是独立 C 模型，不能替代真实 Ghostty VT/GUI 恢复验收。当前
另有[候选无 UI VT 工厂](session-vt-state.md) 接入同一 runtime 的真实 PTY，
已验证独立冻结/编码、查询应答和 detach 后继续交互；[候选容器](session-checkpoint.md)
另已接共用 DATA 路由与无 UI VT 副本，生产原生客户端仍未交付。
模块仍没有生产无 UI 引擎组合、终态编码/导入与 DATA router 的生产客户端接线、
公开 client ABI、SSH stdio 代理、可执行启动与安装。
S/L 与引用的单元/组合证据不等于已完成网络断线恢复，P0–P8 保持未完成。

管理 ATTACH 已通过[预约 owner](session-attach.md) 组合 runtime 引用与单次
token，数据连接可接管并释放成员资格；BIND 后仍不开放输入。
USER 提交的执行许可、command ID 与结果三态见[USER 契约](session-user-commands.md)。
内部裸提交已显式改名，客户端不得回落；USER wire/邮箱组件已接真实 socket/
PTY；共用 DATA router 另已组合捕获/编码任务、S/L、增量/终态与心跳，
其生产 VT/客户端接线仍未交付。

可信恢复宿主通过 `runtime_capture` 提交 CAPTURE；成功的独立对象由
`runtime_release_capture` 返还同一工厂并清空 POD。worker/encoder 可释放
活跃捕获的对象，终态保留对象则仍在生命周期 owner 的 forget/close 回收；
工厂 release_frozen 无旧 state 的线程亲和，须支持这些非 UI 线程。
从 `runtime_final` 借用的 const frozen 绝不能交给 release_capture。

终态另有十类 worker 与四类 runtime 基线、20 项隔离变异，覆盖源 state
关闭/覆盖后的独立副本、多个引用/重复查询、join 后保留、发布屏障、失败/NULL
契约及已知/未知序号。runtime 原 invalid 用例另验证必须提供 release_frozen。
状态消费者仍为 C 模型；保存 frozen 指针不是生产 VT 快照或只读接回已经交付。
命令为 `python3 scripts/mutation-check-final-state.py`，同样只接受普通 FAIL
退出 1、同变异独立绿对照和起跑前 SHA256 回滚。

验证命令：`python3 scripts/mutation-check-session-runtime.py`，以及核心 gate/
build-core。25 组隔离变异只接受普通 FAIL 退出 1，同变异独立对照须绿；遗留
标记预检、INT/TERM 后代清理和起跑前 SHA256 回滚同样检查。原始日志及平台
验证记录见[实施计划](plan-session-daemon.md)。
