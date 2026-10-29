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

UI 已知坑见 [Apple UI 记录](../docs/reference/apple-ui-pitfalls.md)。

## 当前功能与代码入口

下表路径相对 `GhostermApp/Sources/`。页面名称与目录结构以生产代码为准，旧计划中的独立双栏和多窗口标签组仅用于历史追溯。

| 功能 | 代码入口 | 当前边界 |
|---|---|---|
| 顶部/侧边标签、窗口与焦点 | `GhostermApp/TabWindowController.swift`、`TabStrip.swift`、`Sidebar.swift` | 同一窗口内切换，唯一新建入口；不跨启动恢复标签/布局 |
| 终端与文件窗格 | `GhostermAppModel/WorkspaceLayout.swift`、`GhostermApp/WorkspaceView.swift` | 同一布局、身份与宿主；真实拖动等遗留见工作区交接 |
| 连接与配置编辑 | `GhostermApp/ConnectionChooser.swift`、`HostEditorSheet.swift`、`GhostermAppModel/ConfigStore.swift` | 主机不放侧栏；表单从块内指令取初值，已有别名不提供改名字段；尚无继承来源展示，配置事务仍有缺口 |
| SSH 认证与凭据 | `GhostermAppModel/SSHAuthenticationFlow.swift`、`SSHCredentialSelector.swift` | 可取消认证；Keychain 存口令/密码，生产私钥仍读受限文件 |
| 文件与传输 | `GhostermApp/FilesPanelView.swift`、`GhostermAppModel/TransferManager.swift` | 本地↔远端；尚无四方向统一复制队列；暂停/继续/取消忽略后端错误仍改状态，未知结果映射尚待修复 |
| 菜单与偏好 | `GhostermApp/AppMenu.swift`、`GhostermAppModel/SettingsStore.swift`、`Shortcuts.swift` | 设置持久化、已有终端即时更新；Ctrl+Tab / Ctrl+Shift+Tab 循环切换标签 |

修复顺序与验收触发场景见[macOS 实施顺序](../docs/reference/roadmap.md#macos-修复顺序与验收触发场景)。当前代码注释中的“安全写回”“原子写”等描述是目标契约，实际 `try?` 备份及 remove→move 路径尚未兑现，不能把注释当作通过证据。

设置窗口实际有通用、外观、键盘、配置文件四页。「配置文件」页的全文编辑框编辑 `ghostty.conf`，SSH/known_hosts 只显示路径和 Finder 入口，不是 SSH 全文编辑页。该页保存调用 `replaceTerminalConfig` 重载引擎；“只有新开的标签页会读到改动”的 UI 提示仍待修正，文档不沿用这条过时文案。

## 构建与验证

在仓库根运行；full libghostty 产物须先由 `scripts/build-ghostty.sh` 生成：

```bash
bash scripts/gate.sh
bash scripts/check-abi-apple.sh
swift build --package-path apple/GhostermApp
bash scripts/apple-app-gate.sh
```

原生编译、完整宿主测试、像素/系统交互、变异验证分别记账。`apple-app-gate.sh` 的 SSH 部分需隔离 sshd 成功启动；看到 SKIP 要保留原因。已接入 CI 不等于对应提交已在 macOS 15 运行通过；当前已知本机与历史结果见[路线图验收记录](../docs/reference/roadmap.md#验收记账与历史记录)。

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
