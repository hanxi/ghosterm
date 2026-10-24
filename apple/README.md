# apple — macOS / iOS 宿主（SwiftUI）

## 结构（规划）

```
apple/
├── GhostermCore/        Swift 绑定：module map → core/include/ghosterm.h
├── GhostermApp/         SwiftUI 宿主（macOS / iOS 共用）
│   ├── Connections/     主机列表（读 ssh_config）
│   ├── Terminal/        终端页（terminal/apple 的 surface）
│   ├── Files/           SFTP 文件管理器
│   └── Settings/
└── Ghosterm.xcodeproj
```

## 要点

- 终端走 `terminal/apple`（full libghostty + Metal）；
- SSH/本地 shell 走 `core` 的 transport（`gt_transport_open_ssh_shell` /
  `gt_transport_open_pty`）；
- iOS 与 macOS 共用 SwiftUI，差异用 `#if os(...)` 显式标注，不做隐式降级。
- **UI 的已知坑**（AppKit 会静默吃掉 `Menu` 里的视图、抓帧取位图的顺序等）
  见 `docs/reference/apple-ui-pitfalls.md`——动这块前先扫一眼，别重复踩。

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
