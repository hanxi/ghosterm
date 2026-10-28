# 可选 tmux 后台会话重构计划

日期：2026-10-28。状态：六个实施切片与最终验收已完成；依赖、命令决策、测试范围和限制见 [实施清单](optional-tmux-implementation.md)。普通默认入口、自有 sessiond 退役、可选 tmux 共享模块与原生 UI 均已交付。

将默认连接恢复为普通本地 PTY / SSH 终端，把后台保活改为用户显式选择的外部会话能力。首版支持 tmux：一个 Ghosterm 格子附着一个 tmux session；session 内的 window、pane、状态栏、快捷键和鼠标行为全部由普通 tmux 客户端处理。删除自有 sessiond 及其恢复、部署和打包体系，不保留旧 ABI、协议、配置或记录格式的兼容代码。

## 产品范围

- 普通终端不依赖 tmux，不自动检测或启动后台服务。
- 用户打开“后台会话”列表时，探测当前本机或 SSH 目标的 tmux。展示 session 名称、窗口数、已附着客户端数，提供刷新、新建、连接、重命名和结束操作。
- 缺少程序、没有 session、探测失败分别展示。没有安装时不自动安装；用户仍可使用普通终端。
- 每个格子连接一个 session；Ghosterm tab 和分屏布局保持原有职责，不映射 tmux 的 window 或 pane，不使用 control mode。
- 从列表选择或新建 session 后，创建新的 Ghosterm 格子承载 attach；不替换正在运行的普通终端或其他连接。已断开的 tmux 格子可从列表重新选择后原位连接，列表取消不改变原格子。
- 首版默认连接用户已有的 tmux 实例，尊重其配置。可显式指定可执行路径和 socket；不自动另建 Ghosterm 专属 server、不修改用户配置、不强制抢占其他客户端。
- 对 tmux 格子，关闭格子、关闭 tab 或退出应用，仅关闭 attach 客户端。普通终端仍按现有作业确认和关闭规则处理。“结束后台会话”只在刷新后的列表中操作，明确展示目标并确认，不调用 kill-server。
- 断线后显示连接已断开、后端状态未知。“重新连接”刷新并打开列表，由用户确认当前 session 后 attach，不按旧名称或 ID 自动恢复，不自动创建替代 session，不重放输入或命令。首版不做启动自动恢复、后台重试循环；凭据需要交互时走现有认证 UI。
- tmux 内按快捷键 detach 后，格子进入已断开状态并保留重新连接入口，不因此宣称 session 已结束。
- Ghosterm 不主动终止后台任务，实际保留行为遵循用户的 tmux 配置和系统策略。探测并展示 `destroy-unattached`、`exit-unattached` 等会在脱离后结束会话的配置，不自动修改。恢复画面、历史和协议能力遵循 tmux；不承诺原有私有快照的完整状态等价、机器重启恢复或 SFTP 传输保活。
- 已在同一 session 附着的多个格子/外部客户端可能共享窗口选择、输入和尺寸影响；列表显示附着状态，不宣称各自独立。Ghosterm 本身不额外套一层 tmux。
- 用户可以在 tmux 内自由切换 session。格子记录的是连接入口，不宣称始终跟踪当前 session；其名称标注为“连接时选择”。首版不增加客户端追踪、轮询或状态同步协议，重命名和结束只针对列表中重新核验并选中的目标，不使用格子里的旧绑定。

## 目标架构

```text
原生 UI：普通连接 / 后台会话列表 / 格子绑定
    ↓ 低频操作和结果
宿主协调器：认证、取消、选择、连接入口
    ↓
TerminalSurface ↔ Transport ↔ Core
                    ├─ 普通：本地 PTY / SSH PTY shell
                    └─ tmux：本地 PTY / SSH PTY exec → tmux attach

后台会话管理：同一目标的本地命令执行 / SSH 非交互 exec → tmux 命令
```

UI 不直接调用 Core；宿主协调器通过 Transport/管理接缝和语言绑定进入 `core/include/ghosterm.h`。TerminalSurface 不感知 tmux、本地或 SSH，输出仍由 I/O owner 直接 feed，不经过 UI、观察模型或事件队列。查询结果和状态通知才进入 UI。

交互 attach 与管理查询分开：attach 使用真实 PTY，管理使用有界命令执行。保留现有 libssh2 身份验证、known_hosts、Keychain 和 SFTP 连接策略，不允许从 UI 临时拉起系统 ssh 绕过现有认证。不同线程不能并发操作同一 libssh2 session。

