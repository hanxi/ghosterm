# 统一文件窗格工作区 — 交接说明

> 对应 Spec：`统一文件窗格工作区`（终端 / 本地文件 / 远端文件共用同一窗格布局）。
> 仓库：Ghosterm，分支 `main`。本文件记录**已完成范围、验证证据、未完成项**，
> 便于接手者继续，不需要重新推导上下文。

## 1. 已完成范围

### 布局与宿主
- `WorkspaceLayout`：唯一递归布局树（`leaf(PaneID)` / 带稳定节点 ID、轴向与权重的
  `split`），四边停靠、中央交换、关闭塌陷、隐藏过滤、比例更新、最小尺寸聚合
  均为纯函数（`apple/GhostermApp/Sources/GhostermAppModel/WorkspaceLayout.swift`）。
- `TerminalTab` 只保留这一份可写布局；`layout`/`panes` 等旧接口改为注册表与
  终端叶子的只读投影，不再有第二份终端布局状态。
- `WorkspaceHostView`：每个 PaneID 一个稳定 `NSHostingView` 直接子视图，重排只改
  frame；新增/真正关闭才增删宿主。旧 `HSplitView` 递归渲染与
  `RootView.PaneLayoutView` 已删除。
- 默认 A 布局（左终端 : 右文件 = 2:1，右侧本地在上、远端在下）；`恢复默认布局`
  菜单动作只重排现有窗格。

### 外观
- 终端与文件共用 `PaneCard`（圆角、1pt 边框、`PaneGap` 统一内外留白）。
- 文件窗格标题条与终端同高、同焦点强调；导航工具条保留在标题条下并继续用
  `FlowLayout` 折行（窄面板不超框）。
- 关闭标题栏偏好时保留 6pt 悬停拖动把手，文件导航不隐藏。
- 文件窗格最小高度由 `FilesPanelView` **实测头部**（工具条折行 + 路径行 +
  搜索栏）上报 `WorkspaceFilePane.chromeHeight`，宿主按「标题条/把手 + 实测
  头部 + 64pt 内容区 + gap」聚合；首帧用等价旧值 210pt 兜底，宽度/搜索栏
  变化会重新测量并触发布局。

### 窗格拖放
- 窗格拖动使用专用 UTI `com.ghosterm.workspace-pane`，`NSDraggingSource` 只声明
  `.move`；文件条目使用 `com.ghosterm.local-files` / `com.ghosterm.remote-files`，
  两条路由完全分离。
- 拖动时按目标窗格四边预览落点，中央预览整格交换；松手才提交。
- 落点判定抽成纯函数 `WorkspaceDockingGeometry.resolve`（25% 边缘 → 停靠，
  其余 → 中央交换），四边/中央/边界/窗外/自身均有直接断言。
- 拖动会话结束回调统一发 `workspacePaneDragEnded` 通知并清预览，覆盖 Esc /
  系统取消路径（不只依赖 `draggingExited`）。
- 自身落点、跨标签页、已不存在的窗格、超出可用尺寸的落点均拒绝。

### 焦点 / 缩放 / 关闭
- `activeWorkspacePaneID` 统一当前窗格；宿主在文件窗格成为当前时把 AppKit
  firstResponder 交给其表格/大纲视图，终端窗格仍由 `TerminalHostView` 自己取键盘。
  已用真实窗口断言**双向**往返（终端 → 文件 → 终端），且后台标签页（仍挂载）
  的更新不抢当前窗格的 firstResponder。
- 缩放对任意类型窗格生效：兄弟宿主保持原 frame 并停画，还原不丢比例。
- 窗格关闭按钮只关该窗格；终端自行退出只移除该终端，文件窗格仍在时保留标签页；
  失败窗格保留明确错误。
- `⌘⇧E` 隐藏/显示文件窗格不销毁后端，路径与位置保留；纯文件标签页不允许隐藏成空白。
- 文件连接取消票改为**窗格级**（`cancelledConnects` 使用 file pane 的 id），关闭
  连接中的窗格后不再弹主机密钥/凭据框，迟到结果不会挂到新窗格上。

