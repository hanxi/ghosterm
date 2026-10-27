# 能力矩阵

> 口径（铁律 #8）：**✅** 已实现且有闸门/实测证据 · **🟡** 已实现但只在某一档证据下成立
> （必须写明哪档）· **⏳** 契约/占位在，无实现 · **❌** 未开始 · **⛔ WONTFIX** 不做并注明原因。
>
> 核实日期：**2026-10-10**（M2/M3/M4 收口：对话框 / Keychain / 编辑器 / SFTP）。

## 核心（C）

| 能力 | 状态 | 证据 |
|---|---|---|
| 稳定 C ABI（`ghosterm.h` v0） | 🟡 | 36 符号双向对拍（`scripts/check-abi.sh`）；**接口未冻结，破坏性变更允许** |
| `ssh_config` 解析（AST + Host 块） | ✅ | `ctest sshconfig_golden`：4 个 Host 块 / 6 个模式 |
| `ssh_config` 无损往返（**未编辑**） | ✅ | 与原文**逐字节相同**（`test_config` 头号断言） |
| `ssh_config` 求值（typed） | ✅ | `gt_config_resolve`：user/hostname/port/forwardagent/compression/serveraliveinterval/identityfile/proxyjump/setenv/localforward |
| `ssh_config` 求值（通用视图 = `ssh -G`） | ✅ | `gt_config_effective`：**42 键 × 6 host = 252 项**全表语义对拍（布尔归一化）；支持集双向校验 |
| 默认 IdentityFile 候选链 | ✅ | `gt_identity_policy_t` 五档（9_6=7 / 10_3=5 / MASTER=6 / NONE / CUSTOM），每条链注明出处；回落机制有断言 |
| `ssh_config` **编辑** API（改值后无损写回） | ✅ | `gt_config_edit`：`sshconfig_writeback` 9 用例逐字节对拍（`fixtures/writeback.json`）+ `=` 分隔符保留补充断言；B1~B6 变异有牙 |
| `Include` 展开 / glob | ✅ | `process_include`：相对路径基准 = **~/.ssh**（OpenSSH_10.3p1 实测 + 真 ssh `-G` 同口径对拍）、`~`/`~user`、glob 无匹配静默 / 具名缺失显式失败 / 嵌套 + 深度上限 16；被含内容不进写回（铁律 #4）；`test_config` 6 组断言 + 变异 I1~I4 有牙 |
| 事件队列（低频、批量、非阻塞） | ✅ | `src/core.c` 环形缓冲 + 锁；`test_api`；SFTP job（PROGRESS ~16ms 合帧 / JOB_DONE 三态）经此上报 |
| Transport 接缝（本地 PTY） | ✅ | `gt_transport_open_pty`（POSIX forkpty）；`pty_transport` 行为断言（真 forkpty，离线）：EOF 前不丢最后一批 / 退出码 / 字节往返 / 超时不折叠 EOF / winsize / resize / TERM / close(NULL) 安全 / 前台作业四态 / 内核 cwd / **写绝不无限等**（第 13 节：pty 输入队列满时写要有界返回且积压字节随后补送——旧实现的 `poll(-1)` 会钉死会话线程 → 关标签页时 `queue.sync` 把主线程也钉死＝整个 app 无响应）；变异 `mutation-check-transport` P1~P4 有牙。**Swift 端** `StopBoundedTests` 直接断言 `SessionController.stop()` 经 `queue.sync` 屏障有界返回（走生产链路 `sendText → onInput → 会话线程 write`，子进程 `stty -icanon -echo` 才会真触发 EAGAIN 而非 canonical 丢弃），对 P4 验过牙（变异体挂起超时 / 干净 0.7s 绿） |
| Transport 接缝（SSH shell） | ✅ | `gt_transport_open_ssh_shell`（libssh2 pty-req+shell）；`ssh_e2e` 真 sshd 回显/resize |
| Transport：前台作业判定（关闭确认用） | ✅ | `gt_transport_child_state` **四态**（UNKNOWN/EXITED/IDLE/JOB）：POSIX 读内核 `tcgetpgrp(master) == shell 自己的进程组` 判「空闲提示符 vs 有作业」，SSH / ConPTY 报 `UNKNOWN`（**不猜**，铁律 #6）。`test_transport` 第 11 节（IDLE/JOB/EXITED/NULL→UNKNOWN）+ 变异 `mutation-check-transport` P1/P2 有牙。**前置修复**：本地 PTY 子进程曾拿不到控制终端（父进程抢跑 `setpgid` → 子进程 `setsid` 报 EPERM），已修 |
| Transport：会话 cwd | 🟡 | **本地 PTY ✅**：`gt_transport_child_cwd` 内核查询（macOS `proc_pidinfo` / Linux `/proc/<pid>/cwd`），`test_transport` 第 12 节（继承 cwd + `cd` 后随之变）。**远端 cwd 未交付**：OSC 7 在我们钉的 embedded host-IO 下到不了宿主（无 `GHOSTTY_ACTION_PWD`），且 libghostty 要求 host `isLocal`——接线留为 seam，依赖上游支持 / roadmap R2 |
| SSH（libssh2） | ✅ | connect / pubkey·password 认证 / 主机密钥三态 / TOFU；`ssh_e2e` 14 项断言（真 sshd，环回离线）。**写绝不无限等**（§5b：远端不读 stdin 时 `gt__ssh_shell_write` 在 `GT_SSH_WRITE_GRACE_MS` 内有界返回、积压字节走 `wtail` 队列随后补送——与本地 PTY §13 同结构。`TIMEOUT`/`EAGAIN` 不再当硬错丢字节（旧实现把两者都算 `GT_ERR_IO` → 键入的字节就这么没了）。写路径自己设/还 session timeout，不打扰同 session 的 SFTP。e2e 限制：sshd 排空速率 ≈ 40 MiB/s，3 MiB payload 写完时队列**根本没形成**——flush 机制无法被真 sshd e2e 触发，由代码审阅 + PTY §13/P4 的变异覆盖保证同构正确性） |
| SFTP | ✅ | `gt_sftp_*`（libssh2_sftp 封装）：list / stat / lstat / readlink / realpath / mkdir / rmdir / remove / remove_all（递归）/ rename / chmod / walk（递归枚举，visitor 可取消）/ upload / download（job + 事件三态）/ cancel / pause；**每会话一把 I/O 锁 + 单条 I/O worker 线程**（修掉「每 job 一线程共用同一 libssh2 会话」竞态）。e2e 真 sshd 75 项断言 + 变异 `mutation-check-sftp` 10 项 / `mutation-check-exec` 4 项 |
| SSH exec 通道 | ✅ | `gt_ssh_exec*`：远端单命令（stdout/stderr 分离 + 退出码）；把递归搜索/大树遍历放到远端本地算（SFTP readdir 是每条目一次往返）。服务器禁 exec → `GT_ERR_UNSUPPORTED`（不静默降级）。e2e 含**性能对比**（300 文件：exec-find vs sftp-walk，两条路都找全；**环回上 exec 反而慢 ~14×**——起远端进程的开销，优势只在真实 RTT 链路显现）。变异 4 项 |
| 端口转发 -L/-R/-D | ❌ | — |
| known_hosts / TOFU | 🟡 | 明文主机名匹配 + TOFU 落盘 ✅；**hashed 条目 / @revoked 未支持**（命中不了→UNKNOWN，不误判） |
| 本地 PTY | 🟡 | POSIX（macOS/Linux）forkpty ✅；Windows ConPTY ⏳（能力缺失显式报错） |
| 原生异步解析与 Linux 安装启动链 | 🟡 | **macOS→Linux aarch64 原生模块/进程证据**：DNSServiceGetAddrInfo、公开非阻塞认证、独占临时 SSH 安装新 payload、已安装 helper 的公开 session 原 PID/代数/交互/终态与 readonly 再恢复通过；400 核心/454 daemon/76 ABI、14 C/11 原生变异、sanitizer/OFF/Swift Debug-Release 通过。产品 Keychain/UI 编排、catalog 随包、Surface 交换未接，x86_64 未实测，完整 P0 未过，见 [原生 setup](session-setup-api.md)。 |
| 公开可恢复 session ABI / Apple 薄绑定 | 🟡 | **macOS/Linux arm64 C ABI + Apple 绑定证据**：407 核心/461 daemon/79 符号、8 独立头；31 C ABI/15 管理协议/10 原生管理/6 既有绑定变异，sanitizer/OFF 另验。原 PID/新代、resize/color、只读再恢复、原 nonce 查询、无引擎列表与显式终止经本地/sshd及 Mac→Linux 已安装 helper 通过；持久化/UI 与随包未接，完整 P0 未过，见 [公开 ABI](session-client-api.md)。 |
| 本地 stdio provider | 🟡 | **macOS/Linux arm64 原生代理与真实 PTY/VT 证据**：共用 coordinator，8 基线/11 变异，描述符隔离、stderr 有界排空、只回收自有代理、重试不累积子进程。原生产品入口待接，见 [本地 provider](session-local-provider.md)。 |
| 原生可恢复 Surface Transport | 🟡 | **macOS arm64 本地与 Mac→Linux aarch64 真原生组件证据**：同一公开 C 协调器接 full Surface，真实键入、原 stamp、原 PID/更高代/新 Surface、SSH reset/静默丢包/旧 upstream 租约等待通过；UNKNOWN 独立提示，输入有界且断连不重放。AppModel 默认入口、恢复 UI 与随包尚未交付，见 [原生 Transport](session-native-transport.md)。 |
| 原生空 Surface 与 IO 导入 | 🟡 | **macOS arm64 full libghostty/Metal 接缝证据**：11 原生基线/13 变异、红绿像素及既有 26 Surface 回归；Swift/C 绑定 5 项真 Surface/7 变异另验真实键入、账本回收与配色屏障。SDK 隔离构建至 build/；新 attachment 新 Surface、applied/LIVE 后输入解锁；非产品视图交换或完整 P0，见 [导入接缝](session-surface-import.md)。 |
| 共用 SSH provider | 🟡 | 私有 C；11 类基线、19 组变异、Mac/Linux sanitizer 与真实 sshd；真实 Mac→Linux 网络矩阵已改用非阻塞认证并重新通过。公开 ABI 另见上项；原生异步 DNS/Keychain/UI 尚未交付，见 [provider](session-ssh-provider.md) |
| 可取消的 SSH 认证连接器 | 🟡 | 私有 C；非阻塞 TCP/握手/主机密钥等待/userauth、总期限、未知/拒绝隔离、socket 与材料清理；18 组变异、Mac/Linux 真实 sshd 与 sanitizer 通过。共用 provider 已接入；原生异步 DNS/公开 ABI 接线继续实施，见 [连接器](session-ssh-connector.md) |
| 本地/SSH 共用自动恢复协调器 | 🟡 | 私有 C 候选；17 类基线、19 组协调器变异、3 组真实网络夹具变异；真实本地 PTY/VT 与 macOS→Linux TCP reset、静默丢包/旧代租约等待、重新认证/原 PID 恢复通过。非阻塞认证 provider 已接；公开 ABI 另见上项；原生 UI/异步 DNS 未交付，见 [协调器](session-client.md) |
| Linux sessiond 用户态部署 | 🟡 | macOS→Ubuntu aarch64 真 SSH 与原生安装/同 PID 恢复/显式停止闭环通过；x86_64 同流程在 QEMU 用户态通过，原生硬件未验，后端 440 项中 2 项受 QEMU spawn 语义限制失败。应用预编译双架构包已打包；默认 AppModel 入口仍待接线。见 [原生安装](session-native-setup.md)、[打包](session-runtime-package.md) |
| 原生安装/终端/管理共用异步认证 | 🟡 | 主机验证/方法发现先于凭据解锁，错误凭据在原 TCP 上重试且不重置总期限；已认证连接按原请求直接 adoption。Mac/Linux core 410 项/82 符号、Apple 281 项及 8+9+7 组变异通过；新认证路径已重跑真 SSH 断流/黑洞/旧 upstream 租约和同 PID 恢复。平台凭据选择器已通过 5 项测试、6 项变异及中断回滚审计；默认 AppModel 入口仍待接线。见 [共享认证](session-auth-broker.md)、[平台凭据](session-platform-credentials.md) |
| 产品会话生命周期协调器 | 🟡 | **macOS 本地真实 Source/原生模块证据**：目录/PID、更换所有者后持久化恢复、新 Surface/旧输入隔离、显式关闭实际终止、所有者提前释放与遗忘通过；6 项隔离变异及中断回滚审计通过。默认 AppModel/恢复清单 UI 尚未接入，远端底层证据不等同于产品默认流程。见 [产品协调器](session-product-coordinator.md)。 |
| Source 目录与前台作业观测 | 🟡 | **macOS/Linux aarch64 核心及原生模块证据**：413 项/82 ABI；源线程节流、变化合并、独立 DATA 元数据、未知/空闲隔离、UTF-8 与无截断边界，11 C/4 原生变异及中断回滚通过；macOS 真 shell 中文目录/作业/恢复、Linux 双架构安装与远端目录通过，x86_64 为 QEMU 档。默认本地/SSH 创建及分屏已接线。见 [进程元数据](session-process-metadata.md)。 |
| 原生恢复会话打包与配置更新 | 🟡 | macOS 签名 helper/catalog 与 Linux 双架构预编译包已组合；背景/前景/光标 RGB、256 palette 和字体配置经 Source STYLE 与引擎实际应用屏障更新。初次发布/输入等待配置，LIVE 配置期间按键有界保留，断网不重放。Core 409 项/80 符号、无 SSH 后端 443 项（显式排除 3 项 SSH 客户端 e2e）、Apple 280 项及 5+4 组变异通过；默认 AppModel 已接线，P0 未通过。见 [有序配置](session-ordered-style.md) |
| GUI 外会话保活与准确重连（daemon） | 🟡 | **默认 macOS 本地 / →Linux aarch64 产品路径证据**：实际 GUI SIGKILL 后重启，RootView 重新挂载，以带新随机标记的命令响应确认原 instance/session/PID/目录；随后显式停止并观察结束。本地六会话与应用所有者替换恢复通过。历史 P0 原型包含快照/资源/渲染局部验证，但完整系统/GPU/第三方资源预算仍未通过，不能标为全平台完成。见 [默认协调器](session-product-coordinator.md)、[打包与 GUI 探针](session-runtime-package.md)。 |
| 本地/远端共用帧封装 | 🟡 | **仅独立 C 封装证据**（2026-10-20）：`ipc/wire` 不依赖 socket/libssh2/UI；八类拆包/粘包/长度/EOF/二进制基线，十二组目标红/对照绿变异。见[协议说明](session-ipc.md)；已有单帧 writer/connection/引导握手组合；会话命令 dispatcher 与真实本地/SSH 服务入口尚未接线，不代表生产协议或恢复可用。 |
| 本地/远端共用短写续送 | 🟡 | **仅 C 单帧游标证据**（2026-10-20）：`ipc/writer` 保留实际未发送后缀，pending 禁止覆盖，完成/取消释放借用，错误计数锁存；五类基线/八组变异/ASan/UBSan 通过。已接共同 connection/socket 整帧 deadline；服务级/libssh2 调度与租约尚未实现，不能当作真实网络写入有界证据。 |
| 本地/远端共用 Unix 私有端点 | 🟡 | **macOS/Linux arm64 原生组件证据**（2026-10-20）：[端点契约](session-endpoints.md)，逐级目录/两把非阻塞锁、真实命名 socket/peer UID、stale/live/替代节点、fd flags/exec/分配回收；十四类基线/二十组变异/ASan/UBSan/INT-TERM 清理通过。已在共用服务测试宿主组合握手/manager；生产可执行服务与 SSH 代理未交付，不是 Windows 或完整 daemon 支持。 |
| 本地/远端共用有界帧连接 | 🟡 | **macOS/Linux arm64 原生组件组合证据**（2026-10-20）：[连接契约](session-ipc.md) 统一固定分配/双缓冲/整帧 deadline/取消一次关闭；有真实命名端点帧往返、socket 背压时继续接收、EOF/HUP/SIGPIPE 证据。十七类基线/三十组变异通过，增加缓冲清除；握手/token 另见下项。已接共用 service owner 的轮次与部分管理消息；生产宿主、网络租约、会话创建/恢复或实际 SSH 通道仍未交付。 |
| 本地/远端共用 HELLO/BIND 与单次授权 | 🟡 | **macOS/Linux arm64 原生组件组合证据**（2026-10-20）：[引导协议](session-ipc.md) 固定字段、兼容/能力/instance、challenge ACK、单次 token/过期/撤销和预约责任移交；真实 socketpair/attachment、OS 随机源及断连/旧代/时钟/熵故障。二十九类基线/三十四组变异及 ASan/UBSan/INT-TERM 回滚通过；没有生产指纹生成、feature 注册、恢复 dispatcher、可执行服务或 SSH 代理；CREATE 组件见下项，不代表生产身份链或跨平台恢复已交付。 |
| 本地/远端共用服务 owner 与管理协议 | 🟡 | **macOS/Linux arm64 组件证据**（2026-10-20）：[服务契约](session-service.md) 固定容量/有界轮次、真实私有端点→握手→manager 的 LIST/INFO/TERMINATE/PING；管理断连/独立客户端 SIGKILL/关闭 service 不停 PTY，issuer/代次/过期独立清理。24 类基线/37 组变异、sanitizer/INT-TERM 回滚；两平台完整核心 gate 222 项/57 符号通过。CREATE/QUERY_CREATE 另见下项；没有生产可执行产品、恢复 dispatcher、独立 reaper/保留期、SSH 代理或 GUI 恢复入口。 |
| 本地/远端共用 CREATE 与结果查询 | 🟡 | **macOS/Linux arm64 组件证据**（2026-10-20）：[创建契约](session-create.md) 严格参数/工厂所有权、nonce 重放/冲突/保留期/UNKNOWN，真实命名端点 CREATE→manager→PTY，回复丢失后新连接查询原 ID；13 类创建与 8 类 wire 基线、49 组变异。启动前绑定可信 principal/ID，关闭/过期结果表不停止 session，异步启动失败不假装同步成功。仍无生产 launcher/环境规范化、VT/恢复、公开 client ABI 或 SSH 入口，不代表完整 daemon 可用。 |
| 本地/远端共用宿主 runtime | 🟡 | **macOS/Linux arm64 私有 C 组件证据**（2026-10-21）：[runtime 所有权](session-host-runtime.md) 已组合 manager/creation、真实 PTY、session/attachment 对象与独立引用；九类基线/25 组变异、14 个分配点失败回收、相关 38 类 ASan/UBSan 通过。管理协议经 socketpair 创建/重连查询，关闭服务后原 PTY 继续运行。状态工厂测试使用独立 C 模型；DATA 路由与终态保留另见下项，生产 VT/SSH 代理、公开客户端和可执行产品仍未交付。 |
| 本地/远端共用 ATTACH 与数据连接归属 | 🟡 | **macOS/Linux arm64 私有 C 组件证据**（2026-10-21）：[ATTACH 契约](session-attach.md) 组合真实 runtime/PTY、预约引用、单次 token、HELLO/BIND/DETACH 与超时/断连清理；12 类基线/41 组变异，相关 82 类 ASan/UBSan。未读回复、消费前后 issuer 死亡、过期/旧代/身份/容量/熵/时钟均已验，服务关闭后原 PTY 继续交互，BIND 后仍封锁输入。该组基线 data callback 为观测器，DATA 路由另见下项；非生产 VT 恢复/输入授权或真实 SSH 断网证据。 |
| USER 输入执行许可与结果三态 | 🟡 | **macOS/Linux arm64 私有 C 组件证据**（2026-10-21）：[USER 契约](session-user-commands.md) 在 worker 执行前核验代次/LIVE/序号/递增命令 ID，拒绝不停止会话；区分 NOT_EXECUTED、OPERATION_OK、OUTCOME_UNKNOWN，不把相同错误码混成未执行。六类 worker、三类真实 PTY/runtime 基线及 34 组变异、相关 115 类 ASan/UBSan 通过；旧代排队后撤销、ID 重放/新代重置/只读尾部均有证据。状态消费者为 C 模型；USER wire/DATA 路由另见下项，公开客户端与生产 VT/SSH 接线仍未交付。 |
| 本地/远端共用 USER wire 与异步结果邮箱 | 🟡 | **macOS/Linux arm64 私有 C 组件证据**（2026-10-21）：[USER 协议](session-user-commands.md) 的 INPUT/RESIZE/COLOR 与 REJECT/RESULT、有界邮箱、断连引用和 server 退休回调已接真实 socketpair/PTY。4 类 schema 与 7 类 runtime 基线、37 项普通 FAIL 变异、两端相关 107 类 ASan/UBSan、macOS 7 类 TSan 通过；BIND 非 LIVE、旧代排队后撤销/新代原 shell 输入、两档背压与未知结果均有证据。S/L 消费者仍是 C 模型；DATA 路由另见下项，无生产 VT、SSH 代理、公开客户端或可执行 daemon。 |
| 终态 frozen 对象保存与引用保留 | 🟡 | **macOS/Linux arm64 私有 C 组件证据**（2026-10-21）：[runtime 终态契约](session-host-runtime.md#终态冻结与保留) 在 worker 退出关闭 state 前冻结独立对象，result 发布后才可查询；join/重复查询不消费，持有引用不得 forget，最后一次释放后由工厂回收。十类 worker/四类真实 PTY runtime、20 项隔离变异、两端相关 130 类 ASan/UBSan 及 macOS 15 类 TSan 通过；失败/已知序号 0/发布屏障均已验。状态仍是 C 模型，无生产 VT 终态编码/导入或 wire/GUI 只读接回。 |
| 本地/远端共用 DATA 恢复路由 | 🟡 | **macOS/Linux arm64 私有 C 组件证据**（2026-10-21）：[DATA 契约](session-ipc.md) 已组合 worker 捕获/任务编码、分块快照、READY(S/L)、有序 output/resize/color、USER、终态、心跳与撤销后有限错误收尾。5 类 schema/1 类精确任务回收/14 类真实 socketpair/PTY 路由、47 项普通 FAIL 变异、两端相关 162 类 ASan/UBSan 及 macOS 15 类 TSan 通过；S<L<final、WAIT_L 持续输出、旧代在途/终态发布竞态及未知/已知零/冻结失败已验。编码器/客户端仍为 C 模型，无生产 VT/GUI、SSH 代理、公开 client ABI 或可执行 daemon；P0 未通过。 |
| 共用 runtime 的无 UI VT 状态工厂 | 🟡 | **macOS arm64 候选组件证据**（2026-10-21）：[VT 状态适配](session-vt-state.md) 直接使用共用 runtime，真实 PTY 在无 GUI 时收到查询应答，detach 后重新取得原 session 继续输入；独立 frozen 在源关闭后编码，保留 canonical/resources/配色与历史元数据，内联 RGBA 像素实测。七类基线、十二项普通 FAIL/同变异绿对照及 ASan/UBSan 通过。外部资源装载显式禁用；候选容器/真实 DATA 组合另见下项，动画/通用配置、Linux 引擎组合、SSH/GUI 和完整资源账本仍待完成，不代表 P0 或网络恢复已交付。 |
| 共用容器与真实 VT DATA 恢复 | 🟡 | **macOS arm64 真实 VT/PTY 组合 + Linux arm64 容器原生证据**（2026-10-21）：[快照容器](session-checkpoint.md) 将 canonical/resources/continuation 策略、配色/历史与图像配额一起接入现有 DATA encoder/任务所有权；真实 HELLO/CREATE/ATTACH/BIND、分块/S/L、USER、detach 后原 shell 继续应答、重连增量及 readonly 终态通过，最终 canonical/resources 与权威状态一致。十四类基线/37 项普通 FAIL 变异与 ASan/UBSan/TSan；Linux 容器五类基线/十六项变异及固定字节向量一致，工厂/适配/夹具仅严格编译。未接生产原生 surface、Linux 引擎运行或 SSH，不代表跨架构/网络恢复、完整资源事件或 P0 已通过。 |
| 本地/远端共用 attachment 客户端 | 🟡 | **macOS/Linux arm64 私有 C + macOS 真实 VT 组合证据**（2026-10-21）：[客户端契约](session-attachment-client.md) 已实现经绑定连接的分块恢复、引擎 applied(S/L)、READY_L 完整写出后输入解锁、USER stamp/确认/UNKNOWN 与独立期限。真实 VT 接收端已移除手写恢复流程，原 PTY detach 后继续应答/交互，最终 canonical/resources 一致。十二类客户端基线/四十五项变异、两端核心 gate 334 项/57 符号；macOS 26 类 ASan/UBSan/TSan 和服务端/VT 变异回归通过。独立 client 目标不链接 host runtime/PTY/引擎；共用引导/管理客户端另见下项；生产 GUI、SSH 入口、公开 client ABI 与完整资源事件仍待完成。 |
| 本地/远端共用客户端引导与管理 | 🟡 | **macOS/Linux arm64 私有 C + macOS 真实 VT 组合证据**（2026-10-21）：[引导/管理契约](session-client-bootstrap.md) 已实现 HELLO/BIND、ACK 完整写出屏障、instance 核验及 LIST/INFO/CREATE/QUERY_CREATE/ATTACH/TERMINATE/PING；未确认 CREATE 保留 nonce 并报告 UNKNOWN，不自动重建。十一类基线/35 项变异、两端核心 gate 345 项/57 符号通过；macOS 37 类 ASan/UBSan/TSan、Linux 23 类 ASan/UBSan 通过，真实 VT 组合已移除手写引导/管理。独立 client 目标不链接服务端/runtime/PTY/引擎；生产 SSH owner/代理、可执行 daemon、公开 client ABI 与 GUI 入口仍未交付。 |
| 共用 POSIX 宿主可执行入口 | 🟡 | **macOS/Linux arm64 候选进程与真实 VT 原生组合证据**（2026-10-21）：[宿主进程契约](session-host-process.md) 已提供前台 `ghosterm-sessiond serve`，组合私有端点、server/runtime、DATA 和独立 reaper；客户端 SIGKILL 后接回同一 PID，结束隔离/终态保留/容量复用已验。8 类基线/16 项变异、端点 17 类/24 项变异、两端 core gate 348 项/57 符号及相关 47 类 ASan/UBSan、macOS TSan 通过。按需后台启动/stdio 代理见下项；当前无真实 SSH 接线/断网、原生 GUI、公开 client ABI 或完整资源事件；P0 未通过。 |
| 共用按需后台启动与 stdio 代理 | 🟡 | **macOS/Linux arm64 候选进程证据**（2026-10-21）：[启动/代理契约](session-activation.md) 共用私有端点、锁内重查、独立 session/FD/信号隔离、READY/ACK/COMMIT；ACK 后取消或代理死亡不杀原会话。27 类启动/代理测试与 38 项普通 FAIL 变异；纯客户端代理不链接 runtime/PTY/引擎。仍无真实 SSH owner/exec/断网矩阵、公开 ABI、GUI 或安装器；P0 未通过。 |
| 专用 SSH 长连接与 exec 字节流 | 🟡 | **macOS/Linux arm64 本机真 sshd 候选证据**（2026-10-21）：[SSH owner](session-ssh-link.md) 单 owner、双无 PTY channel、有界队列/公平/短写、独立 stderr 排空与取消，七项普通 FAIL 变异；同平台真实 daemon/PTY/VT 经 SSH 创建，客户端 SIGKILL 后新建 TCP/SSH、严格核验 instance 并接回原 PID。两端 core gate 349 项/57 符号，Linux 全量 398 项；相关 ASan/UBSan 通过。尚非 macOS→Linux 跨 OS 部署、自动安装、真实 reset/静默丢包、自动重连协调器或原生 UI；P0 未通过。 |
| POSIX PTY 尺寸能力准入 | 🟡 | **macOS/Linux arm64 私有 C/backend 证据**（2026-10-21）：CREATE/open/resize/共用 runtime 采用同一宿主检查，像素超 65535 显式拒绝，resize 零单元 INVALID，USER 排队前拒绝不消耗 ID、不改变状态或停止会话。三类真实 PTY 内核尺寸与一类 runtime 基线、13 项尺寸变异及相关 119 类 ASan/UBSan 通过。仅限定 POSIX winsize；不代表 Windows 或完整 wire/GUI resize 已交付。 |
| 远端 sessiond，经现有 SSH 端口的 exec 协议流 | 🟡 | 公开 C ABI 与原生 Transport 已通过 macOS→Linux aarch64 真 SSH、断流/黑洞/旧 upstream 租约回收、同 PID 与全新 Surface 恢复；仅复用 SSH 端口。首次安装、持久化记录、管理停止与签名应用 payload 已有独立证据，默认应用入口已通过 Linux aarch64 原生与 x86_64 QEMU 安装/恢复/停止；aarch64 实际 GUI SIGKILL 后重启接回原 PID。见 [原生接线](session-native-lifecycle.md)、[安装](session-native-setup.md) |
| 断网后接回远端原 shell / PTY | 🟡 | **macOS→Linux aarch64 真实 SSH 原生 Transport 证据**：TCP reset、静默黑洞、旧 upstream 租约等待后重新认证，原 session/PID 与新 Surface 恢复，旧代输入撤销，UNKNOWN 输入不自动重发。默认应用入口已通过同一网络矩阵，实际 GUI SIGKILL 重启后以新命令响应核验原 Source/PID；x86_64 同一网络矩阵也已在 QEMU 档通过；原生 x86_64 硬件与完整 P0 尚未验收。见 [原生生命周期](session-native-lifecycle.md)。 |
| 跨平台远端会话客户端与会话宿主 | ⏳ | **仅目标矩阵，未交付**：[客户端/宿主分开验收](../architecture/remote-session.md)。先 macOS → Linux，另需第二种原生客户端和跨 OS/CPU 快照证据；Windows renderer/客户端适配与 ConPTY 宿主分别待验，移动端无本机后台保活承诺，Web 网关另行设计。 |
| 配色集随包 + 主题选择 | ✅ | `build-ghostty.sh` 从 `iterm2_themes` 依赖派生 607 套配色 → `Contents/Resources/ghostty/themes`；CGhostty 推导 `GHOSTTY_RESOURCES_DIR`（上游入口，未设置时兜底）；出包脚本的**包内**配置检查验随包主题被引擎接受（变异 A6 有牙） |
| 终端配置下发（GUI → 引擎） | ✅ | `gt_ghostty_app_new_with_config` / `_app_reload_config` / `_config_get_{float,double,bool}`：设置页写 Ghostty 语法的配置文件，libghostty 自己解析；**判有效看诊断条数**（未知键只记诊断不丢配置）；`font-size` 改完网格密度真的变（`TerminalConfigTests` 11 项，含读回入口的键长度/类型）；数值键的 C 类型分 f32/f64（用错只得到 0）；变异 S1~S4 有牙 |
| 嵌入者自有字节流（host IO 后端） | ✅ | `patches/libghostty/8159838`：`ghostty_surface_process_output` + host 回调；`gate-ghostty-patch.sh` 对拍 4 个导出符号 + `mutation-check-ghostty-patch.sh` 证明有牙 |
| OSC 7501 程序状态（Program Status Protocol） | 🟡 | pin `8159838` 带解析层；**app 路径由补丁 0002 接**（`host_program_status` 回调 + 支持查询回包），端侧 `ProgramStatusReport`/`ProgramStatusStore`/标签条+侧栏角标。**在哪档证据下成立**：无头测到「报告→回调→记录」（`TerminalSurfaceTests` 4 项 + `ProgramStatusStoreTests`/`ProgramStatusBadgeTests`/`ProgramStatusPaneTests`），**UI 角标已取证到像素**：`ProgramStatusBadgeRenderTests` 把侧栏装进 NSHostingView、进程内 `cacheDisplay` 抓帧，断言 blocked 画出橙色像素、error 画出红色像素，并各配**对照**（同布局无程序状态 → 这些颜色为 0；判据靠 `g<180` 把「连接中」的黄色点排除掉，否则对照会假橙）。变异 `program-status` M1~M4 有牙（M4 直接打像素断言）。记录生命周期**两半齐了**：提示符开始（OSC 133）清 working/blocked，程序退出（会话状态）同样清 |
| OSC 133 shell 集成（semantic prompt） | 🟡 | **app 路径由补丁 0003 透出**（上游只转 `C`/`D` 两个事件，`prompt_start` 等没透）：`host_semantic_prompt` 回调带 kind/prompt_kind/exit_code/命令行，序数与上游 vt `GhosttySemanticPromptKind` 一致。**在哪档证据下成立**：`TerminalSurfaceTests` 2 项（四步循环 + 非 0 退出码）走真解析器；**已消费**：`prompt_start`（清 working/blocked）、`output_start`/`command_end`（侧栏显示**上一条命令**的命令行/耗时/退出码，标签胶囊对非 0 退出码画红点）；`input_start` 仍未消费。三态不折叠：没报退出码时不画失败记号、不编耗时。变异 `program-status` M3/M5 证明 kind 判别与失败记号有牙 |

## 终端引擎

| 平台 | 引擎 | 状态 | 证据 |
|---|---|---|---|
| macOS | full libghostty（Metal） | ✅ | **焦点下发**（分屏只有当前窗格的光标才闪）：宿主把焦点报给 libghostty（`ghostty_surface_set_focus` / `ghostty_app_set_focus`），口径 = SwiftUI 当前窗格 **且** 窗口 isKeyWindow；非聚焦面由引擎画成空心块并停闪。`WindowFocusTests`（DEC 1004 焦点上报的 `CSI I`/`CSI O` 字节，带可驱动的 isKeyWindow）+ 引擎侧 `TerminalSurfaceTests.testFocusIsReportedToProgramThroughHostWrite`；变异 `focus` F1~F3 有牙。**非聚焦窗格变暗**：盖 `unfocused-split-fill`（未设回落 `background`）层，alpha = 1 - `unfocused-split-opacity`；`SplitDimTests`（规则穷举 + 端到端抓帧），变异 `split-dim` D1~D3 有牙。**后台标签页停画**：surface 保持挂载但不可见时报 occlusion，引擎停掉渲染线程（窗口遮挡状态变化会重新同步）；`WindowFocusTests` 可见性两条 + 引擎侧 `TerminalSurfaceTests`（DEC 2033 的 `CSI ?999;1n / 2n`），变异 `occlusion` O1~O3 有牙。三层证据：字节→网格（`terminal-gate` 6 项，读 `ghostty_surface_read_text`）；渲染层已挂（`RenderProbeTests` 断言 layer-hosting + bounds/scale）；**像素级**（`PixelRenderTests` 抓自进程内视图帧：「红底真出现在屏幕像素上」+ 对照「换绿底红像素消失」+ 主题背景主色非纯黑 + 白字前景被绘制；变异 P1~P3 有牙）。仍不声称字形形状/抗锯齿细节。**键盘输入（IME）**：`TerminalHostView` 实现 `NSTextInputClient` + `keyDown` 走 `interpretKeyEvents`——中文/日文/韩文输入法可唤起（候选窗 + 组合内联）；组合串经 `ghostty_surface_preedit` 内联显示、**提交**走**键入**语义（`ghostty_surface_committed_text`，非粘贴）、候选窗锚点用 `ghostty_surface_ime_point`。`TerminalIMETests` 8 条（`is NSTextInputClient`+`inputContext`/`ime_point` 宽度/提交字节不带 bracketed paste/keyDown 经输入链路/特殊键编码不变（含 Ctrl+B）/**输入法提交的控制字符原样到达**（`\u{0002}` → 裸 `0x02`，tmux 前缀不被吞；对照上游 ghostty/ghostty discussion #14451——该缺陷在上游 macOS app 层的「keyDown 期间键盘布局 ID 变了 ⇒ 整键丢弃」guard 里，Ghosterm 不编译那一层、自研 keyDown 无此判据，故**不需要 vendor 补丁**）+ 两条对照），变异 `terminal` T5~T8 有牙 |
| iOS | full libghostty（Metal） | ❌ |
| Linux | full libghostty（OpenGL 4.3） | ❌ |
| Web | libghostty-vt + 自绘（wasm） | ❌ |
| Android | libghostty-vt + 自绘 | ❌ |
| HarmonyOS | libghostty-vt + ArkUI Canvas | ❌ |
| Windows | 待定 | ❌ |

## 环境包（runtime-payload）

| 端 | payload | 状态 |
|---|---|---|
| Android | Termux bootstrap + proot（按需下载） | ❌ |
| iOS | iSH（随 IPA 打包，延后） | ❌ |

## 构建与交付（CI）

| 交付物 | 状态 | 证据 / 口径 |
|---|---|---|
| macOS **dev** 包 | 🟡 | 每次 push `main` 出包：`.github/workflows/dev-build.yml` → ad-hoc 签名的 `.dmg`/`.zip` + SHA256SUMS + BUILD-INFO，挂到滚动 tag `dev` 的 prerelease。**仅 arm64**（`GhosttyKit.xcframework` 只有 arm64 切片）、**未公证**（首次打开是否被拦取决于取包工具贴不贴 `com.apple.quarantine`：浏览器与 `brew --cask` 都贴 → 要放行一次；`curl` + `ditto` 不贴标记 → 不触发 Gatekeeper 评估。实测口径与证据档（哪些量到、哪些是推论）见 [dev 包怎么装](../how-to/dev-build.md)）。build 号 = `GITHUB_RUN_NUMBER`，dev/release 两轨不同源单调。不能当自动更新通道（每轮 cdhash 不同）。 |
| macOS dev 包 **Homebrew cask** | 🟡 | tap 在独立公开仓库 [`hanxi/homebrew-ghosterm`](https://github.com/hanxi/homebrew-ghosterm)（`Casks/ghosterm-dev.rb`），指向 `dev` 滚动直链。**🟡 成立的证据档**：`brew tap` + `brew trust` + `brew install --cask ghosterm-dev` 在本机装成、`brew style`/`brew audit --cask --strict` 绿、`brew info --cask` 读出 `depends_on arch: :arm64` 与 `depends_on macos: :tahoe`（26.0 下限由内嵌 Homebrew `libcrypto` 抬上去）。**不解决 Gatekeeper**（Homebrew 自己贴 quarantine，见上一行），**`sha256 :no_check`＝故意不校验字节**（滚动资产钉不住，验字节请对 `SHA256SUMS`）；`:latest` cask 默认不进 `brew outdated`，升级要 `--greedy`。**未实测**：Intel 机器与 macOS < 26 的报错路径没有实机可验，只验到声明被正确读出。cask 的 URL 与本轮资产名由 `scripts/check-dev-cask.sh` 在 dev 轨对拍，牙见 `scripts/mutation-check-package-mac.sh` 的 A10 |
| macOS **release** 包 | 🟡 | tag `v*` / PR / 手动 → `.github/workflows/macos-release.yml`，与 dev 共用同一份可复用工作流 `macos-package.yml`（`channel=release、gates=full`）。**🟡 无 Developer ID 证书 secret ⇒ 目前同样落 ad-hoc**；`--sign` / `--notarize` 分支已就绪，拿到 secret 即用 |
| dev 版**更新检测**（app 内入口） | ✅ | 关于窗口（`Ghosterm → 关于 Ghosterm`）版本号下面那一行：手动点「检查更新」→ 取固定 URL 的 `version.json`（不走 API、不占 60 次/小时额度）→ 以 `git_commit` 为主锚（本机短 sha 前缀比清单的 40 位）。**三态不折叠**：无 commit 记录 / `+dirty` / 渠道不对 / 取不到 / 读不懂，一律报「查不出来 + 原因」，不报「已是最新」。**自动安装 ❌（有意不做）**：`signature=adhoc` + `notarized=false` ⇒ 只给下载直链，不画「安装」按钮。判据与文案由 `UpdateCheckTests`（12 项）钉，入口几何由 `AboutWindowTests` 钉，牙见 `scripts/mutation-check-app-update.sh`（7 组变异）；口径见 [dev 包怎么装](../how-to/dev-build.md) |
| Linux 会话安装包（远端 `sessiond`） | 🟡 | CI 在 `ubuntu:22.04` / `arm64v8/ubuntu:22.04` 容器里由 `scripts/build-session-vt-linux.py` 现造 **x86_64 + aarch64** 两份；实测 `min_glibc=2.34`（逐架构写进随包 `BUILD-INFO.txt`），**无 musl 包**。bundle 的 session contract 必须与 mac 端逐位一致（`scripts/stage-session-runtime.py` 把关）；这条链条由 `scripts/mutation-check-sessiond-linux.py` 的四组变异验牙（少打一片改头树的补丁 / pin 被改 / `.a` 冒充 `--engine` / 改一个头的一字节） |
| Android / iOS 出包入口 | ❌ | 没有构建脚本、没有 workflow——是**未开工**，不是 CI 漏项 |
| 闸门分档 | ✅ | 轻档（dev）：`scripts/gate.sh`（core + ctest + ABI + api_smoke）+ 图标一致性 + `gate-ghostty-patch.sh`；全档（release）：再加 `apple-app-gate.sh` / `terminal-gate.sh` + 10 份 mac 侧变异验证 + Linux 侧契约变异验证。缺工具链时显式 `SKIP` 并让 CI 断言不许 SKIP，不静默跳过（铁律 #6） |

## 各端 UI

| 端 | UI | SSH | SFTP | 本地 shell | ssh_config GUI |
|---|---|---|---|---|---|
| macOS | 🟡 | ✅ | ✅ | ✅ | ✅ |

> macOS 打包口径：`.app`/`.dmg` 由 `scripts/package-mac.sh` 产出，**自包含**已实测
> （递归内嵌依赖闭包 + 运行时断言不从 Homebrew 加载）；默认 ad-hoc 签名。
> **🟡 Developer ID 签名 + 公证未做**（无证书；脚本/CI 已就绪）。
> **🟡 最低系统版本**受内嵌 Homebrew OpenSSL 抬高（minos 26.0），需要自建依赖才能降。
> 签名档位按**证书真实的类**记账（`adhoc` / `dev` / `id` / `none`），身份串不在这两类
> 当场拒——**免费个人账号的 Apple Development 能签但不能公证**，与 ad-hoc 一样被
> Gatekeeper 拦（只多买到「签名身份稳定」），故它不构成第三条分发路；口径见
> [dev 包怎么装](../how-to/dev-build.md#签名档位adhoc--dev--id--none)。

> macOS 一行的口径：
> - SSH ✅ —— `apple-app-gate.sh` 起**真 sshd** 跑通「认证 → shell 通道 → 回显进
>   libghostty 网格 → resize 后远端 `stty size` 与网格一致」；主机密钥走真三态
>   对话框（M2），私钥口令走 Keychain + 超时闸（无签名环境不卡连接线程）；
> - 本地 shell ✅ —— 同上（`GT_TRANSPORT_LOCAL_PTY` 分支），且宿主可执行
>   `GhostermApp --local` / `--ssh-smoke` 直接冒烟（后者覆盖 AppModel 全链路）；
> - SFTP ✅ —— core `gt_sftp_*`（list / stat / lstat / readlink / realpath /
>   mkdir / rmdir / remove / remove_all / rename / chmod / walk / upload /
>   download / cancel / pause）+ SFTPBrowser UI（旧单栏浏览）**与新的内嵌文件
>   面板**（`FilesPanelView`：终端标签页右侧、⌘⇧E、前进后退/上级/刷新/过滤/
>   隐藏文件/⌘F 搜索/右键菜单）；e2e 走真 sshd 的 sftp-server，含失败 job
>   三态断言；变异验证 `sftp` 10 项 + `exec` 4 项 + `files` 4 项。
>   **🟡 文件面板口径**：本地 shell 标签页后端=LocalFileBackend（初始目录跟随
>   终端 cwd，且默认**持续跟随**——终端 `cd` 面板就跟着走）；SSH 标签页懒开
>   第二条会话。**独立 SFTP 标签页=左右双栏**
>   （`DualPaneFilesView`，栏间拖放即传）；**全局传输队列** `TransferManager`
>   （进度/速度/ETA/暂停/继续/取消/重试 + 冲突询问；`transfers` 变异 4 项钉住）。
>   **拖放 + Quick Look ✅**：栏间拖放即传（`DropRouter`）、本地条目拖出到
>   Finder、Finder 拖入远程栏=上传；空格/右键 Quick Look（远端下临时副本）。
>   **树/列表可切 ✅**（懒展开的目录树）；**面板键盘** ✅（⌘F/⇧⌘N/⌘R/⌘I/⌫/空格，
>   只挂在文件列表上，macOS 14+；13 退化到右键菜单）。
>   **顶栏折行 + 关闭按钮 ✅**（2026-10-15，用户实测带出）：工具条原是一行
>   `HStack`，面板窄到 260pt 时右边按钮被画到框外（「顶部按钮超框了」）——改走
>   `FlowLayout` 折行（分行规则是纯函数 `FlowLayout.rows`，可离线断言；单项比
>   整行宽时独占一行，不裁不缩）；顶栏第一行**最右**补 ✕ 关闭按钮（此前只能
>   ⌘⇧E / 「显示」菜单开合），双栏 SFTP 标签页的每栏不传 `onClose`、故无此钮。
>   断言 `PanelChromeTests` 6 条（折行规则 3 + 像素对照 3：窄面板顶栏必须更高、
>   右沿不许有控件被裁、给了 `onClose` 才画得出 ✕），变异 `files` F10~F12 有牙。
>   **仍未做**：iOS 抽屉（无 iOS target）、Finder 拖出**远端**条目的懒下载
>   文件承诺（v1 只支持本地拖出）；
> - UI ✅ —— 窗口 + 侧边栏（标签页/连接两栏）+ 顶部标签条（胶囊）
>   + 顶带按钮组（红绿灯 → 侧栏开关 → `+` → 配置无效时的 ⚠，侧栏开合两态
>   同一个 x，`BandControls` 一份两处用；设置与窗口级动作只在 macOS 全局
>   菜单条里，由 `AppMenuCoverageTests` + 变异 `band-controls` 钉住）
>   + 多标签页（后台标签页 surface 保持挂载）+ 递归分屏（⌘D / ⌘⇧D；窗格
>   退出即收掉、只剩一个则关标签页 + 结构改动后分隔等分，见 `SplitExitTests` /
>   `tabs` 变异 T15/T16）
>   + **标签右键菜单**（`TabContextMenu`，侧栏标签行 / 顶栏当前标签胶囊 /
>   标签条 chip **共用一份**：文件面板开关 + 向右/向下分屏 + 关当前分屏 +
>   锁定 + 关闭；顶栏原本可见的两个分屏按钮移入菜单，右端那个 ⋯ 溢出菜单已删——
>   里面每条都是副本。菜单作用在**被右键的
>   那条**标签上并把它带到前台——右键的未必是当前标签页，写错就「右键 A 却
>   动了 B」，不报错、只在屏幕上不对；`TabContextMenuTests` 4 条 +
>   变异 `tabs` T17/T18 钉住）
>   + 标签页锁定 + 状态覆盖层（连接中/失败/已退出四档显式）+ 设置页（通用/
>   会话/外观/高级）+ 编辑器 + SFTP 浏览器 + 三态对话框 + 传输面板；
>   **遗留（显式）**：标签页恢复/持久化、拖拽重排手势；
> - ssh_config GUI ✅ —— HostEditorSheet（含 `Host *` 通配块编辑，M1 遗留
>   落点）+ 写侧三件套（备份 / 冲突检测 / 原子写，ConfigStoreTests 6 项）。
| Android | ❌ | ❌ | ❌ | ❌ | ❌ |
| iOS | ❌ | ❌ | ❌ | ❌ | ❌ |
| HarmonyOS / Windows / Linux / Web | ❌ | ❌ | ❌ | ❌ | ❌ |
