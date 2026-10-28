# 可选 tmux 实施清单

本清单落实 [重构计划](plan-optional-tmux.md)，以 `e000728` 为起点。目标是普通 PTY/SSH 默认连接、可选 tmux 后台会话和自有 sessiond 全面退役。保留 `vendor/ghostty` 的现有 pin；公开 ABI 删除与新增均按 BREAKING 提交。

## 依赖闭包与保留责任

| 模块 | 当前消费者 | 处理与验证 |
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

最低版本为 **tmux 3.6a**，同时探测所需命令/格式字段；旧版本返回明确不支持。后续扩大兼容范围须在实际版本验证后降低下限。2026-10-28 本机 `/opt/homebrew/bin/tmux` 为 3.6a；在项目 `tmp/` 下隔离 socket/config 验证了以下行为。临时命令研究记录为 `tmp/tmux-analysis/commands.json`；后续共享模块测试接管这些判据。

1. 路径：用户显式绝对路径优先；本机以登录环境和已知系统/包管理目录有界查找，不假定 GUI PATH 等于交互 shell；远端在服务器实际登录 shell 环境内解析后锁定绝对路径。路径未解析、启动文件污染输出、超时分别报错。查询不启动交互终端，不触碰用户已有格子。
2. socket：显式绝对路径优先；默认按 tmux 原有 endpoint 选择规则解析，成功后锁定返回的 `socket_path`。管理与 attach 使用同一个 endpoint，清除继承的 `TMUX`，不误用本机或其他格子的环境。查询与 attach 加 `-N`；只有显式新建允许启动 server，attach 不带 `-A`/`-d`。测试始终指定工程 `tmp/` 的 socket，不用系统临时目录。
3. 列表：`list-sessions -F` 固定输出 `pid/start_time/session_id/session_created/session_windows/session_attached` 与最后一列 `session_name`。3.6a 的 `session_check_name` 将控制字符可视化、点与冒号替换为下划线；实测 TAB/LF 变成字面 `\\t`/`\\n`，中文及空格保留。因此以 TAB 分字段、LF 分记录在此支持版本成立；原样保留名字，不反向解码。拒绝非法字段数、非数值、重复 ID、非预期控制字节、超限与非 UTF-8，不能把脏输出当空列表。
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
| 4 共享 tmux 模块 | 待实施 | 真 tmux、PTY exec、ABI/绑定与命令失败回归 |
| 5 后台会话 UI | 待实施 | 列表、新格子、断开保留、重新选择与配置提示 |
| 6 全量验收 | 待实施 | 原计划全部普通/后台/远端/竞态/清理/变异及干净打包证据 |
