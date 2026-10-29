# apple — macOS / iOS 宿主（SwiftUI）

## 当前结构（2026-10-29）

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
特别注意：`ConfigStore` 的强制备份/原子替换，以及私钥材料完整安全存储接线目前仍有实现缺口，不能把 helper 存在等同于生产能力完成。
UI 已知坑见 [Apple UI 记录](../docs/reference/apple-ui-pitfalls.md)。

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
