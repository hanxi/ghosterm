# 能力矩阵

**当前实现（2026-10-31，§9.70 当前模拟器档已验）**：Android 已接可见复制清单、粘贴预检/冲突确认、独立任务认证和逐项结果页面。任务由 Activity ViewModel 持有，跨页/旋转保留，取消后等待实际资源释放；进程级调度、后台通知、暂停/重试、进程恢复与完整偏好仍待完成。不同远端命名空间关系未知时阻断，不能据同服务器测试宣称跨主机复制已完成。 详细设计与验证见[Android 实现文档](android-implementation.md#970-a5-粘贴确认任务认证与结果页面2026-10-31)。

**验收边界**：§9.70 的 18 项产品定向、24 组新增变异及既有 97 组回归通过；最终完整设备 **437 项通过，零失败/错误/跳过**（1105.73 秒），仅 API 36 arm64 / 4 KB 页模拟器。714 份非 Markdown 源码摘要未变，安装主/测试 APK 与构建一致。Core 47 CTest / 89 ABI、Apple 87/2、JNI 49/40 和两端原生构建通过；未新增真机、API 29、16 KB 页、发行或 CI 证据。

2026-10-28 已完成 [可选 tmux 重构](plan-optional-tmux.md)。默认 PTY/SSH 已恢复，
旧 sessiond、私有恢复与随包 runtime 已退役。可选 tmux 为 🟡：tmux 3.2a 已验证 macOS 本机与 Ubuntu 22.04 SSH 的真实命令和 PTY；3.6a 另验证原生列表、任务 PID 保留、断网与受限账户，最终打包证据见实施清单。仅支持 POSIX 兼容远端登录 shell；不自动安装、恢复或管理旧 daemon。
进度见 [实施清单](optional-tmux-implementation.md)。

> 口径（铁律 #8）：**✅** 已实现且有闸门/实测证据 · **🟡** 已实现但只在某一档证据下成立
> （必须写明哪档）· **⏳** 契约/占位在，无实现 · **❌** 未开始 · **⛔ WONTFIX** 不做并注明原因。
>
> 核对日期：**2026-10-31**。已验通道切片 C ABI 81 项、Apple 79 项引用/2 项登记未用；已验受控 SFTP 打开切片为 82 项、Apple 80/2、JNI 40/42（§9.47）；受控目录浏览为 83 项、Apple 81/2、JNI 40/43，该切片时 Core/Swift 已验、产品待接（§9.49）；当前 §9.65 为 89 ABI、Apple 87/2、JNI 49/40；Android 页面已由 §9.52 接入，独占暂存创建见 §9.54。历史 A1 JNI 11 项引用/62 项待办；A2 首片对拍为 19/54，A3 配置检查切片为 20/54，已提交 SSH 后端为 32/42；身份容器切片对拍为 33/41，身份来源查询切片为 34/41，主机信任记录快照切片为 38/41，事务/移除绑定为 39/41，通道取消切片改为 39/42（新入口替换旧引用），符号检查不代替产品验收。产品首片有 27 项定向测试及 15 组变异证据，通道取消切片的完整 200 项回归通过（历史结果），见 [§9.41](android-implementation.md#941-ssh-通道打开预算与取消2026-10-30)，整个阶段尚未完成。别名重命名共享接口与 A1 Android 表单接线已独立验收。字段来源与 Android 页面接线见 [Android 实施计划 §9.5](android-implementation.md#95-字段来源与编辑归属2026-10-29)，各次构建、设备测试与变异见[路线图验收记录](roadmap.md#验收记账与历史记录)。历史平台/像素/网络证据各自保留范围，未重跑的专项不算本轮结果。

**文件首片边界**：A4 本机/SAF 只读浏览、UTF-8 预览与多选已通过 14 项定向、18 组变异和完整 214 项设备回归。此前草稿编译失败已修复；远端 SFTP 页面随后由 §9.52 接入，§9.60 已接复制来源清单，§9.70 已接 Activity 粘贴任务，进程级队列未接，见 [§9.43](android-implementation.md#943-a4-本机与-saf-文件浏览首片2026-10-30)。

**当前验收边界**：Android 最近完整通过证据见页首及 §9.70；§9.56 首轮旋转失败及同步修正记录保留，限 API 36 arm64 / 4 KB 页模拟器；选区偶发失败保留追踪。§9.47 已完成共享受控 SFTP/主机补丁依赖：17 项协议、十组接口变异、十一组依赖守卫及既有打包变异通过。Android 远端只读文件页面已消费受控入口，macOS 文件产品尚未迁移；整个 A3/A4 与 macOS 产品通道取消仍未完成，见[主机依赖证据](host-ssh2-build.md)和[当前进度](roadmap.md#当前进度总览)。

**页面交付记录**：§9.52 接通 Android 远端文件页面，9 项新增产品设备用例、14 组产品与 18＋15 组既有变异、最终完整 236 项设备回归通过；42 CTest / 85 ABI、Apple 83/2、JNI 45/40 与两端构建通过。macOS 产品未迁移，见 [§9.52](android-implementation.md#952-android-远端文件页面接线2026-10-30)。

**历史切片：A5 共享前置（§9.53，2026-10-31）**：传输打开顺序、覆盖截断、关闭结果与排队取消已修复；43 CTest / 85 ABI、13＋8 组变异、两端构建、Swift 1 项与 Android 22 项定向通过。该片当时非覆盖提交与复制产品未完成；发布接口与 Android 私有目录提交后由 §9.57/§9.58 补齐，§9.60 已接复制清单页面，§9.70 后接粘贴执行。该片没有重跑完整设备/Apple 宿主/打包/CI。见 [§9.53](android-implementation.md#953-a5-共享传输结果前置2026-10-31)。

**阶段记录：A5 远端暂存创建（§9.54，2026-10-31）**：已接独占创建、源 fd 校验和分阶段结果，以及 Swift/JNI/Kotlin 绑定。44 CTest / 86 ABI、Apple 84/2、JNI 46/40；22 组 Core/无库/Swift、九组 Android 新变异及既有 owner 18 组通过，两端原生构建通过。该片当时最终目标发布、复制清单和任务队列仍待实现；macOS 文件产品尚未消费新接口。 设备范围与失败记录见 [§9.54](android-implementation.md#954-a5-远端独占暂存创建2026-10-31)。

## 核心（C）

目录创建须区分两种能力：旧同步 `gt_sftp_mkdir` 已供 macOS 使用；§9.65 已新增带单次总预算、取消及副作用结果的 `gt_sftp_mkdir_controlled`，Swift/JNI/Kotlin 已绑定并专项验证。Android §9.64 实现 HOME/SAF 单目录原语，§9.65 补远端独立请求；统一目录服务已由 §9.66 接入；任务独立连接与页面仍待接入。macOS 当前会复用既有目标目录，不具备整树非覆盖提交或独立目录合并确认，见 [实际调用链](../../apple/README.md#目录创建与合并边界)。

共享写侧已按[§9.37](android-implementation.md#937-主机信任写入事务与移除绑定2026-10-30)接通 TOFU/移除共同事务与两端删除绑定；此前 `526d24d` 的 79 ABI 是只读切片历史结果。Android 管理页面、repository 与指纹已通过 11 项定向、17 组变异及完整 175 项设备回归（[§9.39](android-implementation.md#939-主机验证记录管理页面2026-10-30)）；A3 网络矩阵与 A7 的 16 项偏好仍待完成。macOS 仅有共享绑定与认证消费，尚无记录管理页面。

| 能力 | 状态 | 证据 |
|---|---|---|
| C ABI（`ghosterm.h` v0，未冻结） | 🟡 | §9.57 为 88 符号，Apple 86/2、JNI 48/40；含受控读取到 fd 和非覆盖发布。Android 只读文件页面与暂存层已消费，macOS 产品仍待迁移；§9.57 已补发布入口及两端绑定，原草稿对拍失败保留为历史。接口未冻结，消费者须同步重编译 |
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
| 普通 SSH shell 打开取消与总预算 | 🟡 | §9.41 新增 controlled API，Android native owner 已接原连接余额和取消；三个阶段取消/预算、明确拒绝/断线及请求隔离共 13 项定向、13 组变异通过。Swift 新绑定验真实 sshd 和三组变异，macOS 产品仍使用旧 opener，尚未接入其原认证 deadline/取消，见 [两端边界](../../apple/README.md#ssh-认证后的通道打开边界)及 [实施契约](android-implementation.md#认证后打开-shell-的实施契约) |
| Transport：前台作业判定（关闭确认用） | ✅ | `gt_transport_child_state` **四态**（UNKNOWN/EXITED/IDLE/JOB）：POSIX 读内核 `tcgetpgrp(master) == shell 自己的进程组` 判「空闲提示符 vs 有作业」，SSH / ConPTY 报 `UNKNOWN`（**不猜**，铁律 #6）。`test_transport` 第 11 节（IDLE/JOB/EXITED/NULL→UNKNOWN）+ 变异 `mutation-check-transport` P1/P2 有牙。**前置修复**：本地 PTY 子进程曾拿不到控制终端（父进程抢跑 `setpgid` → 子进程 `setsid` 报 EPERM），已修 |
| Transport：会话 cwd | 🟡 | **本地 PTY ✅**：`gt_transport_child_cwd` 内核查询（macOS `proc_pidinfo` / Linux `/proc/<pid>/cwd`），`test_transport` 第 12 节（继承 cwd + `cd` 后随之变）。**远端 cwd 未交付**：OSC 7 在我们钉的 embedded host-IO 下到不了宿主（无 `GHOSTTY_ACTION_PWD`），且 libghostty 要求 host `isLocal`——接线留为 seam，依赖上游支持 / roadmap R2 |
| SSH（libssh2） | 🟡 | 普通条目的连接/认证已有以下证据，标记拒绝及未支持格式的边界见 known_hosts 行。connect / pubkey·password 认证 / 主机密钥三态 / TOFU；`ssh_e2e` 行为断言（真 sshd，环回离线）。**写绝不无限等**（§5b：远端不读 stdin 时 `gt__ssh_shell_write` 在 `GT_SSH_WRITE_GRACE_MS` 内有界返回、积压字节走 `wtail` 队列随后补送——与本地 PTY §13 同结构。`TIMEOUT`/`EAGAIN` 不再当硬错丢字节（旧实现把两者都算 `GT_ERR_IO` → 键入的字节就这么没了）。写路径自己设/还 session timeout，不打扰同 session 的 SFTP。e2e 限制：sshd 排空速率 ≈ 40 MiB/s，3 MiB payload 写完时队列**根本没形成**——flush 机制无法被真 sshd e2e 触发，由代码审阅 + PTY §13/P4 的变异覆盖保证同构正确性） |
| SFTP | 🟡 | `gt_sftp_*`（libssh2_sftp 封装）：list / stat / lstat / readlink / realpath / mkdir / rmdir / remove / remove_all（递归）/ rename / chmod / walk（递归枚举，visitor 可取消）/ upload / download（job + 事件三态）/ cancel / pause；**每会话一把 I/O 锁 + 单条 I/O worker 线程**（修掉「每 job 一线程共用同一 libssh2 会话」竞态）。e2e 真 sshd 75 项断言 + 当前变异 `mutation-check-sftp` 八组 / `mutation-check-exec` 三组（新增超时恢复）（历史断言数量不当作当前变异组数）；覆盖截断与关闭检查已由 §9.53 补齐；旧下载的完整长度/源变化校验、非覆盖提交和过载下终态保障仍有缺口，见 [Apple 下载边界](../../apple/README.md#旧下载的完整性边界) |
| SFTP 受控读取到暂存 fd | 🟡 | §9.56：Core/Swift/JNI/Kotlin 与 Android 匿名暂存接通；21 Core＋1 无库＋4 Swift 及 13 组 Android 变异通过；零上限空源、长度/属性/关闭和 fd 释放已验。远端发布原语后续由 §9.57 补齐，§9.70 已接 Android 任务页面，macOS 旧下载路径未迁移 |
| Android 请求内冲突决策/执行清单 | 🟡 | §9.63：15 项定向、42 组变异与完整 342 项，仅 API 36 arm64 / 4 KB 页模拟器。决定隔离、目录/不支持项跳过、只读清单、名称预算及真实写入集成已验；§9.70 已有任务/冲突页面消费者，不预留名称或绕过关系未知 |
| Android 目标预检/冲突元数据 | 🟡 | §9.62：13 项定向、34 组变异与最终完整 327 项，仅 API 36 arm64 / 4 KB 页模拟器。真实 HOME/SAF/SSH 目标读取、创建能力三态、已知目录关系与只读冲突已验；不同远端身份关系未知；冲突决策数据层后由 §9.63 补齐，目录创建由 §9.64–§9.66 接入，任务与 UI 由 §9.70 接入 |
| Android 粘贴请求/来源树扫描 | 🟡 | §9.61：API 36 arm64 / 4 KB 页模拟器 13 项定向、34 组变异；真实本机/SAF/SSH 元数据枚举，空目录、未知大小、来源复核、预算与取消已验。不等于内容快照；目标预检后由 §9.62 接入；目录创建与任务页面后由 §9.64–§9.70 接入；跨远端命名空间关系待补 |
| Android 文件来源清单 | 🟡 | §9.60：API 36 arm64 / 4 KB 页模拟器 9 项定向、16 组变异；真实本机/SAF/SSH 来源、跨页/旋转、替换/清空已验。不启动传输，来源扫描已由 §9.61 补齐，目标预检后由 §9.62 接入，粘贴页面由 §9.70 接入，跨远端命名空间关系尚未实现 |
| Android SAF 新文档写入 | 🟡 | §9.59：API 36 arm64 / 4 KB 页模拟器 16 项设备、30 组变异；创建、关闭后回读与取消租约已验。只消费 ContentResolver，不提供通用原子提交；来源扫描已由 §9.61 补齐；目录创建和任务页面后由 §9.64/§9.70 接入，部分文档恢复未接 |
| Android 应用私有目录文件发布 | 🟡 | §9.58：API 36 arm64 / 4 KB 页模拟器十项设备、24 组新增及 23 组暂存变异；私有命名候选与 NOREPLACE，不依赖被 App SELinux 拒绝的硬链接。SAF 新文档另由 §9.59 补齐；来源扫描已由 §9.61 补齐；本机/SAF 单目录创建由 §9.64 接入，远端目录后由 §9.65/§9.66 补齐；任务容量与进程恢复尚未接入；不改变匿名来源快照或 macOS 文件队列 |
| SFTP 最终目标非覆盖发布 | 🟡 | §9.57：Core/Swift/JNI/Kotlin 已验；真实 OpenSSH 文件/目录/链接冲突、模拟协议故障及 API 36 arm64 设备证据。查询成功才记冲突，只有明确不存在才尝试 flags=0 的 rename；成功/拒绝/未知分别保留。Android 已由 §9.69/§9.70 消费，macOS 复制产品未消费，不承诺通用原子提交 |
| SFTP 独占暂存创建 | 🟡 | §9.54：Core/Swift/JNI/Kotlin 已接 EXCL、0600、普通 fd 与分阶段结果，22 组 Core/无库/Swift、九组 Android 变异通过；只创建任务暂存路径，不发布最终目标，macOS 产品未消费 |
| 本机/SAF 匿名来源快照 | 🟡 | Android §9.55：容量/数量预算、来源片段、只读租约及真实 SAF/SFTP 集成；23 组变异与完整 252 项设备回归，仅 API 36 arm64 / 4 KB 页模拟器；§9.70 已接 Activity 任务页面；取消上传跨层租约保持由 §9.56 接入，macOS 无此暂存实现 |
| SFTP 打开预算与取消 | 🟡 | §9.47：Core/Swift/helper、默认补丁库 17 项协议、十组接口变异与十一组依赖守卫通过。macOS 产品仍用旧 opener，Android owner/页面已按 §9.51/§9.52 消费；后续操作不受打开预算保护，[详细范围](host-ssh2-build.md) |
| SFTP 单目录创建预算与取消 | 🟡 | §9.65：Core/Swift/JNI/Kotlin 已接，真实与协议矩阵 81 组、26 Core＋1 无库＋5 Swift、Android 12 项定向/20 组变异通过；专用连接、一次预算、六种创建状态及实际释放。统一目录服务后由 §9.66 接入；§9.70 已接 Android 任务/页面，macOS 产品迁移未接；设备仅 API 36 arm64 / 4 KB 页 |
| SFTP 目录浏览预算与取消 | 🟡 | §9.49：Core/Swift 将规范路径、完整列表与成功关闭目录合并；36 项协议、19 组变异通过。专用连接，不能与传输 job 共用；Android owner/页面已由 §9.51/§9.52 接入，macOS 产品尚未接入 |
| SFTP 只读预览预算与取消 | 🟡 | §9.50：Core/Swift 单预算覆盖 LSTAT/OPEN/FSTAT/READ/CLOSE，容量与实际 EOF 校验，46 项协议及 20 组变异通过。Android JNI/严格文本解码与专用 owner 关闭已按 §9.51 接入，页面与代次已由 §9.52 接入，缓存/外部打开待补；macOS Quick Look 未消费 |
| SSH exec 通道 | ✅ | `gt_ssh_exec*`：远端单命令（stdout/stderr 分离 + 退出码）；把递归搜索/大树遍历放到远端本地算（减少宿主发起的目录/属性请求）。服务器禁 exec → `GT_ERR_UNSUPPORTED`（不静默降级）。e2e 含**性能对比**（300 文件：exec-find vs sftp-walk，两条路都找全；**环回上 exec 反而慢 ~14×**——起远端进程的开销，真实 RTT 链路收益仍须实测，不能由环回结果推定）。当前 `mutation-check-exec.sh` 三组变异（含恢复共享会话超时）；不与历史断言数混计 |
| 端口转发 -L/-R/-D | ❌ | — |
| known_hosts / TOFU | 🟡 | 明文条目/模式、TOFU、撤销优先及读取错误已有真 ed25519/ECDSA sshd 的同步/公开异步 API 证据，25 组隔离变异通过（K17/K20 为独立文件故障，其余经真实 SSH）；TOFU 与移除已共用锁、备份、原子替换与提交状态；17 C + 2 Swift 事务变异范围见 §9.37。非协作写者竞态/断电恢复不在已验范围；Android 管理页面已另按 §9.39 验收，macOS 无管理页面。已解析的 hashed 条目、当前主机的未知标记、无匹配普通记录的 CA 规则显式报不支持；未实现哈希匹配/主机证书验证，不称完整 OpenSSH 兼容。macOS SFTP 产品认证回归与 Swift 绑定已验，Android native 状态/拒绝路径已在 §9.25 验证，页面首片及未完成边界见 §9.27；见 [§9.22](android-implementation.md#922-a3-共享主机信任前置2026-10-30) |
| 本地 PTY | 🟡 | POSIX（macOS/Linux）forkpty ✅；Windows ConPTY ❌（只有规划，能力缺失显式报错） |
| 可取消的 SSH 认证 | 🟡 | 通用公开 `ssh_auth.h` 与 Apple `SSHAuthenticator`/DNS 解析已从旧安装链拆出；C 连接器与公开翻译测试、8 组认证 ABI / 6 组凭据选择变异通过。Apple 普通 SSH、SFTP、tmux 管理/attach 已统一经 `SSHAuthenticationFlow`；Android/JNI 已在 §9.25 接入同一 native owner，信任/凭据等待的取消和超时已有真 sshd 证据；DNS 取消/预算、产品页面已有定向测试；macOS 仍只交付单地址；Android 已接 TCP 候选推进和共用总预算，12 项真实双栈/丢包/取消定向与 14 组变异见 §9.40。Android 通道打开取消见 §9.41；真实域名 DNS 和更多网络/设备矩阵仍待补，详见 [网络收尾](android-implementation.md#46-a3-网络与取消收尾)。 |
| POSIX PTY 尺寸能力 | 🟡 | macOS 共享后端 `pty_dimensions` 三类基线随核心闸门通过；`4924a2d` 另修复子进程继承信号屏蔽，真实窗口→本地 tmux / 本地 shell→系统 ssh→远端 tmux 无输入缩放与 PID 保留已有两条回归，PTY 继承三组变异通过。保留像素尺寸与默认值校验，非全平台证据；见 [实施记录](optional-tmux-implementation.md#后续-pty-尺寸通知修复2026-10-30)。 |
| 可选 tmux 后台会话 | 🟡 | 3.2a 已在 macOS 本机与 Ubuntu 22.04 SSH 验证真 PTY attach 和列表/管理；3.6a 另验证原生键鼠、任务保留、断网、背压、未知回复及受限账户。仅 POSIX 兼容远端 shell；其他版本与平台证据有限，不自动恢复，受用户 tmux 与系统清理策略影响。 |
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
| Android | libghostty-vt + 自绘 | 🟡 | A0 已验；A2 本地 PTY→VT→Canvas 与 InputConnection 首片已交付，文本操作切片接入文本选择/复制粘贴/本地清屏；API 36 arm64 / 4 KB 页模拟器验证见实现计划 §9.14。选区边缘滚动/Ctrl 生命周期切片已验（§9.15）；Ctrl 标点、模式感知 Enter 与真实 Gboard 拼音基本路径已验（§9.16）；后续按键重复/释放与焦点归属已验（§9.17，94 项完整设备测试、18+2 组选组变异）；后续修饰键/数字键盘/组合音符及静止文本闪烁已验（§9.18–§9.19），§9.21 短时取帧与性能探针通过完整 101 项、四组性能变异及五组回归；已有模拟器 debug 基线，真机/release/长负载及物理键盘/布局矩阵未完成 |
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

Android UI 的 🟡 指主机列表/搜索/增改、配置编辑和设置/关于已在 API 36 arm64 模拟器验证；配置保存有备份、摘要复核与原子替换，按应用私有目录和协作锁约束工作，不承诺任意非协作写入者下的无竞态提交。A1 已展示继承来源与共享别名影响，并支持同值显式覆盖、删除指令恢复继承；别名重命名与配置错误恢复已接入，SAF 选择/预览/取消、失败恢复和其余表单校验已完成 A1 验收；本地终端首片已验，整个 A2 尚未完成，本机/SAF 文件浏览首片已验，远端文件页面已接（§9.52），完整设置未实现。[最终 HTML](../design/android-prototype.html)继续作为页面基准，[A1 实施记录](android-implementation.md#92-a1-主机与配置首片2026-10-29)区分已验证与待办。

A1 阶段验收为 2026-10-30 的 47 项测试与 59 组配置/页面变异；A2 首片完整 63 项设备测试及 21 组终端变异为 §9.13 的历史证据。文本操作切片增加文本操作，最新完整设备结果见页首及 §9.70，§9.44 工作区曾有 1 项失败，偶发原因仍待定位，数量与覆盖边界见[§9.70](android-implementation.md#970-a5-粘贴确认任务认证与结果页面2026-10-31)；此前通道/选区早期失败保留在 §9.41，不累加历史测试数量。产品/身份首片原先的完整套件失败已修正测试观测与线程归属后复验，失败记录保留在 §9.27。A1 证据见[§9.11](android-implementation.md#911-saf-导入预览与-a1-收尾2026-10-30)。读取失败/保存冲突使旧快照失效；重载先确认草稿，错误跨页保留，成功刷新编辑目标，失败不报空成功态。§9.8 的错误恢复草稿已由 §9.10 补齐，SAF 与表单余项由 §9.11 完成；A1 只在当前设备档位收口，完整客户端仍在开发。此前工作区的继承编辑草稿已补齐接线，原 Kotlin 编译失败已解决；历史记录见[§9.6](android-implementation.md#96-文档复核时的工作区草稿2026-10-29)，当前实现与验证见[§9.7](android-implementation.md#97-显式覆盖与恢复继承2026-10-30)。

Android 修饰键、数字键盘与组合音符切片已验收，该切片完整结果为 **99 项设备测试，0 失败/错误/跳过**，文本变异选跑 24 组通过，见[§9.18](android-implementation.md#918-修饰键数字键盘与组合音符2026-10-30)。本片仍限 API 36 arm64 / 4 KB 页模拟器，不改变整个 A2 或下表 macOS 的完成状态。

静止画面的闪烁文字随后已修复，实际窗口像素验证及五组渲染变异通过；该切片完整设备结果为 **100 项，0 失败/错误/跳过**，见[§9.19](android-implementation.md#919-静止画面的闪烁文本2026-10-30)。后续短时取帧与 debug 性能基线见[§9.21](android-implementation.md#921-输入后短时取帧与性能基线2026-10-30)，完整 101 项设备测试通过；整个 A2 尚未完成。

SAF 私钥导入的原请求归属、旋转保留、只读读取和取消已验：五项定向、七组新增变异及完整 135 项设备测试通过，历史失败与修正见[§9.29](android-implementation.md#929-saf-私钥导入生命周期2026-10-30)。加密 OpenSSH ed25519 重试与成功密码流程已在 §9.31 验证；身份设置首片见 §9.33，主机身份关联见 §9.35，信任记录管理已由 §9.39 交付，网络矩阵仍待补，A3 保持 🟡；A1 配置文档导入的证据独立保留。

身份设置已接通列表/详情、OpenSSH 类型/指纹、导入与确认移除；损坏条目独立显示，过期删除拒绝，原文档与已建立连接保留。身份设置八项定向、十组新增变异已有记录；主机身份关联切片已接配置引用、认证候选与引用详情，完整 160 项设备回归通过（§9.35）。known_hosts 管理已由 §9.39 交付，A7 的 16 项偏好仍未完成；A7 应记为身份子页部分交付，不能笼统标为未开始。早期失败保留在 §9.32，当前范围见[§9.33](android-implementation.md#933-ssh-身份设置与公钥信息2026-10-30)。

Android 主机验证管理的 🟡 证据限 API 36 arm64 / 4 KB 页模拟器：真实 SSH 指纹、移除后重验及旧会话 PID/IO，重复记录/只读规则、错误结果、旋转和离页写入已验。非协作外部写者、断电恢复、更多设备与完整设置未包含；不据此增加 macOS 管理页面完成状态。

**A5/A6 产品接线（§9.70，当前模拟器档已验）**：执行器已由 `FileTasksViewModel` / `FileTasksScreen` 消费，任务认证复用现有主机信任与 Keystore 流程。18 项定向、24 组新增与既有 97 组变异通过；本片当前模拟器档验证通过，最近完整为 §9.70 的 437 项通过。进程级调度、后台/暂停/重试/恢复未接，不标整个 A5/A6 完成。

### Android 文件首片

本机/SAF 单目录创建为 🟡：§9.64 的 API 36 arm64 / 4 KB 页模拟器上，16 项目录基线和 26 组变异通过；包含空目录、嵌套、不透明 ID、实际名称、非覆盖、取消晚到及随后真实文件发布。远端原语后由 §9.65、统一服务由 §9.66 接入；§9.69 已接目录树执行数据层，§9.70 已接页面入口，进程级调度与恢复未接。

SAF 新文档写入为 🟡：§9.59 的 API 36 arm64 / 4 KB 页模拟器上，16 项设备用例及 30 组变异已验创建/写入/关闭/回读、未知副作用与取消租约。它是 data 层设施；复制来源清单由 §9.60、目录执行器由 §9.69、任务页面由 §9.70 接入，自动清理和进程恢复仍未实现。

本机 HOME 与持久授权 SAF 树的列表、只读 UTF-8 预览和多选为 🟡，仅在 §9.43 的 API 36 arm64 / 4 KB 页模拟器档成立。超过 4096 项报错，预览上限 256 KiB，未知流按实际读取限额；点按打开、长按进入勾选。远端 SFTP 后端为 🟡（§9.51 的 13 项/18 组及 52 项同档设备证据），页面为 🟡（§9.52 的 9 项产品用例、14 组产品变异及完整 236 项同档设备证据），复制来源清单已由 §9.60 接入，§9.70 已接 Activity 粘贴任务，进程级队列仍未实现；隐藏开关只是进程内状态，不代表 A7 持久设置完成。

### macOS 当前实现与限制

新增 shell 集成与 App 内建功能分别记账：`7939ef8` 的 Bash/Zsh 提示符钩子须显式安装，不在启动、唤醒或 SSH 连接时自动注入；不会因此增加远端 cwd、OSC 133 或跨进程恢复能力。

| 面 | 已实现与证据 | 未完成或证据边界 |
|---|---|---|
| SSH 与认证 | `AppModel` / `SSHAuthenticationFlow` 共用可取消认证；真实 sshd→shell→网格→resize；hostkey 未知/拒绝/变化/撤销分开。撤销及不支持规则经 SFTP 产品入口验证，不弹信任/凭据框 | 未支持配置已在解析地址前阻断，仍未执行跳板/转发等高级选项；私钥材料生产路径仍从受限文件读取，见路线图 P0。哈希主机名与主机证书未实现；本片验了 SSH/SFTP/tmux 配置拒绝入口，未新增终端网格或 tmux 成功会话证据 |
| 本地 shell | 普通 PTY、登录语义、cwd、控制终端、背压、关闭与描述符隔离；`4924a2d` 清空子进程继承的信号屏蔽，补齐 ssh/tmux 的尺寸通知 | 普通会话不承诺跨 App 退出保活；信号/真实窗口证据见上方 POSIX PTY 行，不扩大为全部平台 |
| 提示符鼠标残留清理 🟡 | Bash/Zsh 脚本及带备份的显式安装器；macOS 26.6.2 / tmux 3.6a，本地和系统 SSH→隔离环回 sshd 的真实 full 引擎复现/清理、正常 TUI 鼠标、重连及八组变异已有记录 | 实际 shell 须加载脚本，远端需远端安装；前台程序未退出时不触发。真实休眠/用户远端未验，Codex 输入框未复现，不称其缺陷已修复；见[操作与证据](../how-to/mouse-report-recovery.md) |
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

**复制原语缺口**：旧 `gt_sftp_upload/download` 为路径式 job，没有独占创建/非覆盖提交参数；新 `gt_sftp_create_controlled` 与 Swift/JNI/Kotlin 已提供从 fd 分块写入任务暂存路径的独占创建和结果状态（§9.54），§9.57 已补远端发布及绑定，应用级任务仍未接。旧 job 上传已按 §9.53 使用 WRITE|CREAT|TRUNC，不残留旧尾；失败可能留下部分内容，不保证原目标保留。现有“先检查冲突再传”不能保证并发时不覆盖。Android 四方向复制需先补共享契约，macOS 现有覆盖/冲突行为也需对应回归，见[路线图](roadmap.md)。

**传输完成与调度边界**：共享下载打开顺序、覆盖截断和两侧关闭检查已按 §9.53 修复；§9.54 已有独占暂存创建，§9.57 增加远端非覆盖发布（不承诺通用原子性），旧 job 后续失败仍可能留下部分文件。每 SFTP 会话 FIFO 不等于全局并发上限；macOS 重试仅复用弱引用仍可取得的原 backend，不自动重连或续传。`gt_sftp_walk` 的负深度模式没有实际硬深度护栏，macOS 无限递归调用方仍受此缺口影响。调度、重试和 walk 仍是源码核对的未完成项；传输结果修复的实际证据见 §9.53，调用链与后续条件见 [Apple 传输边界](../../apple/README.md#传输提交调度与重试边界)及路线图。

**目录结果边界**：macOS `TransferManager.expandDirectory` 排除目录内符号链接，上传不递归链接、下载不给链接建子任务；当前没有排除明细/计数，空文件集合会完成父任务。既有目录下载测试验证普通文件入队且链接排除，不证明目录逐项完整复制。用户可见的跳过原因和汇总仍待实现，不能把普通文件成功扩大为整棵目录镜像成功。文件手势、剪贴板、正常退出与设置的两端差异见 [Apple 宿主说明](../../apple/README.md#与-android-最终方案的差异)。

macOS 缺口的修复顺序与失败场景见[路线图](roadmap.md#macos-修复顺序与验收触发场景)。本轮只更新事实说明，未修复配置事务、表单或传输状态机。

**凭据验证边界**：共享内存私钥错误分类已修复，Android 四项凭据定向、七组变异及完整 139 项设备回归通过；共享 Core 真 sshd 新回归/一组变异和既有连接器 18 组通过。macOS 完成原生重新链接及 11 组既有认证回归，不冒充新增 Apple 加密密钥产品用例。密钥证据限 OpenSSH ed25519，原失败见 §9.30，当前见[§9.31](android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。

**证据入口**：最新完整宿主为 `4cbfcf4` 的 App 349 项：347 通过、1 项开发态主题资源跳过、1 项键盘设置页像素失败；恢复旧终端源码也复现，未记全绿。该提交已修复跨屏 DPI/Metal 比例/像素尺寸同步，三项原生回归、六组手工变异、Terminal 28 项及 release 构建通过，真实双屏视觉复验仍待完成，见[跨屏记录](optional-tmux-implementation.md#后续跨显示器缩放修复2026-10-31)。[标签/窗格计划](plan-tab-chrome-refactor.md)的 341 项/1 跳过保留为历史通过记录；真实桌面激活/全屏另列，不扩大到未运行系统。[工作区交接](workspace-handoff.md)保留已验范围与缺口，`workspace` 21 组变异仍属手工证据。

**发行边界**：源码 SwiftPM 声明 macOS 13，但目前发行包为 arm64，嵌入依赖的 minos 抬高至 macOS 26；不能据源码声明推断包可在 13 上安装。自包含/ad-hoc 已有包证据，Developer ID/公证仍待实际证书与包验收，见[构建与安装](../how-to/dev-build.md)。