### 文件传输
- 本地→远端上传、远端→本地下载，默认复制不删源；多选与目录均支持。
- 源身份 = (PaneID, EndpointID)；下载用**源远端**后端，上传用**目标远端**后端，
  失效源明确拒绝，绝不借用当前标签页/最近后端。
- 落点目录在松手时快照，异步解码期间目标窗格导航不改变目的地；目录条目落点为该目录。
- 目录任务增加父级汇总 job：进度/总量汇总、空目录也有完成回调、失败不折算成功、
  取消不被迟到展开覆盖；重试绑定原任务端点。
- 成功后刷新实际落点窗格；冲突支持覆盖/跳过/改名与「应用到全部」。

### 入口与旧界面迁移
- 删除 `DualPaneFilesView` / `DualPaneSession`；SFTP 标签页进入同一工作区（本地+远端
  两栏），保留原认证与主机密钥校验。
- 新增 `TabKind.localFiles`（纯本地文件标签页，不启动 shell）+ 主菜单
  「打开本地文件窗格」、标签右键菜单入口、新建菜单「本地文件」。
- 远端文件标题栏提供「打开本地文件窗格」，已有本地窗格时聚焦而不重复创建。

## 2. 验证证据（本机 macOS 26 / arm64）

| 命令 | 结果 |
|---|---|
| `scripts/gate.sh` | C 核心 414 项测试全过，ABI 对拍通过 |
| `scripts/apple-app-gate.sh`（真 sshd） | 310 项、8 项环境跳过、0 失败 |
| `scripts/mutation-check-workspace.py` | 16 组：目标红 / 对照绿 / SHA256 复原 |
| `scripts/mutation-check-pane-chrome.sh` | 27 项全过（含 P5/P7/P8/P9 锚点迁移） |
| `scripts/mutation-check-tabs.sh` | 全过（T16 改为布局等分语义） |
| `scripts/mutation-check-files.sh` / `-dragdrop` / `-transfers` / `-split-divider` / `-focus` / `-occlusion` | 全过 |
| `apple/GhostermTerminal` IME + Surface | 23 项全过 |
| `scripts/build-mac-app.sh` | PACKAGE-MAC PASS，ad-hoc 签名 |

关键端到端覆盖：
- 真 sshd：目录+空目录上传/下载、关闭文件窗格不取消已入队任务、完成后刷新落点。
- 真实 `/bin/sh`：布局连续四边/中央移动后仍为同一 PID、同一 `TerminalHostView`、
  同一 `TerminalSurface`，期间输入法组合态不丢。
- 两个不同远端后端：下载/重试不串源。

所有临时数据与日志在 `tmp/`（已忽略），运行前置设置 `TMPDIR=<repo>/tmp`。

## 3. 未完成 / 需要加强的项

按 Spec 条款列出，**不影响当前已交付行为可用性**，但尚未达到「全部条款均有直接证据」。

### 3.1 本轮已补（2026-10-23）

1. **文件窗格最小高度**：`FilesPanelView` 实测头部（含 `FlowLayout` 折行与搜索栏）
   经 `onChromeHeightChange` → `WorkspaceFilePane.chromeHeight` 上报；宿主按
   「标题条/把手 + 实测头部 + 64pt 内容区 + gap」计算。断言覆盖宽/窄、搜索开/关、
   标题栏开/关；变异「文件最小高度用实测头部」目标红、对照绿。
2. **Esc 取消的预览清理**：拖动会话结束回调统一发 `workspacePaneDragEnded`，宿主
   清高亮，不再只依赖 `draggingExited`。已验证「结束回调 → 通知 → 宿主清预览」；
   AppKit 对 Esc 的会话终止是系统默认行为。变异「拖动取消清预览」有牙。
3. **拖动落点几何**：抽出纯函数 `WorkspaceDockingGeometry.resolve`，四边/中央/
   25% 边界/窗外/自身均有断言；变异「拖动落点中央判定」有牙。