后端边界只包含实际需要的 `probe/list/create/attach/rename/terminate`，结果携带明确能力和失败状态。首版只有 tmux 适配器，不做通用插件系统，不把 tmux 参数、输出格式散落在 SwiftUI 或 Surface 中。后续 zmx 等后端独立适配，不通过替换可执行名称冒充兼容；不支持的操作返回显式不支持。

实现采用一个共享 C tmux 模块、现有 Transport 的最小扩展和一个薄的宿主协调器；查询与长连接按职责分开，不再引入 session manager、私有 wire、后台注册表或通用后端工厂。可直接复用的认证、命令执行和 I/O owner 不另写一份。公开句柄保持 opaque，结果经 POD 与 NUL 结尾字符串跨语言传递；模块内部的命令细节不暴露给 UI。

## 命令与状态契约

1. 本地管理命令以 argv 启动，限制输出、执行时间并支持取消；不引入平台 UI 依赖。tmux 的解析、目标选择及参数构造放在共享 C 管理模块，平台绑定只做类型转换。
2. 远端管理复用 SSH 非交互 exec。当前 `gt_transport_open_ssh_shell` 只有 PTY + shell；为 attach 增加有界的 PTY + exec Transport 入口，并同步 Swift 绑定。不能用一次性无 PTY exec 代替交互终端，也不能向已显示的 shell 注入一串 attach 命令。
3. tmux 路径和版本只在用户请求后台能力时解析；处理 GUI 与交互 shell 的 PATH 差异，允许显式指定路径，后续命令使用已解析路径。首版以可验证的最小版本和真实功能探测声明支持范围。
4. list 使用 tmux 格式化输出和固定字段，标识与显示名称分离。评估并测试名称含空格、分隔符、换行及非 ASCII 的解析；不能依赖未经验证的按行切分。超限或读不懂明确失败，不返回空列表。
5. session ID 只用于本次查询和操作，不作为跨重启的持久身份。列表项保留后端可提供的 server/session 标识和创建时间，连接、重命名、结束前重新核验；目标变化或无法确认时使选择失效并要求刷新。全部命令使用精确 session ID，不按名称做模糊匹配。读取与操作之间仍可能发生外部变化，不宣称提供事务或跨 server 重启的强身份保证；不为此创建 token、标记或新后台协议。重新连接总是由用户从新列表选择，旧记录不能触发操作。
6. 探测区分缺程序、受支持、不支持及查询失败；列表区分已知为空与无法查询。不能把所有 exit 1 解释成“没有会话”，需结合受控查询和 endpoint 证据判定。
7. session 名称、路径、socket 和其他用户输入不得直接拼接进 shell。远端 exec 必须使用单一受测试的 POSIX shell 参数引用器，并处理 tmux 命令分隔符、选项和目标匹配规则。
8. 连接只 attach，不使用会隐式创建的命令，不带强制 detach 其他客户端的选项。新建是独立动作；网络中断导致结果未知时，不自动重发创建、重命名或结束操作，先刷新查询。
9. 交互通道的 EOF 只证明客户端结束，不等于 session 结束。审计 `SessionController` 当前 EOF 无退出状态时回落 0 的逻辑，改为诚实的未知结果。关闭和取消必须有界，不阻塞主线程。
10. 首版不新增持久化 session 注册表。格子内仅保存本次连接的目标配置和显示信息；已有工作区记录如需保存入口，只保存后端、主机/账户、可执行路径、socket 和上次显示名称，凭据仅保存平台安全存储引用。这些字段只是重新打开列表的提示，不是自动恢复授权，不持久化 session ID。终端字节、输入、环境变量、解锁密钥均不进入记录；不读取旧 sessiond 格式。

## 旧代码清理范围

删除以最终调用关系为依据，每项标为删除、保留或拆分后保留；不得仅因文件名包含 session 就整批删除。

