# 计划：文件面板顶栏修复 + 标签右键菜单

> 状态：**已交付**（2026-10-15）——全量闸门绿、变异补齐、已提交。
> 相关：`capability-matrix.md`（SFTP / files 行）、`roadmap.md`（M4.5 遗留 / M4.6）、
> `plan-files-manager.md`（内嵌文件面板的出处）。
> 基线：`0d1634d`（S6-iOS 证据链补全之后）。
> 触发：用户实测三条反馈（见 §1）。

## 0. 一句话

内嵌文件面板（终端标签页右侧那条「SFTP 侧边栏」，⌘⇧E 唤出）顶栏**改折行**
（修「按钮超框」）+ 加**关闭按钮**（修「没法关」）；「这条标签能做什么」的动作
收进**标签右键菜单**（用户建议），顶栏那两个分屏按钮也一并移入。

## 1. 用户反馈与答案

| # | 反馈（原话） | 结论 | 落点 |
|---|---|---|---|
| 1 | 「SFTP 侧边栏如何关闭？有没有关闭按钮？」 | 原来只能 ⌘⇧E / 菜单「显示 → 文件面板」，**面板上确实没有关闭按钮**；现补一个 | FilesPanelView |
| 2 | 「SFTP 侧边栏顶部按钮超框了。」 | 工具条原是一行 `HStack`（不折行、不报错），260–340pt 面板放不下 → 右边按钮被画到框外裁掉 | FlowLayout |
| 3 | 「只有快捷键打开，可在 tab 栏对应标签加右键菜单，把分割窗口按钮也收进去」 | 加**标签右键菜单**（侧栏行 / 顶栏胶囊 / 标签条 chip 共用一份），含文件面板开关 + 分屏；顶栏可见的分屏按钮移入菜单 | TabContextMenu |

## 2. 与仓库既有约定的关系

- **铁律 #1（三层接缝）**：右键菜单只发意图、落点仍在 `AppModel` / `TabManager`，
  视图不直接改结构——与 `TabStrip` / `Sidebar` 既有做法一致。
- **iGhostVT 对照**：标签右键菜单（锁定 / 关闭 / 关闭分屏）iGhostVT 侧栏已有；
  我们把**同一份菜单**复用到顶栏胶囊与标签条 chip（iGhostVT 只侧栏有），
  顺手补上文件面板开关（我们比 iGhostVT 多的一层）。
- **「按钮待在它要收拢的那条边旁」**（`sidebar-chrome` 变异钉住的既有规则）：
  面板在右侧 → 关闭按钮放顶栏**最右**，折行后仍固定在右上角。

## 3. 决策表

| # | 决策 | 值 | 理由 |
|---|---|---|---|
| A | 工具条超框的修法 | **折行**（`FlowLayout`），不是溢出菜单 | 所有工具保持可见可点；不用把动作藏进 `⋯` |
| B | 关闭按钮位置 | 顶栏第一行**最右**，固定不随折行跑 | 面板在右 → 按钮在它收拢的那条边（同 `sidebar.leading` 的约定） |
| C | 右键菜单的实现 | **一份** `TabContextMenu`，侧栏行 / 顶栏胶囊 / 标签条 chip 共用 | 免得修一处漏一处（AGENTS.md 里踩过同类坑） |
| D | 分屏按钮 | 顶栏那两个**移除**，移入右键菜单 | 用户建议；⌘D / ⇧⌘D 与「显示」菜单不受影响 |
| E | `onClose` 可选 | 双栏 SFTP 标签页的每栏**不传** → 无关闭按钮 | 那里关的是整个标签页，没有「关掉一栏」的语义 |
| F | 菜单作用的标签页 | `split(_:axis:)` / `toggleFilesPanel(for:)` 作用在**被右键的那条**，并把它带到前台 | 右键的那条未必是当前标签页；写错会「右键 A 却动了 B」 |

## 4. 改动清单

**新增**
- `apple/GhostermApp/Sources/GhostermApp/FlowLayout.swift` —— 折行 `Layout`（macOS 13+）
  + 纯函数 `FlowLayout.rows(widths:available:spacing:)`（贪心折行，可离线断言；
  单项比整行还宽时独占一行——不裁不缩）。
