# apple — macOS / iOS 宿主（SwiftUI）

代码核对：2026-10-31，macOS 源码与 `72bfe1b` 一致；Android 进度同步至 §9.64。当前产品是 **macOS 原生终端与文件工作区**；iOS 尚无可用 target。普通 PTY/SSH、标签/分屏、SFTP 主流程与显式 tmux 已交付；配置保存、私钥存储、文件异常处理和发行仍有具体缺口，见[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

**共享接口与产品进度分开**：Swift 已绑定受控 SFTP 打开、浏览、预览、独占创建、读取到 fd 和非覆盖发布；桌面文件队列与 Quick Look 仍使用旧 job。当前为 88 个公共 ABI、Apple 86 项引用/2 项登记未用。实际调用链见下方[接线表](#共享文件接口与产品接线)，不能把 Android 私有目录提交或设备回归记为 macOS 功能完成。

Android §9.64 的本机/SAF 单目录创建为平台数据层能力，不改变 macOS `mkdir` 调用、目录重试或旧传输队列。桌面异常处理与迁移仍按本页边界验收。

最近相关验证：§9.57 完成发布接口专项及五组 Swift 变异；§9.58–§9.64 分别完成 macOS 原生构建、ad-hoc 包及依赖/引擎校验。它们没有新增完整 macOS 宿主或系统交互证据；详细范围与失败记录分别保留在[§9.57](../docs/reference/android-implementation.md#957-a5-远端非覆盖发布2026-10-31)、[§9.58](../docs/reference/android-implementation.md#958-a5-应用私有目录非覆盖发布2026-10-31)、[§9.59](../docs/reference/android-implementation.md#959-a5-saf-新文档写入2026-10-31)、[§9.60](../docs/reference/android-implementation.md#960-a5-复制来源清单与文件页面2026-10-31)、[§9.61](../docs/reference/android-implementation.md#961-a5-粘贴请求与来源树扫描2026-10-31)、[§9.62](../docs/reference/android-implementation.md#962-a5-目标预检与冲突元数据2026-10-31)、[§9.63](../docs/reference/android-implementation.md#963-a5-请求内冲突决策与执行清单2026-10-31)、[§9.64](../docs/reference/android-implementation.md#964-a5-本机与-saf-目录创建2026-10-31)及[工作区交接](../docs/reference/workspace-handoff.md)。

## 当前结构（2026-10-31）

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
多条身份路径有具体的表单缺口：`loadInitial` 对每条 `IdentityFile` 都重新赋值给同一个输入框，最后只显示最后一条；保存再提交这一列表。因此原有两行身份路径不能因输入框标着“逗号分隔多条”就视为会无损保留。新增的共享来源查询尚未替换此读取路径；修复需覆盖未改保存、多条身份顺序、继承/Include 和路径引用，不能只增加控件。
`AppModel.applyEdits/addHost` 先修改内存配置，再调用存储；写盘抛错时只向页面报错，尚未恢复内存模型。`HostListModel.reload` 也未过滤不含通配符的否定模式（如 `!secret`），不能把所有派生条目都视为可连接主机。
主机列表还没有去重：Core 的 `gt_config_host_pattern` 按块枚举每个模式，`HostListModel.reload` 逐项追加，`HostEntry.id` 又直接使用 pattern；两个同名 Host 块会生成相同 ID 的列表项，连接选择器和管理器均用这些 ID。应把连接目标列表与配置块列表分开：连接目标去重，配置块仍保留独立来源和歧义编辑保护。此处为源码核对，未新增重复行的原生 UI 实测。
`SSHConfig.inspect(alias:)` 已接入并有值复制/来源转换的定向测试与变异，尚未接入 `HostEditorSheet`；存在 Swift 绑定不表示 macOS 表单已提供继承值展示、别名重命名或完整包含文件编辑。新增接口状态见[实现计划 §9.5](../docs/reference/android-implementation.md#95-字段来源与编辑归属2026-10-29)。
已新增 `SSHConfig.rename(alias:to:)` 绑定，配套 C 接口只修改内存配置，**不负责备份或写盘**；`HostEditorSheet` 仍仅在新建时显示别名输入框。共享改名能力不能抵消 macOS 的表单/事务缺口。改名切片为 73 个公共符号、Apple 71 个引用/2 个登记未用；连接检查切片为 74 / 72+2；IdentityFile 来源查询切片为 75 / 73+2；主机信任快照切片为 79 / 77+2，事务/移除绑定为 80 / 78+2，已验通道取消切片为 81 / 79+2；受控 SFTP 切片为 82 / 80+2（§9.47），接口协议验证不表示产品接线已完成。改名的共享/绑定验收见[§9.9](../docs/reference/android-implementation.md#99-主机别名重命名2026-10-30)。

共享 TOFU 写入现与 `KnownHosts.remove(path:expected:index:)` 使用同一事务；删除绑定返回 status、committed 与 conflict，提交后的 IO 错误不能解释为未改动。确认期间重新核对撤销/冲突，备份、锁、原子替换与故障验证见[§9.37](../docs/reference/android-implementation.md#937-主机信任写入事务与移除绑定2026-10-30)。本端仍没有信任记录管理页面；这项 shared known_hosts 事务不修复 `ConfigStore` 的 SSH 配置保存缺口。

共享主机信任已修复标记误信任：`core/src/ssh.c` 保留标记，匹配的撤销密钥优先于任何顺序的普通信任记录；读取错误明确失败。CA-only、作用于当前主机的未知标记和已解析的 hashed 条目显式报不支持。CA 与明确匹配的普通记录并存时仅依据普通记录信任；尚未实现主机证书验证或哈希匹配。Swift 已接 `.revoked` 状态与拒绝文案，SFTP 产品入口通过真实 sshd 验证不再弹信任/凭据框。普通 SSH、SFTP、tmux 共用此认证流程，但本片未分别重跑 SSH 终端及 tmux UI；详见[共享主机信任验收](../docs/reference/android-implementation.md#922-a3-共享主机信任前置2026-10-30)。连接前配置检查已接共享判定，见实现计划 §9.24。

`SSHConfig.identities(alias:)` 已绑定共享 `gt_config_identity_at`，复制 IdentityFile 的原序条目及显式/继承/Include 来源，遇到 Match 或相关非法参数明确抛错。Android 已在其主机编辑/认证/详情使用此来源查询；macOS 表单尚未消费，不能将绑定记为 GUI 已支持。共享 8 C + 1 Swift 变异、Swift 7 项配置定向及两端构建证据见[§9.34](../docs/reference/android-implementation.md#934-identityfile-来源查询与平台绑定2026-10-30)。

Android 主机关联写入的 `IdentityFile "ghosterm-identity:<UUID>"` 是该安装的加密容器引用。当前 `SSHCredentialSelector.Storage.readKey` 按文件路径读取，不解析这种引用；复制 Android 配置到 macOS 不会迁移密钥，不能承诺自动关联或直接登录。普通 IdentityFile 路径仍按现有 macOS 规则处理；后续跨端导入需明确重新关联及不支持提示，不把路径读取失败当成可用身份。Android 的选择、保存事务和引用列表验收见[§9.35](../docs/reference/android-implementation.md#935-主机身份关联与引用详情2026-10-30)，不改变本端配置/私钥待办。

`KnownHosts.records` 新增只读绑定，复制原始字节范围、marker 与字段形态，保留畸形记录；两项 Swift 定向和一组绑定变异通过。只读接口本身不读取/写回文件，也未接本端管理页面；写盘随后由 §9.37 的共享事务实现。认证枚举与快照已共用词法读取，原有 ed25519/ECDSA sshd 的 20 组信任变异重跑通过，范围见[§9.36](../docs/reference/android-implementation.md#936-主机信任记录快照与平台绑定2026-10-30)。

UI 已知坑见 [Apple UI 记录](../docs/reference/apple-ui-pitfalls.md)。

共享认证后续修复内存 OpenSSH 私钥的错误口令重试：libssh2/OpenSSL 返回 `FILE` 时，仅私钥认证路径按凭据失败请求替换材料，握手/协议错误仍保持原分类。共享真实 sshd 的加密 ed25519 回归及一组变异、连接器 18 组、macOS App 强制重新链接与既有 11 组产品认证变异通过；Android 另验实际密码/口令页面，不据此宣称已新增 Apple 加密密钥 UI 用例或完成私钥安全存储。记录见[§9.31](../docs/reference/android-implementation.md#931-加密私钥重试与密码产品流程2026-10-30)。

主机信任保存失败会阻断连接：`ssh.c` 的 `kh_append` 当前调用 `gt__kh_transaction`，已替代 §9.23 当时的直接追加。同步/异步认证均传播失败，macOS SFTP 显示“无法保存主机信任记录，连接已停止”，不请求凭据。确认后还会重读规则：新增撤销、密钥变化或不支持规则会阻断；同一公钥被另一连接先保存时不重复追加。共享实现见[§9.37](../docs/reference/android-implementation.md#937-主机信任写入事务与移除绑定2026-10-30)，早期错误传播证据保留在[§9.23](../docs/reference/android-implementation.md#923-主机信任保存失败阻断2026-10-30)。原子替换成功后的同步/回读失败可能已经改变文件，不能将“认证停止”理解为“信任记录必定未保存”；不宣称任意外部写者安全或断电恢复已验。

### 三种配置文件的保存边界

| 文件 | 当前生产写入路径 | 尚未完成的保障 |
|---|---|---|
| SSH `config` | `ConfigStore.save`；备份错误被忽略，删除旧文件后移动新文件 | 强制备份、原子替换、冲突与失败后内存恢复；M3 仍未收口 |
| `known_hosts` | `ssh.c` → `known_hosts_store.c`；TOFU 与删除绑定共用协调、备份和原子替换 | 本端管理 UI 未接；备份清理、任意非协作写者与断电边界未收口 |
| Ghostty `ghostty.conf` | `SettingsStore.saveTerminalConfig`；备份失败抛错，已有文件经 `replaceItemAt` 替换 | 重复键、读取错误、外部新建冲突与引擎拒绝后的恢复仍待修 |

这些路径的源码与专项证据分开记账；共享信任事务通过不改变 M3 配置编辑和 M4.5 终端设置的进度。macOS 设置页目前仅展示 known_hosts 路径和 Finder 入口，没有记录列表、指纹详情或删除确认。

## 共享文件接口与产品接线

生产调用方已按 `5c6b1cd` 复核；共享接口最近的新增验收仍为 §9.57。共享 Core 已验接口、Swift 包装层和 macOS 产品各自有完成边界；Android 的页面或设备通过不能勾选 macOS 产品迁移。

| 操作 | Core / Swift 已交付 | macOS 实际消费者 | Android 实际消费者 |
|---|---|---|---|
| SFTP 打开 | `gt_ssh_sftp_controlled` / 带预算及取消的 `sftp` 重载 | `AppModel` 文件工厂仍调无参 `ssh.sftp()` | `files_jni.c` 专用 owner，文件页面已接 |
| 目录浏览、文本预览 | `gt_sftp_browse_controlled` / `browse`；`gt_sftp_preview_controlled` / `preview` | `SFTPFileBackend` 仍拆分 list/realpath；Quick Look 经旧下载 job | `RemoteFileSession` → repository → 只读文件页面 |
| 独占暂存创建 | `gt_sftp_create_controlled` / `createExclusive` | 尚无产品调用，原队列仍用路径式 upload | JNI/Kotlin 与匿名快照集成已验，复制任务未接 |
| 远端读取到 fd | `gt_sftp_receive_controlled` / `receive` | `SFTPFileBackend.download` → `sftp.download` → 旧 job | `TransferStagingStore.receive` 已验，应用级任务 store 未接 |
| 最终非覆盖发布 | `gt_sftp_publish_controlled` / `publish`；状态、远端错误码、总预算与取消 | Swift 绑定已验；桌面队列仍无产品调用 | JNI/Kotlin `RemoteFileSession.publish` 已验；粘贴任务和执行页面未接 |
| 目录创建 | 旧 `gt_sftp_mkdir` / `mkdir` 已有；受控目录创建接口尚无 | `SFTPFileBackend.mkdir` 经同步队列调用旧入口；目录展开会复用已存在目标 | §9.64 的 `FileDirectoryCreator` 只支持应用 HOME/SAF；`RemoteFileSession` 无 mkdir，任务与页面未接 |

Android §9.59 另新增平台数据层 SafFilePublisher，经 ContentResolver 创建并回读新文档；不提供 Swift 绑定，也没有改变桌面队列、Quick Look 或文件复制交互。Android §9.60 已接文件清单产品页面，但不调用 Swift 或旧 job；本端四方向统一队列仍未交付。Android §9.61 的粘贴请求与来源扫描只消费平台目录读取函数，不改变 macOS 的 walk、旧 job、取消或提交保证；本端仍须独立迁移与验收。§9.62 的 Android 目标预检也不替换本端 `exists`、冲突全局策略或同步队列；创建权限未知与查询失败仍需本端独立修复。

§9.57 已补齐发布接口及两端绑定，验证范围见 [实施记录](../docs/reference/android-implementation.md#957-a5-远端非覆盖发布2026-10-31)；此前未绑定草稿的对拍失败只属于历史复核。接入受控文件接口前，桌面需先建立专用 owner 和任务 fd 暂存，保证网络/队列等待不占主线程，再逐项迁移取消、进度、错误与最终发布。不得把新接口混入同一连接上仍运行旧 job 的流程。完成条件与优先级见[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

## 当前功能与代码入口

下表链接定位当前生产代码；2026-10-31 复核的是实际接线与限制。旧计划中的独立双栏和多窗口标签组仅用于历史追溯，新增 Android 证据不改变 macOS 完成状态。

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

### SSH 地址选择与取消边界

macOS 的 [SSHAddressResolver](GhostermCore/Sources/GhostermCore/SSHAddressResolver.swift) 使用异步 DNSService，支持数字 IPv4、IPv6 和 IPv6 scope；默认先查询 IPv4。只有所选地址族返回 `kDNSServiceErr_NoSuchRecord` 才查询另一族，首个有效地址到达即完成并释放 DNS 查询。因此这是 DNS 缺记录时的换族，不是 TCP 失败后的备用地址重试，也没有并行竞速连接。

[SSHAuthenticationFlow](GhostermApp/Sources/GhostermAppModel/SSHAuthenticationFlow.swift) 在 owner 队列建立 60 秒总预算，包含 DNS、主机验证与凭据等待；解析成功后仅将剩余预算交给一个认证器。取消撤销 DNS completion、提示请求及认证器，旧响应受请求 ID 和取消状态检查。普通 SSH、独立文件连接及 tmux 共用该认证流程；任一选中地址 TCP 失败就结束本次流程，不会继续尝试同一域名的其他地址。其后的 shell/SFTP/tmux 通道生命周期须分别验收，不能将认证预算扩大为连接全过程所有 I/O 都在 60 秒内结束。

现有 `SSHAddressResolverTests` 覆盖数字地址/scope、真实 localhost DNS、非法参数及取消释放，不包含多地址 TCP 切换。Android 已由 native owner 接入候选推进，定向与变异范围见 [§9.40](../docs/reference/android-implementation.md#940-ssh-候选地址与-tcp-取消2026-10-30)，不改变本端单地址行为；macOS 的后续完成条件已列入[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。后续保持原始 hostname/端口作为信任身份；自动换地址仅限尚未进入 SSH 协商的 TCP 阶段，不因密钥变化、撤销、拒绝或认证失败换一个地址继续。此段为源码核对，未新增网络故障实测。

### SSH 认证后的通道打开边界

普通终端的生产入口是 `AppModel.startSSH`：先等待 `SSHAuthenticationFlow` 完成，再在连接 owner 队列调用 `Transport.openSSHShell`。认证完成时会撤销认证计时器；传给下一阶段的只是 `SSHSession`，没有剩余 deadline。共享 `gt__ssh_shell_open` 为 channel-open → PTY → shell 新建一个 5000 ms 的准入预算，取消回调传 `NULL`。这不是从 DNS 开始贯穿到 shell 可输入的单一 60 秒预算，也不能用已建立会话的 `SessionController.stop()` 证据证明打开阶段可立即取消。

关闭连接中的窗格时，`cancelledConnects` 和窗格 ID 检查会阻止成功返回的旧 transport 挂到已关闭窗格；这项结果归属保护不等于中断正在执行的 Core 通道请求。现有预算在非阻塞请求的 EAGAIN 等待中检查，锁等待和清理不能据此承诺严格的五秒端到端上界。此处为源码核对，没有新增停滞服务器实测。

共享核心已有 `gt_transport_open_ssh_shell_controlled` 和 Swift 的带 timeout/cancellation 重载（Android 计划 §9.41）；Swift 真实 sshd 与三组绑定变异已验零预算、取消及 PTY 尺寸/回显。Android 产品已改用新入口，macOS `AppModel.startSSH` 仍走上文旧入口，不能把绑定可用记为桌面交互已修复。

共享通道代码现仅把明确的 PTY/shell 请求拒绝映射为 UNSUPPORTED，断线保持 IO；原 exec/tmux 预算继续调用 `gt__command_expired`。最终 SSH 五组、exec 两组、真实 tmux SSH 六组及 C/Swift 绑定变异已通过；失败过程保留在[切片记录](../docs/reference/android-implementation.md#941-ssh-通道打开预算与取消2026-10-30)。本轮没有新增完整 macOS 宿主或产品取消验收。

macOS 后续需将原请求的剩余预算和取消信号传到通道 owner，分别验证 channel-open、PTY 和 shell 三个阶段停滞、拒绝、超时及取消；取消后不得继续发后续请求或挂载晚到会话，并验证 owner 释放和正常真实 shell 对照。若修改公开 ABI，同步 Swift/JNI 与真实平台构建。SFTP 子系统和 tmux 管理/attach 各有入口，须独立验收，不能从普通 shell 的修复推定它们已完成。跨端计划见 [Android 网络收尾](../docs/reference/android-implementation.md#46-a3-网络与取消收尾)及[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

### SFTP 打开与取消边界

`AppModel.makeSFTPFileBackend` 完成认证后，在专用 owner 队列调用无参 `ssh.sftp()`，没有传递原认证的剩余 deadline 或取消回调。专用线程避免在主线程执行这一步，但不保证中途取消能打断 libssh2；晚到结果归属检查也不是底层请求取消。

`SSHSession.sftp(timeoutMilliseconds:cancellation:)` 与主机补丁 libssh2 已完成本机验收，默认 Core 和两个 SwiftPM 包消费同一版本。17 项协议用例、十组接口变异、十一组依赖守卫及既有打包变异通过；此前测试观测面错误已修正。完整证据与构建准备见[主机依赖与 SFTP 打开](../docs/reference/host-ssh2-build.md)。

macOS `makeSFTPFileBackend` 仍调用无参 `sftp()`，Android 后续已由 §9.51/§9.52 接入专用文件 owner/JNI 与页面，已传递预算和取消；macOS 后续需传递原请求预算与取消，其 list/realpath/预览/关闭仍使用原有阻塞契约，不能从 opener 测试推定整条文件连接有界。macOS 的队列主线程等待与导航问题仍独立待修；本轮接口/依赖验收没有新增桌面产品关闭/取消或完整宿主证据。

### 目录浏览与元数据接线边界

共享层新增 `SFTPSession.browse(path:timeoutMilliseconds:maxEntries:cancellation:)`，该切片为 83 ABI、Apple 81/2；36 项协议及 19 组浏览变异已通过，包含三组 Swift 与无库档。原 Swift 定位失败已修正，历史记录保留。`SFTPFileBackend.list/realpath` 和 `AppModel.makeSFTPFileBackend` 仍用旧入口，绑定验收不是桌面产品验收；见 [§9.49](../docs/reference/android-implementation.md#949-受控目录浏览与共享会话修复2026-10-30)。

新的 `SFTPEntry/SFTPStat` 带 `hasSize/isRegularFile`，C 转换保留真实标志；但 `SFTPFileBackend.entry` 创建 `FileEntry` 时丢弃它们，`kind` 将非目录/非链接一律映射为 `.file`。`FileEntry` 只有数值大小，`sizeText` 会将未知大小的零值显示成文件大小，特殊或未知类型也可能被当作普通文件。此为源码核对，尚无新增桌面故障实测。

迁移时须把“未知大小”与“已知零字节”、普通文件与特殊/未知类型贯穿后端、列表、预览准入和任务规划；不能仅加绑定字段便宣称修复。分别以缺少 SIZE/权限属性、零字节普通文件、FIFO 和普通文件对照验收，并加变异证明产品适配层不会重新折叠语义。新浏览入口需要独占连接，不能直接套在同时承担传输 job 的现有后端上。

### 与 Android 最终方案的差异

| 操作 | macOS 当前实现 | Android 方案与实现边界 |
|---|---|---|
| 文件打开与选择 | 原生列表单击选择、双击打开；右键“拷贝路径”只复制路径文本 | 方案为点按打开、长按勾选；本机/SAF 与远端浏览、文本预览已接，见 Android §9.43/§9.52 |
| 文件复制 | 本地↔远端的传输队列、拖放与冲突对话框；无四方向统一复制清单 | §9.60 已接常驻来源清单/详情/替换与清空，§9.61 已接请求与来源扫描；四方向粘贴执行仍属 A5/A6 待办。终端文本剪贴板独立，不等于文件复制完成 |
| 普通 shell 退出 | `handlePaneExit` 移除终端叶子，其余文件/终端叶子保留；最后一片退出关闭标签 | 正常 EOF 后保留终端内容供浏览/复制，显式关闭再释放；错误退出不承诺保留 |
| 设置 | 通用、外观、键盘、配置文件四页 | 最终设计五类、16 项偏好；已提交配置/关于入口，身份设置首片已验（§9.33），主机验证管理已验（§9.39），完整偏好留给 A7 |
| 主机身份 | 表单仍按文本路径编辑，认证从受限文件读 PEM；来源绑定未接表单 | §9.35 已接本安装容器引用、配置候选和引用主机详情；不提供跨端密钥迁移 |
| 后台会话 | 显式 tmux 入口已交付，关闭仅断开客户端 | A8 尚未开始；本地终端返回首页不等于进程死亡后保活 |

文件浏览还需按平台分别验收：macOS 的目录请求目前没有代次保护，返回历史在读取前修改，刷新不清树缓存；远端 Quick Look 下载完成后直接打开预览，没有应用层大小限额、回收和迟到结果保护。Android 的请求代次与有界文本读取不能作为这些桌面路径的验证。macOS 后续分别按下文的[目录导航](#目录导航与缓存边界)和[预览生命周期](#quick-look-预览边界)收尾，不能将两端的“文件浏览”合并成一个完成项。

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

### 目录导航与缓存边界

`FilesPanelModel.navigate` 将请求放进串行队列，但没有导航 generation。每个完成回调都会发布路径/条目并把 `isLoading` 设为 false；A 请求之后又请求 B 时，A 完成仍会先展示 A 并撤下加载状态，即使 B 尚在等待。这里的问题是旧请求仍然发布，不是串行队列发生乱序。页面工具条也没有在加载期间统一禁用导航；搜索使用的 token 没有覆盖目录请求。

`goBack/goForward` 在读取目标目录之前就弹出历史并推入另一栈；目标读取失败时 catch 只显示错误，没有恢复历史。当前位置与旧列表保留，却已消耗历史项，不能承诺失败后后退/前进仍保持原序列。应为待导航目标与历史变更建立请求身份，成功后提交；失败保留可重试目标和原历史，并避免前一请求清除后一请求的加载状态。

`refresh` 只重新列出 `currentPath`，不清理 `treeChildren`；`loadTreeChildren` 一旦已有缓存就不再查询。因此不仅失败缓存无法重试，成功加载过的树节点在新增/删除目录后也不会因为点击刷新而更新。须定义按目录失效的缓存和迟到结果保护；刷新、文件操作完成和重新展开应一致。此处是源码核对，未新增原生交互验收；后续需用可控制响应的后端验 A/B 连续导航、失败后历史不变、树节点外部增删后刷新，并保留普通导航对照和变异验证。

共享底层的两项生命周期问题已在 §9.49 修复：任务提交在移交 worker 前保存 ID，防止返回前对象已被释放；exec 读取结束恢复原会话超时，不将 100 ms 片段泄漏给 SFTP。真实 SFTP/ASan 与三组 exec 变异有证据；不改变下面的文件原子提交、冲突、重试与产品主线程等待缺口。

### 传输提交、调度与重试边界

§9.54 新增 Swift `SFTPSession.createExclusive`：从调用方普通 fd 分块读取，创建 0600 的远端暂存文件，返回完整的拒绝/未知/已创建/完成状态。底层只使用 EXCL，不删除或发布最终路径；原文件 fd 偏移保持。macOS 文件产品仍使用原 job，此新绑定没有改变桌面冲突/任务/预览流程。

共享 [SFTP job 实现](../core/src/sftp.c) 已按 Android 计划 §9.53 修复：下载先打开远端再截断本地目标，上传以 CREATE|TRUNC 覆盖，上传/下载检查远端 close 和本地 `fclose`；已取消的排队任务不打开文件，上传源定位错误不触碰远端目标。macOS 现有绑定消费同一实现，但这些仍是直接覆盖接口，IO/关闭失败可能留下部分文件。队列成功状态不等于原子提交，进度 100% 也不是提交证据。任务暂存、非覆盖提交及完整桌面冲突/失败交互仍待实现；本片共享故障回归和 Swift 绑定定向不能代替完整宿主验收。

### 旧下载的完整性边界

截至 §9.56 切片，`job_download` 的首次 FSTAT 只提供进度总量；FSTAT 失败或缺 SIZE 时使用零值，读取返回 EOF 即设置成功，再检查两侧关闭结果。它没有比较实际字节数与已知源长度，也没有在读取结束后复核源属性。因此“读到 EOF 且关闭成功”不等于“复制了选择时的完整源文件”；源中途缩短或服务器提前返回 EOF 时，现有代码仍可能报告成功。这是旧路径的源码核对结论，新增接口的协议测试不验证这条旧路径。

`TransferManager.finish(.done)` 还会将显示进度补为 total，Quick Look 则在 `.done` 回调打开副本，不能从进度满格或预览打开推定源一致性。§9.50 的受控文本预览有独立长度检查，但桌面 Quick Look 没有消费该入口；§9.55 的匿名暂存只在 Android 数据层实现，也没有改变 macOS 下载路径。

§9.56 的 `SFTPSession.receive` 已在独立受控入口区分未知长度、已知零字节和非零长度，并复核源属性/EOF/关闭；Swift 绑定专项与真实构建通过。但 `FileBackend.download` 仍只返回旧 job ID，`SFTPFileBackend.download` 仍转调 `sftp.download`，`TransferManager` 和 Quick Look 均未使用新 fd 接口。后续须迁移专用 owner、任务暂存、进度/结果和最终发布，不能只替换一个函数名；接口要求独占连接，禁止和同一会话的旧 job 并发使用。桌面迁移须另验提前 EOF、源变化、空文件、失败不发布及取消释放；元数据相同也不能声称获得原子内容快照。

### 传输队列与重试作用域

目录下载与递归权限传入 `maxDepth: -1`，Core 当前只裁剪超过 64 的正深度，负值遍历没有硬深度检查；公开注释的安全护栏尚未兑现。目录深度预算、超限错误及部分结果必须补齐，详见路线图共享缺口。

`CoreEventPump` 是唯一事件消费者，`TransferManager` 是全局任务列表；它们不是全局并发调度器。每个 `gt_sftp_t` 有自己的 FIFO worker，同一会话的 job 顺序执行，不同连接没有统一并发上限；`start` 提交拿到 job ID 后即标记 running，不能从页面上的 running 数量推断正在传字节的数量。

任务执行期间强持有 backend；`finish` 释放该引用，`retryBackend` 只保留弱引用。因此重试复用尚存活的原端点，不会自动重连、重新认证或从断点续传。端点已经释放时显示“原传输端点已关闭，无法重试”；端点仍在但已断线也可能再次失败。关闭文件窗格不取消在途任务，并不意味着任务结束后仍能无条件重试。Android A6 必须单独实现端点重新获取与冲突复核，不能照搬这一生命周期作为完整重试契约。

冲突对话框的“应用到全部”当前写入全局 `TransferManager.shared.applyToAll`，`resolveConflict` 优先读取它；任务结束、清空已完成和 `removeAll` 都没有重置此值。因此它实际跨后续批次、端点和重试生效，不能描述成“仅本次批量操作”。若此前选了覆盖，后来遇到冲突可能不再询问。应建立批次 ID 与决策作用域，批次结束后失效；Android A5/A6 按独立 `PasteRequest` 实施，不继承这一行为。

目录重试会移除旧父任务并再次调用 `expandDirectory`，重新枚举当前整棵目录；旧子任务不因此删除，已成功文件也没有从新一轮扫描中排除。因此当前“重试”不等同只重试失败子项，且冲突会继续受上述全局选择影响。单文件重试重置同一行的 job ID/进度再提交，也不提供字节断点续传。两项为本轮源码接线核对，未新增真实文件覆盖实验或原生 UI 测试；修复条件见路线图 P1。

“保留两者”也还没有有界的名称分配契约。`TransferManager.uniqueName` 从 ` (1)` 开始，以 `while true` 逐个调用存在性查询，直到找到空名字；没有尝试次数、总时间或 UTF-8 名称长度限制。调用发生在 `@MainActor` 的 `start` 内，远端查询又经过 `SFTPFileBackend.queue.sync`，大量同名候选或慢网络会延长主线程等待。返回的名称没有被占用，后续仍是旧覆盖 job，查询后出现的同名文件可能被覆盖。修复需异步且可取消的有界查找、名称长度校验与最终非覆盖提交；达到预算应显示失败，不能无限编号。此为 `150ffd5` 源码核对，尚无对应故障行为验收；Android §9.63 已交付请求内决策和有界名称规划，但没有改动本端调用方，不能直接算作本端修复。

### 目录创建与合并边界

`TransferManager.expandDirectory` 当前在上传前用 `exists` 判断目标，只有 false 才调用 `mkdir`；已存在目标不会先验证是否为普通目录。下载使用 `FileManager.createDirectory(withIntermediateDirectories: true)`，可复用已有目录。根目录和子目录都沿此路径处理，文件子项随后各自进入旧传输队列；这不是整棵目录的非覆盖发布，也没有独立的目录合并确认。

远端调用链为 [TransferManager](GhostermApp/Sources/GhostermAppModel/TransferManager.swift) → [SFTPFileBackend](GhostermApp/Sources/GhostermAppModel/SFTPFileBackend.swift) → Swift `SFTPSession.mkdir` → [gt_sftp_mkdir](../core/src/sftp.c)。旧 Core 入口只返回同步状态，失败统一映射为 IO；没有单次总预算、取消回调或“已创建但回包/核验失败”的结果。目录创建发生在子文件传输之前，取消或后续失败可能留下空目录或部分树，当前父任务没有逐目录副作用记录。

后续须明确目录冲突、复用/合并与非覆盖的产品契约，先验证目标类型和归属，再按任务保存已创建位置及未知结果；不能靠 `exists` 的 false 推断服务器明确不存在。受控远端创建是共享前置，Android §9.64 的 HOME/SAF 原语不能补齐它。对应实施要求见 [§6.16](../docs/reference/android-implementation.md#616-远端目录与任务接线的下一步)，桌面迁移还须验证自己的队列、取消和部分结果页面。本节仅记录源码复核，没有新增故障或系统交互验收。

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
# 完整 Xcode 位于此路径时，在同一 shell 统一选择工具链；按实际安装路径调整
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
mkdir -p "$PWD/tmp"
export TMPDIR="$PWD/tmp"
# 首次准备锁定的主机 SSH 依赖，再构建 Core 与 App
python3 scripts/build-host-ssh2.py --fetch  # 首次准备；缓存齐备后省略 --fetch
bash scripts/build-core.sh
bash scripts/gate.sh
bash scripts/check-abi-apple.sh
swift build --package-path apple/GhostermApp
bash scripts/apple-app-gate.sh
```

原生编译、完整宿主测试、像素/系统交互、变异验证分别记账。`apple-app-gate.sh` 的 SSH 部分需隔离 sshd 成功启动；看到 SKIP 要保留原因。已接入 CI 不等于对应提交已在 macOS 15 运行通过；当前已知本机与历史结果见[路线图验收记录](../docs/reference/roadmap.md#验收记账与历史记录)。

dev 的 main push 和 release 的 PR 都有路径过滤，连续 dev 运行还可能被取消；本次纯文档提交不承诺自动生成新包。具体触发范围见 [CI 触发说明](../docs/how-to/dev-build.md#ci-是怎么串起来的)，验收规则仍以 AGENTS 为准。

当前 full workflow 尚未对 `apple-app-gate.sh` 的全部跳过输出统一判失败；sshd 启动失败时，该脚本可以只跑其余用例后返回成功。图标缺依赖失败及打包/认证变异的 SKIP 守卫不能替代这项检查，后续需按必跑项目收口。

另一个独立缺口是失败传播：`apple-app-gate.sh` 使用 `set -uo pipefail`，没有 `-e`，调用 `check-abi-apple.sh` 后也没有检查返回值。因此 ABI 子检查失败本身不会终止这个脚本，后续构建/测试若成功，仍可能打印 `APPLE APP GATE PASS`。目前应单独核验上面的 ABI 命令结果；该源码结论不表示已复现某次 CI 假绿。修复需要隔离注入 ABI 失败并证明宿主闸门非零退出，保留正常本地构建对照，不能只补日志文字。

构建 Core 与 App 时须使用一致的 Xcode 环境。此次文档核对中，系统 `xcode-select` 指向 Command Line Tools，默认 `gate.sh` 链接 Xcode 27 SDK 的 TBD 时失败（`unknown architecture`）；在同一命令设置完整 Xcode 的 `DEVELOPER_DIR` 后 46 项 Core 测试与 88 ABI 闸门通过。不能从脚本使用了 `xcrun` 推定编译器、链接器和缓存 SDK 已一致，也不固定假设 Xcode SDK 一定是 26.5。

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
  （libssh2 → libcrypto；其余依赖按实际 Mach-O 加载项递归收集）、改 install name、复验无绝对路径，并做
  运行时断言（`DYLD_PRINT_LIBRARIES` 里不得出现 Homebrew 路径）。
- 签名顺序是「先内嵌库、再 .app」；BUILD-INFO.txt 必须在签名**之前**写进
  bundle，否则 code seal 失效（这条被 dmg 挂载复验抓到过）。

参考：[Termini](https://github.com/hudsonkit/Termini)（MIT）、[iGhostVT](https://github.com/owngoal-dev/iGhostVT)（MIT）、[VVTerm](https://github.com/vivy-company/vvterm)（GPL-3.0）。
