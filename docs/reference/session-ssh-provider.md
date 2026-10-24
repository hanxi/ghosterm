# 共用 SSH provider

状态：2026-10-21，私有 C provider 已取代网络恢复夹具的阻塞认证 worker。
macOS→Linux 的真实 RST、静默丢包/旧代租约等待、重新认证和原 PID 恢复已通过。
公开 ABI、原生异步 DNS/安全存储/UI 继续接入。

## 原生任务与核心 owner

`core/src/ipc/ssh-session-provider.{h,c}` 将非阻塞认证连接器和 SSH 双通道 owner
接到共用 session-client。平台只负责异步 DNS、解锁凭据和 hostkey 确认；SSH
握手、认证、长连接、取消和断网重连由共享 C 逻辑处理，没有每个平台一套恢复。

每次尝试发一个低频 `requested(request, attempt)`，回调立即返回。原生异步任务
完成后，在同一非 UI IO owner 调用 supply，提交新鲜 SSH 对象和已解析地址。
supply 成功才接管对象；请求已过期/被取消、代数不符、重复完成或参数错误均不
接管，原生任务须释放自己的对象。解析地址与原始 SSH hostkey 身份保持分离。

请求 ID 在一个 provider 生命周期内不复用。reset 取消旧请求，晚到的旧结果
不能混入新尝试。原生异步任务应持有平台 owner 或弱引用并在所属队列交付；
close 后不得再调用已释放的 C 句柄。取消通知不等待 DNS、Keychain 或 UI 任务；
平台应取消/丢弃它们，不能让后台任务借用已经释放的 core context。

已接管的 SSH 由非阻塞连接器驱动。未知 hostkey 只发一次低频 hostkey 通知，
等待平台在回调返回后再调用 decide；既不重复弹窗，也不自动接受。回调内重入
supply/reject/decide 明确拒绝。变更、拒绝和认证失败保持具体错误。

一个认证总期限覆盖原生 DNS/凭据等待、TCP、握手、hostkey UI 和 userauth；
supply 将剩余时间交给连接器，不能重新开始期限。取消通知每个未完成请求至多
一次。网络 IO/期限可重试，认证/hostkey/协议/时钟错误终止；GT_OK 不是拒绝原因。

认证后立即交给原有双通道 owner，握手材料随 SSH 对象交接擦除。代理、daemon、
runtime 路径在 open 时深复制，不能借用 Swift 临时 C 字符串。终端 payload
沿 stream/protocol/engine sink 直连；原生回调只承载低频认证元数据。

## 生命周期

session-client 的 attachment/control/bootstrap 流先关闭，provider reset 最后
关闭 SSH 连接；provider 必须活到这些流销毁以后。provider 关闭从不 TERMINATE
远端 session。请求取消、连接断开与结束远端进程是不同操作。

本轮真实网络测试在 callback 返回后提交已生成的测试密钥与预先解析的环回地址，
全部 TCP/SSH 认证由生产 provider 执行；没有阻塞认证 worker。测试明确接受
可丢弃 sshd 的 hostkey，这不是生产默认策略。原生 DNS/平台 Keychain 接线尚未
完成，已解析地址的测试结果不能冒充这一档产品能力。

## 证据

```bash
bash scripts/gate.sh
ctest --test-dir core/build -R '^ssh_provider_' --output-on-failure
python3 scripts/mutation-check-ssh-provider.py
```

macOS/Linux 核心 385 项、daemon 436 项、57 公共 ABI；11 类 provider 基线、
19 组隔离变异及关闭后端的 UNSUPPORTED 通过。真实 sshd 另验证回调顺序、路径
深复制（调用方立即改坏原缓冲）、hostkey 提示去重和包含 NUL/0xff 的双向字节。
每个变异要求普通 FAIL/exit 1、同变异绿对照和 SHA256 回滚；标记预检 exit 2、
INT/TERM 后代清理与 SHA256 回滚已验证。

macOS ASan/UBSan、Linux ASan/UBSan/LSan 通过。真实 macOS→Linux 故障矩阵与
三组网络夹具变异使用新 provider 重新通过，原 instance/session/PID 不变、
generation 增加，恢复后可继续输入并接收明确终态。可丢弃远端 daemon 已按
SO_PEERCRED/可执行路径/运行目录及 pidfd 清理，不涉及用户已有 daemon。

参见 [自动恢复协调器](session-client.md)、[认证连接器](session-ssh-connector.md)、
[Linux 部署](session-linux-deploy.md)。原生 Surface 的已有视图重导入、公开 session
ABI/绑定与产品入口、完整 P0 仍继续实施。