- `apple/GhostermApp/Sources/GhostermApp/TabContextMenu.swift` —— 标签右键菜单（三处共用）。
- `apple/GhostermApp/Tests/GhostermAppTests/PanelChromeTests.swift` —— 6 条
  （折行规则 3 + 顶栏折行更高 / 不裁 2 + 关闭按钮对照 1，像素断言）。
- `apple/GhostermApp/Tests/GhostermAppTests/TabContextMenuTests.swift` —— 4 条（命令接缝）。

**修改**
- `FilesPanelView.swift` —— 顶栏改 `FlowLayout` 折行；`onClose` 参数 + ✕ 按钮。
- `RootView.swift` —— 内嵌面板传 `onClose: { tab.filesPanelVisible = false }`。
- `AppModel.swift` —— 新增 `toggleFilesPanel(for:)` / `split(_:axis:)` / `closePane(in:)`。
- `TabStrip.swift` —— 移除顶栏 `splitControls`；当前标签胶囊 + 标签条 chip 加 `.contextMenu`。
- `Sidebar.swift` —— 标签行右键菜单改用共用 `TabContextMenu`。

## 5. 证据（全部已跑）

- `bash scripts/apple-app-gate.sh` → **APPLE APP GATE PASS**
  （C ABI 绑定对拍 55 项 + 真 sshd e2e + `swift test` **211 tests / 0 failures**，
  1 项 skip 为既有 iOS 口径）。
- `bash scripts/mutation-check-files.sh` → **14 passed / 0 failed**，其中新增
  **F10**（`FlowLayout.rows` 永不折行）、**F11**（顶栏退回一行 `HStack`）、
  **F12**（去掉 ✕ 关闭按钮）三条**都变红**——新断言有牙。
- `bash scripts/mutation-check-tabs.sh` → **33 passed / 0 failed**，其中新增
  **T17**（右键分屏作用到当前标签页）、**T18**（右键开面板作用到当前标签页）
  都变红，且同变异下对照（`TerminalConfigTests`）仍绿 = 红有指向性。
- `mutation-check-sidebar-chrome.sh` 的 7 处锚点逐条核对**仍命中**
  （`if !showsSidebar { sidebarToggle }`、Sidebar 的两处 `sidebar.leading`、
  `.fill(isActive ? …)`、`window.title = ""`、`.background(WheelScrollsHorizontally())`、
  `HorizontalScrollAssist` 的翻转）；`tabs` 的 T7 锚点由脚本自身断言，已随 33/33 通过。
- 目视抓帧：260pt 面板工具条折成两行、右上角 ✕ 在位；700pt 一行、✕ 在最右、
  无 `onClose` 时该处为空。

## 6. 落地方式

两片提交（直接 `main`）：

1. `feat(apple): 文件面板顶栏折行 + 关闭按钮（修「按钮超框 / 没法关」）`
   —— `FlowLayout.swift`（新）/ `FilesPanelView.swift` / `RootView.swift` /
   `PanelChromeTests.swift`（新）/ `mutation-check-files.sh`（F10~F12）。
2. `feat(apple): 标签右键菜单（文件面板 / 分屏），分屏按钮移入菜单`
   —— `TabContextMenu.swift`（新）/ `AppModel.swift` / `TabStrip.swift` /
   `Sidebar.swift` / `TabContextMenuTests.swift`（新）/
   `mutation-check-tabs.sh`（T17/T18）+ 文档（`AGENTS.md` /
   `capability-matrix.md` / `roadmap.md` M4.6 S8 / 本份计划）。

**变异份数不变**（沿用既有两份脚本扩展），故 README / AGENTS.md 的
「现有 26 份」无需改。

## 7. 遗留与注意

- `FlowLayout` 依赖 `Layout` 协议（macOS 13+）；`Package.swift` 平台已是
  `.macOS(.v13)`，无碍。
- 移除顶栏可见分屏按钮 → 发现性下降（用户主动建议）；能力仍在右键菜单 /
  快捷键（⌘D / ⇧⌘D）/「显示」菜单。

## 8. 复现

```bash
cd apple/GhostermApp
swift build
swift test --filter PanelChromeTests
swift test --filter TabContextMenuTests
cd <仓库根> && bash scripts/apple-app-gate.sh   # 恢复任务时先跑这个
```
