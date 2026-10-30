# apple — macOS / iOS 宿主（SwiftUI）

## 当前结构（2026-10-30）

```text
apple/
├── GhostermCore/       SwiftPM；module map → core/include/ghosterm.h
├── GhostermTerminal/   SwiftPM；full libghostty + Metal、TerminalSurface/IME
└── GhostermApp/        SwiftPM；当前仅 macOS
    ├── Sources/GhostermApp/       SwiftUI/AppKit 页面与单窗口宿主
    ├── Sources/GhostermAppModel/  会话、认证、工作区、文件、设置模型
    ├── Tests/GhostermAppTests/
    └── Resources/Info.plist.in
```

没有 `Ghosterm.xcodeproj`；`.app` 由脚本组装。`Package.swift` 仅声明 macOS，iOS 尚无可用 target/引擎产物，不是已经共享的一套宿主。

`TabWindowController` 管单窗口；顶部/侧边导航共用标签模型，只有一个新建按钮，侧栏只列活动标签。
`WorkspaceLayout` / `WorkspaceHostView` 统一终端和文件窗格，旧独立双栏组件已退役。
终端通过 `SessionController` 接 Transport，SFTP 使用专用 SSH owner；普通连接与 tmux 共用异步认证。

共享 PTY 已在 `4924a2d` 修复子进程继承信号屏蔽的问题。窗口尺寸经引擎、会话 owner 和 PTY 更新后，SIGWINCH 能通知本地 tmux，或由本地 shell 内的系统 ssh 转发至远端 tmux；放大/缩小不再依赖额外敲键，宿主线程屏蔽集合不变。两条真实窗口回归与三组 PTY 继承变异的记录见[tmux 实施清单](../docs/reference/optional-tmux-implementation.md#后续-pty-尺寸通知修复2026-10-30)，不等于本轮完整宿主或 CI 重跑。

已交付与遗留见[路线图](../docs/reference/roadmap.md)、[能力矩阵](../docs/reference/capability-matrix.md)。

`7939ef8` 另交付 Bash/Zsh 提示符鼠标模式清理脚本：程序异常退出回到提示符时清理残留，避免鼠标移动变成 shell 输入。它位于 `scripts/shell-integration/`，通过显式安装器加载到实际运行 shell 的主机；当前 App/打包脚本没有自动安装或注入步骤，远端 tmux 需要远端安装。macOS 26.6.2 / tmux 3.6a 的本地和系统 SSH→隔离环回 sshd 已有复现及修复记录，八组变异通过；真实整机休眠与用户远端尚未验。安装方法、正常 TUI 边界及 Codex 未复现记录见[鼠标残留恢复](../docs/how-to/mouse-report-recovery.md)。这项提示符钩子不提供远端 cwd 上报，也不补齐 OSC 133 集成。

共享 `gt_config_edit` 已修复非连续重复指令误删中间行的问题，Apple 与 JNI 使用同一实现。`ConfigStore` 的强制备份/原子替换、私钥材料安全存储接线仍有缺口；Android 写盘事务不等于 macOS 写盘事务已修复。
`HostListModel.directives` 目前仍以 Swift 简化行扫描提供表单初值，`HostEditorSheet.save` 提交全部显示字段；不能把 Core 的稀疏编辑保证扩大为整个表单的“只改脏字段”。tab、`=`、引号、重复指令与未改保存的保真须独立补齐，见路线图 P0。
`AppModel.applyEdits/addHost` 先修改内存配置，再调用存储；写盘抛错时只向页面报错，尚未恢复内存模型。`HostListModel.reload` 也未过滤不含通配符的否定模式（如 `!secret`），不能把所有派生条目都视为可连接主机。
`SSHConfig.inspect(alias:)` 已接入并有值复制/来源转换的定向测试与变异，尚未接入 `HostEditorSheet`；存在 Swift 绑定不表示 macOS 表单已提供继承值展示、别名重命名或完整包含文件编辑。新增接口状态见[实现计划 §9.5](../docs/reference/android-implementation.md#95-字段来源与编辑归属2026-10-29)。
已新增 `SSHConfig.rename(alias:to:)` 绑定，配套 C 接口只修改内存配置，**不负责备份或写盘**；`HostEditorSheet` 仍仅在新建时显示别名输入框。共享改名能力不能抵消 macOS 的表单/事务缺口。改名切片为 73 个公共符号、Apple 71 个引用/2 个登记未用；连接检查切片为 74 / 72+2；IdentityFile 来源查询加入后当前为 75 / 73+2。改名的共享/绑定验收见[§9.9](../docs/reference/android-implementation.md#99-主机别名重命名2026-10-30)。

共享主机信任已修复标记误信任：`core/src/ssh.c` 保留标记，匹配的撤销密钥优先于任何顺序的普通信任记录；读取错误明确失败。CA-only、作用于当前主机的未知标记和已解析的 hashed 条目显式报不支持。CA 与明确匹配的普通记录并存时仅依据普通记录信任；尚未实现主机证书验证或哈希匹配。Swift 已接 `.revoked` 状态与拒绝文案，SFTP 产品入口通过真实 sshd 验证不再弹信任/凭据框。普通 SSH、SFTP、tmux 共用此认证流程，但本片未分别重跑 SSH 终端及 tmux UI；详见[共享主机信任验收](../docs/reference/android-implementation.md#922-a3-共享主机信任前置2026-10-30)。连接前配置检查已接共享判定，见实现计划 §9.24。

`SSHConfig.identities(alias:)` 已绑定共享 `gt_config_identity_at`，复制 IdentityFile 的原序条目及显式/继承/Include 来源，遇到 Match 或相关非法参数明确抛错。此为 Android 主机身份关联的共享前置；macOS 表单也尚未消费，不能将绑定记为 GUI 已支持。共享 8 C + 1 Swift 变异、Swift 7 项配置定向及两端构建证据见[§9.34](../docs/reference/android-implementation.md#934-identityfile-来源查询与平台绑定2026-10-30)。

UI 已知坑见 [Apple UI 记录](../docs/reference/apple-ui-pitfalls.md)。

共享认证后续修复内存 OpenSSH 私钥的错误口令重试：libssh2/OpenSSL 返回 `FILE` 时，仅私钥认证路径按凭据失败请求替换材料，握手/协议错误仍保持原分类。共享真实 sshd 的加密 ed25519 回归及一组变异、连接器 18 组、macOS App 强制重新链接与既有 11 组产品认证变异通过；Android 另验实际密码/口令页面，不据此宣称已新增 Apple 加密密钥 UI 用例或完成私钥安全存储。记录见[§9.31](../docs/reference/android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。

主机信任写盘错误现已阻断连接：共享 `kh_append` 检查写入与关闭结果，同步/异步认证均传播错误；macOS SFTP 在接受未知主机但保存失败后明确提示“无法保存主机信任记录，连接已停止”，不请求凭据。真实文件错误及产品路径验收见[§9.23](../docs/reference/android-implementation.md#923-主机信任保存失败阻断2026-10-30)。当前追加没有全文件原子事务或部分写入回滚保证，不能把错误可见性扩大为这类能力已完成。

## 当前功能与代码入口

下表链接定位当前生产代码；2026-10-30 复核的是实际接线与限制。旧计划中的独立双栏和多窗口标签组仅用于历史追溯，新增 Android 证据不改变 macOS 完成状态。

| 功能 | 代码入口 | 当前边界 |
|---|---|---|
| 顶部/侧边标签、窗口与焦点 | [TabWindowController](GhostermApp/Sources/GhostermApp/TabWindowController.swift)、[TabStrip](GhostermApp/Sources/GhostermApp/TabStrip.swift)、[Sidebar](GhostermApp/Sources/GhostermApp/Sidebar.swift) | 同一窗口内切换，唯一新建入口；不跨启动恢复标签/布局 |
| 终端与文件窗格 | [WorkspaceLayout](GhostermApp/Sources/GhostermAppModel/WorkspaceLayout.swift)、[WorkspaceView](GhostermApp/Sources/GhostermApp/WorkspaceView.swift) | 同一布局、身份与宿主；真实拖动等遗留见工作区交接 |
| 连接与配置编辑 | [ConnectionChooser](GhostermApp/Sources/GhostermApp/ConnectionChooser.swift)、[HostEditorSheet](GhostermApp/Sources/GhostermApp/HostEditorSheet.swift)、[ConfigStore](GhostermApp/Sources/GhostermAppModel/ConfigStore.swift) | 主机不放侧栏；表单从块内指令取初值，已有别名不提供改名字段；尚无继承来源展示，配置事务仍有缺口 |
| SSH 认证与凭据 | [SSHAuthenticationFlow](GhostermApp/Sources/GhostermAppModel/SSHAuthenticationFlow.swift)、[SSHCredentialSelector](GhostermApp/Sources/GhostermAppModel/SSHCredentialSelector.swift) | 可取消认证；Keychain 存口令/密码，生产私钥仍读受限文件 |
| 文件与传输 | [FilesPanelView](GhostermApp/Sources/GhostermApp/FilesPanelView.swift)、[TransferManager](GhostermApp/Sources/GhostermAppModel/TransferManager.swift) | 本地↔远端；尚无四方向统一复制队列；暂停/继续/取消忽略后端错误仍改状态，未知结果映射尚待修复 |
| 菜单与偏好 | [AppMenu](GhostermApp/Sources/GhostermApp/AppMenu.swift)、[SettingsStore](GhostermApp/Sources/GhostermAppModel/SettingsStore.swift)、[Shortcuts](GhostermApp/Sources/GhostermAppModel/Shortcuts.swift) | 正常设置已持久化并重载已有终端；存储异常/重复键及过时提示仍待修；Ctrl+Tab / Ctrl+Shift+Tab 循环切换标签 |
| 配置到连接 | [HostListModel](GhostermApp/Sources/GhostermAppModel/HostListModel.swift)、[SSHConnectionTarget](GhostermApp/Sources/GhostermAppModel/SSHConnectionTarget.swift) | 共享配置检查的拒绝原因随连接目标传递，在 DNS 前阻断；高级选项尚不执行 |
| 终端文本与剪贴板 | [TerminalSurfaceView](GhostermTerminal/Sources/GhostermTerminal/TerminalSurfaceView.swift)、[shim](GhostermTerminal/Sources/CGhostty/shim.c) | IME、标准菜单 copy/paste 已接；引擎剪贴板回调与粘贴确认仍待补，列为 P1 |

**终端标准复制粘贴已接通，完整剪贴板能力仍有余项**：`beffb49` 为 `TerminalHostView` 增加 `copy:` / `paste:`，复制读取 full 引擎真实选区并写入 `NSPasteboard.general`；无选区时保留剪贴板。粘贴读取系统字符串并经 `TerminalSurface.sendText` 使用引擎的粘贴编码。专用 `TerminalClipboardTests` 驱动离屏真实窗口、鼠标选区和 NSMenu 快捷键，覆盖无选区、复制范围以及中文/多行 bracketed paste；测试显式将菜单 target 设为窗口 firstResponder，不能据此宣称全局主菜单自动查找目标已实测。四组变异脚本已入库，仍属手工闸门；本次文档复核未重跑它们。

`CGhostty/shim.c` 的引擎读取回调仍返回不可用，写入/确认回调为空；上述 Cocoa 菜单路径并未补齐 OSC 52、选中自动复制或粘贴确认。文件路径复制与文件传输也有各自的接线，不能互相替代验收。

修复顺序与验收触发场景见[macOS 实施顺序](../docs/reference/roadmap.md#macos-修复顺序与验收触发场景)。当前代码注释中的“安全写回”“原子写”等描述是目标契约，实际 `try?` 备份及 remove→move 路径尚未兑现，不能把注释当作通过证据。

**配置求值不等于连接执行**：共享 `gt_config_check_connection` 检查当前普通 SSH 档位可执行的显式指令，并返回关键词及来源；`HostListModel` 在求值同一份配置时复制结果，`SSHConnectionTarget` / `RemoteTarget` 携带该快照。`SSHAuthenticationFlow.start` 在创建定时器及解析器前拒绝不支持项，普通 SSH、独立/内嵌 SFTP 和 tmux 共用。ProxyJump、转发、环境注入等能力仍未实现，不会因检查接口存在而变为支持。

校验遵守已知标量 first-wins、累积列表、Host 否定模式与 Include 来源；未知适用指令和任何 Match 条件保守拒绝，仍保留配置原字节，不执行 Match exec。普通字段只接受本档位实现的值，显式关闭部分未实现功能可通过；详细准入表、保守边界和实际验证见 [§9.24](../docs/reference/android-implementation.md#924-共享连接配置准入与两端接线2026-10-30)。检查不等于完整 OpenSSH 兼容，也不新增跳板/转发实现。非法端口可保留在配置编辑器中，转换为 UInt16 不再崩溃，其配置问题会阻止建连。

### 与 Android 最终方案的差异

| 操作 | macOS 当前实现 | Android 方案与实现边界 |
|---|---|---|
| 文件打开与选择 | 原生列表单击选择、双击打开；右键“拷贝路径”只复制路径文本 | 方案为点按打开、长按勾选；文件页尚未实现 |
| 文件复制 | 本地↔远端的传输队列、拖放与冲突对话框；无四方向统一复制清单 | 常驻待粘贴清单和四方向复制属于 A4–A6；终端文本剪贴板已接入，不等于文件复制完成 |
| 普通 shell 退出 | `handlePaneExit` 移除终端叶子，其余文件/终端叶子保留；最后一片退出关闭标签 | 正常 EOF 后保留终端内容供浏览/复制，显式关闭再释放；错误退出不承诺保留 |
| 设置 | 通用、外观、键盘、配置文件四页 | 最终设计五类、16 项偏好；已提交配置/关于入口，身份设置首片已验（§9.33），完整设置留给 A7 |
| 后台会话 | 显式 tmux 入口已交付，关闭仅断开客户端 | A8 尚未开始；本地终端返回首页不等于进程死亡后保活 |

目录传输也不等于完整目录镜像：`TransferManager.expandDirectory` 下载时排除符号链接，上传枚举遇到符号链接时跳过且不递归。被排除条目不生成子任务，也不进入父任务的跳过计数；只有链接的目录可能按空文件集合报完成。现有目录下载测试明确只提交两个普通文件、排除链接，2026-10-30 定向重跑通过；它使用 fake 后端，验证的是展开/汇总模型，不是真实 SSH 目录复制。需要补齐用户可见的跳过原因和汇总结果，不能将“完成”解释为原目录所有条目均已复制；见路线图的目录复制完整性项。

设置窗口实际有通用、外观、键盘、配置文件四页。「配置文件」页的全文编辑框编辑 `ghostty.conf`，SSH/known_hosts 只显示路径和 Finder 入口，不是 SSH 全文编辑页。该页保存调用 `replaceTerminalConfig` 重载引擎；“只有新开的标签页会读到改动”的 UI 提示仍待修正，文档不沿用这条过时文案。

终端配置存储与 SSH 配置存储须分开核对：`SettingsStore.saveTerminalConfig` 的备份失败会抛错，提交使用 `replaceItemAt`；没有 `ConfigStore` 的先删后移流程。但 `rawConfig()` 将目录/读取错误折为空文本，加载时 stat 缺失后即使文件被外部创建也不报冲突。`merging` 只改首个同名键，而 `value` 读取最后一个：两行 `font-size = 10` / `font-size = 12` 改成 18 后，回读仍为 12。2026-10-30 直接编译生产 `SettingsStore.swift` 的隔离探针已复现这些结果；正常单键改为 18 和首次创建均成功。这是存储/文本处理证据，未替代真实引擎或页面验收。

`AppModel.applyTerminalSettings` 写盘后才请求引擎重载，重载失败没有恢复旧文件；`replaceTerminalConfig` 尝试恢复旧文件但忽略恢复错误。因此“错误已提示”不等于“磁盘与已有 surface 都保持旧值”。修复与失败注入条件纳入[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

设置说明还有三类过时文案：外观页称已有终端保持旧字号；标题栏称动作仅在当前格显示（实际还有悬停/键盘入口）；通用页仍描述恢复会话及关闭必结束。当前普通启动不恢复会话，显式 tmux 关闭只断客户端。文档按真实代码说明，这些页面提示单列待修，不记作已经改好。

关于窗口的更新检测只在用户点击时联网。`UpdateCheck.swift` 已处理缺字段、无本机 commit、渠道不符和网络错误，但远端 commit 的格式、摘要字符集及下载来源尚未严格校验；按 commit 不同提示“有新版”也不表示已经比较提交祖先关系。该缺口与安装脚本的 SHA256/签名校验分开，见[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

### 引擎配置与宿主动作边界

`SettingsStore.terminalSettingKeys` 是配置元数据，生产 `SettingsView` 没有遍历它生成全部控件。该目录中的 `copy-on-select`、`confirm-close-surface` 不应被描述成现有产品已经兑现的两个开关。Ghostty 配置页可以提交引擎语法，但语法被接受不代表平台回调已接通。

当前 `CGhostty/shim.c` 的系统剪贴板读回调返回不可用，写入/确认回调为空；因此引擎的选中自动复制没有系统剪贴板落点。`close_surface_cb` 也为空，`TerminalPane.closeNeedsConfirmation` 按状态与前台作业判断，`TerminalTab` 再汇总文件连接与传输；tmux 客户端关闭另行处理。宿主没有读取 `confirm-close-surface` 来改变这些判据。不要从引擎配置声明推断该键控制标签、窗格或窗口关闭。

这是源码接线核对，未新增开关行为测试。标准菜单复制粘贴已经接通，但 `paste:` 当前直接发送，不显示危险/多行粘贴确认；引擎发起的剪贴板请求、自动选中复制及确认/取消仍按路线图 P1 实施；若将来提供关闭策略偏好，需要先明确其与未知作业、传输及 tmux 的关系并独立验收。

### 文件查询与线程边界

[SFTPFileBackend](GhostermApp/Sources/GhostermAppModel/SFTPFileBackend.swift) 使用串行队列保证同一后端调用保序，但公开方法是 `queue.sync`。`@MainActor TransferManager.start/pause/resume/cancel` 同步调用该后端，整次远端 walk/search 又占用同一队列。因此入队、冲突查询或控制操作可能等待网络或正在执行的遍历；不能将“有后台队列”解释为“主线程不会等待”。终端 `SessionController.stop` 的有界验证不覆盖这条文件路径。

`exists` 目前将任何 `lstat` 异常转换为 false。上传冲突检查、自动编号和目录创建据此判断不存在，无法区分确实不存在、无权查看与连接失败。需改为可区分三种结果的查询，查询失败不得继续当作空目标提交；即使查询修复，非覆盖创建仍须由共享写入契约保证。

[FilesPanelModel.search](GhostermApp/Sources/GhostermAppModel/FilesPanelModel.swift) 已提供搜索入口和 exec/walk 档位显示，但错误与取消尚不完整：exec 打开阶段任意异常都会回退 walk；`readFind` 忽略 stderr、没有调用现有 `SSHExec.exitStatus()`，条目 `lstat` 失败也被跳过，结束后仍返回 exec 档。远端命令失败可能显示零条或部分搜索结果为完成。页面 `cancelSearch` 只更新 token 并清列表，传给后端的回调仍返回 true；它阻止旧结果显示，尚未取消远端工作，无输出时的 read timeout 会继续循环。

文件错误边界不限于 SFTP。`FilesPanelModel.loadTreeChildren` 把列目录异常转成空数组并缓存；后续展开因缓存已存在不再读取，失败节点会看起来像空目录。`runLocalSearch` 用 `try?` 忽略遍历错误，随后仍标完成；取消只改变结果 token，visitor 仍返回 true。`setPermissions` 虽记录子项 chmod 错误，却忽略 `walk` 自身抛出的错误，可能只改了根或部分条目便正常刷新。`FileBackend.swift` 中的本地后端还会跳过 lstat 失败的条目，枚举未接入逐项错误报告。普通列表导航的 catch 会显示错误，不能把树/搜索的缺口扩大成所有目录错误都被吞掉。

后续需为树节点提供可重试的错误状态、搜索提供失败/部分结果、递归权限汇总扫描与修改结果；这与终端 I/O 和传输队列控制是不同验收面。当前只是源码核对，没有新增文件系统拒绝/扫描失败的运行证据。

以上为当前源码调用链结论，尚无本轮慢网络/失败命令的端到端验收。修复顺序为查询三态与异步 owner、远端退出状态和取消，再回归传输冲突/控制与页面切换；具体退出条件列在[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

### 传输提交、调度与重试边界

共享 [SFTP job 实现](../core/src/sftp.c) 仍有结果完整性缺口：下载在远端打开前就用 `wb` 打开本地目标，远端打开失败不保证旧目标不变；上传和下载均未检查 `libssh2_sftp_close` / `fclose` 的返回值，缓冲关闭失败可能未体现在终态中。现有队列成功状态不保证原目标受到事务保护，进度 100% 也不是提交证据。应先写任务暂存、核验关闭再提交，并补真实故障回归；这是源码核对发现，尚未修复，不能借已通过的正常传输测试消除此项。

目录下载与递归权限传入 `maxDepth: -1`，Core 当前只裁剪超过 64 的正深度，负值遍历没有硬深度检查；公开注释的安全护栏尚未兑现。目录深度预算、超限错误及部分结果必须补齐，详见路线图共享缺口。

`CoreEventPump` 是唯一事件消费者，`TransferManager` 是全局任务列表；它们不是全局并发调度器。每个 `gt_sftp_t` 有自己的 FIFO worker，同一会话的 job 顺序执行，不同连接没有统一并发上限；`start` 提交拿到 job ID 后即标记 running，不能从页面上的 running 数量推断正在传字节的数量。

任务执行期间强持有 backend；`finish` 释放该引用，`retryBackend` 只保留弱引用。因此重试复用尚存活的原端点，不会自动重连、重新认证或从断点续传。端点已经释放时显示“原传输端点已关闭，无法重试”；端点仍在但已断线也可能再次失败。关闭文件窗格不取消在途任务，并不意味着任务结束后仍能无条件重试。Android A6 必须单独实现端点重新获取与冲突复核，不能照搬这一生命周期作为完整重试契约。

### 递归权限与链接边界

`FilesPanelModel.setPermissions` 对 walk 返回的每个条目调用 chmod，没有排除 `.symlink`。本地 `LocalFileBackend.chmod_` 先 lstat 再调用 `Darwin.chmod`，没有“不跟随链接”的修改路径；目录内链接因此可能把权限修改引向选中目录以外的目标。远端调用 `gt_sftp_chmod` → `libssh2_sftp_setstat`，也没有单独的链接处理契约。目录复制跳过链接的规则不适用于当前权限操作，不能把两者混称为“递归操作都不跟随链接”。

这是源码核对，未执行权限修改探针。后续应明确链接跳过/不支持策略，汇总跳过与扫描失败，并以目录内链接指向隔离外部文件的场景验证外部目标权限不变；路径检查与实际修改之间的替换竞态也须独立处理。正常文件 chmod 作为对照，不能只增加一条 UI 提示便记为完成。

### Quick Look 预览边界

当前 `WorkspaceFileView` 将本地预览接到 `QuickLook.toggle`，远端接到 `QuickLook.previewRemote`。后者创建 `NSTemporaryDirectory()/ghosterm-preview/<UUID>/<文件名>`，经 `TransferManager` 下载，只有 `.done` 回调才打开预览；每次远端请求都会生成新副本。测试/开发时须将 `TMPDIR` 指向工程 `tmp/`，不能假定这条生产路径固定使用仓库目录。

当前代码没有预览大小限额、应用层缓存回收或请求代次校验；关闭文件窗格不会撤销该回调，多次预览的完成顺序也没有保护。`QuickLookPanel` 替换 URL 或隐藏窗口时没有删除副本；目录创建错误被 `try?` 忽略，后续下载失败可进入传输队列错误状态。不能承诺关闭预览立即清理、过期下载不会再弹窗，或大文件已受限。`FilesDragDropTests.testQuickLookTempPath` 仅验证路径形状，真实 QLPreviewPanel 弹出与这些生命周期场景没有因此通过。

修复须为预览建立请求身份、大小/磁盘预算和副本所有权，晚到结果不得覆盖当前预览；失败、取消及预览释放后的清理不能删除其他任务或原文件。Android A4 另按其受控缓存和只读打开契约实现，不直接复制 macOS 当前流程。待办统一见[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

## 构建与验证

在仓库根运行；full libghostty 产物须先由 `scripts/build-ghostty.sh` 生成：

```bash
bash scripts/gate.sh
bash scripts/check-abi-apple.sh
swift build --package-path apple/GhostermApp
bash scripts/apple-app-gate.sh
```

原生编译、完整宿主测试、像素/系统交互、变异验证分别记账。`apple-app-gate.sh` 的 SSH 部分需隔离 sshd 成功启动；看到 SKIP 要保留原因。已接入 CI 不等于对应提交已在 macOS 15 运行通过；当前已知本机与历史结果见[路线图验收记录](../docs/reference/roadmap.md#验收记账与历史记录)。

当前 full workflow 尚未对 `apple-app-gate.sh` 的全部跳过输出统一判失败；sshd 启动失败时，该脚本可以只跑其余用例后返回成功。图标缺依赖失败及打包/认证变异的 SKIP 守卫不能替代这项检查，后续需按必跑项目收口。

另一个独立缺口是失败传播：`apple-app-gate.sh` 使用 `set -uo pipefail`，没有 `-e`，调用 `check-abi-apple.sh` 后也没有检查返回值。因此 ABI 子检查失败本身不会终止这个脚本，后续构建/测试若成功，仍可能打印 `APPLE APP GATE PASS`。目前应单独核验上面的 ABI 命令结果；该源码结论不表示已复现某次 CI 假绿。修复需要隔离注入 ABI 失败并证明宿主闸门非零退出，保留正常本地构建对照，不能只补日志文字。

仅安装 Command Line Tools 时，`swift build` 成功不代表 XCTest 可用；本机曾在测试编译时报 `no such module 'XCTest'`。有完整 Xcode 时可对单次命令设置 `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`，临时目录设为工程 `tmp/`，无需更改全局 `xcode-select`。共享 C archive 更新后的绑定测试需确保重新链接；旧 SwiftPM 测试可执行文件不能作为新 C 实现的证据。

## 打包（macOS）

```bash
bash scripts/package-mac.sh                       # dist/Ghosterm.app + .dmg + .zip + SHA256SUMS
bash scripts/package-mac.sh --app-only            # 只要 .app
bash scripts/build-mac-app.sh                     # 快速路径（薄壳，内部同一个脚本）
bash scripts/package-mac.sh --version 0.1.0 --sign "Developer ID Application: …" --notarize <profile>
```

口径（细节见 `assets/icons/README.md` 与脚本内注释）：

- **bundle 契约的唯一事实来源**是 `GhostermApp/Resources/Info.plist.in`
  （`@VERSION@` / `@BUILD@` / `@MIN_OS@` 由脚本填充）。Apple 端还是 SwiftPM 包，
  `.app` 由脚本组装；将来落 Xcode 工程时这份 plist 是迁移基准。
- **图标**取自 `assets/icons/apple/AppIcon.appiconset/`（母版 SVG 的派生物），
  打包时用 `iconutil` 合成 `AppIcon.icns`——不在打包脚本里重画图标。
- **自包含**由 `scripts/lib/embed-dylibs.py` 保证：递归收依赖闭包
  （libssh2 → libssl/libcrypto）、改 install name、复验无绝对路径，并做
  运行时断言（`DYLD_PRINT_LIBRARIES` 里不得出现 Homebrew 路径）。
- 签名顺序是「先内嵌库、再 .app」；BUILD-INFO.txt 必须在签名**之前**写进
  bundle，否则 code seal 失效（这条被 dmg 挂载复验抓到过）。

参考：[Termini](https://github.com/hudsonkit/Termini)（MIT）、[iGhostVT](https://github.com/owngoal-dev/iGhostVT)（MIT）、[VVTerm](https://github.com/vivy-company/vvterm)（GPL-3.0）。
