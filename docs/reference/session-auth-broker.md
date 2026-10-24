# 原生会话共享异步认证

`SessionAuthenticationFlow` 是安装、终端长连接与无引擎管理连接共用的认证流程。
在一个非 UI 串行 owner 上完成异步 DNS、TCP、SSH 握手与主机密钥验证，取得服务器
实际提供的方法表后才请求凭据。不能在握手前用空方法表猜测“私钥需要口令”。

公开 C 认证器增加 deferred-credentials 模式和 CREDENTIALS 状态；
`gt_session_authenticator_supply` 只在这一状态接收同 host/user/port/known_hosts 的
新鲜材料。未知主机先等待裁决，错误密钥可在同一连接上继续尝试，最多 33 个候选。
DNS、UI、方法发现与候选重试共用原始 60 秒期限，不能给每次候选重置计时。
方式不匹配返回 UNSUPPORTED，失败供给保留调用方所有权，成功才清空材料句柄。

原生 Flow 在 main 请求平台凭据/主机密钥，完成再异步回原 owner。每个提示有独立取消
令牌和递增 ID；跨恢复请求也不复用。取消、迟到/重复完成、错误 ID 均不能进入新的
连接。UNKNOWN 明确取消为“未确认”，不折成拒绝；主机密钥不匹配明确失败。
方法表与错误状态按原值交给选择层，能力缺失不能被包装成普通网络重试。

认证成功后取得独占 `SessionSetupConnection`：安装器使用它 probe/SFTP/原子发布后
关闭；长连接和管理 broker 经 `gt_session_auth_adopt` 直接把它移交给公共 session，
不会再握手或另建一条 TCP，也不借用已有 SFTP/终端会话。原请求身份、总期限与无重入
限制仍有效；旧请求/过期请求不消费连接。移交后 Core SSH link 负责 exec 与双流 IO。

`SessionAuthenticationBroker` 只把共用 Flow 接到 coordinator 的 requested/cancelled
回调；endpoint 弱持有终端/管理对象，所有调用排在 Core 回调之外。普通 IO 失败交给
coordinator 恢复，不把窗格直接标为永久失败；最终失败仍由其状态报告。凭据取消、
主机密钥或方法错误明确报告。安装始终取独立新连接，不把安装中的连接借给长连接。

## 验证

```bash
bash scripts/gate.sh
python3 scripts/mutation-check-session-deferred-auth.py
python3 scripts/mutation-check-session-auth-broker.py
python3 scripts/mutation-check-session-setup-transport.py  # 需专用 Linux 夹具
```

macOS 与原生 Linux 核心 410 项/82 ABI 符号、Apple 281 项（6 项专用环境跳过）通过。
真实 sshd 的 C 用例验证方法发现前不收材料、错误密钥同连接重试、原总期限、身份/
方法拒绝、CREDENTIALS 取消，以及 adoption 的旧请求/期限/回调重入与所有权。
8 组 C 变异、9 组原生认证变异和 7 组 Linux 安装变异都有普通断言红与同变异绿对照；
marker exit 2、INT/TERM 后代清理及 SHA256 回滚单独验证。安装中断另实际等到 Linux
Source 活跃，核验 peer UID/PID/executable/runtime 后经 pidfd 停止专用 daemon。

原生认证测试使用真实握手，但其中 fake endpoint 只观察动作交付，不声称执行了
adoption；实际移交由 C 用例与 Mac→Linux 原生安装/恢复/停止用例证明。新认证器
接线后，aarch64 闭环约 5 秒；96 KiB 输入、TCP reset、双向黑洞、旧 upstream 保留到
租约到期、同 PID/session/instance、新 Surface/更高代及旧输入拒绝重跑约 18 秒。
只清理专用夹具和已核验的 pidfd，不按进程名结束服务。

本次为 C ABI BREAKING，Apple 绑定同步；远端 GTSD/GTVC wire 没有改变，仍使用严格
contract `c679f90f78c31ab35a3b3443214eaea11cbeab9f0f6b45a3627cfb50e945fd33`。
相同 contract 表示同一后端契约，不包含旧协议适配或版本协商。协议升级策略仍按
“当前快速实现、旧契约明确不支持；后续再讨论”执行。

平台 Keychain/凭据选择层已另行接入并验证，默认 AppModel 入口仍在接线；本项不代表完整产品或 P0 完成。

上述哈希是认证切片的历史证据；后续 DATA 进程元数据更新后的当前严格契约见 [进程元数据](session-process-metadata.md)。
