# 可选 tmux 实施清单

本清单落实 [重构计划](plan-optional-tmux.md)，以 `e000728` 为起点。目标是普通 PTY/SSH 默认连接、可选 tmux 后台会话和自有 sessiond 全面退役。保留 `vendor/ghostty` 的现有 pin；公开 ABI 删除与新增均按 BREAKING 提交。

## 当前状态（2026-10-30）

六个实施切片与最终验收已完成；交付范围是共享 C 与 macOS 宿主，Android 的列表与 attach 仍在 A8 待办。下方依赖表记录重构前的消费者及当时迁移决定，列出的 daemon/session/IPC 路径已退役，不是当前待删除清单。共享接口基线 `854e473` 增加 IdentityFile 来源查询后，Core 为 36 项 CTest、75 个公共符号，Apple 绑定为 73/2，JNI 为 34/41；该只读接口不增加 tmux 能力或验收范围。连接检查切片的 35/74 和本次 tmux 验收当时的 30/70 作为历史保留。各次结果分别见[路线图](roadmap.md#验收记账与历史记录)。

共享认证后续已修复 known_hosts 标记误信任，Swift 保留撤销状态，未支持规则在请求凭据前停止；该片证据为共享 C 同步/异步及 macOS SFTP 入口，不是新的 tmux attach/UI 验收。当前 `auth` 脚本为 11 组，包含主机信任、保存失败与连接配置准入；下面三组是重构当时的结果，后续见 [§9.22](android-implementation.md#922-a3-共享主机信任前置2026-10-30)至 [§9.24](android-implementation.md#924-共享连接配置准入与两端接线2026-10-30)。

## 后续 PTY 尺寸通知修复（2026-10-30）

`4924a2d` 修复从 GUI/工作线程继承信号屏蔽集合的问题：`core/src/io/pty_posix.c` 在终端子进程 exec 前执行 `sigprocmask(SIG_SETMASK, 空集合)`，不改变调用线程。之前内核 PTY 尺寸虽然已更新，系统 ssh/tmux 仍可能收不到 SIGWINCH，表现为拖动窗口后画面尺寸不更新。

新增 `TmuxResizeTests` 两条离屏 AppKit 真实窗口回归分别覆盖本地 tmux，以及普通本地 shell 中运行系统 ssh 后进入远端 tmux；SSH 目标为隔离环回 sshd。放大、缩小均不依赖额外键入，内层任务保持同一 PID。这条系统 ssh 路径与 Ghosterm 直接使用 libssh2 的远端 attach 路径分别记账，不是新增真实桌面激活或 Linux 远端证据。

该提交记录的验证为 Core 35 项 / 74 ABI、macOS 原生构建、真实 sshd 的两条原生缩放用例、PTY 继承三组隔离变异（含 `--apple`）和本地 tmux 八模式。变异旧信号屏蔽行为时，原生目标红、API/主机列表独立对照绿，恢复绿并核对 SHA256。本次文档同步核对源码与提交记录，没有重跑这些测试，不增加 Linux/Android 设备、完整宿主、发行包或 CI 证据。

## 后续提示符鼠标模式清理（2026-10-30）

`7939ef8` 新增 `scripts/shell-integration/mouse-recovery.bash` / `.zsh` 与显式安装脚本。它处理 tmux 窗格内程序异常退出后仍保留鼠标模式的情况：实际 shell 下一次显示提示符时清理，不在宿主唤醒时重置、不注入终端输入、不改 tmux 管理协议。App 未自动安装，远端 shell 需在远端加载；不是旧 runtime/daemon 的回归。

该提交的证据为 macOS 26.6.2 / tmux 3.6a、本地与系统 SSH→隔离环回 sshd 的 Bash/Zsh 复现/清理、原 PID/前缀键/正常 TUI 鼠标/新 surface 重连，以及八组隔离变异和 Core 闸门。Codex CLI 0.164.0 的输入框未复现序列文本输入，不能记为其缺陷修复。没有真实整机休眠或用户远端服务器证据，也未增加 CI 20 份覆盖；本次文档核对未重跑。安装、卸载和命令见[使用说明](../how-to/mouse-report-recovery.md)。

## 重构时的依赖闭包与保留责任

| 模块 | 重构前消费者 | 处理与验证 |
| --- | --- | --- |
| `daemon/` 全部 | helper builder、Linux builder、部署、daemon probes | 删除全部源码与构建目标；同时删除 CI Linux bundle job、needs、下载/上传 artifact 及打包调用 |
| `core/src/session/` 的 runtime/worker/manager/creation/create-spec/state-owner/checkpoint-task/attachment/restore-stream/client-api/deploy | `daemon/`、IPC server/client、公开 session ABI | 删除及配套 C tests；从 `core/CMakeLists.txt` 移除 `ghosterm_client_*` 目标与专用对象库 |
| `core/src/session/setup-api.c` | `SessionSetup.swift` 的异步认证和安装、SSH session provider | 拆分保留通用认证，删除 Linux 安装和旧会话 provider；通用认证单独放 SSH 头与实现，不能删掉认证交互 |
| `core/src/session/{command-status,program-status,process-meta}.*` | 前两者被 runtime/worker 和 daemon 使用；普通 UI 自有 `ProgramStatusStore`/`LastCommand`，普通 Surface 接收 OSC 事件 | 移除 daemon 消费后删除 C 私有模型；保留原生普通 OSC 状态、标签角标和语义提示测试，不按文件名批量删 |
| `core/src/ipc/` 全部 | sessiond 管理/DATA、attachment、proxy、专用 SSH owner | 删除专用 wire/provider/connection；`ssh-connector.h` 由通用 `ssh.c` 使用，移至 SSH 私有目录并保留 connector tests；`base/secure-random.*` 按最后消费者决定 |
| `core/include/ghosterm/{session,session_setup}.h` | 聚合头、Swift wrappers、旧测试 | 移除所有旧会话与安装声明；通用认证先迁出，保持只有聚合头跨语言；`check-abi.sh` 双向对拍 |
| `GhostermCore` 的 `RecoverableSession.swift`、`SessionSetup.swift` | App 恢复、安装、认证 | 删除恢复与安装包装；保留 resolver、SSH/PTY、exec/SFTP，通用异步认证迁出；Swift Debug/Release 编译 |
| `GhostermAppModel` 的 `NativeSessionCoordinator`、`RecoverableSurfaceTransport`、`RecoverablePaneSession`、`SessionRecord{Store,Context}`、`Session{Control,Setup}Transport`、`SessionRuntimeCatalog`、`SavedSessionPresentation` | `AppModel`、`TerminalPane`、sidebar/root UI、专用夹具 | 删除恢复/持久记录/安装/管理 UI 及专用 tests；`SessionController` 保留普通 I/O；普通默认连接先替换调用点 |
| `SessionAuthentication{Flow,Broker}`、`SessionCredentialSelector` | setup/control/recoverable 认证；selector 的目标类型绑在旧 Record 上 | 抽出主机/账户配置与认证生命周期，复用 hostkey/Keychain 提示；无旧 Record/Recoverable 类型依赖；保留 auth 与取消回归 |
| `GhostermTerminal` replica policy、import/output/style/sequence 接口与 `GhostermGhosttyShim` 对应入口 | `RecoverableSurfaceTransport` 与 replica tests | 删除全部 replica 扩展，保留常规 Surface 创建/输出/IME/焦点/可见性/配色/OSC 回调；真实原生构建与像素回归 |
| `terminal/{session-state,state}/` 全部 | session probes、engine 构建与 daemon | 删除；恢复普通 full libghostty 的独立构建，不得继续间接 import `probe-session-import.py` |
| `patches/libghostty/8159838/experimental/` | session engine、预算、快照、replica | 删除全部；保留 `0001-host-io-backend`、`0002-program-status`、`0003-semantic-prompt` 与 WHY；shim 如仍用 experimental 符号先移除调用，再构建干净 staging；不改 vendor |
| `scripts/{build,probe,mutation-check,stage,package,prepare}-session*` 与 daemon-vt 等专用校验 | package、CI、各探针相互 import | 专用链路整体退役；有普通连接/认证断言的脚本拆分保留；`build-ghostty.sh` 改用普通引擎构建器；共享 timeout/themes/签名逻辑保留 |
| `package-mac.sh` 与 `macos-package.yml` | main dev/release 出包、package mutation | 删除 helper 构建、catalog、Linux bundles、签名遍历与参数；保留更新清单、主题、签名四档、公证守卫和 Homebrew cask 对拍；干净检出打包验包内无 sessiond |
| `docs/architecture/{session-runtime,remote-session}.md`、`docs/reference/session-*`、`plan-session-daemon.md`、Linux 会话 how-to | README/overview/roadmap/矩阵/许可/dev-build 的交叉引用 | 删除被替代文档并同步所有引用；AGENTS 的 CI 与变异范围随退役提交更新，旧历史证据不冒充新支持范围 |

删除必须覆盖消费者及对应夹具、脚本、构建目标。通用 sshconfig、SSH/SFTP/exec、PTY 继承、主机密钥、IME、terminal、tabs/chrome/settings、焦点、遮挡、配色、程序角标、更新与打包断言继续保留。新验收的强弱以原计划为准，不能通过删除保留能力的断言换绿。

## 首版命令决策

最低版本为 **tmux 3.2a**，同时探测所需命令/格式字段；旧版本返回明确不支持。3.2a 已在 macOS 本机和 Ubuntu 22.04 SSH 目标完成真实会话操作测试。2026-10-28 本机 `/opt/homebrew/bin/tmux` 为 3.6a；在项目 `tmp/` 下隔离 socket/config 验证了以下行为。临时命令研究记录为 `tmp/tmux-analysis/commands.json`；后续共享模块测试接管这些判据。

1. 路径：用户显式绝对路径优先；本机以所选登录 shell 有界查找，不假定 GUI PATH 等于交互 shell；远端在服务器实际 POSIX 兼容登录 shell 环境内解析后锁定绝对路径。路径未解析、启动文件污染输出、超时分别报错。查询不启动交互终端，不触碰用户已有格子。
2. socket：显式绝对路径优先；默认按 tmux 原有 endpoint 选择规则解析，成功后锁定返回的 `socket_path`。管理与 attach 使用同一个 endpoint，清除继承的 `TMUX`，不误用本机或其他格子的环境。查询与 attach 加 `-N`；只有显式新建允许启动 server，attach 不带 `-A`/`-d`。测试始终指定工程 `tmp/` 的 socket，不用系统临时目录。
3. 列表：`list-sessions -F` 固定输出 `pid/start_time/session_id/session_created/session_windows/session_attached` 与最后一列 `session_name`。3.6a 的 `session_check_name` 将控制字符可视化、点与冒号替换为下划线；实测 TAB/LF 变成字面 `\\t`/`\\n`，中文及空格保留。查询与 attach 显式加 `-u`，避免 C locale 把中文规范化成下划线；诊断只固定 `LC_MESSAGES`，移除覆盖它的 `LC_ALL`。因此以 TAB 分字段、LF 分记录在此支持版本成立；原样保留名字，不反向解码。拒绝非法字段数、非数值、重复 ID、非预期控制字节、超限与非 UTF-8，不能把脏输出当空列表。
4. 目标：使用本次列表中的 `$<数字>` session ID，并记录 server PID/start_time、session_created 和 endpoint。操作前重查身份，不匹配即失效。查询后至操作间仍有竞态；不宣称跨 server 重启的原子身份保证，不保存 ID 或自动 attach。外部客户端切 session 后格子只保留“连接时选择”的显示。
5. 引用：本地 argv 与远端 shell 引用分开。远端先构造 tmux argv，再统一 POSIX 单引号编码。tmux 自身还解析末尾分号并对新建/重命名名称做 format expansion；名称中的 `#` 要按 format 规则保护，末尾 `;` 按 tmux argv 规则保护，位置参数加 `--`。实测名称 `#{pid}`、`;`、`tail;`、`-option` 都能作为字面名称使用。新建后展示 tmux 回读的实际名称，不能承诺 `a.b:c` 不被规范化。
6. 结果：成功输出、退出码和 stderr 分开保留；命令执行有总 deadline/取消/输出上限。所有 exit 1 不等价：无 server、坏 socket、权限拒绝、格式错误分别处理。无 server 只有在版本支持、固定 locale 的已验证连接诊断匹配时标为“未运行”，其余查询失败；“已知空列表”只来自成功解析的列表。失联的变更显示结果未知，刷新前不重发。
7. 保留配置：服务级查 `show-options -s -v exit-unattached`，每个 session 查 `show-options -A -v -t ID destroy-unattached`，其中 `-A` 必须包含继承值。少了 `-A` 时本机查询返回空串，不能把空串当 off；查不清即显示未知。用户 hooks、系统登出清理与服务重启仍影响保活。
8. 交互：本地 `GT_PTY_CMD_DIRECT` 的 PTY 跑 argv；远端扩展 PTY + exec Transport，持有已认证 SSH owner。禁止无 PTY exec 充当终端、禁止向普通 shell 注入 attach、禁止并发借用 SFTP session。管理连接与长连接分别拥有串行 owner，但复用同一认证实现。

依据为 [3.6a 手册](https://github.com/tmux/tmux/blob/3.6a/tmux.1)、[名称规范化源码](https://github.com/tmux/tmux/blob/3.6a/session.c)、[重命名源码](https://github.com/tmux/tmux/blob/3.6a/cmd-rename-session.c)、[新建源码](https://github.com/tmux/tmux/blob/3.6a/cmd-new-session.c)。版本锚定和本地研究不能替代真实 SSH、GUI、断网与打包验收。

## 实施状态

| 切片 | 状态 | 完成证据 |
| --- | --- | --- |
| 1 依赖与命令契约 | 已完成 | 源码消费者、3.6a 命令研究、相对链接与 diff 校验；基线 core gate 414 项、82 公共符号及 build-core 通过 |
| 2 普通默认连接 | 已完成 | 默认 PTY/SSH、移除旧恢复入口；App 329 项（6 项旧会话专用环境、1 项随包主题跳过）、Core Swift 真 SSH、Terminal 29 项（5 项旧 replica 夹具跳过）通过；无 runtime 隔离副本 4 组默认入口变异及标签页变异 37 项检查通过 |
| 3 退役旧系统 | 已完成 | 删除 daemon/私有 IPC/快照与 replica/旧 ABI/安装及 CI bundle 链；通用认证独立保留。干净检出上 core 28 项/64 ABI 与原生 release 打包通过；主工作区 App 299 项（1 项主题环境跳过）、Terminal 24 项通过；打包 63 项、认证 ABI 8 组、凭据 6 组、PTY 尺寸 10 组、公开头/PTY 继承变异通过。普通 SDK 为同 pin 的 3 份补丁，无旧 runtime |
| 4 共享 tmux 模块 | 已完成 | Core 30 项/70 ABI；macOS 与 Ubuntu 22.04（GCC 11、libssh2 1.10）本地真 tmux 六模式和 Mac→Linux libssh2 六模式通过；13 组隔离变异有指向性及 SHA 回滚。薄 Swift 绑定、SSH PTY exec、总 deadline/取消/输出限额与非阻塞 UI 关闭已接通；断网和受限账户仍在最终验收切片 |
| 5 后台会话 UI | 已完成 | 本机/主机入口、列表操作、独立 attach、新标签页与断开格子重新选择；普通终端/SFTP/管理/attach 共用异步认证。App 303 项（1 项随包主题环境跳过）通过；UI 7 组隔离变异、真 sshd 认证 3 组变异通过；内层任务 PID 不变、关闭误杀目标红，未知/路径选择及运行格子保护有指向性 |
| 6 全量验收 | 已完成 | 本地/SSH 八模式、七种网络故障、三种受限账户、19/9/6 组 tmux 变异、干净打包 63 项通过；普通 PTY/登录 shell/终端/焦点/标签页变异分别 12/13/19/14/37 项通过，均回滚复绿 |

## 最终验收口径（2026-10-28）

最终切片补齐错误码及绑定、终端退出未知、关闭标准描述符时的子进程 IO、真实 SSH 故障与原生鼠标验收，清除最后的无消费者私有入口及认证记录。保留流式 exec 复用同一有界通道准入；SFTP、普通 SSH 与通用认证各自保留独立变异。规则更新只校正实际 ABI 绑定、CI 覆盖与新增测试，并未降低普通终端闸门。

- 离线核心：30 项 CTest、70 个公共 ABI；Swift 引用 68 项，另外 2 项明确登记。关闭 libssh2 的构建以及公开头/API/tmux 四项离线冒烟验证不支持分支；不将要求真实 SSH 的测试算作该配置通过。
- 原生 App：305 项，1 项只在随包主题环境成立的检查跳过；主题由干净发行包的实际引擎配置检查补验。新增测试直接运行 full libghostty 与 AppKit NSEvent，鼠标选择 tmux 内层 pane，Ghosterm 仍只有一个格子。
- 真实 tmux：macOS 与 Ubuntu 22.04（GCC 11、libssh2 1.10）本地八模式、Mac→Linux libssh2 八模式；owner 被 SIGKILL 后原任务仍输出，重新认证/选择后 PID 一致。
- SSH 故障：reset、静默断网、恢复同一链路后的 3 MiB SHA256、创建/结束回复丢失、管理查询总超时/取消。无自动重发；仅新连接重新认证。PTY 拒绝返回 UNSUPPORTED，exec 拒绝与仅 SFTP 账户明确失败；后者仍能用独立 SFTP 连接。
- 干净打包：同一 vendor pin 在独立检出中重新构建三份普通补丁，公共 `build-ghostty.sh` 同步暂存 725 套主题；release Swift 构建、实际依赖加载、自包含/签名/配置检查与无旧 runtime 校验通过。App 约 28 MB，打包 63 项变异无 SKIP，七份脚本 SHA 回滚一致。
- 普通终端：Terminal 24 项及 19 项变异检查、焦点 14 项、标签页 37 项检查通过；字节进网格、已有 surface 配色更新、reload_config、输入法预编辑/提交、keyDown 与 Ctrl+B 控制字节均有目标红和独立对照。
- 普通核心回归：PTY transport 12 项变异、登录 shell 13 项变异通过；回滚后再次 core gate 30 项/70 ABI 全绿。Swift Core 离线 14 项中两项仅在注入真实 SSH 时运行，随后真实 sshd 两项通过，未把跳过计作 SSH 证据。
- 新增变异：共享 tmux 19 组、原生 UI 9 组、SSH 故障 6 组；每组有目标红、同变异独立对照绿、回滚复绿与 SHA 校验。SSH/exec/SFTP 的通用变异分别为 4/2/8 组，保留真实 sshd；暂停同时破坏读/写两条循环，避免重复锚点或旧可执行文件冒充结果。

以上只声明已验证的 tmux 3.2a 和 3.6a、macOS full libghostty 和实际测过的 Linux SSH 目标。用户 hooks、destroy/exit-unattached、登出清理及机器重启仍可能结束任务；不提供私有快照等价、启动恢复、SFTP 保活、其他 shell/版本或其他平台的支持承诺。

2026-10-28 CI 复验：Homebrew 提供的 tmux 3.7c 对 Tab/LF 名称明确拒绝，点与冒号则保留；
3.6a 将前者转义、后者替换为下划线。两种结果都由真实 tmux 对拍，拒绝时验证原名称不变；
不得把拒绝折成未知或成功。
