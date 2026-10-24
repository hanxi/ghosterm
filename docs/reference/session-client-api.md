# 可恢复会话的公开 C ABI 与 Apple 绑定

`core/include/ghosterm/session.h` 经聚合头 `ghosterm.h` 导出 opaque session，
UI 仍经 Transport/Surface 消费核心。它组合现有 session-client、local stdio
provider 与 SSH provider，没有另一套恢复逻辑。当前按项目要求整体同步更新，
不提供旧后端适配。新增公开 ABI 的提交标记 BREAKING。

## 构建与数据通路

open 深复制 helper/runtime 路径和创建请求，step 才连接/启动代理。创建使用
稳定 nonce，恢复指定原 instance/session；CREATE 是否已到达不明时只查询原
nonce，不再创建。默认固定恢复期限 60s，SSH 请求/凭据/DNS/hostkey 等待共用
认证期限；公开参数保留明确上限，0 选默认。快照默认上限 16MiB，最多 64MiB。

所有 API、原生完成通知和 engine sink 回调使用同一非 UI 串行 IO owner。
snapshot/event 借用仅到回调返回，BUSY 不保留指针，ACCEPTED 代表已消费/自有
复制，实际 applied 单独轮询。快照和终端输出直接给引擎，不进入 UI 队列。
未知事件/不支持的能力明确失败；resize 的像素尺寸与 COLOR 的 dark 不丢失。

输入携带引擎编码时的 generation 和实际 applied sequence。公开接口经新的
submit_at 保留原序号，要求 LIVE cut <= sequence <= actual applied 且代数一致；
原有私有 submit 仍由客户端生成 stamp。旧代、屏障前和未来状态的输入不排队。
队列满接受零字节，QUEUED 仅表示自有复制；未确认操作结果为 UNKNOWN，不重放。
用户操作的 receipt 保留 command ID/序号/拒绝理由，不能冒充 shell 命令执行完成。

info 同时携带恢复状态、原身份、代数、实际应用位置及终态。has_exit_code 为假时
退出数值归零，unknown 不伪装成 exit 0；真实已知 0 仍可表示。state_error 与进程
终态分开，不能吞掉恢复失败。

## 取消、回调与生命周期

回调不得重入。step/input/info/auth 完成和 close 都拒绝重入，避免在 callback
里释放仍在执行的核心对象。cancel 幂等，只 detach，不 TERMINATE daemon/PTY。
本地 close 非阻塞；若代理未回收则返回 false，宿主必须保留句柄与 sink/auth
context 并在同一 IO owner 继续调用，true 后才释放。远端关闭不等 SSH 对端回复。
本地调用 SSH 认证接口明确返回 UNSUPPORTED。

`RecoverableSession.swift` 是薄绑定：检查 owner、不在主线程调用 Core；字符串
arena 保持到 C 深复制结束，临时借用字节不跨回调。异步认证请求 ID 原样传递，
supply 构造并只在成功时交接新鲜、已解锁 SSH 材料，迟到完成释放自有对象。
不复用已有 SSHSession 的句柄或 SFTP 连接。上下文随 C owner 活着；遗漏显式关闭
时，deinit 把取消/回收交给原 owner，不在 UI 线程 wait/join。

## 创建结果、列表与显式终止

options 必须且只能选择 create、session、pending_nonce、catalog_only 一档。
恢复与 pending_nonce 都绑定原 instance；pending 查询只发 QUERY_CREATE，记录过期
或结果未知时显式失败，绝不换 nonce 自动新建。首次 step 返回
`has_admitted_create=true` 时，宿主必须在下一次 step 可能发送 CREATE 前持久化
原 nonce 和已确认 instance。这个时点已用停止底层写入的真实帧夹具验证。

catalog_only 不需要 snapshot_format 或 engine sink，也不开 DATA 流。
`gt_session_catalog` 的 NOT_FOUND 表示尚未加载；OK/count=0 才是已知空列表。
最多 64 条元数据，全有或全无，小容量返回所需 count；断线或 refresh 后立即撤销
旧快照。refresh 是只读重查，同一 management owner 继续处理心跳。

`gt_session_terminate` 与 detach/cancel/close 分开。info.stop 保留目标、请求 ID、
PENDING/KNOWN/UNKNOWN、明确的 control status 与错误。KNOWN/OK 只表示服务端
接受终止请求；进程是否退出、退出码是否已知仍看结果记录。未确认的 TERMINATE
不在重连时重发；本代自己的待终止/未知终止会锁输入，拒绝状态不假装停止成功。
请求 ID 属于当前 management 连接，身份仍由 instance、session 和 owner 共同限定。

Swift 的 catalog 返回 nil/[] 分别表示未加载/已知空列表；Row 与 Stop 只搬 POD，
不创建 Surface。所有管理操作继续遵循同一 owner、非重入与非阻塞 close 规则。

## 验证与范围

```bash
bash scripts/gate.sh
bash scripts/check-abi-apple.sh
python3 scripts/mutation-check-session-client.py
python3 scripts/mutation-check-session-management.py
python3 scripts/mutation-check-session-api.py
python3 scripts/mutation-check-session-binding.py
python3 scripts/mutation-check-session-management-swift.py
python3 scripts/probe-recoverable-transport.py --management
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun swift test --package-path apple/GhostermCore
```

2026-10-21 macOS/Linux arm64 核心 407 项、daemon 461 项、79 公共符号；Apple
绑定 77 项，2 项沿用显式未绑定登记，8 份公开头独立 C11 编译与无环依赖通过。
macOS ASan/UBSan 与 Linux ASan/UBSan/LSan 各 32 项相关测试、OFF 后端 28 项通过。

31 组公开 C ABI、15 组管理协议、19 组既有恢复协调器、10 组原生管理和 6 组
既有 Swift 绑定变异通过，均要求普通断言失败、同变异绿对照及 SHA256 回滚；
标记预检 exit 2、INT/TERM 后代清理已验证。Swift 全包 Debug/Release 各 25 项，4 个需要真实环境
的用例显式跳过；管理用例另用真实 daemon/PTY 单独执行通过，不把 skip 计作证据。

生产 C ABI 的真实本地/sshd/daemon/PTY/VT 流程另验证 resize/color、原 PID/
更高代/旧输入拒绝、终态/只读再恢复、仅持原 nonce 的新句柄查回活会话、无引擎
列表、显式终止活进程、refresh 后观察最终结果。相同 API 也在 Mac→Linux 已安装
46 补丁 payload 上重新通过。Swift 真实管理测试另验：渲染失败后 Source 仍活着，
取消未确认终止得到 UNKNOWN，新句柄查询仍活会话，显式重试后分开观察请求接受
与真实结果。UNKNOWN 不自动重发。

[原生 Transport](session-native-transport.md)、[setup 安装](session-setup-api.md)
已有独立组件证据。AppModel 默认入口、持久化记录/列表 UI、Keychain/凭据交互、
helper/catalog 随包与完整资源/总预算 P0 继续实施。此切片不代表原生产品全部交付。
