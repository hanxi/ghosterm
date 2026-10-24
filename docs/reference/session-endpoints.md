# 会话私有端点与启动锁

状态：**Unix 端点基础已实现；daemon/SSH 产品链路未交付**。日期：2026-10-20。
部署见[本地与远端会话架构](../architecture/remote-session.md)，消息格式见
[共用协议封装](session-ipc.md)。实现位于 `core/src/ipc/unix-endpoint.{h,c}`，
私有接口不供平台绑定或 UI 直接调用。

## 同一个入口，两种客户端

本地客户端直接连接用户私有 Unix stream socket。远端 SSH exec 代理也连接该
主机上同一个 socket，转发协议流。sessiond 不需要识别 GUI 在本机还是远端；
本模块不包含 session、PTY、引擎或 UI 调用，关闭连接/端点不会直接结束会话。
Windows named pipe 后端仍待实现，不把本 Unix 实现当作 Windows 支持证据。

## 目录与身份

平台接线层提供短的绝对运行时目录；不在这里读取环境或猜测 HOME，不回退系统
/tmp。目录必须没有点、重复斜杠、末尾斜杠或 symlink 组件。逐级以 openat/fstatat
核验根/当前用户所有权，祖先不得有 group/world 写权限；最终目录必须属于当前
有效 UID，权限恰为 0700。显式 create 时逐级创建缺失组件，模式为 0700，随后重新核验身份和权限；只读打开不创建目录，不自动修复既有目录权限。此行为供本地和远端代理冷启动共用，避免 ~/.local/state/ghosterm 等父目录缺失导致首次启动失败。
路径连同 `/session.sock` 和 NUL 超过实际 sun_path 容量时明确返回 INVALID。

打开后持有目录 fd 与设备/inode 身份；后续启动/连接/accept 再核验路径仍指向
原目录。leaf 的 rename/替换不被视为同一个端点。文件操作经持有的目录 fd 进行。
不承诺隔离同 UID 任意恶意进程，内核 peer UID 校验不能替代完整协议握手/token。

连接与 accept 成功前，通过内核查询有效 peer UID：Apple/BSD 使用 getpeereid，
Linux 使用 SO_PEERCRED，必须等于当前用户。只读目录/socket 的 mode 不足以
认证 peer；查询失败或 UID 不同显式拒绝，不支持的平台返回 UNSUPPORTED。
参考 [getpeereid](https://man.openbsd.org/getpeereid.3) 与
[Linux unix(7)](https://man7.org/linux/man-pages/man7/unix.7.html)。

## 两把锁的不同寿命

| 节点 | 持有方 | 寿命 |
|---|---|---|
| startup.lock | 启动客户端或管理代理 | 连接 → 加锁 → 再连接 → 必要时启动 → 等待有界就绪 → 释放 |
| daemon.lock | daemon | 监听前取得，直到监听关闭才释放 |
| session.sock | daemon | 绑定后 chmod 0600，关闭时只清理自身仍未被替换的节点 |

两把锁使用独立普通文件和非阻塞 flock；锁文件必须当前用户所有、0600、单硬
链接，拒绝 symlink、FIFO、目录、额外硬链接与替换后的 inode。锁文件不删除，
避免删除再创建导致两个进程锁住不同 inode。客户端持有启动锁时，daemon 仍能
取得自己的寿命锁；启动客户端退出也不会释放 daemon 所持的独立锁。
本模块不启动进程，不证明 helper 已脱离 SSH 的登录会话/cgroup/Job 生命周期。

## 监听、连接与清理

listen 先取得 daemon 锁，再检查固定 socket 节点。活跃或连接积压的已有服务
返回 BUSY。只有节点为本用户 0600 socket、连接明确 absent/refused、锁及设备/
inode 仍匹配时才能删除旧节点再绑定。普通文件、外来节点、symlink、未知 I/O
错误不能当成“旧 socket”清掉，也不能因版本握手失败删除一个活着的服务。

目录 0700 保护 bind 到 chmod 0600 之间的节点；实现不改变进程全局 umask。
创建的 socket、打开的目录/锁、accept 和 connect 交付的 fd 都设 CLOEXEC，
交付 socket 为 NONBLOCK；Apple 另设 SO_NOSIGPIPE。这不替代 fork/exec 前的
调用方 fd 隔离规则。

accept 每次只做一次尝试，空队列或 EINTR 返回 WAIT。connect 的 timeout 范围
为 0–1000 ms，poll 使用跨 EINTR 的单调 deadline；失败关闭临时 socket，WAIT
不保留一个隐式进行中的连接。NOT_FOUND 只代表节点不存在或连接被拒绝；权限/
身份错误、IO、WAIT 分开返回。协议 owner 仍需全连接/半包/握手 deadline 与限额。

关闭端点始终回收自身 fd/对象。在 daemon 锁仍匹配时，只 unlink 当前节点仍为
该监听设备/inode 的 socket；替代节点保留。目录与锁文件保留，不递归删除运行
目录。一个尚未取得监听的客户端关闭时不会 unlink 服务端 socket。

## 当前验证与未覆盖项

macOS arm64 下十四类 C 基线使用真实命名 socket、flock、fork/exec：双向流与
fd flags、跨进程启动锁竞争及两锁共存、stale 回收、live 保护、替代节点保留、
目录权限/symlink、错误节点/锁硬链接/FIFO、peer UID 决策、调用方分配/二十次
回收无 fd 增长、目录替换拒绝、非法参数和 exec 后目录/锁/监听 fd 无继承。
UID 负例使用真实 socket 的内核凭据与另一 expected UID 对照；不是两个真实
OS 用户的端到端连接测试。

`python3 scripts/mutation-check-unix-endpoint.py` 对二十组故意破坏进行隔离编译，
每组必须目标断言失败、同一变异独立对照成功并 SHA256 回滚。阻塞锁变异由父
测试进程超时后结束子进程并明确断言失败；外部 watchdog/编译失败不算目标红。

十四类 ASan/UBSan 通过；遗留标记退出 2 与 SIGINT/TERM 中断实际编译器后的
后代退出/源码复原已验。核心 gate 152 项、57 公共符号通过。日志为工程
`tmp/unix-endpoint-mutations-final.log`、`tmp/unix-endpoint-harness.log`、
`tmp/unix-endpoint-sanitizers.log`、`tmp/unix-endpoint-gate-final.log`。CTest 使用
已规范化的工程 tmp 路径，避免夹具自己的 `core/../tmp` 被路径规则拒绝。

[共用连接层](session-ipc.md) 已有真实端点到帧请求/响应证据；这不能宣布生产
listener 已接线：还缺服务级非阻塞循环、连接数量/握手限额、
协议/token/instance、进程启动与签名安装、SSH 代理、租约及 session/runtime 组合。
Linux 原生/多用户/登录清理策略、Windows named pipe 也未验；P0/P2 仍未通过。

2026-10-21 冷启动补充验证：新增 nested 用例检查只读无副作用、逐级私有创建及可监听，27 项端点变异/18 类基线在 macOS 与 Linux arm64 通过，包含父目录缺失、只读误创建与非私有新父目录的故意破坏；核心 414 项及 82 个公共符号对拍通过。
