# 原生会话记录与创建屏障

Apple 的 `SessionRecordStore` 只保存恢复所需的身份元数据：本地/SSH 类型、
helper 路径、runtime、strict contract、SSH 目标与 Keychain 查找引用、创建 nonce、
daemon instance、session ID 和已观察到的结束/停止结果。终端字节、attachment token、
启动命令、环境变量、密码、解锁密钥不进入记录。

记录不是后端兼容层。当前只接受 JSON envelope format 1；未知格式、损坏、权限不安全
都明确失败并保留原文件，不把错误解释成空清单，不自动覆盖或迁移。

## 写入时序

1. 在后台存储 owner 保存创建意图，生成一次 nonce。
2. 协调器报告 `hasAdmittedCreate` 时，CREATE 已进入待发队列，但尚未发送。
3. `RecoverableSurfaceTransport` 暂停所有 session step，通过 admission 回调保存
   原 instance 和 nonce；存储采用私有文件、临时文件 fsync、原子 rename、目录 fsync。
4. 成功完成回调后才恢复 step。保存失败或超过五秒，在 IO owner 立即取消并回收代理；
   重复/迟到回调不能复活请求。保存期间和失败后都不得发送 CREATE。
5. 后续只在身份、状态、结果等低频元数据变化时更新记录；sequence 变化不触发落盘。

恢复已知 session 必须使用原 instance；创建结果不确定时只用原 instance/nonce 查询，
不重新 CREATE。尚未获得 admission 的意图也不能在重启后自行重放启动命令。

停止前先保存 `stopUnknown`，然后才提交 TERMINATE。KNOWN/OK 只证明接收了请求；
只有实际结果或 catalog 的 `hasResult` 证明结束。未知结果不能折叠为 exit 0，网络
中断不能解释为 Source 退出，未确认的停止不能在重启后自动重发。

## 存储约束

后台 serial owner；每个事务重读最新清单，非阻塞 flock 防止跨进程丢更新。
上层元数据 owner 有界重试短暂锁竞争；停止的不确定状态和已结束状态禁止旧写入者倒退。
目录逐级 openat/O_NOFOLLOW，最终目录 0700、文件 0600、同用户、单硬链接；
拒绝可被其他用户写入的父目录及符号链接。发布前重新核验路径的设备号/inode。
最多 1024 条、1 MiB；保留身份与已知结果的单调性，拒绝重复 Source/创建 nonce。

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  xcrun swift test --package-path apple/GhostermApp --filter SessionRecordStoreTests
python3 scripts/probe-recoverable-transport.py --admission
python3 scripts/mutation-check-session-records.py
python3 scripts/mutation-check-recoverable-transport.py --admission
```

真实 daemon 探针以无 Surface 的 catalog 观察：持久化回调挂起时 Source 清单不变；
原子记录提交后才出现实际 PTY；写失败、五秒到期和迟到成功回调不能新建 Source。四组创建屏障变异与十二组
记录存储变异只接受
普通 XCTest 断言红及同变异下独立绿色对照，不把编译错误、崩溃、超时算作证据。

目前这是 Transport/记录组件的验证；默认 AppModel 入口、重启发现界面和显式关闭
编排仍须接通，不能据此宣称 GUI 恢复产品流程已完成。