| 范围 | 清理内容 | 必须保留的边界 |
| --- | --- | --- |
| `daemon/` | executable、proxy、VT 状态、快照、专用 tests/probes 整体删除 | 不保留空服务目标或兼容启动器 |
| `core/src/session/` | 删除 runtime、manager、worker、attachment、restore、部署、旧 client/setup API 等专用实现 | 审计 command/program status 的实际消费者，仍在用的终端元数据拆到职责明确的模块 |
| `core/src/ipc/` | 删除 sessiond 握手、token、管理/DATA wire、租约、客户端、server、local/SSH provider 和专用连接层 | 随机数、通用 SSH/PTY/安全工具仅在新链路确有消费者时保留；必要时移动目录 |
| 公开 C ABI | 移除 `ghosterm/session.h`、`session_setup.h`、聚合 include 和对应导出 | 保留通用 Transport、PTY、SSH/SFTP、配置与错误类型；同步新入口和所有绑定 |
| `apple/GhostermCore/` | 删除 `RecoverableSession`、`SessionSetup` 及仅服务旧链路的包装/测试 | 保留普通 Transport、SSH 和认证基础 |
| `apple/GhostermApp/` | 删除 `NativeSessionCoordinator`、`RecoverableSurfaceTransport`、`RecoverablePaneSession`、旧 RecordStore/Catalog、setup/control transport、旧恢复 UI 和夹具 | 恢复普通连接；审计 `SessionAuthenticationFlow/Broker/CredentialSelector`，通用认证提取后复用；保留 `SessionController` 的普通 IO 职责 |
| `apple/GhostermTerminal/` | 删除 replica 创建、快照导入、序号屏障及专用测试 | 保留常规 Surface、IME、配色、焦点、遮挡、滚动和终端元数据功能 |
| `terminal/` | 清理 `session-state/`、`state/` 中仅服务 daemon 的冻结、容器、资源预算和探针 | 不删除实际终端渲染仍需的代码 |
| `patches/` | 审计恢复快照、replica 和无 UI 状态扩展补丁及 WHY，清理只为 daemon 存在的部分 | full libghostty 嵌入、输入法、焦点等仍需的补丁保留；不就地修改 vendor，不无关升级 pin |
| `scripts/` | 删除 daemon 引擎/宿主/Linux bundle 构建、staging、部署、旧探针/变异脚本和依赖校验 | 通用 build/gate、终端与普通连接回归保留；旧复合测试拆出仍有效的断言 |
| 打包和 CI | 清除 `package-mac.sh` 的 session runtime 参数、内嵌/签名；移除专用 Linux bundle job、artifact、needs 和缓存项 | 保留 macOS 签名、公证、主题、更新检测及普通核心/终端构建证据 |
| 文档和规则 | 删除被替代的 sessiond 计划、架构、reference、how-to；更新 roadmap、能力矩阵、许可、开发/打包说明和 AGENTS | 新文档明确仅可选 tmux；旧闸门只在其能力整体退役后删除，不能用删测试掩盖仍保留能力的回归 |

不为旧安装写迁移、回滚或清理器；旧本地记录不读取，不导入新列表。新版本不启动、连接或管理旧 daemon，不擅自结束用户机器上已运行的旧进程，不扫描/删除远端旧安装目录。仓库与新产物中必须无旧 runtime。

## 实施切片

| 顺序 | 交付 | 验证和完成条件 |
| --- | --- | --- |
| 1 | 建立消费者清单及测试对应表，确定 tmux 最小支持版本、名称编码、操作前校验和 PATH 方案；同步新的架构决定与规则 | 每个待删模块有归属；旧强恢复承诺被明确替换；无需升级 vendor pin；实施前解决命令格式细节 |
| 2 | 默认入口切回普通 PTY / SSH，抽出复用认证和连接 owner，删除旧恢复 UI/编排，同步改造默认入口夹具和断言 | 无 tmux/无 daemon 安装也可本地和 SSH 登录、分屏；SFTP/文件面板可用；默认启动不探测 tmux；该提交自身的构建与测试成立 |
| 3 | 整体退役旧会话系统：一起删除 daemon、旧核心 API/协议、Swift wrappers、Surface replica、专用 terminal 代码及补丁，并同步删除构建目标、打包调用、CI job/artifact/needs、专用夹具/闸门和失效文档 | 此提交完成时干净检出可构建、测试、打包；App 无旧 runtime，CI 无旧 bundle 依赖；保留的普通终端功能回归通过 |
| 4 | 新建最小 tmux 共享管理模块，复用或补足有界本地命令执行；扩展 SSH PTY exec Transport 及绑定 | 真 tmux 的探测/list/create/rename/terminate 和交互 attach 通过；输入引用、取消、超时、结果状态覆盖；ABI 对拍通过 |
| 5 | 增加后台会话列表、新格子连接、断开格子重新选择，以及配置影响提示；复用现有目标配置，不建新记录库 | 不覆盖现有任务；关闭 attach 不主动结束 session；重连从新列表选择；tmux 内切 session 后不会按旧绑定误执行管理操作 |
| 6 | 最终审计残留、目录职责、文档链接与规则；补齐本地/远端端到端和清理检查的变异证据 | 从干净检出复验；无旧兼容分支、无失效依赖、无未使用公开入口；能力矩阵和实际支持范围一致 |

