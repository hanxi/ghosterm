# 能力矩阵

> 当前源码、线上发行与验证范围统一见[当前状态](current-status.md)。以下按平台与历史切片区分，不将旧测试结果记作当前版本通过。

> 状态口径：✅ 已实现且有证据；🟡 已实现但仅在所述证据范围成立；⏳ 契约/占位存在、未实现；❌ 未开始；⛔ WONTFIX 不做并注明原因。平台与历史切片的证据不互相替代。

## 核心（C）

目录创建须区分两种能力：旧同步 `gt_sftp_mkdir` 已供 macOS 使用；§9.65 已新增带单次总预算、取消及副作用结果的 `gt_sftp_mkdir_controlled`，Swift/JNI/Kotlin 已绑定并专项验证。Android §9.64 实现 HOME/SAF 单目录原语，§9.65 补远端独立请求；统一目录服务已由 §9.66 接入；任务独立连接已由 §9.67 接入，执行器与页面分别由 §9.69/§9.70 接入。macOS 当前会复用既有目标目录，不具备整树非覆盖提交或独立目录合并确认，见 [实际调用链](../../apple/README.md#目录创建与合并边界)。

共享写侧已按[§9.37](android-implementation.md#937-主机信任写入事务与移除绑定2026-10-30)接通 TOFU/移除共同事务与两端删除绑定；此前 `526d24d` 的 79 ABI 是只读切片历史结果。Android 管理页面、repository 与指纹已通过 11 项定向、17 组变异及完整 175 项设备回归（[§9.39](android-implementation.md#939-主机验证记录管理页面2026-10-30)）；A3 网络矩阵仍待完成，A7 的 16 项偏好后续已接入（§9.83–§9.91）。macOS 仅有共享绑定与认证消费，尚无记录管理页面。

| 能力 | 状态 | 证据 |
|---|---|---|
| C ABI（`ghosterm.h` v0，未冻结） | 🟡 | [插件计划第二十六批](plan-plugin-bridge.md) 当前 154 符号，Apple 150/4、JNI 116/38，通道 / Session / JSON 请求 / 资源 Store / 保留快照、scoped source 与可选 runtime 已接 native worker，原生根授权、完整业务 schema 及 App 插件页面待接；§9.122–§9.123 的 94 符号、Apple 90/4、JNI 56/38 为历史切片；§9.122 增加受控显式删除，§9.123 已接 Android 历史清理与独立记录；含受控读取到 fd、非覆盖发布和单目录创建。Android 只读文件页面与暂存层已消费，macOS 产品仍待迁移；§9.57 已补发布入口及两端绑定，原草稿对拍失败保留为历史。接口未冻结，消费者须同步重编译 |
| 插件 SSH / Session / 请求 / 资源 / scoped source worker（视图待接） | 🟡 | [插件计划第九至二十三批](plan-plugin-bridge.md)：公共通道十一项、Session 十项、请求八项、资源十六项、文件来源七项 ABI / Swift queue / JNI worker；Core 378 CTest / 147 ABI。Source 26 本机 / 3 真实关闭边界及两三跳四 SSH 场景、Swift 8、API 36 arm64 / 4 KiB 页 Android 8 项通过；双向各 256 KiB 与八十个确认 RPC，Source / 资源 / RPC 独立完成记录各六份。Source 只读授权根 fd 内普通文件、同 fd 两遍核验，最多每 Session 16 个，成功 destroy 前 pin 父对象；关闭 IO 与原失败分开，无外泄 transfer handle。新 24 C / 5 Swift / 6 JNI / 1 Kotlin 与旧私有 Source / 公开 Session / 资源的 73 C / 6 Swift 分套通过，共 115 组手工变异，不增加 CI 二十份。关闭 provider / 无 SSH 档仍为 147 导出，缺能力明确不支持。上述为第二十三批历史范围；第二十六批已接可选 runtime 公共门面。App 根授权 / 批准 / 视图、完整 SDK 服务 / gterm IPC / 完整多实例未接，产品尚未消费 ProxyJump；不是完整插件档。其它端 / 物理设备 / 封闭内网 / WAN 未验，网关只预留、不实现 |
| Portable 插件 SDK（请求首片） | 🟡 | [插件计划第二十四批](plan-plugin-bridge.md)：strict TypeScript / ES2020、类型化请求与对象 / 文本 / 整数 codec、schema / 取消 / Outcome / 撤销边界；Core/QuickJS-NG 0.18.0 上 16 场景（13 真实 RPC、纯值 / 入队前拒绝 / 故障 adapter 各一）、36 Ajv 对拍、四类静态负向及九组编译后变异通过，均为手工 / 本机 ASan/UBSan 证据。上述为第二十四批范围；第二十五 / 二十六批已接受控 runtime / native adapter 与两端 worker。完整 PluginContext / 资源进程服务 / App 消费仍待接，不把 SDK 当整个插件完成。未来网关未实现 |
| Core 可选 JS runtime / 两端 worker（App 未启用） | 🟡 | [插件计划第二十五至二十七批](plan-plugin-bridge.md)：QuickJS-NG pin / 输入固定，默认 OFF。公共 runtime / roots 配置、Swift / Kotlin / JNI 与 ResourceBroker 已接；当前 Config POD 布局变更须重编译，Core 397 CTest / 154 ABI、Apple 150/4、JNI 116/38。第二十七批 32 C / QuickJS 场景、Swift / API36 arm64 4KiB 页模拟器 Android 各 11 项；真实授权目录 256 KiB / 空文件发布、回执、快照分块读、伪造 ref 拒绝、取消 / 期限 / OOM / 关闭已分片验证，另三 adapter 故障不算真实端点故障。新 31 变异及旧相关复验共 85 组分套手工，ASan/UBSan，无 LSan、不增加 CI 二十份；最新资源专项 26 组完整通过。Config 只接 native 显式授权 POSIX 根，资源 ID 不是读取权限；无 Store 明确缺服务，不实现 cwd / TTY 猜测、SAF 根或 OS 沙箱。App 激活 / 根授权 / 资源视图、完整 SDK / 进程 / gterm / 包安装 / 实际插件 / 多实例仍待接；非本轮 SSH / WAN / 真机 / 签名发行结论，未来网关未实现 |
| 插件物理目录快照（元数据校验待接） | 🟡 | [插件计划第二十八批](plan-plugin-bridge.md)：私有 C owner 从目录 fd 采集固定字节 / SHA256；1024 文件、16 MiB 单文件 / 64 MiB 内容、另限 4096 物理条目，链接 / 特殊文件 / 非法路径拒绝。32 本机 / 配置＋9 syscall 边界、28 组编译后 ASan/UBSan 变异通过，Core 438 CTest / 154 ABI；provider / SSH / JS OFF 同 41 项、严格 C11 与 NDK / Android bridge 编译。仅物理副本，原生 manifest / integrity / Unicode / 来源 / 模块加载未接，不授予执行；无 App / 设备 / 安装或发行结论，网关未实现 |
| `ssh_config` 解析（AST + Host 块） | ✅ | `ctest sshconfig_golden`：4 个 Host 块 / 6 个模式 |
| `ssh_config` 无损往返（**未编辑**） | ✅ | 与原文**逐字节相同**（`test_config` 头号断言） |
| `ssh_config` 求值（typed） | ✅ | `gt_config_resolve`：user/hostname/port/forwardagent/compression/serveraliveinterval/identityfile/proxyjump/setenv/localforward |
| `ssh_config` 求值（通用视图 = `ssh -G`） | ✅ | `gt_config_effective`：**42 键 × 6 host = 252 项**全表语义对拍（布尔归一化）；支持集双向校验 |
| 求值配置用于实际 SSH 连接 | 🟡 | 共享检查返回不支持指令及来源，Apple 连接目标携带快照，SSH/SFTP/tmux 在 DNS 前拒绝；Android 主机按钮已展示拒绝原因。高级选项执行未实现，Android 产品连接页面已有定向和完整设备证据，最终目标在同一 AST 上重新检查；native SSH owner 见 §9.25；当前准入档位及 Match/未知指令的保守边界见 [§9.24](android-implementation.md#924-共享连接配置准入与两端接线2026-10-30) |
| 默认 IdentityFile 候选链 | ✅ | `gt_identity_policy_t` 五档（9_6=7 / 10_3=5 / MASTER=6 / NONE / CUSTOM），每条链注明出处；回落机制有断言 |
| `ssh_config` **编辑** API（改值后无损写回） | 🟡 | `gt_config_edit`：9 个原夹具 + 5 个补充用例逐字节对拍；非连续重复指令替换/删除已按实际匹配行修复，未知行、注释、CRLF 和相邻块保留，B7/B8 变异护此行为。Android 另验实际保存与编辑器输出保护；来源查询与包含文件/重名块保护已接入；不代表表单已支持全部指令；改名接口另列下行 |
| 配置字段来源与编辑归属 | 🟡 | `gt_config_inspect_host`、Swift/JNI 绑定及 8 组 C / 1 组 Swift 变异；Android 来源提示、只读保护及配置往返已接入。仅覆盖 HostName/User/Port，macOS 表单尚未消费，Android 显式覆盖/恢复继承已有 32 项设备/32 组配置变异证据，重命名在后续切片单独验收；见 [Android §9.7](android-implementation.md#97-显式覆盖与恢复继承2026-10-30) |
| IdentityFile 来源查询 | 🟡 | `gt_config_identity_at`、Swift/JNI 已接；原顺序累积条目与本块/继承/Include 来源，不展开路径、不补默认身份；Match/非法参数拒绝，8 C + 1 Swift 和 3 组 Android 变异通过。来源接口证据见 [§9.34](android-implementation.md#934-identityfile-来源查询与平台绑定2026-10-30)；Android 主机身份选择与引用列表已接，见 §9.35，macOS 表单未接 |
| 主机信任记录快照 | 🟡 | 只读 `gt_known_hosts_*` 与 Swift/JNI 已接；与认证复用词法读侧，保留 marker、畸形条目和原始字节范围。37 CTest、10 C + 1 Swift + 4 JNI/Kotlin 变异及既有 20 组真实 SSH 信任变异通过；不等于密钥验证、管理页面或安全移除，见 [§9.36](android-implementation.md#936-主机信任记录快照与平台绑定2026-10-30) |
| 主机信任写入事务 | 🟡 | TOFU 与 `gt_known_hosts_remove` 共用锁、备份、同目录原子替换及提交结果；§9.37 的 38 CTest / 80 ABI、17 C + 2 Swift 事务变异及两端绑定证据通过。Android 管理页另见 §9.39，macOS 仅有绑定和认证消费。协作写者以外的并发与断电恢复未验 |
| 配置别名重命名 | 🟡 | `gt_config_rename_host`、Swift/JNI 与 Android 表单接线；34 项 Core CTest 中含 `sshconfig_rename`，12 C / 1 Swift 隔离变异和 Swift 定向证据通过；Android API 36 arm64 / 4 KB 页模拟器 36 项设备测试与 36 组配置/页面变异通过。只改目标 token，写盘由平台事务承担；macOS GUI 未接入，见 [§9.9](android-implementation.md#99-主机别名重命名2026-10-30) |
| `Include` 展开 / glob | ✅ | `process_include`：相对路径基准 = **~/.ssh**（OpenSSH_10.3p1 实测 + 真 ssh `-G` 同口径对拍）、`~`/`~user`、glob 无匹配静默 / 具名缺失显式失败 / 嵌套 + 深度上限 16；被含内容不进写回（铁律 #4）；`test_config` 6 组断言 + 变异 I1~I4 有牙 |
| 事件队列（低频、批量轮询） | 🟡 | `src/core.c` 为加锁的 256 项环形缓冲；`test_api` 与 SFTP job 的 PROGRESS / JOB_DONE 证据已存在。队列满会覆盖最旧事件，尚无 job 终态查询 ABI，不能保证过载时终态不丢；需按路线图补保留/查询保障 |
| Transport 接缝（本地 PTY） | ✅ | `gt_transport_open_pty`（POSIX forkpty）；`pty_transport` 行为断言（真 forkpty，离线）：EOF 前不丢最后一批 / 退出码 / 字节往返 / 超时不折叠 EOF / winsize / resize / TERM / close(NULL) 安全 / 前台作业四态 / 内核 cwd / **写绝不无限等**（第 13 节：pty 输入队列满时写要有界返回且积压字节随后补送——旧实现的 `poll(-1)` 会钉死会话线程 → 关标签页时 `queue.sync` 把主线程也钉死＝整个 app 无响应）；变异 `mutation-check-transport` P1~P4 有牙。**Swift 端** `StopBoundedTests` 断言 `SessionController.stop()` 只取消且有界返回，并等待弱引用证明 I/O owner 真正释放（当前 stop 不再同步等待队列）（走生产链路 `sendText → onInput → 会话线程 write`，子进程 `stty -icanon -echo` 才会真触发 EAGAIN 而非 canonical 丢弃），对 P4 验过牙（变异体挂起超时 / 干净 0.7s 绿） |
| Transport 接缝（SSH shell） | ✅ | `gt_transport_open_ssh_shell`（libssh2 pty-req+shell）；`ssh_e2e` 真 sshd 回显/resize；`b27c787` 修复零超时轮询（临时 nonblocking 后恢复会话设置），有真实远端屏障和五组 SSH 变异证据，见 §9.27；不扩大为所有通道打开/关闭均有界 |
| SSH 已确认 EOF 与真正断流区分 | 🟡 | `f39cb6d` 已发行；Core 50 CTest / 94 ABI、三组 C 边界编译变异、真 sshd 97 项、API 29 / API 36 各 29 项及 macOS 原生构建已验。后续 `ae164f1` 两端完整 942 项已通过（§9.191），不借给更新源码，真实手机频繁断流根因仍未全部确定；不增加自动重连，未知退出码保持未知，见 §9.185、§9.187 |
| 普通 SSH shell 打开取消与总预算 | 🟡 | §9.41 新增 controlled API，Android native owner 已接原连接余额和取消；三个阶段取消/预算、明确拒绝/断线及请求隔离共 13 项定向、13 组变异通过。Swift 新绑定验真实 sshd 和三组变异，macOS 产品仍使用旧 opener，尚未接入其原认证 deadline/取消，见 [两端边界](../../apple/README.md#ssh-认证后的通道打开边界)及 [实施契约](android-implementation.md#认证后打开-shell-的实施契约) |
| Transport：前台作业判定（关闭确认用） | ✅ | `gt_transport_child_state` **四态**（UNKNOWN/EXITED/IDLE/JOB）：POSIX 读内核 `tcgetpgrp(master) == shell 自己的进程组` 判「空闲提示符 vs 有作业」，SSH / ConPTY 报 `UNKNOWN`（**不猜**，铁律 #6）。`test_transport` 第 11 节（IDLE/JOB/EXITED/NULL→UNKNOWN）+ 变异 `mutation-check-transport` P1/P2 有牙。**前置修复**：本地 PTY 子进程曾拿不到控制终端（父进程抢跑 `setpgid` → 子进程 `setsid` 报 EPERM），已修 |
| Transport：会话 cwd | 🟡 | **本地 PTY ✅**：`gt_transport_child_cwd` 内核查询（macOS `proc_pidinfo` / Linux `/proc/<pid>/cwd`），`test_transport` 第 12 节（继承 cwd + `cd` 后随之变）。**远端 cwd 未交付**：OSC 7 在我们钉的 embedded host-IO 下到不了宿主（无 `GHOSTTY_ACTION_PWD`），且 libghostty 要求 host `isLocal`——接线留为 seam，依赖上游支持 / roadmap R2 |
| SSH（libssh2） | 🟡 | 普通条目的连接/认证已有以下证据，标记拒绝及未支持格式的边界见 known_hosts 行。connect / pubkey·password 认证 / 主机密钥三态 / TOFU；`ssh_e2e` 行为断言（真 sshd，环回离线）。**写绝不无限等**（§5b：远端不读 stdin 时 `gt__ssh_shell_write` 在 `GT_SSH_WRITE_GRACE_MS` 内有界返回、积压字节走 `wtail` 队列随后补送——与本地 PTY §13 同结构。`TIMEOUT`/`EAGAIN` 不再当硬错丢字节（旧实现把两者都算 `GT_ERR_IO` → 键入的字节就这么没了）。写路径自己设/还 session timeout，不打扰同 session 的 SFTP。e2e 限制：sshd 排空速率 ≈ 40 MiB/s，3 MiB payload 写完时队列**根本没形成**——flush 机制无法被真 sshd e2e 触发，由代码审阅 + PTY §13/P4 的变异覆盖保证同构正确性） |
| SFTP | 🟡 | `gt_sftp_*`（libssh2_sftp 封装）：list / stat / lstat / readlink / realpath / mkdir / rmdir / remove / remove_all（递归）/ rename / chmod / walk（递归枚举，visitor 可取消）/ upload / download（job + 事件三态）/ cancel / pause；**每会话一把 I/O 锁 + 单条 I/O worker 线程**（修掉「每 job 一线程共用同一 libssh2 会话」竞态）。e2e 真 sshd 75 项断言 + 当前变异 `mutation-check-sftp` 八组 / `mutation-check-exec` 三组（新增超时恢复）（历史断言数量不当作当前变异组数）；覆盖截断与关闭检查已由 §9.53 补齐；旧下载的完整长度/源变化校验、非覆盖提交和过载下终态保障仍有缺口，见 [Apple 下载边界](../../apple/README.md#旧下载的完整性边界) |
| SFTP 受控读取到暂存 fd | 🟡 | §9.56：Core/Swift/JNI/Kotlin 与 Android 匿名暂存接通；21 Core＋1 无库＋4 Swift 及 13 组 Android 变异通过；零上限空源、长度/属性/关闭和 fd 释放已验。远端发布原语后续由 §9.57 补齐，§9.70 已接 Android 任务页面，macOS 旧下载路径未迁移 |
| Android 请求内冲突决策/执行清单 | 🟡 | §9.63：15 项定向、42 组变异与完整 342 项，仅 API 36 arm64 / 4 KB 页模拟器。决定隔离、目录/不支持项跳过、只读清单、名称预算及真实写入集成已验；§9.70 已有任务/冲突页面消费者，不预留名称或绕过关系未知 |
| Android 目标预检/冲突元数据 | 🟡 | §9.62：13 项定向、34 组变异与最终完整 327 项，仅 API 36 arm64 / 4 KB 页模拟器。真实 HOME/SAF/SSH 目标读取、创建能力三态、已知目录关系与只读冲突已验；不同远端身份关系未知；冲突决策数据层后由 §9.63 补齐，目录创建由 §9.64–§9.66 接入，任务与 UI 由 §9.70 接入 |
| Android 粘贴请求/来源树扫描 | 🟡 | §9.61：API 36 arm64 / 4 KB 页模拟器 13 项定向、34 组变异；真实本机/SAF/SSH 元数据枚举，空目录、未知大小、来源复核、预算与取消已验。不等于内容快照；目标预检后由 §9.62 接入；目录创建与任务页面后由 §9.64–§9.70 接入；未知跨远端关系的请求内确认已由 §9.110 接入；不证明任意服务器独立 |
| Android 文件来源清单 | 🟡 | §9.60：API 36 arm64 / 4 KB 页模拟器 9 项定向、16 组变异；真实本机/SAF/SSH 来源、跨页/旋转、替换/清空已验。不启动传输，来源扫描已由 §9.61 补齐，目标预检后由 §9.62 接入，粘贴页面由 §9.70 接入，未知跨远端关系的显式确认已由 §9.110 接入 |
| Android SAF 新文档写入 | 🟡 | §9.59：API 36 arm64 / 4 KB 页模拟器 16 项设备、30 组变异；创建、关闭后回读与取消租约已验。只消费 ContentResolver，不提供通用原子提交；来源扫描已由 §9.61 补齐；目录创建和任务页面后由 §9.64/§9.70 接入，部分文档恢复未接 |
| Android 应用私有目录文件发布 | 🟡 | §9.58：API 36 arm64 / 4 KB 页模拟器十项设备、24 组新增及 23 组暂存变异；私有命名候选与 NOREPLACE，不依赖被 App SELinux 拒绝的硬链接。SAF 新文档另由 §9.59 补齐；来源扫描已由 §9.61 补齐；本机/SAF 单目录创建由 §9.64 接入，远端目录后由 §9.65/§9.66 补齐；任务容量已由 §9.72/§9.106 接入，历史只读加载见 §9.73；不自动恢复执行；不改变匿名来源快照或 macOS 文件队列 |
| SFTP 最终目标非覆盖发布 | 🟡 | §9.57：Core/Swift/JNI/Kotlin 已验；真实 OpenSSH 文件/目录/链接冲突、模拟协议故障及 API 36 arm64 设备证据。查询成功才记冲突，只有明确不存在才尝试 flags=0 的 rename；成功/拒绝/未知分别保留。Android 已由 §9.69/§9.70 消费，macOS 复制产品未消费，不承诺通用原子提交 |
| SFTP 独占暂存创建 | 🟡 | §9.54：Core/Swift/JNI/Kotlin 已接 EXCL、0600、普通 fd 与分阶段结果，22 组 Core/无库/Swift、九组 Android 变异通过；只创建任务暂存路径，不发布最终目标，macOS 产品未消费 |
| 本机/SAF 匿名来源快照 | 🟡 | Android §9.55：容量/数量预算、来源片段、只读租约及真实 SAF/SFTP 集成；23 组变异与完整 252 项设备回归，仅 API 36 arm64 / 4 KB 页模拟器；§9.70 已接 Activity 任务页面；取消上传跨层租约保持由 §9.56 接入，macOS 无此暂存实现 |
| SFTP 打开预算与取消 | 🟡 | §9.47：Core/Swift/helper、默认补丁库 17 项协议、十组接口变异与十一组依赖守卫通过。macOS 产品仍用旧 opener，Android owner/页面已按 §9.51/§9.52 消费；后续操作不受打开预算保护，[详细范围](host-ssh2-build.md) |
| SFTP 单目录创建预算与取消 | 🟡 | §9.65：Core/Swift/JNI/Kotlin 已接，真实与协议矩阵 81 组、26 Core＋1 无库＋5 Swift、Android 12 项定向/20 组变异通过；专用连接、一次预算、六种创建状态及实际释放。统一目录服务后由 §9.66 接入；§9.70 已接 Android 任务/页面，macOS 产品迁移未接；设备仅 API 36 arm64 / 4 KB 页 |
| SFTP 目录浏览预算与取消 | 🟡 | §9.49：Core/Swift 将规范路径、完整列表与成功关闭目录合并；36 项协议、19 组变异通过。专用连接，不能与传输 job 共用；Android owner/页面已由 §9.51/§9.52 接入，macOS 产品尚未接入 |
| SFTP 只读预览预算与取消 | 🟡 | §9.50：Core/Swift 单预算覆盖 LSTAT/OPEN/FSTAT/READ/CLOSE，容量与实际 EOF 校验，46 项协议及 20 组变异通过。Android JNI/严格文本解码与专用 owner 关闭已按 §9.51 接入，页面与代次已由 §9.52 接入；§9.92 外部只读副本工作区已接并进行专项验收，该切片回归/发行已由 §9.92–§9.94 记录，完整设备矩阵仍待补；macOS Quick Look 未消费 |
| SSH exec 通道 | ✅ | `gt_ssh_exec*`：远端单命令（stdout/stderr 分离 + 退出码）；把递归搜索/大树遍历放到远端本地算（减少宿主发起的目录/属性请求）。服务器禁 exec → `GT_ERR_UNSUPPORTED`（不静默降级）。e2e 含**性能对比**（300 文件：exec-find vs sftp-walk，两条路都找全；**环回上 exec 反而慢 ~14×**——起远端进程的开销，真实 RTT 链路收益仍须实测，不能由环回结果推定）。当前 `mutation-check-exec.sh` 三组变异（含恢复共享会话超时）；不与历史断言数混计 |
| 端口转发 -L/-R/-D | ❌ | — |
| known_hosts / TOFU | 🟡 | 明文条目/模式、TOFU、撤销优先及读取错误已有真 ed25519/ECDSA sshd 的同步/公开异步 API 证据，25 组隔离变异通过（K17/K20 为独立文件故障，其余经真实 SSH）；TOFU 与移除已共用锁、备份、原子替换与提交状态；17 C + 2 Swift 事务变异范围见 §9.37。非协作写者竞态/断电恢复不在已验范围；Android 管理页面已另按 §9.39 验收，macOS 无管理页面。已解析的 hashed 条目、当前主机的未知标记、无匹配普通记录的 CA 规则显式报不支持；未实现哈希匹配/主机证书验证，不称完整 OpenSSH 兼容。macOS SFTP 产品认证回归与 Swift 绑定已验，Android native 状态/拒绝路径已在 §9.25 验证，页面首片及未完成边界见 §9.27；见 [§9.22](android-implementation.md#922-a3-共享主机信任前置2026-10-30) |
| 本地 PTY | 🟡 | POSIX（macOS/Linux）forkpty ✅；Windows ConPTY ❌（只有规划，能力缺失显式报错） |
| 可取消的 SSH 认证 | 🟡 | 通用公开 `ssh_auth.h` 与 Apple `SSHAuthenticator`/DNS 解析已从旧安装链拆出；C 连接器与公开翻译测试、8 组认证 ABI / 6 组凭据选择变异通过。Apple 普通 SSH、SFTP、tmux 管理/attach 已统一经 `SSHAuthenticationFlow`；Android/JNI 已在 §9.25 接入同一 native owner，信任/凭据等待的取消和超时已有真 sshd 证据；DNS 取消/预算、产品页面已有定向测试；macOS 仍只交付单地址；Android 已接 TCP 候选推进和共用总预算，12 项真实双栈/丢包/取消定向与 14 组变异见 §9.40。Android 通道打开取消见 §9.41；真实域名 DNS 和更多网络/设备矩阵仍待补，详见 [网络收尾](android-implementation.md#46-a3-网络与取消收尾)。 |
| POSIX PTY 尺寸能力 | 🟡 | macOS 共享后端 `pty_dimensions` 三类基线随核心闸门通过；`4924a2d` 另修复子进程继承信号屏蔽，真实窗口→本地 tmux / 本地 shell→系统 ssh→远端 tmux 无输入缩放与 PID 保留已有两条回归，PTY 继承三组变异通过。保留像素尺寸与默认值校验，非全平台证据；见 [实施记录](optional-tmux-implementation.md#后续-pty-尺寸通知修复2026-10-30)。 |
| 可选 tmux 后台会话 | 🟡 | 3.2a 已在 macOS 本机与 Ubuntu 22.04 SSH 验证真 PTY attach 和列表/管理；3.6a 另验证原生键鼠、任务保留、断网、背压、未知回复及受限账户。Android 远端管理/独立 attach 已接；`ee2a83f` 两端各 45 项/完整十二组，`a63ec46` 两端各三项/两组增量，累计十三个保护组分轮通过；最新签名包 16 KB 页实际操作已验，见[后台会话记录](android-implementation.md#android-tmux)。仅 POSIX 兼容远端 shell；其他版本与平台证据有限，不自动恢复，受用户 tmux 与系统清理策略影响。 |
| 配色集随包 + 主题选择 | ✅ | `build-ghostty.sh` 从锁定的 `iterm2_themes` 依赖派生配色集（以本轮构建实际数量为准） → `Contents/Resources/ghostty/themes`；CGhostty 推导 `GHOSTTY_RESOURCES_DIR`（上游入口，未设置时兜底）；出包脚本的**包内**配置检查验随包主题被引擎接受（变异 A6 有牙） |
| 终端配置下发（GUI → 引擎） | 🟡 | 正常配置及已有 surface 重载已有证据；存储读取错误、重复键与验证失败后的磁盘状态另有缺口，见下方 macOS 限制。 `gt_ghostty_app_new_with_config` / `_app_reload_config` / `_config_get_{float,double,bool}`：设置页写 Ghostty 语法的配置文件，libghostty 自己解析；**判有效看诊断条数**（未知键只记诊断不丢配置）；`font-size` 改完网格密度真的变（`TerminalConfigTests` 11 项，含读回入口的键长度/类型）；数值键的 C 类型分 f32/f64（用错只得到 0）；变异 S1~S4 有牙 |
| 嵌入者自有字节流（host IO 后端） | ✅ | `patches/libghostty/8159838`：`ghostty_surface_process_output` + host 回调；`gate-ghostty-patch.sh` 对拍 4 个导出符号 + `mutation-check-ghostty-patch.sh` 证明有牙 |
| OSC 7501 程序状态（Program Status Protocol） | 🟡 | pin `8159838` 带解析层；**app 路径由补丁 0002 接**（`host_program_status` 回调 + 支持查询回包），端侧 `ProgramStatusReport`/`ProgramStatusStore`/标签条+侧栏角标。**在哪档证据下成立**：无头测到「报告→回调→记录」（`TerminalSurfaceTests` 4 项 + `ProgramStatusStoreTests`/`ProgramStatusBadgeTests`/`ProgramStatusPaneTests`），**UI 角标已取证到像素**：`ProgramStatusBadgeRenderTests` 把侧栏装进 NSHostingView、进程内 `cacheDisplay` 抓帧，断言 blocked 画出橙色像素、error 画出红色像素，并各配**对照**（同布局无程序状态 → 这些颜色为 0；判据靠 `g<180` 把「连接中」的黄色点排除掉，否则对照会假橙）。变异 `program-status` M1~M4 有牙（M4 直接打像素断言）。记录生命周期**两半齐了**：提示符开始（OSC 133）清 working/blocked，程序退出（会话状态）同样清 |
| OSC 133 shell 集成（semantic prompt） | 🟡 | **app 路径由补丁 0003 透出**（上游只转 `C`/`D` 两个事件，`prompt_start` 等没透）：`host_semantic_prompt` 回调带 kind/prompt_kind/exit_code/命令行，序数与上游 vt `GhosttySemanticPromptKind` 一致。**在哪档证据下成立**：`TerminalSurfaceTests` 2 项（四步循环 + 非 0 退出码）走真解析器；**已消费**：`prompt_start`（清 working/blocked）、`output_start`/`command_end`（侧栏显示**上一条命令**的命令行/耗时/退出码，标签胶囊对非 0 退出码画红点）；`input_start` 仍未消费。三态不折叠：没报退出码时不画失败记号、不编耗时。变异 `program-status` M3/M5 证明 kind 判别与失败记号有牙 |

## 终端引擎

| 平台 | 引擎 | 状态 | 证据 |
|---|---|---|---|
| macOS | full libghostty（Metal） | ✅ | **焦点下发**（分屏只有当前窗格的光标才闪）：宿主把焦点报给 libghostty（`ghostty_surface_set_focus` / `ghostty_app_set_focus`），口径 = SwiftUI 当前窗格 **且** 窗口 isKeyWindow；非聚焦面由引擎画成空心块并停闪。`WindowFocusTests`（DEC 1004 焦点上报的 `CSI I`/`CSI O` 字节，带可驱动的 isKeyWindow）+ 引擎侧 `TerminalSurfaceTests.testFocusIsReportedToProgramThroughHostWrite`；变异 `focus` F1~F3 有牙。**非聚焦窗格变暗**：盖 `unfocused-split-fill`（未设回落 `background`）层，alpha = 1 - `unfocused-split-opacity`；`SplitDimTests`（规则穷举 + 端到端抓帧），变异 `split-dim` D1~D3 有牙。**后台标签页停画**：surface 保持挂载但不可见时报 occlusion，引擎停掉渲染线程（窗口遮挡状态变化会重新同步）；`WindowFocusTests` 可见性两条 + 引擎侧 `TerminalSurfaceTests`（DEC 2033 的 `CSI ?999;1n / 2n`），变异 `occlusion` O1~O3 有牙。三层证据：字节→网格（`terminal-gate` 的网格断言，读 `ghostty_surface_read_text`）；渲染层已挂（`RenderProbeTests` 断言 layer-hosting + bounds/scale）；**像素级**（`PixelRenderTests` 抓自进程内视图帧：「红底真出现在屏幕像素上」+ 对照「换绿底红像素消失」+ 主题背景主色非纯黑 + 白字前景被绘制；变异 P1~P3 有牙）。仍不声称字形形状/抗锯齿细节。**键盘输入（IME）**：`TerminalHostView` 实现 `NSTextInputClient` + `keyDown` 走 `interpretKeyEvents`——中文/日文/韩文输入法可唤起（候选窗 + 组合内联）；组合串经 `ghostty_surface_preedit` 内联显示、**提交**走**键入**语义（`ghostty_surface_committed_text`，非粘贴）、候选窗锚点用 `ghostty_surface_ime_point`。`TerminalIMETests` 8 条（`is NSTextInputClient`+`inputContext`/`ime_point` 宽度/提交字节不带 bracketed paste/keyDown 经输入链路/特殊键编码不变（含 Ctrl+B）/**输入法提交的控制字符原样到达**（`\u{0002}` → 裸 `0x02`，tmux 前缀不被吞；对照上游 ghostty/ghostty discussion #14451——该缺陷在上游 macOS app 层的「keyDown 期间键盘布局 ID 变了 ⇒ 整键丢弃」guard 里，Ghosterm 不编译那一层、自研 keyDown 无此判据，故**不需要 vendor 补丁**）+ 两条对照），变异 `terminal` T5~T8 有牙 |
| iOS | full libghostty（Metal） | ❌ |
| Linux | full libghostty（OpenGL 4.3） | ❌ |
| Web | libghostty-vt + 自绘（wasm） | ❌ |
| Android | libghostty-vt + 自绘 | 🟡 | A0 已验；A2 本地 PTY→VT→Canvas 与 InputConnection 首片已交付，文本操作切片接入文本选择/复制粘贴/本地清屏；API 36 arm64 / 4 KB 页模拟器验证见实现计划 §9.14。选区边缘滚动/Ctrl 生命周期切片已验（§9.15）；Ctrl 标点、模式感知 Enter 与真实 Gboard 拼音基本路径已验（§9.16）；后续按键重复/释放与焦点归属已验（§9.17，94 项完整设备测试、18+2 组选组变异）；后续修饰键/数字键盘/组合音符及静止文本闪烁已验（§9.18–§9.19），§9.21 短时取帧与性能探针通过完整 101 项、四组性能变异及五组回归；模拟器 debug 基线与 API 29 正式包十分钟输出观察已记录（§9.137–§9.138）；真机、完整发行档/端点长负载及物理键盘/布局矩阵未完成 |
| Android 终端后台保护 | Application 进程内 owner + specialUse 前台服务 + 可选 partial wake lock | 🟡 | 工作区候选；API 29/36 已验真实服务、通知、短时后台 IO、presenter 清理后 owner 保留和锁释放；API 36 组合 71 项通过。非零 ServerAliveInterval 仍未接，不承诺 Doze/强停/换网不断线，见 [§9.198](android-implementation.md#9198-终端交互与后台连接保护候选2026-11-04定向设备验证) |
| Android Kitty 图形/动画 | libghostty-vt + 系统 PNG + Canvas | 🟡 | 已接通静态、Unicode、帧传输/播放/停止/合成及隐藏停 tick；API 29 图形 21 项、API 36 图形与既有终端 50 项、十组变异通过；云端签名 R8 APK 已在 16 KB 页模拟器验证实际动画/删除，范围见[当前证据](android-implementation.md#android-kitty) |
| Android Nerd Fonts 符号 | 内置 Symbols Nerd Font Mono + 系统等宽回退 | 🟡 | API 29 / API 36 各 15 项组合专项通过；八类图标（含补充私用区）真实 PTY→VT→Canvas 像素、ASCII 字宽、中文/Emoji 和包内许可已验；云端签名 R8 包已验 16 KB 页升级、旧文件与实际图标/CJK/Emoji 显示；完整设备与 A2 验收分开记账，见[字体记录](android-implementation.md#android-nerd-fonts) |
| HarmonyOS | libghostty-vt + ArkUI Canvas | ❌ |
| Windows | 待定 | ❌ |

## 环境包（runtime-payload）

| 端 | payload | 状态 |
|---|---|---|
| Android | Termux bootstrap + proot（按需下载） | ❌ |
| iOS | iSH（按需下载，不进 IPA；延后） | ❌ |

## 构建与交付（CI）

| 交付物 | 状态 | 证据 / 口径 |
|---|---|---|
| macOS **dev** 包 | 🟡 | 命中路径过滤的 `main` push 或手动触发，构建及发布成功后出包；连续运行可被取消，不能保证 dev 与 main 最新提交一致：`.github/workflows/dev-build.yml` → ad-hoc 签名的 `.dmg`/`.zip` + SHA256SUMS + BUILD-INFO，挂到滚动 tag `dev` 的 prerelease。**仅 arm64**（`GhosttyKit.xcframework` 只有 arm64 切片）、**未公证**（首次打开是否被拦取决于取包工具贴不贴 `com.apple.quarantine`：浏览器与 `brew --cask` 都贴 → 要放行一次；`curl` + `ditto` 不贴标记 → 不触发 Gatekeeper 评估。实测口径与证据档（哪些量到、哪些是推论）见 [dev 包怎么装](../how-to/dev-build.md)）。build 号 = `GITHUB_RUN_NUMBER`，dev/release 两轨不同源单调。不能当自动更新通道（每轮 cdhash 不同）。 |
| macOS dev 包 **Homebrew cask** | 🟡 | tap 在独立公开仓库 [`hanxi/homebrew-ghosterm`](https://github.com/hanxi/homebrew-ghosterm)（`Casks/ghosterm-dev.rb`），指向 `dev` 滚动直链。**🟡 成立的证据档**：`brew tap` + `brew trust` + `brew install --cask ghosterm-dev` 在本机装成、`brew style`/`brew audit --cask --strict` 绿、`brew info --cask` 读出 `depends_on arch: :arm64` 与 `depends_on macos: :tahoe`（26.0 下限由内嵌 Homebrew `libcrypto` 抬上去）。**不解决 Gatekeeper**（Homebrew 自己贴 quarantine，见上一行），**`sha256 :no_check`＝故意不校验字节**（滚动资产钉不住，验字节请对 `SHA256SUMS`）；`:latest` cask 默认不进 `brew outdated`，升级要 `--greedy`。**未实测**：Intel 机器与 macOS < 26 的报错路径没有实机可验，只验到声明被正确读出。cask 的 URL 与本轮资产名由 `scripts/check-dev-cask.sh` 在 dev 轨对拍，牙见 `scripts/mutation-check-package-mac.sh` 的 A10 |
| macOS **release** 包 | 🟡 | tag `v*` / PR / 手动 → `.github/workflows/macos-release.yml`，与 dev 共用同一份可复用工作流 `macos-package.yml`（`channel=release、gates=full`）。**🟡 无 Developer ID 证书 secret ⇒ 目前同样落 ad-hoc**；`--sign` / `--notarize` 分支已就绪，拿到 secret 即用 |
| dev 版**更新检测**（app 内入口） | 🟡 | 关于窗口（`Ghosterm → 关于 Ghosterm`）版本号下面那一行：手动点「检查更新」→ 取固定 URL 的 `version.json`（不走 API、不占 60 次/小时额度）→ 以 `git_commit` 为主锚（本机短 sha 前缀比清单的 40 位）。**三态不折叠**：无 commit 记录 / `+dirty` / 渠道不对 / 取不到 / 读不懂，一律报「查不出来 + 原因」，不报「已是最新」。**自动安装 ❌（有意不做）**：`signature=adhoc` + `notarized=false` ⇒ 只给下载直链，不画「安装」按钮。已有缺字段/状态判据与文案由 `UpdateCheckTests`（12 项）钉，入口几何由 `AboutWindowTests` 钉；远端 commit 格式、摘要字符集与下载来源校验仍有缺口，“有新版”仅表示 commit 不同。牙见 `scripts/mutation-check-app-update.sh`（7 组变异）；口径见 [dev 包怎么装](../how-to/dev-build.md) |
| Android debug APK | 🟡 | `scripts/build-android.sh` 已生成 APK，并在 API 36 arm64 模拟器装载运行；无 Android workflow，无真机/16 KB 页/发行证据 |
| iOS 出包入口 | ❌ | 没有构建脚本、没有 workflow |
| 闸门分档 | ✅ | 轻档（dev）：`scripts/gate.sh`（core + ctest + ABI + api_smoke）+ 图标一致性 + `gate-ghostty-patch.sh`；full 档共接入 20 份变异，准确清单以 `AGENTS.md` 与 `.github/workflows/macos-package.yml` 为准。标签导航与窗格交互合并在 `tab-chrome`，不再依赖原生多窗口标签组的私有实现。dev/full 都另以本轮 ZIP 验一键安装脚本（签名/SHA/递归 quarantine/回滚），不触碰真实应用目录。接线不等于本轮 CI 通过；`pane-chrome / settings-ui / workspace` 等手工证据与真实系统激活、全屏另行记录。图标脚本缺依赖直接失败，workflow 显式拒绝打包/认证变异日志中的 `SKIP`，Apple 步骤先检查 Swift；尚无所有宿主输出的统一跳过审计，`apple-app-gate.sh` 的 SSH e2e 可因 sshd 失败而跳过后返回成功。不能把配置接线记为完整覆盖，收口见路线图 |

## 各端 UI

| 端 | UI | SSH | SFTP | 本地 shell | ssh_config GUI |
|---|---|---|---|---|---|
| macOS | 🟡 | 🟡 | 🟡 | ✅ | 🟡 |
| Android | 🟡 A1/A2 首片已验 | 🟡 native 后端已验；产品首片已有完整设备证据（§9.27） | 🟡 浏览/预览（§9.52）、粘贴/任务页面（§9.70，当前模拟器档已验） | 🟡 A2 首片模拟器证据 | 🟡 |
| iOS | ❌ | ❌ | ❌ | ❌ | ❌ |
| HarmonyOS / Windows / Linux / Web | ❌ | ❌ | ❌ | ❌ | ❌ |

Android UI 的 🟡 指主机列表/搜索/增改、配置编辑和设置/关于已在 API 36 arm64 模拟器验证；配置保存有备份、摘要复核与原子替换，按应用私有目录和协作锁约束工作，不承诺任意非协作写入者下的无竞态提交。A1 已展示继承来源与共享别名影响，并支持同值显式覆盖、删除指令恢复继承；别名重命名与配置错误恢复已接入，SAF 选择/预览/取消、失败恢复和其余表单校验已完成 A1 验收；本地终端首片已验，整个 A2 尚未完成，本机/SAF 文件浏览首片已验，远端文件页面已接（§9.52），完整设置与正式图标已由 `29d3f3d` 发行（§9.91），其余设备矩阵仍待完成。[最终 HTML](../design/android-prototype.html)继续作为页面基准，[A1 实施记录](android-implementation.md#92-a1-主机与配置首片2026-10-29)区分已验证与待办。

A1 阶段验收为 2026-10-30 的 47 项测试与 59 组配置/页面变异；A2 首片完整 63 项设备测试及 21 组终端变异为 §9.13 的历史证据。文本操作切片增加文本操作，最新完整设备结果见页首及 [§9.72](android-implementation.md#972-a6-fifo-文件任务队列2026-10-31)；该轮首次完整运行因模拟器消失中断，原因未知，最终重跑通过。§9.44 工作区曾有 1 项选区失败，偶发原因仍待定位；此前通道/选区早期失败保留在 §9.41，不累加历史测试数量。产品/身份首片原先的完整套件失败已修正测试观测与线程归属后复验，失败记录保留在 §9.27。A1 证据见[§9.11](android-implementation.md#911-saf-导入预览与-a1-收尾2026-10-30)。读取失败/保存冲突使旧快照失效；重载先确认草稿，错误跨页保留，成功刷新编辑目标，失败不报空成功态。§9.8 的错误恢复草稿已由 §9.10 补齐，SAF 与表单余项由 §9.11 完成；A1 只在当前设备档位收口，完整客户端仍在开发。此前工作区的继承编辑草稿已补齐接线，原 Kotlin 编译失败已解决；历史记录见[§9.6](android-implementation.md#96-文档复核时的工作区草稿2026-10-29)，当前实现与验证见[§9.7](android-implementation.md#97-显式覆盖与恢复继承2026-10-30)。

Android 修饰键、数字键盘与组合音符切片已验收，该切片完整结果为 **99 项设备测试，0 失败/错误/跳过**，文本变异选跑 24 组通过，见[§9.18](android-implementation.md#918-修饰键数字键盘与组合音符2026-10-30)。本片仍限 API 36 arm64 / 4 KB 页模拟器，不改变整个 A2 或下表 macOS 的完成状态。

静止画面的闪烁文字随后已修复，实际窗口像素验证及五组渲染变异通过；该切片完整设备结果为 **100 项，0 失败/错误/跳过**，见[§9.19](android-implementation.md#919-静止画面的闪烁文本2026-10-30)。后续短时取帧与 debug 性能基线见[§9.21](android-implementation.md#921-输入后短时取帧与性能基线2026-10-30)，完整 101 项设备测试通过；整个 A2 尚未完成。

SAF 私钥导入的原请求归属、旋转保留、只读读取和取消已验：五项定向、七组新增变异及完整 135 项设备测试通过，历史失败与修正见[§9.29](android-implementation.md#929-saf-私钥导入生命周期2026-10-30)。加密 OpenSSH ed25519 重试与成功密码流程已在 §9.31 验证；身份设置首片见 §9.33，主机身份关联见 §9.35，信任记录管理已由 §9.39 交付，网络矩阵仍待补，A3 保持 🟡；A1 配置文档导入的证据独立保留。

身份设置已接通列表/详情、OpenSSH 类型/指纹、导入与确认移除；损坏条目独立显示，过期删除拒绝，原文档与已建立连接保留。身份设置八项定向、十组新增变异已有记录；主机身份关联切片已接配置引用、认证候选与引用详情，完整 160 项设备回归通过（§9.35）。known_hosts 管理已由 §9.39 交付；§9.82 的 16 项偏好存储及 §9.83–§9.91 的页面、已有会话/文件消费者与恢复保护已经交付；完整设备矩阵仍待补，A7 五类设置与原有 16 项偏好已完成当时模拟器档；完整真机/发行矩阵仍按 A10 验证。早期失败保留在 §9.32，当前范围见[§9.33](android-implementation.md#933-ssh-身份设置与公钥信息2026-10-30)。

Android 主机验证管理的 🟡 证据限 API 36 arm64 / 4 KB 页模拟器：真实 SSH 指纹、移除后重验及旧会话 PID/IO，重复记录/只读规则、错误结果、旋转和离页写入已验。非协作外部写者、断电恢复、更多设备与完整设置未包含；不据此增加 macOS 管理页面完成状态。

**A5/A6 产品接线历史（§9.72）**：§9.70 已接执行器、粘贴确认与任务认证，§9.71 已接进程持有，§9.72 已接 FIFO 队列。队列 8 项定向、14 组新增与既有 39 组变异通过；该片完整设备 453 项通过。后续 §9.73 已验只读历史、§9.74 已验前台服务/通知、§9.77 已验当前文件阶段进度与新完整 491 项；不同远端关系的显式确认已由 §9.110 接入，暂停/安全重试由 §9.98 接入；不自动恢复执行，不标整个 A5/A6 完成；历史专项数量保留在各切片记录。

### Android 文件首片

本机/SAF 单目录创建为 🟡：§9.64 的 API 36 arm64 / 4 KB 页模拟器上，16 项目录基线和 26 组变异通过；包含空目录、嵌套、不透明 ID、实际名称、非覆盖、取消晚到及随后真实文件发布。远端原语后由 §9.65、统一服务由 §9.66 接入；§9.69 已接目录树执行数据层，§9.70 已接页面入口，§9.71 已接进程持有，§9.72 已接 FIFO 队列；恢复未接。

SAF 新文档写入为 🟡：§9.59 的 API 36 arm64 / 4 KB 页模拟器上，16 项设备用例及 30 组变异已验创建/写入/关闭/回读、未知副作用与取消租约。它是 data 层设施；复制来源清单由 §9.60、目录执行器由 §9.69、任务页面由 §9.70 接入，自动清理和进程恢复仍未实现。

本机 HOME 与持久授权 SAF 树的列表、只读 UTF-8 预览和多选为 🟡，仅在 §9.43 的 API 36 arm64 / 4 KB 页模拟器档成立。超过 4096 项报错，预览上限 256 KiB，未知流按实际读取限额；点按打开、长按进入勾选。远端 SFTP 后端为 🟡（§9.51 的 13 项/18 组及 52 项同档设备证据），页面为 🟡（§9.52 的 9 项产品用例、14 组产品变异及完整 236 项同档设备证据），复制来源清单已由 §9.60 接入，§9.71 已接进程共享粘贴任务，§9.72 已接队列并通过当前模拟器档回归；隐藏开关与排序偏好已由 §9.91 持久化并接入文件列表。

### macOS 当前实现与限制

| 任务能力 | macOS 当前生产实现 | Android 当前边界 |
|---|---|---|
| 进程内任务 | 全局内存队列；Core FIFO 按 SFTP 会话串行，非跨连接全局限并发 | §9.72 已验进程共享 FIFO，最多 32 个未结束请求、一个执行名额 |
| 重启后历史 | 未实现，无持久任务加载 | §9.73 只读记录已验；18＋4 项定向、新增 27 组记录/中断/发现变异、九种 SIGKILL 及受影响旧变异通过；完整 475 项通过，仅 API 36 arm64 / 4 KB 页模拟器；不自动重放历史 |
| 重试/恢复 | 同进程重试需原 backend 可取得；目录重扫，非失败项重试或断点续传 | §9.98 已有进程内安全失败项重试，§9.105 已有确认后的历史重新发起，§9.121 已有结果不明来源显式重选；均创建新请求并重新预检/认证，不恢复旧冲突授权或自动续传 |
| 取消完成 | 立即显示取消；该行晚到事件被丢弃，实际副作用与释放仍有缺口 | 等资源释放并提交最终记录；记录失败单独展示并阻断队列 |

桌面源码依据见 [任务历史与重启边界](../../apple/README.md#任务历史与重启边界)；该表不扩大各切片已运行的验收范围。

新增 shell 集成与 App 内建功能分别记账：普通本地 zsh 已自动加载随包鼠标恢复钩子，Bash/远端仍按 `7939ef8` 的显式安装器集成。不在唤醒时重置前台程序，不增加远端 cwd、OSC 133 或跨进程恢复能力。

| 面 | 已实现与证据 | 未完成或证据边界 |
|---|---|---|
| SSH 与认证 | `AppModel` / `SSHAuthenticationFlow` 共用可取消认证；真实 sshd→shell→网格→resize；hostkey 未知/拒绝/变化/撤销分开。撤销及不支持规则经 SFTP 产品入口验证，不弹信任/凭据框 | 未支持配置已在解析地址前阻断，仍未执行跳板/转发等高级选项；私钥材料生产路径仍从受限文件读取，见路线图 P0。哈希主机名与主机证书未实现；本片验了 SSH/SFTP/tmux 配置拒绝入口，未新增终端网格或 tmux 成功会话证据 |
| 本地 shell | 普通 PTY、登录语义、cwd、控制终端、背压、关闭与描述符隔离；`4924a2d` 清空子进程继承的信号屏蔽，补齐 ssh/tmux 的尺寸通知 | 普通会话不承诺跨 App 退出保活；信号/真实窗口证据见上方 POSIX PTY 行，不扩大为全部平台 |
| 提示符鼠标残留清理 🟡 | 普通本地 zsh 自动加载随包钩子；Bash/远端有带备份的独立安装器。macOS 26.6.2 / tmux 3.6a 的三层隔离 SSH 断线回到本地提示符、真实 AppKit 按下/松开、原 PID/键盘与正常 TUI 字节对照通过；五项启动组合及四组自动接线变异另有记录 | `.direct` / 自定义启动参数（仅 `-i` 除外）/ 其它 shell 不走自动接线；远端需远端安装，前台程序未退出时不触发。实际硬件休眠/用户远端未验，Codex 输入框未复现，不称其缺陷已修复；见[操作与证据](../how-to/mouse-report-recovery.md) |
| 标签导航 | `TabWindowController` 单窗口；顶部/侧边切换、UUID 重排与关闭、唯一加号、搜索/菜单、Ctrl+Tab / Ctrl+Shift+Tab 循环切换；无精简侧栏、无主机分组 | 跨重启标签持久化未做（偏好已持久化）；本机证据不能代替 macOS 15 CI |
| 工作区 | `WorkspaceLayout` / `WorkspaceHostView` 为统一终端/文件布局；四边停靠、中央交换、隐藏、缩放与焦点 | 真拖动图像、逐帧零尺寸、跨标签最小尺寸等见工作区交接 |
| 窗格外观 | 跟随终端配色；当前分屏顶部蓝线；gap>0 细框，gap=0 共享分隔线；悬停/菜单分屏预览与 Shift | 离屏像素、可控窗口焦点、真实桌面激活/全屏是不同档证据 |
| 文件浏览 | `FilesPanelView` 与本地/SFTP 后端；列表/树、搜索、操作、工具条折行、原生单击选择/双击打开与键盘归属 | 远端 cwd 未交付，不能声称与 SSH 当前目录同步 |
| 目录导航与缓存 | 普通目录列表、前进/后退、刷新与树懒加载已有接线 | 导航无代次、早完成请求提前清除加载状态；失败不回滚已改变的历史；刷新不失效树缓存。尚无本轮行为验收，见 [Apple 导航边界](../../apple/README.md#目录导航与缓存边界) |
| 文件树、本地搜索与递归权限 | 页面/正常操作链路已接入 | 树加载错误被缓存成空节点；本地搜索忽略 walk 错误仍标完成，取消未停遍历；递归权限忽略 walk 自身失败，本地 lstat 失败条目被跳过。源码核对结论，异常路径仍待验收，见 [Apple 说明](../../apple/README.md#文件查询与线程边界) |
| 递归权限的链接处理 | 普通条目 chmod 与递归入口已接 | 未过滤 walk 返回的链接；本地 lstat 后仍用 chmod，不能保证目录外链接目标权限不变。复制跳过链接不等于权限操作跳过，见 [Apple 说明](../../apple/README.md#递归权限与链接边界) |
| 远端搜索与取消 | 已接 exec 优先/walk 回退及页面档位显示 | 任意 exec 打开失败均回退；stderr/退出状态与条目查询失败被忽略，可能显示不完整结果为完成；取消只撤下旧 UI 结果，尚未停止远端工作 |
| 文件查询与响应性 | SFTP 后端串行保序，目录展开在后台执行 | 存在性查询折叠读取错误；传输入队/控制仍在 MainActor 同步等后端队列，可能被长遍历阻塞；见 [Apple 说明](../../apple/README.md#文件查询与线程边界) |
| 独立文件标签 | SFTP 标签在同一工作区中创建本地+远端窗格；支持纯本地文件标签 | `SFTPBrowser`、`DualPaneFilesView`、`DualPaneSession` 均已退役，不能作为当前组件名 |
| 文件传输 | `CoreEventPump` 唯一事件泵；`TransferManager` 父子任务、进度/速度/ETA、暂停继续取消重试的正常路由；本地↔远端 | 控制异常被忽略仍改变状态，未知 job 结果折为失败；本地↔本地、远端↔远端统一队列未做；目录冲突 UI 等完整流程仍需补证据 |
| 目录暂停与汇总 | 已有父行暂停与子文件控制路由、目录完成计数 | 扫描阶段页面没有暂停入口，模型暂停也不停扫描/创建，新子项先提交再暂停；清除终态子行会影响后续父任务字节汇总；活动数同时计父子行。仅源码核对，未修复或新增行为验收，见 [Apple 说明](../../apple/README.md#目录暂停与汇总边界) |
| 目录结果汇总 | 普通目录完成计数与失败优先分支已接 | 没有失败时取消/跳过由后到结果覆盖，父摘要依赖完成顺序；没有部分成功状态，多条失败仅保留最后文案。源码核对、未修复，见 [Apple 说明](../../apple/README.md#目录终态与回调顺序) |
| 冲突范围与目录重试 | 已有覆盖/跳过/保留两份对话框和重试入口 | “应用到全部”为全局值，完成/清空后不重置，后续批次可能继续使用；目录重试重新扫描整目录，不仅失败项。源码核对，批次隔离及部分成功重试仍待验收，见路线图 P1 |
| 系统集成 | 栏间复制、Finder 拖入远端、本地代表条目拖出 Finder、Quick Look（远端临时下载） | 多选拖出 Finder 仍不完整；远端拖出 Finder 的文件承诺未做；iOS 无宿主 |
| 远端预览生命周期 | 临时下载成功后打开 Quick Look 已接；现有测试覆盖路径形状 | 没有请求代次、大小预算或应用层副本回收；关闭窗格后回调仍可能打开预览。真实 QLPreviewPanel 与缓存生命周期须另验，见 [Apple 说明](../../apple/README.md#quick-look-预览边界) |
| 配置 GUI | Host 编辑、通配规则、读取失败提示与 Core 稀疏 API；共享重复指令误删已修复；`ConfigStoreTests` 覆盖正常备份和冲突 | 表单简化扫描并提交全部字段，多条 IdentityFile 只回显最后一行；内存先改、写盘失败未恢复；`ConfigStore.save` 吞备份失败、先删后移；可连接列表未过滤否定模式、重复 Host 块产生同 ID，均见路线图待办 |
| 配置来源与别名改名 | Swift 已有来源查询与改名绑定；共享契约与平台绑定分别验收 | macOS `HostEditorSheet` 未消费来源，也没有已有主机改名字段；Android 容器引用不是本端可读取的密钥路径，不因共享 API 或 Android 页面通过而标本端 GUI 完成 |
| 终端设置与输入 | 正常配置下配色/字号对已有 surface 生效，应用外观即时更新；macOS IME、辅助状态与焦点 | `SettingsStore` 读取错误折为空、外部新建文件不报冲突、重复键编辑与回读不一致；引擎拒绝后的磁盘/恢复状态待修。full 引擎配置机制不能自动套用于 Android vt 自绘 |
| 终端系统剪贴板 | `beffb49` 接通标准 `copy:` / `paste:`、真实选区与 NSPasteboard；专用测试覆盖显式 firstResponder 菜单快捷键、中文/多行 bracketed paste | 全局主菜单自动路由未由该测试覆盖；粘贴确认、OSC 52 与选中自动复制尚未接通。shim 读取不可用，写入/确认为空；本次文档复核未重跑专用测试/四组变异 |

**宿主闸门失败传播**：`apple-app-gate.sh` 未启用 `set -e`，ABI 子检查也未显式检查返回值；后续成功可能掩盖该失败。它与 SSH e2e 可跳过的问题分别待修；单独 ABI 检查、实际测试结束状态及覆盖范围须分别核验，不以最后一行 PASS 代替。源码依据见 [Apple 构建说明](../../apple/README.md#构建与验证)，本次未新增故障注入或 CI 实测。

**CI 触发范围**：dev 的 main push 有忽略路径，release 的 PR 有路径白名单；仅 `docs/` 或 `android/` 的改动不触发两者，`terminal/android/` 则不同。Markdown 在 dev push 被忽略，但 `apple/README.md` 等白名单内文档的 PR 仍会触发 release。dev 旧运行还可能因后续 push 被取消。不能把“有出包工作流”表述为“每个提交都有发布包”；准确范围见 [CI 说明](../how-to/dev-build.md#ci-是怎么串起来的)，这些过滤不改变合并前应执行的闸门。

**三种保存路径分别记账**：known_hosts 的 TOFU/移除已使用共享锁、备份、原子替换和提交结果；SSH `ConfigStore` 仍先删后移且忽略备份失败；终端 `SettingsStore` 使用备份与 `replaceItemAt`，仍有重复键、读取错误、冲突和引擎恢复缺口。共享信任事务不等于 macOS 配置 GUI、设置或 SFTP 传输已经修复，见 [Apple 保存边界](../../apple/README.md#三种配置文件的保存边界)。

**SSH 地址选择**：macOS 的异步 DNS 仅在所选族缺记录时换族，首个有效地址即完成解析；Android 已按 §9.40 传递全部候选，在同一 native owner 内顺序尝试，2 秒 TCP 宽限且共用总预算；协商开始后不换地址。12 项定向与 14 组变异证明设备回环双栈 SSH、丢包与取消，不扩大为公网或真实 DNS 矩阵。实现边界与后续条件见 [Apple 网络说明](../../apple/README.md#ssh-地址选择与取消边界)和路线图 P1；macOS 网络路径没有本轮新增实测。

**安全存储缺口**：当前 `SSHCredentialSelector.Storage.readKey` 从文件读取 PEM（限制 owner、权限、常规文件、大小且不跟随链接），Keychain 生产接线存口令/密码。`KeychainStore` 虽有 PEM helper，不能据此声称生产流程已做到私钥导入后只读安全存储。该缺口按 `AGENTS.md` 铁律 #5 继续修复，不修改规则来迁就代码。

**设置页范围**：macOS 为通用、外观、键盘、配置文件四页；配置文件页展示 SSH/known_hosts 路径，文本编辑对象是 Ghostty 配置。保存走 `replaceTerminalConfig` 并重载引擎，外观字号/配置页的“只有新标签读取”、标题栏按钮只在当前格显示、通用页恢复/关闭会话的提示均已落后于代码，列为 UI 文案待修；Android 的五类/16 项设置是单独设计，不代表 macOS 当前分类。

**引擎配置不等于宿主能力**：`terminalSettingKeys` 中的 `copy-on-select` / `confirm-close-surface` 是元数据，当前设置页面不据此生成全部控件。full 引擎的剪贴板回调尚未接通（Cocoa 标准复制/粘贴已接），宿主关闭判据也不读取后一个键；引擎能解析配置不能作为产品功能证据，见 [Apple 接线边界](../../apple/README.md#引擎配置与宿主动作边界)。

**复制原语与产品接线**：旧 `gt_sftp_upload/download` 为路径式 job，没有独占创建/非覆盖提交参数；新 `gt_sftp_create_controlled` 与 Swift/JNI/Kotlin 已提供从 fd 分块写入任务暂存路径的独占创建和结果状态（§9.54），§9.57 已补远端发布及绑定。Android §9.69/§9.70 已将这些接口接入复制执行器与任务页面，§9.71/§9.72 已接进程持有与 FIFO 队列；不同远端关系保持未知并由 §9.110 提供请求内显式确认；§9.73 已验只读记录，§9.98/§9.105/§9.121 已接安全重试及显式重新发起，不自动恢复执行。macOS 队列尚未迁移：旧 job 上传虽已按 §9.53 使用 WRITE|CREAT|TRUNC，不残留旧尾，失败仍可能留下部分内容，不保证原目标保留。“先检查冲突再传”不能保证并发时不覆盖，桌面迁移与覆盖/冲突回归见[路线图](roadmap.md)。

**传输完成与调度边界**：共享下载打开顺序、覆盖截断和两侧关闭检查已按 §9.53 修复；§9.54 已有独占暂存创建，§9.57 增加远端非覆盖发布（不承诺通用原子性），旧 job 后续失败仍可能留下部分文件。macOS 旧 job 的每 SFTP 会话 FIFO 不等于全局并发上限；Android §9.72 已在进程内统一限制一个执行名额，扫描、确认与取消清理也占用该名额。macOS 重试仅复用弱引用仍可取得的原 backend，不自动重连或续传。`gt_sftp_walk` 的负深度模式没有实际硬深度护栏，macOS 无限递归调用方仍受此缺口影响。macOS 全局调度、重试和共享 walk 护栏仍是源码核对的未完成项；传输结果修复的实际证据见 §9.53，调用链与后续条件见 [Apple 传输边界](../../apple/README.md#传输提交调度与重试边界)及路线图。

**目录结果边界**：macOS `TransferManager.expandDirectory` 排除目录内符号链接，上传不递归链接、下载不给链接建子任务；当前没有排除明细/计数，空文件集合会完成父任务。既有目录下载测试验证普通文件入队且链接排除，不证明目录逐项完整复制。用户可见的跳过原因和汇总仍待实现，不能把普通文件成功扩大为整棵目录镜像成功。文件手势、剪贴板、正常退出与设置的两端差异见 [Apple 宿主说明](../../apple/README.md#与-android-最终方案的差异)。

macOS 缺口的修复顺序与失败场景见[路线图](roadmap.md#macos-修复顺序与验收触发场景)。本轮只更新事实说明，未修复配置事务、表单或传输状态机。

**凭据验证边界**：共享内存私钥错误分类已修复，Android 四项凭据定向、七组变异及完整 139 项设备回归通过；共享 Core 真 sshd 新回归/一组变异和既有连接器 18 组通过。macOS 完成原生重新链接及 11 组既有认证回归，不冒充新增 Apple 加密密钥产品用例。密钥证据限 OpenSSH ed25519，原失败见 §9.30，当前见[§9.31](android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。

**证据入口**：最新完整宿主为 `4cbfcf4` 的 App 349 项：347 通过、1 项开发态主题资源跳过、1 项键盘设置页像素失败；恢复旧终端源码也复现，该历史轮未记全绿。键盘设置页布局后由字体切片修正并通过关联定向（56 通过/1 开发态资源跳过），未重跑完整宿主。该提交已修复跨屏 DPI/Metal 比例/像素尺寸同步，三项原生回归、六组手工变异、Terminal 28 项及 release 构建通过，真实双屏视觉复验仍待完成，见[跨屏记录](optional-tmux-implementation.md#后续跨显示器缩放修复2026-10-31)。[标签/窗格计划](plan-tab-chrome-refactor.md)的 341 项/1 跳过保留为历史通过记录；真实桌面激活/全屏另列，不扩大到未运行系统。[工作区交接](workspace-handoff.md)保留已验范围与缺口，`workspace` 21 组变异仍属手工证据。

**发行边界**：源码 SwiftPM 声明 macOS 13，但目前发行包为 arm64，每轮嵌入依赖的 minos 决定实际系统下限；2026-11-04 读取的线上清单为 macOS 15.0（Mac `3345893`），Homebrew cask 的下限单独核对；不能据源码声明推断包可在 13 上安装。自包含/ad-hoc 已有包证据，Developer ID/公证仍待实际证书与包验收，见[构建与安装](../how-to/dev-build.md)。
