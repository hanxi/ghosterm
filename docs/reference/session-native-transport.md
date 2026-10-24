# 原生 Surface Transport

`RecoverableSurfaceTransport` 是 Apple 宿主中的 Transport 接线。它经
`RecoverableSession` 调同一公开 C ABI，local/SSH 只在 options/provider 处分流，
Surface 不知道连接类型。[原生窗格与管理生命周期](session-native-lifecycle.md)已经
接通实际 RootView 的视图交换与键盘焦点。AppModel 默认连接入口仍继续实施，
不能据此宣称整个产品已交付。

## 生命周期与线程

同一 serial IO owner 驱动 session step、快照导入、增量与 applied。snapshot 回调
只解码 POD policy，异步请求主线程创建新空 Surface，返回 BUSY；借用的快照留在
协调器，绝不通过主线程。新 Surface 回到 IO 后一次性导入，实际应用屏障与 LIVE
成立才开放输入，并向主线程发送新 Surface 身份。每代都换新实例，不覆盖旧
Terminal 内存；旧视图在断连后只读，直到新视图可用。

OUTPUT/RESIZE 直接进引擎；配色先在 IO 建立序号屏障，主线程只应用配色元数据，
再由实际 applied 放行。状态、receipt、notice 与失败通知是低频元数据。

输入在编码回调处携带原 generation/sequence。进入 GCD 前先用锁限制为 128 项、
256 KiB，防止把无界队列藏在 DispatchQueue 中；每段最多 32 KiB，整次编码回调先原子准入再拆段，Core backpressure 保留队首。
断连时撤销输入许可并清空待办，不向下一代重发。队列溢出明确报告并脱离会话。
UNKNOWN receipt 单独通知，保留恢复过程；不能当成拒绝或连接失败。显式 detach
仍保留 receipt 元数据，但不额外制造连接失败。非阻塞 close 继续在原 owner
轮询本地代理回收，不等待 UI。

## 原生验证

```bash
python3 scripts/probe-recoverable-transport.py
python3 scripts/mutation-check-recoverable-transport.py
```

探针运行独立的本地 daemon/真实 PTY，经公开协调器输入、脱离、恢复，读原生
Surface 网格验证原 PID/session/instance、增加的 generation、新 Surface、原输入
序号与旧代输入拒绝。探针只停止自己持有的 daemon；业务 detach 不结束 Source。
96 KiB 真实输入另以外部 POSIX cksum 锚点验全量字节；五组隔离变异破坏快照导入、输入启用、原始序号、分段完整性及输出应用，各要求普通 XCTest
断言失败，同一变异下 ssh_config 只读对照仍绿；编译失败、信号或超时不算红。

同一测试也支持下列夹具环境，通过真实 SSH 连接已安装 Linux helper；它们仅为
测试参数，不是产品配置格式：

| 环境变量 | 内容 |
| --- | --- |
| `GHOSTERM_REPLICA_RUNTIME` | 专用、用户私有的远端 runtime 目录 |
| `GHOSTERM_REPLICA_HELPERS` | 已安装的匹配版本 helper 目录 |
| `GHOSTERM_REPLICA_CONTRACT` | 64 位十六进制 strict contract |
| `GHOSTERM_REPLICA_SSH_PORT` | 本地环回 SSH 转发/网络故障 relay 端口 |
| `GHOSTERM_REPLICA_SSH_USER` | 夹具用户 |
| `GHOSTERM_REPLICA_SSH_KEY` | 一次性 sshd 的测试私钥文件，永不输出 |
| `GHOSTERM_REPLICA_SSH_KNOWN` | 夹具 known_hosts |
| `GHOSTERM_REPLICA_FINGERPRINT` | 首次 relay 主机密钥的独立指纹锚点 |

设置后运行 `xcrun swift test --package-path apple/GhostermApp --filter
RecoverableSurfaceTransportTests`。复用 `scripts/probe-session-network.py` 时，把
`{port}` 传给上述 SSH 端口；测试会发 reset、blackhole、online，验证同一个
Transport 重新认证、旧 upstream 保持打开直到真正租约到期、新 Surface 与原 PID。
夹具必须独立；远端 daemon 的清理由夹具所有者按 peer UID/PID、可执行路径、runtime
和 pidfd 核验后执行，不能按进程名杀用户的 daemon。

2026-10-21 macOS arm64→Ubuntu aarch64 已完成以上真实原生流程，网络用例约 18 秒。
该次 prebuilt release 为 `e598d34dcc30-aarch64-f5dd39531901`，strict contract 为
`6eb49f2dc35d3d1f826ab4c6c5586bfd06b1b9b5087ad8c5d5669b8dc26dd995`；经原生
setup ABI 上传/校验/原子安装后使用，远端不编译、不装系统依赖。前一补丁集的
旧 payload 不兼容，不能用于这次证据。x86_64 与远端 Mac/Windows 尚未实测。

## 继续实施

[会话记录与创建屏障](session-records.md)已完成组件实现和真实 daemon 验证。
同 pane 的视图身份更新、从记录恢复与显式 TERMINATE 已完成组件验证；
AppModel 默认连接/分屏、记录发现与创建查询入口、Keychain/凭据 UI 编排和
helper/catalog 随包尚需接通。
任意主题/字体配置仍需纳入有序状态更新；现有 binding 的固定 Source 配色屏障
不代表整个设置入口完成。完整 P0 的全局/GPU/系统内部预算也尚未验收。
