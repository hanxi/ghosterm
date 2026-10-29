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

已交付与遗留见[路线图](../docs/reference/roadmap.md)、[能力矩阵](../docs/reference/capability-matrix.md)。
共享 `gt_config_edit` 已修复非连续重复指令误删中间行的问题，Apple 与 JNI 使用同一实现。`ConfigStore` 的强制备份/原子替换、私钥材料安全存储接线仍有缺口；Android 写盘事务不等于 macOS 写盘事务已修复。
`HostListModel.directives` 目前仍以 Swift 简化行扫描提供表单初值，`HostEditorSheet.save` 提交全部显示字段；不能把 Core 的稀疏编辑保证扩大为整个表单的“只改脏字段”。tab、`=`、引号、重复指令与未改保存的保真须独立补齐，见路线图 P0。
`AppModel.applyEdits/addHost` 先修改内存配置，再调用存储；写盘抛错时只向页面报错，尚未恢复内存模型。`HostListModel.reload` 也未过滤不含通配符的否定模式（如 `!secret`），不能把所有派生条目都视为可连接主机。
`SSHConfig.inspect(alias:)` 已接入并有值复制/来源转换的定向测试与变异，尚未接入 `HostEditorSheet`；存在 Swift 绑定不表示 macOS 表单已提供继承值展示、别名重命名或完整包含文件编辑。新增接口状态见[实现计划 §9.5](../docs/reference/android-implementation.md#95-字段来源与编辑归属2026-10-29)。
已新增 `SSHConfig.rename(alias:to:)` 绑定，配套 C 接口只修改内存配置，**不负责备份或写盘**；`HostEditorSheet` 仍仅在新建时显示别名输入框。共享改名能力不能抵消 macOS 的表单/事务缺口。当前为 73 个公共符号、Apple 71 个引用/2 个登记未用，改名的共享/绑定验收见[§9.9](../docs/reference/android-implementation.md#99-主机别名重命名2026-10-30)。

共享主机信任仍有 P0 缺口：`core/src/ssh.c` 丢弃 known_hosts 的 `@revoked` / `@cert-authority` 等标记后按普通条目匹配，同主机同密钥可被当成已知。macOS 普通 SSH、SFTP、tmux 共用的认证链路受影响；现有普通条目测试不能替代撤销/CA 语义验收。修复及 Android A3 的共同前置见[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

UI 已知坑见 [Apple UI 记录](../docs/reference/apple-ui-pitfalls.md)。

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
| 配置到连接 | [HostListModel](GhostermApp/Sources/GhostermAppModel/HostListModel.swift)、[SSHConnectionTarget](GhostermApp/Sources/GhostermAppModel/SSHConnectionTarget.swift) | 高级配置求值结果未传入连接，缺少连接前能力校验；列为 P0 |
| 终端文本与剪贴板 | [TerminalSurfaceView](GhostermTerminal/Sources/GhostermTerminal/TerminalSurfaceView.swift)、[shim](GhostermTerminal/Sources/CGhostty/shim.c) | IME 已接通；系统复制/粘贴和确认回调仍占位，列为 P1 |

**终端系统剪贴板尚未接通**：`GhostermTerminal/Sources/CGhostty/shim.c` 的读取回调返回 `GHOSTTY_CLIPBOARD_READ_UNAVAILABLE`，写入和确认回调为空。`TerminalHostView` 尚无 `copy:` / `paste:` 响应方法；编辑菜单虽注册了这两个 selector，不能据此宣布终端复制粘贴可用。`TerminalSurface.sendText` 的粘贴编码测试只证明给定文本能送到程序，不包含系统剪贴板读取、选区复制或确认框。文件路径复制、标签信息复制及设置文本复制各有独立 `NSPasteboard` 接线，也不补全终端链路。此为源码核对结果，尚无本轮真实菜单操作复测；实现与验收列入路线图 P1。

修复顺序与验收触发场景见[macOS 实施顺序](../docs/reference/roadmap.md#macos-修复顺序与验收触发场景)。当前代码注释中的“安全写回”“原子写”等描述是目标契约，实际 `try?` 备份及 remove→move 路径尚未兑现，不能把注释当作通过证据。

**配置求值不等于连接执行**：`GhostermCore.ResolvedHost` 已返回 ProxyJump、ForwardAgent、Compression、ServerAliveInterval、SetEnv、LocalForward；`HostListModel.reload` 只把地址、用户、端口和身份文件放入 `HostEntry`，后续 `SSHConnectionTarget` / `SSHOptions` 也没有承接这些高级选项。`SSHAuthenticationFlow.start` 直接解析目标地址，当前未统一拒绝这些未支持配置。因此配置中写了跳板也不能据此认为连接会经过跳板；42 键 `ssh -G` 对拍只验求值，不验连接行为。

连接前能力校验已列入路线图 P0，需覆盖普通 SSH、独立/内嵌 SFTP 与远端 tmux；未支持的有效选项应列名报错，并在解析地址、建连和索取凭据前停止。本段依据生产代码调用链核对，尚未新增真实跳板或网络端点测试，不记为已修复。

### 与 Android 最终方案的差异

| 操作 | macOS 当前实现 | Android 方案与实现边界 |
|---|---|---|
| 文件打开与选择 | 原生列表单击选择、双击打开；右键“拷贝路径”只复制路径文本 | 方案为点按打开、长按勾选；文件页尚未实现 |
| 文件复制 | 本地↔远端的传输队列、拖放与冲突对话框；无四方向统一复制清单 | 常驻待粘贴清单和四方向复制属于 A4–A6；终端文本剪贴板已接入，不等于文件复制完成 |
| 普通 shell 退出 | `handlePaneExit` 移除终端叶子，其余文件/终端叶子保留；最后一片退出关闭标签 | 正常 EOF 后保留终端内容供浏览/复制，显式关闭再释放；错误退出不承诺保留 |
| 设置 | 通用、外观、键盘、配置文件四页 | 最终设计五类、16 项偏好；当前仅配置/关于入口，完整设置留给 A7 |
| 后台会话 | 显式 tmux 入口已交付，关闭仅断开客户端 | A8 尚未开始；本地终端返回首页不等于进程死亡后保活 |

目录传输也不等于完整目录镜像：`TransferManager.expandDirectory` 下载时排除符号链接，上传枚举遇到符号链接时跳过且不递归。被排除条目不生成子任务，也不进入父任务的跳过计数；只有链接的目录可能按空文件集合报完成。现有目录下载测试明确只提交两个普通文件、排除链接，2026-10-30 定向重跑通过；它使用 fake 后端，验证的是展开/汇总模型，不是真实 SSH 目录复制。需要补齐用户可见的跳过原因和汇总结果，不能将“完成”解释为原目录所有条目均已复制；见路线图的目录复制完整性项。

设置窗口实际有通用、外观、键盘、配置文件四页。「配置文件」页的全文编辑框编辑 `ghostty.conf`，SSH/known_hosts 只显示路径和 Finder 入口，不是 SSH 全文编辑页。该页保存调用 `replaceTerminalConfig` 重载引擎；“只有新开的标签页会读到改动”的 UI 提示仍待修正，文档不沿用这条过时文案。

终端配置存储与 SSH 配置存储须分开核对：`SettingsStore.saveTerminalConfig` 的备份失败会抛错，提交使用 `replaceItemAt`；没有 `ConfigStore` 的先删后移流程。但 `rawConfig()` 将目录/读取错误折为空文本，加载时 stat 缺失后即使文件被外部创建也不报冲突。`merging` 只改首个同名键，而 `value` 读取最后一个：两行 `font-size = 10` / `font-size = 12` 改成 18 后，回读仍为 12。2026-10-30 直接编译生产 `SettingsStore.swift` 的隔离探针已复现这些结果；正常单键改为 18 和首次创建均成功。这是存储/文本处理证据，未替代真实引擎或页面验收。

`AppModel.applyTerminalSettings` 写盘后才请求引擎重载，重载失败没有恢复旧文件；`replaceTerminalConfig` 尝试恢复旧文件但忽略恢复错误。因此“错误已提示”不等于“磁盘与已有 surface 都保持旧值”。修复与失败注入条件纳入[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

设置说明还有三类过时文案：外观页称已有终端保持旧字号；标题栏称动作仅在当前格显示（实际还有悬停/键盘入口）；通用页仍描述恢复会话及关闭必结束。当前普通启动不恢复会话，显式 tmux 关闭只断客户端。文档按真实代码说明，这些页面提示单列待修，不记作已经改好。

关于窗口的更新检测只在用户点击时联网。`UpdateCheck.swift` 已处理缺字段、无本机 commit、渠道不符和网络错误，但远端 commit 的格式、摘要字符集及下载来源尚未严格校验；按 commit 不同提示“有新版”也不表示已经比较提交祖先关系。该缺口与安装脚本的 SHA256/签名校验分开，见[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

### 文件查询与线程边界

[SFTPFileBackend](GhostermApp/Sources/GhostermAppModel/SFTPFileBackend.swift) 使用串行队列保证同一后端调用保序，但公开方法是 `queue.sync`。`@MainActor TransferManager.start/pause/resume/cancel` 同步调用该后端，整次远端 walk/search 又占用同一队列。因此入队、冲突查询或控制操作可能等待网络或正在执行的遍历；不能将“有后台队列”解释为“主线程不会等待”。终端 `SessionController.stop` 的有界验证不覆盖这条文件路径。

`exists` 目前将任何 `lstat` 异常转换为 false。上传冲突检查、自动编号和目录创建据此判断不存在，无法区分确实不存在、无权查看与连接失败。需改为可区分三种结果的查询，查询失败不得继续当作空目标提交；即使查询修复，非覆盖创建仍须由共享写入契约保证。

[FilesPanelModel.search](GhostermApp/Sources/GhostermAppModel/FilesPanelModel.swift) 已提供搜索入口和 exec/walk 档位显示，但错误与取消尚不完整：exec 打开阶段任意异常都会回退 walk；`readFind` 忽略 stderr、没有调用现有 `SSHExec.exitStatus()`，条目 `lstat` 失败也被跳过，结束后仍返回 exec 档。远端命令失败可能显示零条或部分搜索结果为完成。页面 `cancelSearch` 只更新 token 并清列表，传给后端的回调仍返回 true；它阻止旧结果显示，尚未取消远端工作，无输出时的 read timeout 会继续循环。

文件错误边界不限于 SFTP。`FilesPanelModel.loadTreeChildren` 把列目录异常转成空数组并缓存；后续展开因缓存已存在不再读取，失败节点会看起来像空目录。`runLocalSearch` 用 `try?` 忽略遍历错误，随后仍标完成；取消只改变结果 token，visitor 仍返回 true。`setPermissions` 虽记录子项 chmod 错误，却忽略 `walk` 自身抛出的错误，可能只改了根或部分条目便正常刷新。`FileBackend.swift` 中的本地后端还会跳过 lstat 失败的条目，枚举未接入逐项错误报告。普通列表导航的 catch 会显示错误，不能把树/搜索的缺口扩大成所有目录错误都被吞掉。

后续需为树节点提供可重试的错误状态、搜索提供失败/部分结果、递归权限汇总扫描与修改结果；这与终端 I/O 和传输队列控制是不同验收面。当前只是源码核对，没有新增文件系统拒绝/扫描失败的运行证据。

以上为当前源码调用链结论，尚无本轮慢网络/失败命令的端到端验收。修复顺序为查询三态与异步 owner、远端退出状态和取消，再回归传输冲突/控制与页面切换；具体退出条件列在[路线图](../docs/reference/roadmap.md#macos-下一步与共享缺口)。

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