切片在 main 上按 Conventional Commits 提交，每片只做一件事，按影响范围完成验证再提交。旧系统退役的源码、构建、打包、CI 和测试删除属于同一件事，必须同步落地；不能先删源码，留到后续提交才修调用方。若需细分第 3 步，每个子切片都按完整依赖闭包删除，保持构建、测试和打包成立。main 每次提交都会自动出包，中间版本可以暂时只有普通终端；不增加双后端兼容开关。涉及公开 C ABI 删除/新增的提交显式标注 BREAKING。

## 验收与变异验证

**普通路径**：本机和远端均未安装 tmux 时，新建终端、SSH 认证、输入、resize、关闭、SFTP 和文件传输工作；不出现安装器或失败恢复弹窗。真实编译 Apple 三个 Swift package 和应用，不能只跑宿主 JS/静态扫描。保留适用的 terminal、transport、login-shell、auth、focus、IME、tabs 和打包等现有回归。

**可选后台路径**：使用隔离 socket 和配置启动真实 tmux，验证列表为空/有会话、session 改名及特殊字符、新建和结束、多 window/pane 在一个格子内交互、鼠标/前缀键、窗口 resize、外部客户端共存、快捷键 detach。选择后新建格子，原格子的任务 PID 和输入输出不受影响；取消列表不改变连接。默认保留配置下，关闭格子和退出应用后任务 PID 保持，重新从列表连接获得持续变化的输出。另以 `destroy-unattached`、`exit-unattached` 配置验证提示和真实结束行为，不能将其判为 Ghosterm 保活成功。覆盖 tmux 不存在、权限失败、不支持版本、脏输出、超限、取消和查询超时。

**远端路径**：真实 sshd + libssh2，通过已有 hostkey/认证链路 list/attach；TCP reset 和静默断网后重新认证，接回原任务。验证拒绝执行命令、PTY 拒绝和仅 SFTP 账户的明确错误。管理查询不能污染终端、不能抢占 SFTP 的 session owner。测试账户的退出清理策略纳入保活边界。

**选择与管理操作**：删除原 session 后同名新建、server 重启后 ID 重用，旧入口和旧列表均不能触发自动连接、重命名或结束；刷新后由用户重新选择。目标校验不一致则拒绝操作。用户在 tmux 内切到另一 session 后，格子仍只标注连接时选择，不提供按旧绑定结束会话的按钮；从列表明确选中哪个就只操作哪个。新建或结束回复丢失显示结果未知，不自动重发。结束一个 session 后其他 session 和未受该结束影响的外部客户端保持正常。

**架构清理**：实际构建图、链接符号、Swift 绑定、包内文件和 CI artifact 引用不含旧服务路径；从干净检出而非已有缓存验证。源码搜索是辅助证据，不能只以名称正则作为删除正确性的证明。

每个新增断言都附变异验证。至少破坏缺失判定、目标引用、attach 偷偷新建、关闭误杀、未知折叠、输入/resize 接线、远端 PTY、身份校验及包内残留检查；要求目标红、同变异下独立对照绿、复原后绿。遵守 AGENTS 的标记预扫、SHA256 回滚、INT/TERM trap、失败输出和 `gt_timeout` 要求。测试程序、socket、日志及脚本临时文件全部落根目录 `tmp/`，禁止使用系统 `/tmp`；夹具只清理自己创建的进程和 session。

以上验证针对实际代码重构：完成前运行 `bash scripts/gate.sh`、`bash scripts/build-core.sh`、`bash scripts/check-abi.sh`、对应 Apple 原生构建和适用的变异脚本，并运行真实本地/远端 tmux 与干净打包验证。未具备环境的用例明确记录，不能把 SKIP 计为通过。更新 AGENTS 的闸门与 CI 覆盖描述，使其与最终保留的能力一致。仅修改本计划文档时只检查内容、引用和 diff 格式，不触发构建或代码闸门。

## 后续后端

tmux 首版闭环之后再单独评估 zmx 等后端。共享 UI 只依赖会话标识、能力和操作结果，后端自己负责列表编码、命令、身份及恢复语义。添加新后端必须有真实本地/SSH 证据和独立能力声明，不恢复自有 daemon，也不为尚未实现的后端增加假成功入口。

协议与命令参考：[tmux 官方手册](https://github.com/tmux/tmux/blob/master/tmux.1)。实现时钉住验收版本；该链接的 master 行为不直接作为全部系统版本的兼容承诺。
