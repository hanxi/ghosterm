# 原生 Linux 首次安装流程

`SessionSetupTransport` 在专用后台 owner 使用 [共享认证 Flow](session-auth-broker.md)，
先握手/验证主机/取得方法表，再请求凭据并取得独占安装连接。安装完成后返回
immutable helper 路径；长连接另走新鲜 Flow 并直接移交已认证连接，不借用安装/SFTP 句柄。

认证等待共用 60 秒期限。凭据按请求作用域接收一次；主机密钥裁决异步回 owner，
重复与迟到完成被撤销，UNKNOWN 取消连接，不作为拒绝。认证失败可按候选顺序
请求新凭据，最多 33 次；成功序号作为元数据交回凭据选择层，不保存密钥材料。

Linux probe 后按实际架构选择应用内预编译包，并先核对 GTB1 arch/strict contract。
上传、远端 sha256、loader/glibc 和原子发布继续由公开 C 安装 API 实施。整个 probe
与安装共用另一个 120 秒预算；期间取消使用线程安全令牌，不等待 UI。取消与失败
保留 installation 的 publicationUnknown，不能宣称远端回滚。远端不编译，不安装
系统依赖，不要求 sudo，不替换已运行的 daemon 或会话。

默认部署到 `$HOME/.local/share/ghosterm/sessiond`，runtime 位于
`$HOME/.local/state/ghosterm/<contract-prefix>`；专用夹具可显式覆盖两者。
远端 Mac/Windows 和未附带的 Linux 架构明确不支持，不静默换连接路线。

## 真实闭环

`SessionSetupTransportTests.testRealLinuxInstallNativePaneResumeAndExplicitStop`
已在 macOS arm64→Ubuntu aarch64 执行：

1. 新 known_hosts 的真实裁决与重复完成隔离；应用/包 contract 不符在安装前拒绝。
2. 被改坏的 SHA256 表明确安装失败，不能返回安装成功。
3. probe、预编译包上传/校验/发布、独立 SSH 长连接与实际 RootView 窗格。
4. 活会话期间再次安装同 release，原 PID 与输入保持有效。
5. 从私有记录重开同一窗格，新 Surface 实际挂载、原 PID 不变、旧输入拒绝。
6. 独立管理连接显式终止，实际观察 hasResult，不把 ACK 当作退出。

包含失败路径的用例约 4 秒；七组隔离变异与同变异绿色对照通过。
取消前启动单测、Apple 全包 274 项（14 项专用环境跳过）通过；真实用例单独运行，
不把 skip 算作证据。INT/TERM 验证在变异中实际创建 Linux Source 后中断，Mac
probe/Swift 后代全部退出，Linux peer UID/PID/executable/runtime 经 pidfd 核验停止，
专用安装和 runtime 清理，源码 SHA256 回滚。

```bash
# 仅用于已配置的一次性 Linux SSH 夹具；环境还需提供私钥路径、known_hosts、
# SSH 端口/用户、独立主机密钥指纹、bundle 和 strict contract。
# install/runtime-scope 必须为同目录下两个 ghosterm-test-* 私有夹具目录。
# runtime 可位于 scope 内最多四层；不指定 scope 时沿用 runtime 本身。
python3 scripts/probe-native-linux-setup.py
python3 scripts/probe-native-linux-setup.py --default-entry
python3 scripts/mutation-check-session-setup-transport.py
```

探针 teardown 的 OpenSSH/Python 是测试清理工具，应用的远端安装不依赖它们。
macOS XCTest 可另起进程组，探针按 kqueue 追踪后代退出并清理各自进程组，
不能只结束 swift 父进程就宣称资源已回收。

这证明安装、原生窗格、记录与管理组件的完整接线；随包 catalog 与本地 helper 分发已接线，见 [运行时打包](session-runtime-package.md)。
[有序主题/字体更新](session-ordered-style.md) 已接线；默认 AppModel 入口和 Keychain 凭据选择已接线，默认入口证据见 [产品协调器](session-product-coordinator.md)。


## Linux x86_64 的证据档位

同一原生安装/窗格用例另在 Ubuntu 24.04 x86_64 的 QEMU 用户态容器通过，约 11 秒，
包括重复认证完成、损坏包拒绝、复用安装、同 PID 恢复与显式停止。它证明跨架构预编译包
及原生客户端组合，尚无原生 x86_64 硬件证据，不记为全平台验收完成。

容器内使用 Linux 原生文件系统运行的 440 个 Debug 后端用例中 438 通过、2 失败：
`local_provider_missing` 与 `session_public_api` 要求 `posix_spawn` 对不存在的可执行文件
同步返回 ENOENT；QEMU 用户态把 CLONE_VM|CLONE_VFORK 转成 fork，父进程先于 exec
失败返回，变成异步 IO 失败。未放宽断言。SSH 关闭的后端构建未运行三个 SSH 客户端用例。
最初从 macOS 共享目录执行的 Unix socket chmod 失败不能作为后端失败基线，正式结果
使用 Linux tmpfs；它也不能替代原生 x86_64 完整 gate。

此档清理不冒充 pidfd 支持：QEMU 缺少对应能力时，必须提供本次自建、有固定标签的
一次性容器 ID，核验后停止整个容器，再通过原生 ARM Linux 控制连接删除专用夹具目录。
应用部署不使用 Docker 或 Python。