4. **焦点往返**：真实窗口断言终端 → 文件 → 终端双向切换，以及后台标签页更新
   不抢 firstResponder。纯文件标签页首帧的 firstResponder 是窗口自身，列表由
   SwiftUI 在下一轮 runloop 才建出，故接管分同步 + 一次延迟重试两段。变异
   「文件拿键盘」「首帧文件键盘延迟接管」「后台标签不抢键盘」各有牙。
5. **系统标题栏带内工作区不参与「拖动=移窗」**（「窗格拖动会把整个窗口拖走」
   回归）：侧栏展开时右列没有顶栏，工作区上沿顶到窗口 y=0，窗格标题条（26pt）
   落在系统标题栏带（~28pt）内。AppKit 按**命中视图**的 `mouseDownCanMoveWindow`
   （非不透明视图默认 YES）决定要不要把 mouseDown 拖动劫持成移窗，被劫持的视图
   收不到 mouseDragged——`WorkspaceDragSourceView` 的 4pt 起拖永远不触发。修复：
   工作区子树里我们拥有的视图（窗格宿主/分隔条/拖动把手/终端 surface 锚点/终端
   视图）全部返回 false——工作区永不移动窗口，移窗只归顶带 `WindowDragArea`。
   断言：28pt 带内三档深度×全宽采样，hitTest 命中视图一律拒绝移窗（AppKit 只问
   最深命中视图，全树遍历会把无关的 SwiftUI 内部视图误报，采样才是操作性不变量）。
   变异「标题栏带内不移窗」有牙。
6. **文件行点击选中**（「点文件没高亮」回归）：元凶是行级
   `.contentShape(Rectangle()).onTapGesture(count:2)`——macOS SwiftUI 文档化
   问题，行上 tap 手势会吞掉 List 处理选中所需的点击（单击不高亮、双击照常
   打开，与用户症状吻合）。修复：行上不放任何手势，单击选中交还 NSTableView
   原生；双击打开改走 AppKit 通道（`ListDoubleClick` 给底层表装 doubleAction，
   `entryIndex` 处理错误行导致的行号偏移）。探针在表层驱动选中，断言
   tag↔绑定回流、键盘归属、doubleAction 已装机、行号映射纯函数。变异
   「双击通道装机」「错误行偏移映射」「tag 绑定回流」各有牙。
7. **测试框架边界哨兵**：`testSyntheticClicksCannotDriveSwiftUIListSelection`
   实证三种派发配方（window.sendEvent / NSApp.sendEvent / 全入队+runloop 泵）
   都驱动不了裸 List（零生产代码参与）的行选中——SwiftUI 点击选中需要真实
   事件流，XCTest 下不可达；对照配方（表级 selectRowIndexes→绑定回流）必须
   走得通。若某天哨兵变红=限制解除，文件窗格探针应升级为真实点击路径。

### 3.2 仍未完成

1. **拖动图像仍未在真实 `NSDraggingSession` 下断言**：起拖被标题栏带劫持的
   根因已修复并有带内采样断言（§3.1 第 5 条），但 4pt 起拖阈值与拖动图像本身
   仍缺真实会话下的证据（落点几何、取消清理已覆盖）。
2. **无瞬时零尺寸只有代码防护，没有逐帧证据**。`layout()` 有
   `width/height > 0` 过滤，移动测试也断言每步 >0；但 Spec 要求「无瞬时零尺寸」，
   建议在连续 dock 过程中逐次抓 frame（含首次挂载前）作为证据。
3. **多选拖出到 Finder 不完整**。本地条目拖动只为「代表条目」注册一个 fileURL，
   多选拖到 Finder 只会得到一项；远端拖出 Finder 按 Spec 不在范围。建议改为注册
   多 fileURL（`NSItemProvider` 逐项）或明确禁用多选拖出并给出提示。
