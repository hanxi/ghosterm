# 原生启动链：解析、认证与 Linux 安装

`ghosterm/session_setup.h` 经 `ghosterm.h` 公开非阻塞 authenticator 与有界的
Linux probe/install。它们包装既有共用连接器/安装器，不复制 SSH 或部署逻辑。
当前单版本整体更新，新增公开 ABI 按 BREAKING 提交，不增加后端兼容协商。

## 原生地址与凭据

macOS `SessionAddressResolver` 使用 DNSServiceGetAddrInfo/DispatchQueue，不调用
阻塞 getaddrinfo。IPv4/IPv6 literal 直接转换，保留 IPv6 scope；域名查询优先
指定地址族，NoSuchRecord 时在原期限内换另一族。调用方可在连接尝试间交替
首选族。当前每次选择该次查询的首个地址，尚无全部地址竞速/Happy Eyeballs。
所有完成都异步回到同一非 UI owner；cancel 撤销完成，清理 query 与期限任务。
DNSServiceRef 按 SDK 要求在原 dispatch queue 释放；任务仅在完成/取消/超时前
自持，迟到完成不能借用已释放的 Core 请求。

原生层解锁凭据后，SessionAuthenticator 构造新鲜 SSH 对象，将已解析的二进制
地址与原 hostkey 身份分开交给同一个 C 连接器。step 有界、可随时 close，hostkey
UNKNOWN 等待显式 decide，MISMATCH/REJECT 保留错误；已知 0 与未知不混淆。
原生凭据/DNS/UI 的总期限由启动流程/长连接 provider 包住，独立 authenticator
期限从其 open 起计，调用方应传剩余预算。时钟回调不得重入。

READY 只 take 一次，交给专用 SessionSetupConnection；没有 SFTP 或旧 SSHSession
别名。关闭 authenticator 不关闭已经移交的 SSH。临时连接只用于安装，安装完
关闭；长连接 Transport 再通过共用 provider 重新认证，因此不与安装器并发借用
同一 libssh2 session。秘钥/口令不进入安装包、进度、诊断或快照。

## 安装

probe/install 在独立的非 UI setup owner 执行；不在 UI 或已运行终端的 IO owner
阻塞。单次远端 IO 总期限默认 120s、最大 120s，取消标志可跨队列设置；进度只
承载阶段/文件编号/计数，限频后再交 UI。安装回调不得重入。Swift close 在安装
调用进行中明确返回 false，不能从进度回调释放 SSH。

安装读取调用方信任的固定六文件 catalog，保留既有 arch/glibc/loader、目录权限、
SHA256、EXCL 上传、完整校验后执行、原子发布和可核验复用规则。无需远端
编译器、包管理器或 sudo；不停止已有 daemon/session。publication_unknown 在
失败时也传回，不能把连接中断解释成回滚；probe 失败不发布半填元数据。
完整边界见 [Linux 部署](session-linux-deploy.md)。

## 证据

```bash
bash scripts/gate.sh
bash scripts/check-abi-apple.sh
python3 scripts/mutation-check-session-setup.py
python3 scripts/mutation-check-session-native.py
bash scripts/probe-session-setup-swift.sh
```

macOS/Linux arm64：400 核心、454 daemon、76 公共符号；Apple 对拍 74 项绑定与
既有 2 项未绑定登记，8 份公开头独立 C11 编译。macOS ASan/UBSan、Linux
ASan/UBSan/LSan、关闭 SSH 后端的显式 UNSUPPORTED 通过。

14 组 C 翻译边界变异与 11 组原生解析/认证/安装绑定变异均为普通断言失败、
同变异独立绿对照和 SHA256 回滚；标记预检 exit 2、INT/TERM 后代清理通过。
另重跑原 6 组会话绑定变异。Swift Debug/Release 全包 22 项通过，其中 3 项
需要真 SSH 环境的测试显式跳过；真实 setup 另由 sshd 探针注入临时密钥运行，
不把这些 skip 计成 SSH 通过。6 类解析基线含实际 localhost 异步 DNS、IPv4、
IPv6、scope、无效参数和取消/owner 释放。

真实 macOS→Linux 已通过原生 Swift 路径：异步解析 → 新 authenticator → 明确
接受可丢弃测试 sshd 的指纹 → take → Linux probe → 上传/验证/发布预编译包。
测试安装回调中 close 被拒绝、缺失 catalog 错误不丢失，取消可中止后续探测。
Linux VT 已按 44 个补丁的当前集重新交叉构建，Linux builder 构建 helper 后
打包；客户端与 payload strict hash 一致。安装后的新 helper 再经生产公开
session API 完成 CREATE、resize/color、detach/原 PID/更高代恢复、继续输入、
终态和 readonly 再恢复。远端测试 daemon 已按 SO_PEERCRED、可执行路径、
runtime 参数及 pidfd 清理，只停止本次测试实例。

这仍是原生模块与进程闭环证据，不代表 Surface/UI 产品入口已接通。平台
Keychain/凭据对话框的启动编排、产品包随附 catalog、原生视图交换和完整 P0
继续实施；测试只覆盖 Linux aarch64，未将此结果扩称为 x86_64 或 Mac/Windows
远端支持。

原生宿主的组合流程与真实安装/RootView/恢复/停止证据见
[原生 Linux 首次安装流程](session-native-setup.md)。