4. **端点活性校验是注册表级，不是连接级**。`WorkspaceFileDrop.validateDrop` 只检查
   窗格/端点仍注册且 panel 非空；远端已断线时仍显示可接收，失败在传输队列暴露。
   建议在 `validateDrop` 增加 `TransferBackend` 可用性探测（缓存结果，避免每次拖动
   IO），失败时 `forbidden` 并提示。
5. **目录传输的 UI 级流程未端到端验证**。冲突弹框逐文件触发、父任务暂停/继续、
   取消与 skip 的组合、权限失败映射只在 data 层或部分覆盖。建议补：在传输队列窗口
   层面驱动一次「目录含同名文件」的冲突选择与取消。
6. **恢复默认布局仅实现未验收**：未断言「不新建/不关闭会话」、多终端/多文件窗格时
   的稳定顺序，以及自定义布局后打开新文件窗格不重置。
7. **窗口最小尺寸只按当前标签页宿主计算**。`contentMinSize` 在活动标签页布局变化时
    设置；多标签页各自最小需求更大时可能被低估。建议取所有标签页布局最小尺寸的最大值。
8. **文件列表快捷键（⌘F / 删除 / 空格）尚无合成按键回归**：当前已断言文件列表
    拿到 firstResponder（键盘不会再落到终端），但按键行为本身仍靠 `PanelKeyHandler`
    的单元覆盖。
9. **旧变异脚本的语义迁移需复核**：`mutation-check-pane-chrome.sh` 的 P7/P8/P9 已改为
    新宿主锚点；`mutation-check-split-divider.sh` 改为透明手柄语义。统一工作区变异
    已从 11 组扩到 20 组（新增最小高度/落点/取消/焦点/标题栏带/双击通道/行号映射/
    tag 回流），文件端点仍靠间接覆盖。
10. **行级 `.onDrag` 与点击选中的共存未在真机实证**：社区报告 macOS 上 List 行的
    `.onDrag` 在部分版本也会干扰选中。本轮只移除了文档化元凶（tap 手势）并保留
    onDrag（栏间拖/Finder 拖出的核心交互）。若真机打包后「点击没高亮」仍复现，
    onDrag 是第二嫌疑：临时摘除验证，必要时改用专用拖动手柄（像窗格标题条那样）。

## 4. 明确不在本次范围

- 跨标签页、跨 macOS 窗口移动窗格。
- 远端条目拖出到 Finder（异步懒下载文件承诺）。
- 本地↔本地复制、远端↔远端传输。
- 布局跨重启持久化（仅按标签页存活期记忆）。

## 5. 已知环境限制

- `build-mac-app.sh` 出的包为 ad-hoc 签名，`LSMinimumSystemVersion` 被 Homebrew
  libssh2/OpenSSL 抬到 26.0；正式分发需自建依赖 + Developer ID + 公证。
- `apple-app-gate.sh` 的 8 项跳过依赖真 sshd/钥匙串环境；离线跑不等于已验证。
- 本提交所在工作树还包含**此前未提交**的会话生命周期 / daemon 改动（`core/src/session/deploy.*`、
  `daemon/*`、`SessionSetupTransport`、`WindowPresentation`、相关 docs/scripts）。它们与工作区
  改动共享 `AppModel`/`TerminalTab`/测试文件，无法按 hunk 干净拆分，故随本次提交一并落库；
  后续如需单独回溯，请按上列路径区分。

## 6. 建议接手顺序

1. 原生拖动手势剩余验收（3.2 第 1 项）→ 4pt 起拖阈值与拖动图像。
2. 多选拖出 Finder（3.2 第 3 项）→ 明确的 Spec 缺口。
3. 端点活性与目录冲突 UI 验收（3.2 第 4、5 项）→ 稳性。
4. 恢复默认布局与多标签页最小尺寸验收（3.2 第 6、7 项）。
5. 文件列表快捷键的合成按键回归（3.2 第 8 项）。
6. 每项完成后先跑对应 XCTest，再补一条 `mutation-check-workspace.py` 变异（目标红/对照绿/
   SHA256 复原），最后统一跑 `gate.sh` + `apple-app-gate.sh` + 打包。
