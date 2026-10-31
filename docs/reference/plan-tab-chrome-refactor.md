# macOS 标签栏与窗格界面重构计划

> 状态：实施中。S1–S4 已落地并完成对应本机验证；S5 的本机交叉回归、原生截图及真实系统焦点 / 全屏验收已完成；macOS 15 CI 待验。
> 日期：2026-10-29。初始源码基线：`54331f1`；本轮窗格实现起点：`99342b6`。实现与验收进度分别见第 2.1、8 节。
> 2026-10-29 用户追加调整：顶部采用 Studio 式胶囊与最右加号，侧栏采用 Rail 式列表末尾新建入口，整体删除精简侧栏。以下历史 S1–S5 中的精简模式记录仅说明当时证据，不代表当前产品行为。
> 设计依据：[已确认的交互原型](../design/tabs-chrome-prototype.html)与[侧栏模式示意图](../design/tabs-split-preview.png)。
> 执行约束：以仓库 `AGENTS.md` 为准；每个切片包含源码、调用方、测试与变异的同步调整，检查通过后直接提交 `main`。

## 1. 已确定的产品行为

本轮仅改 macOS 原生窗口、标签导航、连接入口和窗格外观，继续使用 AppKit / SwiftUI、full libghostty 与既有 Transport 接缝。

| 区域 | 最终行为 |
|---|---|
| 标签布局 | 外观设置只选择顶部或侧边；移除精简侧栏。所有标签平铺，不分组 |
| 侧边模式 | 参考 Tern Rail：顶部仅留红绿灯；唯一的「＋ 新建标签页」紧随最后一个标签并随列表滚动；终端列上方没有全宽标题带、当前标签胶囊或占位空行 |
| 顶部模式 | 参考 Tern Studio：胶囊标签排列在红绿灯右侧，唯一的「＋」固定最右端；标签多时独立滚动，不挤走加号；不再显示侧边标签列表 |
| 常驻操作 | 删除窗口中的侧栏切换按钮和搜索图标；布局在外观设置，标签搜索保留菜单入口 |
| 连接 | 侧栏只列打开的标签；点击「＋」按需选择本地终端、SSH、文件或显式 tmux 入口 |
| 窗格分屏 | 按钮位于各窗格标题右侧，当前窗格或鼠标悬停时显示；操作对象就是按钮所属窗格 |
| 切分预览 | 悬停或键盘聚焦分屏按钮时，仅在目标窗格内显示方向和新增区域；预览不改变布局与输入焦点 |
| 切分完成 | 新窗格获得焦点；沿用已有本地 / SSH / 文件窗格的分屏语义 |
| 其他入口 | 窗格右键菜单、主菜单、关闭确认、缩放、拖动、文件功能继续可用 |

原型中的像素、示例进程、固定目录、假测试输出和配色只是示意。原生界面使用真实状态与现有字号、主题、间距偏好。特别是 HTML 的 48px 标题栏不等于当前原生 `WindowChrome.barHeight = 38pt`；节省空间以实际窗口几何为验收依据。

## 2. 现有实现与改动落点

下表保留初始基线的改动分析，不代表这些旧文件仍然存在。当前代码与后续任务见第 2.1 节；实施记录见第 8 节。以下路径均相对 `apple/GhostermApp/`。

| 现有代码 | 当前事实 | 重构处理 |
|---|---|---|
| `Sources/GhostermApp/GhostermApp.swift` | 生产入口创建 `NativeTitlebarWindow`，安装 `NativeTabWindows` | 改为单个原生窗口；终端生命周期继续归 TabManager / TerminalPane |
| `Sources/GhostermApp/NativeTabWindows.swift` | 每个应用标签有自己的 NSWindow；模型、原生选择、标题和顺序双向同步 | 退役每标签一个窗口的映射，关闭与 sheet 在唯一窗口内按身份路由 |
| `Sources/GhostermApp/NativeTitlebarWindow.swift` | 创建 `NSToolbar`，约束系统标签栏；关闭与右键菜单按 AppKit backing item 身份路由 | 退役私有标签栏补丁，顶部由统一原生导航视图绘制 |
| `Sources/GhostermApp/RootView.swift` | `nativeTabs && tabs.count > 1` 时终端列留一条 `barHeight` 空带；另有非原生标签测试路径 | 移除双路径与局部状态，以统一布局决定导航；工作区宿主保持同一层级 |
| `Sources/GhostermAppModel/WindowChrome.swift` | 全尺寸内容、原生红绿灯定位、resize 后重装、拖动与系统双击行为 | 由布局提供窗口控件安全区，保护终端与文件窗格的鼠标事件 |
| `Sources/GhostermApp/Sidebar.swift` | 标签、主机和通配块都在同一列表，主机区还承担编辑、新增与刷新入口 | 搬走主机管理能力后，改成纯标签列表，增加精简样式与排序 |
| `Sources/GhostermApp/BandControls.swift` | 红绿灯留白、侧栏开关、加号与配置错误提示共用一组 | 删除布局开关，保留唯一加号；错误提示迁移到明确可见的错误条 |
| `Sources/GhostermApp/TabStrip.swift` | 自绘标签条、滚轮横滚、`NewTabMenu`；生产顶部实际走 AppKit 标签栏 | 作为唯一生产顶部导航，复用动作与滚动并增加按 UUID 重排 |
| `Sources/GhostermApp/SettingsView.swift` | 外观页已有字号、主题、窗格间距、标题栏开关 | 添加标签位置与精简侧栏，保存后立即作用于现存窗口 |
| `Sources/GhostermApp/PaneChrome.swift`、`RootView.swift` 的 `PaneView` | 分屏按钮已经在窗格上，但终端标题栏只在当前窗格显示按钮 | 扩展为当前 / 悬停 / 键盘操作可见，保留身份明确的命令与预览 |
| `Sources/GhostermApp/WorkspaceFileView.swift` | 文件窗格独立实现标题与分屏、缩放、关闭按钮，当前未统一显隐 | 与终端窗格统一显示规则，同时保留文件专属动作 |
| `Sources/GhostermApp/WorkspaceView.swift` | `WorkspaceHostView.hosts` 按窗格 UUID 保持原生宿主；已有拖动落点预览与 `mouseDownCanMoveWindow = false` | 切分预览另存瞬时状态，不复用拖动预览状态，不重挂现有宿主 |
| `Sources/GhostermApp/AppMenu.swift`、`Sources/GhostermAppModel/Shortcuts.swift` | 菜单与快捷键目录双向校验；文件面板目前在「显示」菜单 | 统一布局命令，补主机管理与标签搜索；文件面板入口保留现有位置与 ⇧⌘E |

### 2.1 当前代码复核与剩余工作

`2efecae` 是 S1–S5 收尾阶段的历史基线；此后顶部胶囊/侧栏末尾新建、移除精简模式、窗格顶部蓝线与细框、Ctrl+Tab 切换已继续落地。配置重命名已在 `294d73a` 提交，Android A1 后续在 `186b95c` 完成当前设备档验收；这些切片没有改动 macOS 导航/工作区实现，也未产生新的完整宿主或 macOS 15 CI 证据。各次变更的独立证据保留在第 8 节；下表描述最终实现，不把旧截图和旧测试数量当作当前状态。

| 当前落点 | 已有基础 | 后续任务 |
|---|---|---|
| `TabWindowController.swift`、`RootView.swift` | 单窗口、唯一根视图；顶部与侧边互斥，后台工作区保持挂载 | macOS 26.6.2 的全屏往返、尺寸恢复和控件避让已验证；待 macOS 15 CI |
| `TabPresentation.swift`、`SettingsStore.swift`、`SettingsView.swift` | 标签位置持久化，外观设置已接入；精简模式及偏好已移除 | 本机设置与宿主身份检查通过；待 macOS 15 验证 |
| `Sidebar.swift`、`TabStrip.swift`、`TabInteraction.swift` | 纯标签列表、顶部最右 / 侧栏末尾唯一加号、按 UUID 选择与排序；不保留精简布局 | 最终两种导航有深浅色离屏截图；早期真实桌面三种布局是历史证据，不代表仍有第三种布局 |
| `ConnectionChooser.swift`（含 `HostManagerView`）、`TabSearchView.swift` | 主机管理迁出侧栏，连接与标签搜索已有独立入口 | 本机对应回归通过；待 macOS 15 验证 |
| `AppModel.swift`、`PaneActions.swift` | 分屏、关闭、缩放按标签 / 窗格 UUID 路由，终端与文件共用动作描述；目标校验和菜单 / 按键已验证 | 文件、关闭确认与默认入口闸门已通过 |
| `WorkspaceView.swift`、`SplitPreview.swift` | 独立预览层与清理接线已验证，含像素、命中、resize、失焦、切页、缩放、拖动与目标消失 | 本机对应闸门及真实系统焦点 / 全屏已通过 |

终端设置的正常重载已接入，但不能由此推断配置读写完整正确：2026-10-30 文档复核发现重复键编辑/回读不一致、读取错误折为空及初始缺失后的冲突判定缺口，详见[路线图 P1](roadmap.md#macos-下一步与共享缺口)。设置页字号/标题按钮/会话说明仍有旧文案，不影响下列历史导航证据的归属。

S1–S5 的本机实现与收尾已完成，macOS 15 对应提交的 CI 验收仍待核验。窗格边框收尾的 341 项测试、1 项开发态资源跳过、零失败是本计划的历史完整记录；更后的 Ctrl+Tab 切片记录六项定向测试和 menu 九组变异，不能把 341 项称作该切片重新全跑。最近完整宿主运行仍为 `4cbfcf4` 的 349 项：347 通过、1 跳过、1 项键盘设置页像素失败，未记全绿；跨屏修复的定向通过与真实双屏待复验分别见[后续记录](optional-tmux-implementation.md#后续跨显示器缩放修复2026-10-31)。锁屏期间确定性预览测试注入窗口焦点状态，生产失焦保护保持有效；普通 NSWindow 的真实激活、全屏和按键落点另有历史记录（第 8 节），不扩大为最新外观的桌面验收。

## 3. 窗口层方案与先行验证

### 3.1 S0 实测结果与最终架构决定（2026-10-29）

macOS 26 上用两个真实 NSWindow 组成原生标签组，分别在 anchor 与选中窗口调用公开的 `toggleTabBar(nil)`；每次等待 AppKit 完成布局后读取 `isTabBarVisible`，三次结果都是 `true`。公开接口无法隐藏多窗口标签组的栏，不能满足侧边布局。探针在工程 `tmp/tab-chrome-probe/main.swift`，不作为产品依赖。

根据用户补充的“不考虑兼容问题，确保代码架构简洁”，本轮改为**一个原生 NSWindow + 同一 TabManager + 两种原生导航视图**。顶部使用 SwiftUI / AppKit 控件绘制已确认的标签样式，侧边只改变导航布局。移除 `NativeTabWindows`、`NativeTitlebarWindow` 及其基于 AppKit 私有 ivar 的标签控件识别，不保留双宿主开关或测试专属渲染路径。

终端并不随导航重建：工作区一直在同一层级，按标签 UUID 保留宿主，后台继续挂载并停止渲染。切换导航只改变工作区的可用矩形。sheet 属于这一宿主窗口，关闭确认按标签 UUID 定位。窗口关闭必须确认可能中断的任务，不能沿用“每个窗口只关一页”的旧假设。

### 3.2 替代验收

旧原生窗口组断言随实现一起迁移为真实单窗口的等价功能证据，不删掉生命周期验证：

- 0 / 1 / 多标签、顶部 / 侧边两种布局，反复切换、resize 和全屏后几何正确；没有重复栏或顶部空带。精简侧栏已删除，历史三布局结果仅作追溯。
- 保持同一 NSWindow、WorkspaceHostView、TerminalHostView、surface 和 shell PID；当前 firstResponder 正确。
- 标签选择、重排、重名标签关闭与右键菜单按 UUID 定位，最后一页关闭保留空工作区。
- 红绿灯真实矩形与加号避让；拖动只在导航空白处生效，工作区不被标题栏抢鼠标事件。
- tmux、主机编辑与关闭确认仅由一个窗口呈现，切标签不产生另一份 sheet。
- 本机 macOS 26 实跑；macOS 15 证据由对应 CI 提供，未运行时不得声称通过。

原计划中“顶部原生栏 / 原生组双向同步”的表述由本节决定替代。相关脚本保留业务断言，移除 AppKit 私有视图类名与 backing ivar 的测试依赖。

## 4. 状态与动作设计

### 4.1 布局偏好

当前定义在 `Sources/GhostermAppModel/TabPresentation.swift`，由 `SettingsStore` 持久化：

- `TabPlacement`：`sidebar` / `top`，默认侧边。
- 精简侧栏及 `compactSidebar` 已删除，不再是第三种布局或待实现开关。
- 偏好写入 UserDefaults，不写入 Ghostty 配置或 `ssh_config`；非法存储值有明确默认解释。
- `AppModel` 发布唯一的布局状态，窗口根视图观察它。移除 `sidebarVisible` 与 RootView 本地开合状态作为并列生产真相。
- ⌥⌘S 与「显示 → 切换标签位置」调用 `toggleTabPlacement()`；主菜单与 `ShortcutCatalog` 均已接入，不再直接翻转旧 Bool。

原生红绿灯的安全区由 `WindowChrome` 与窗口实际几何决定，不能按 HTML 像素宽度缩小、重绘或遮住系统按钮。顶部加号固定右端，侧栏新建入口随标签列表滚动。

### 4.2 新建连接与主机管理

生产实现为 `ConnectionChooser.swift` 中的 `ConnectionChooser` 与 `HostManagerView`，唯一加号打开连接选择器。配置编辑复用 `HostEditorSheet` / `ConfigStore`，连接调用现有模型与认证，不存在计划中命名的 `NewTabChooser.swift`。

侧栏主机区已删除，以下入口已接入：

- 本地 shell、SSH 主机、本地文件、远端 SFTP、显式后台 tmux 会话。
- 「管理 SSH 配置…」：可搜索的主机列表、新增 Host、编辑现有 Host、刷新，以及通配块编辑。通配块可编辑但不出现在可连接列表中。
- 主机管理同时可从主菜单「文件」进入，不依赖打开新建面板。
- 配置未创建 / 合法空配置与 `loadError` 区分；读取失败明确显示错误与重试，不伪装成没有主机。
- 新建、编辑与 tmux 等 sheet 保留打开时的窗口归属；切换标签不会重复展示或迁移到另一窗口。

`⌘T` 保留当前 `newTabLikeActive()` 的行为：本地新建本地、SSH 新建同目标连接、文件标签新建对应文件标签。加号打开选择器与快捷键直接新建可以并存；HTML 的 ⌘T 行为不作为覆盖既有产品行为的依据。

### 4.3 标签搜索与状态信息

- 搜索只针对打开的标签，选择结果按 UUID 激活；重名标题不能选错对象。
- 「窗口 → 搜索标签页…」使用 `⇧⌘A`，已与主菜单、快捷键表及按键路由接线；Ctrl+Tab / Ctrl+Shift+Tab 按当前标签顺序循环切换。
- 不照搬原型的 ⌘K：`vendor/ghostty/src/config/Config.zig` 在当前 pin 中把 macOS Super+K 绑定为 `clear_screen`，本轮保留它的清屏用途。
- 标签显示真实连接状态、锁定、运行 / 等待提示、失败命令记号及必要的副标题；顶部与侧边共用身份和状态，不保留精简模式。
- 排序统一写 `TabManager.move`；侧栏拖动、顶部拖动、搜索与快捷切换均读取同一顺序。
- `terminalConfigError` 从旧 `BandControls` 移到可见错误条，提供进入设置的入口；正常状态不多画一颗图标，出错时不得静默。

### 4.4 窗格操作与预览

`AppModel` 已按标签 / 窗格 UUID 分派分屏、关闭和缩放命令。非当前窗格的悬停按钮操作所属对象，不借用当前窗格作为隐含目标；`PaneActions.swift` 提供共同的动作描述。

这些是 Swift 应用层接缝，不改 C ABI。每次执行时确认标签与窗格仍存在、类型正确且允许操作；失效的 UUID 不得回落到当前窗格。快捷键传当前对象，窗格按钮与右键菜单传所属对象，最终复用现有分屏 / 关闭实现。

`PaneActionsView`、`PaneContextMenu` 与 `SplitPreview` 已接入终端和文件窗格，行为约束如下：

- 可见条件为当前、悬停或键盘操作。控件使用固定预留宽度，出现时不改变窗格尺寸、不让标题反复跳动；隐藏后不可点击，键盘与 AX 仍有可发现的操作路径。
- 不因追求示意图的两颗按钮删除已有缩放、还原、关闭与远端文件专属动作；标题栏隐藏时保留主菜单、右键和现有隐藏拖动把手。
- 预览只持有标签 UUID、窗格 UUID 与方向，不能创建 surface / shell、改选中状态、改布局比例或发送终端尺寸变化。
- 覆盖层放在实际目标的内容区域，不能遮住按钮、截获终端输入、吞掉文件拖放；不进入终端字节流。
- 离开按钮、失焦、关闭菜单、取消、目标消失、切换标签、切换布局、缩放及拖动结束时清除预览。布局尺寸变化时重新按目标几何计算，不能留下旧矩形。
- 窗格拖动落点预览与分屏按钮预览分开存储；拖动期间不同时显示两套覆盖层。
- 本地分屏继承当前目录，SSH 仍建立对应连接，文件窗格仍复制文件浏览器语义。tmux 的显式选择与关闭只断开客户端等既有规则不变。
- 单个终端窗格标题不增加关闭标签页的隐藏入口；现有最后窗格关闭、任务未知时确认和文件关闭范围继续按实际模型执行，不照搬 HTML 演示里的禁用规则。

## 5. 实施切片与依赖

每片同步补测试和变异，不能把测试集中拖到最后一片。

| 切片 | 交付 | 主要文件 | 完成条件 |
|---|---|---|---|
| S0 窗口方案验证（已完成） | 多窗口组不能隐藏标签栏，改为单个原生窗口，见第 3 节 | AppKit 真实窗口探针 | 已记录公开 API 的限制与架构决定；生命周期证据在 S3 迁移 |
| S1 布局偏好（已落地） | 唯一布局状态、存储、外观设置控件与主菜单命令 | `TabPresentation`、`AppModel`、`SettingsView`、`AppMenu`、`Shortcuts` | 偏好发布与保存即时生效；测试注入隔离 UserDefaults，不污染用户偏好；最终布局已在 S3 接入 |
| S2 连接入口搬迁（已落地） | 新建选择器、主机管理与 sheet 归属 | `ConnectionChooser` / `HostManagerView`、`HostEditorSheet`、`RootView` | 主机新增、编辑、通配块、刷新、错误、SFTP、tmux 均可达；旧侧栏主机区已删除 |
| S3 标签与窗口外壳（已落地） | 无顶部空带的侧栏、顶部标签栏、唯一加号、错误条和标签搜索 | `Sidebar`、`RootView`、`TabStrip`、`TabWindowController`、`WindowChrome` | 生产宿主通过零 / 单 / 多标签、重排、resize、全屏与红绿灯避让；旧窗口组和精简模式已退役 |
| S4 窗格交互（已落地） | 统一显隐、身份命令、右键和切分预览 | `PaneChrome`、`PaneActions`、`WorkspaceFileView`、`PaneAppearance`、`WorkspaceView`、`SplitPreview`、`AppModel` | 非当前窗格操作不误切其他对象；预览不改变焦点和尺寸；实际切分聚焦新格且原任务不变 |
| S5 验收与收尾 | 交叉场景原生截图、CI 覆盖与文档、移除失效分支和锚点 | 测试、相关变异脚本、`macos-package.yml`、`AGENTS.md`、能力矩阵、UI pitfalls | 下表证据全部归位，旧断言退役有替代证明，未执行的平台证据如实列出 |

实施顺序为 S0 → S1 → S2 → S3 → S4 → S5。S3 已完成单窗口生命周期与断言迁移；当前剩余项是 S5 的 macOS 15 对应提交 CI 证据，不能重新按旧多窗口组或精简布局开工。

S4a（窗格动作）与 S4b（分屏预览）已随 `f6b72db` 完成，包含对应测试与 22 组变异。S5 进度如下：

1. **交叉回归（本机通过）**：第 6.3 节的相关闸门已复验。收尾发现文件双击 selector 错误和两处旧测试漏洞，已修复并补变异证据，详见第 8 节。
2. **系统与视觉证据**：七张原生离屏截图和六张真实桌面窗口截图已生成；系统激活、全屏进入 / 退出及尺寸恢复已验证。macOS 15 仍需取得对应提交的 CI 结果，历史绿灯不作本轮证据。
3. **文档与交付**：同步规则、能力矩阵、CI 覆盖及遗留限制；清理失效描述。只有第 7 节完成条件全部满足，才将整个计划标记为交付。

最终截图至少包括：侧边与顶部两种布局的深 / 浅色、多窗格悬停预览、终端与文件混排；空态、长标题与大量标签另做原生几何检查。截图用于视觉评审，焦点和会话保留仍以真实事件与对象 / PID 断言为准。

## 6. 验收与变异映射

### 6.1 必须保留和新增的证据

| 验收对象 | 观测与失败条件 | 现有测试 / 脚本落点 |
|---|---|---|
| 侧边无顶栏、顶部不遮内容 | 真实 NSWindow 中量 `WorkspaceHostView` 顶边与窗口顶边、栏与红绿灯的矩形；不靠纯函数或背景颜色判断 | `TabWindowTests`、`PaneChromeTests`；`tab-chrome`、`pane-chrome` |
| 唯一加号与无切换按钮 | 每种布局、错误状态、空态下实际控件数量与命中范围；顶部加号固定右端，侧栏加号紧随最后一个标签；两种布局都验原生按钮避让 | `TabStripVisibilityTests`、`SidebarChromeTests`；`tab-chrome` |
| 标签选择、顺序、关闭归属 | 顶部与侧边共用模型顺序；重复标题、非当前标签右键、非当前标签关闭有明确目标 | `TabWindowTests`、`TabContextMenuTests`；`tab-chrome`、`tabs` |
| 模式切换不重建终端 | 两个真实 shell 前后 PID、host / surface 对象身份相同；当前 firstResponder 正确，后台不抢输入 | `TabWindowTests`、`WindowFocusTests`、`WorkspaceTests`；`tab-chrome`、`focus`、`occlusion` |
| 选择器与主机管理 | 合法空列表、读取失败、通配块不可连接、编辑后备份 / 冲突处理、重复连接、sheet 归属 | `TabManagerTests`、配置测试、新选择器测试；`tabs`、`tab-chrome`、`default-entry`、认证相关脚本 |
| 设置与快捷键 | 隔离存储、非法值、即时传播；精简模式已整体退役；真 NSMenuItem 与 `ShortcutCatalog` 一致；⌘K 仍清屏 | `SettingsUITests`、`AppMenuCoverageTests`、终端输入集成测试；`settings-ui`、`tab-chrome`、`terminal` |
| 窗格按钮可见性 | 当前 / 非当前 / 悬停 / 键盘 / 标题栏隐藏；终端和文件窗格均验证控件命中与几何不跳动 | `PaneChromeTests`、`WorkspaceTests`、新窗格动作测试；`pane-chrome`、`sidebar-hover` 的跟踪经验 |
| 分屏目标与切分预览 | 悬停 B 不激活 B、不创建会话、不改 A 尺寸；点击 B 的按钮才切 B；失效 UUID 不回落；离开 / resize / 切页清覆盖 | 新预览几何与命令集成测试；`pane-chrome`、`workspace` 扩展 |
| 缩放、拖动、文件与 tmux | 不重置兄弟几何、拖动比例或文件工具条折行；预览不吞输入；后台任务和传输不受布局模式影响 | `PaneZoomTests`、`WorkspaceTests`、文件与 tmux 测试；`workspace`、`split-divider`、`files`、`tmux-ui` |

### 6.2 旧规则如何更新

本次用户确认的新设计替代以下旧规则，实施时必须改写对应断言与变异目标：

- `native-tabs` 的原生组断言迁移到单窗口的真实导航：顶部 / 侧边互斥，选择、标题、关闭确认与重排继续有牙。
- `native-sheet` 的固定占位锚点改为顶部预留正确 / 侧边不预留；tmux sheet 验证唯一宿主与切页不重复呈现。
- `band-controls` 的「侧栏开关在加号左边」改为无开关、唯一加号；配置错误可见、设置 / 传输 / 关于菜单可达继续保留。
- `sidebar-chrome` 的开关位置断言改为实际红绿灯与新建控件的避让；当前标签高亮、滚轮与悬停关闭继续保留。
- `PaneTitleBar.showsButtons(isFocused:)` 的只看焦点规则扩展到悬停与键盘；不能删掉既有标题栏、间距、缩放和聚焦视觉断言来换绿。

新增变异至少覆盖：侧栏重新塞入顶部空带、重复加号、布局设置不传播、配置错误被当空列表、主机管理入口丢失、操作 B 却切 A、悬停抢焦点或提前创建会话、覆盖层拦截输入、目标消失后预览残留、关闭按钮绕过目标确认。每项均记录目标红、同变异独立对照绿、复原绿与 SHA256 一致。

脚本继续使用工程 `tmp/`、起跑前标记扫描、输入锚点前置验证、EXIT / INT / TERM 恢复、`gt_timeout_require` 与 `gt_timeout`。不得以编译失败、找不到锚点或缺依赖冒充目标断言红。

### 6.3 执行检查

源码实施后的基础检查：

```bash
bash scripts/gate.sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer TMPDIR="$PWD/tmp" swift build --package-path apple/GhostermApp
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer GHOSTERM_AUTOMATION_OFFSCREEN=1 TMPDIR="$PWD/tmp" bash scripts/apple-app-gate.sh
```

按切片运行对应变异；最终至少复验 `tabs / tab-chrome / sidebar-hover / chrome / pane-chrome / settings-ui / focus / split-dim / occlusion / default-entry`，并执行 `python3 scripts/mutation-check-workspace.py`。动到终端输入、文件动作或 tmux 接线时，分别补跑 `terminal / files / tmux-ui`。

当前 full CI 接入 20 份变异脚本；新导航、窗格预览与外观均由 `mutation-check-tab-chrome.py` 覆盖，旧 `native-*` 五份脚本已退役。`workspace / pane-chrome / settings-ui` 等仍属手工证据；准确清单以 `AGENTS.md` 与 `.github/workflows/macos-package.yml` 为准。工作流接线不等于本轮 macOS 15 已通过，不能写成「CI 绿即本计划全验收」。测试里出现缺依赖或平台不适用的 SKIP，单独列出，不抵作该场景证据。

## 7. 范围与完成定义

除第 8 节已记录的配置读取错误修复外，本轮不改 `core/include/ghosterm.h`、C 实现、Transport、Ghostty pin、终端字节通路、SSH 配置存储协议或其他端 UI；不引入分组、工作区同步、私有 sessiond 或新的恢复机制。若实施确实需要突破这些边界，先单独记录原因与新增验收范围。

完成时同时满足：已确认的无顶部侧栏与窗格按钮在真实 macOS 窗口中成立；顶部模式可用；主机管理与原有关键动作可达；切换布局不重连、不丢焦点；新断言均有变异证据；两档系统证据和手工 / CI 覆盖如实记账。只有达到这些条件，才把本文状态从实施中改为已交付。

## 8. 实施记录

- S0：macOS 26 实测多窗口原生标签组不能隐藏栏，决定改为单窗口宿主（第 3 节）。
- S1 基础：标签位置 / 精简侧栏保存到注入的 UserDefaults，外观设置已加入对应控件；AppModel 通过既有 `@Published preferences` 传播。窗口壳与精简外观仍待 S3，菜单命名也在 S3 统一。
- S1 本机证据：`scripts/gate.sh` 31 项测试、71 个 C ABI 符号通过；`apple-app-gate.sh` 构建与 319 项测试零失败，包含真实隔离 sshd。1 项随包主题测试因开发态无 `.app` 资源而跳过，不算主题打包证据。外观页实测底部留白 18pt。
- S1 新变异：`mutation-check-tab-chrome.py --group preferences` 三组全部目标红、同变异对照绿、复原绿且 SHA256 一致。已有 `settings-ui` 的页面高度变异锚点随内容更新，全量复验列入 S5。
- 本机测试进程使用 `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` 与工程 `TMPDIR`；系统 `xcode-select` 指向 Command Line Tools，直接使用该工具链缺 XCTest。没有修改系统开发工具设置。

### S2 验证发现的必要边界扩展

真实错误夹具发现 `core/src/config.c` 的 `gt_config_parse_file` 在 `fread` 失败时未检查 `ferror`，把读目录产生的 EISDIR 当作空配置。这使选择器无法区分“尚未配置”与“读取失败”。因此本轮允许在该函数补读取错误检查，不改 C ABI；新增 C 回归与隔离变异，另跑 C gate / ABI / sshconfig 变异，界面继续复用统一配置入口，不新增一份 Swift 解析或磁盘读取实现。

S2 新入口已经接入：文件菜单的「新建连接…」与「管理 SSH 配置…」，以及加号菜单的「选择连接…」。选择器提供主机搜索与终端 / 文件 / 显式 tmux 三种方式；管理器支持搜索、新增、编辑通配块、刷新与错误重试。旧侧栏主机区和加号快捷菜单将在 S3 窗口壳迁移时统一删除，当前不将导航外观记为交付。

S2 验证：14 项连接选择器 / 配置编辑 / 菜单测试通过；读取失败已通过真实目录夹具触发，两个面板都用像素验证警告可见。`mutation-check-tab-chrome.py` 合并偏好与连接入口，共九组目标红、同变异对照绿、复原绿与 SHA256 一致。C 侧 `gate.sh` 为 32 项测试与 71 个符号通过，配置变异为 15 组（含新 A9）；恢复钩子另验证 EXIT / INT / TERM 恢复原字节并保留退出码，未带标记的外部修改不被覆盖且 SHA 不一致会报错。

S2 完整宿主回归：`apple-app-gate.sh` 构建与 324 项测试零失败，包含真实隔离 sshd；仍只有开发态随包主题检查这一项 SKIP，不能作为打包主题证据。下一步为 S3：生产窗口外壳、唯一加号、纯标签侧栏、顶部标签、搜索与重排，以及旧窗口组 / 测试双路径的同步退役。

### S3 单窗口导航已落地（跨系统与整体视觉验收待 S5）

生产已改为 `TabWindowController` 单窗口与唯一 `makeRootHostingView`，删除每标签窗口映射、私有 AppKit 标签栏识别、旧 BandControls 与无调用方的 HostAlias / TabStripVisibility。顶部和侧边共用 TabManager、TabInteraction 与 TabContextMenu，按 UUID 选择、排序、关闭。侧边只列打开的标签，无全宽顶部占位；精简侧栏宽 88pt，红绿灯与加号分行。唯一加号打开连接选择器，⇧⌘A 从主菜单打开标签搜索。

`TabWindowTests`、`TabNavigationContextTests`、`TabStripVisibilityTests` 与 `TabSearchTests` 替代旧 NativeTabWindows / NativeTabContextMenu 测试。真实窗口证据覆盖空 / 单 / 多标签、布局和尺寸变化、重名标签点击 / 关闭 / 菜单落点、终端宿主与 surface 身份、shell PID、窗格焦点状态与 firstResponder、tmux sheet 归属及统一退出裁决。

搜索通过真实菜单按键、原生文本输入与回车验证：按副标题找到重名标签、按 UUID 激活、搜索期间标题变动即时更新。macOS 26 的独立 NSMenu 探针发现新搜索键需要使用大写 `A` 才能匹配带 Shift 的事件；集成测试保护这一点。⌘K 用真实 shell 输出后的终端网格验证仍能清屏。S4 同时复核既有带 Shift 的窗格 / 文件菜单字母键，不能只依赖快捷键文字对拍。

变异脚本已收敛：五份 native 脚本与旧 band-controls / sidebar-chrome 并入 `mutation-check-tab-chrome.py`，共 39 组（preferences 3、connections 6、navigation 8、lifecycle 6、menu 5、sidebar 4、search 7），本机全部目标红、同变异独立对照绿、复原绿与 SHA256 一致。sidebar-hover 的两组改验共享 HoverTracking 与真实关闭落点，已复验通过。pane-chrome 的 P3/P4 迁到 navigation，其余窗格断言保留，最终再与 S4 一起复验。

完整宿主回归为 **324 项测试、零失败**，包含真实隔离 sshd；1 项随包主题测试因开发态无 .app 资源而跳过，不算打包证据。C gate 为 **32 项测试与 71 个公共符号通过**，Apple 绑定对拍为 69 项引用 / 2 项登记未用。新搜索快捷键增加后键盘设置页高度同步调整，实测留白 22pt。

full CI 从 24 份脚本收敛到 20 份，同时新增偏好、连接选择器、菜单与搜索的覆盖；这是接线记录，尚未取得本轮 CI 结果。真实全屏、macOS 15、S4 窗格交互、最终原生截图与相关完整变异仍未完成，不能把 S3 的本机通过当作整个重构交付。


### S4 窗格动作与预览已落地（整体交付仍待 S5）

`AppModel` 的分屏、关闭和缩放统一按 `tabID / paneID` 解析目标；无效、外来或隐藏文件目标不回落到当前窗格。终端与文件标题共用 `PaneActionsView` 和 `PaneContextMenu`，动作按钮固定占位，按当前 / 悬停 / 键盘焦点显现。标题隐藏时从 6pt 拖动把手进入同一菜单；终端内容区继续把鼠标交给终端引擎。移除外层重复点击手势，防止按钮完成分屏后又把焦点改回旧格。

`SplitPreview` 是独立、不接收输入的原生覆盖视图，按目标内容区显示右半区或下半区。AppKit 管理视图顺序，绘制时解析动态强调色；像素、命中与几何分别验证，不把 path 存在当作已绘制。预览状态与窗格拖动落点分离；仅在系统真正开始拖动后抑制分屏预览，避免异步准备拖动数据时提前松手留下错误状态。窗口失焦、切页、布局 / 当前窗格改变、缩放、Escape、离开按钮及目标移除都会清理，窗口 resize 则重算矩形。

新增 `PaneCommandTests` 三项与 `PaneActionsTests` 十项，覆盖真实 shell 目录继承、失效身份、文件动作、原生按钮事件、键盘焦点、预览像素与命中、窗口 / 标签失焦、尺寸变化、隐藏标题菜单和 Shift 快捷键。后两条按键验证发现旧 ⇧⌘D / ⇧⌘E 使用小写 keyEquivalent 无法匹配真实事件，已改为大写；原有快捷键目录与标签菜单对拍仍通过。

2026-10-29 本机证据：`apple-app-gate.sh` 为 **336 项测试，1 项随包主题跳过，零失败**，包含隔离真 sshd；C gate 为 **32 项测试，71 个 ABI 符号通过**，Apple 绑定为 69 项引用 / 2 项登记未用。`tab-chrome --group panes` 的 22 组变异已全部目标红、同变异独立对照绿、恢复绿且 SHA256 一致（全脚本现为 61 组）；既有 `pane-chrome` 为 24 个检查、`tabs` 为 37 个检查通过，终端包 `terminal-gate.sh` 为 24 项测试通过。原生 `swift build` 通过。日志位于工程 `tmp/pane-actions-*`、`tmp/pane-local-files-*` 与 `tmp/pane-file-focus-*`。

收尾另修两条真实回归：远端文件窗格的旧「打开本地文件」动作必须验证标签仍存在且源窗格可见；通过按钮分屏后必须把键盘交给新文件列表，而被动浏览按钮时保持焦点不被文件刷新抢走。两者都先复现失败再修复，并各有两个变异。旧 `showsSurface` 缩放占位分支已无调用方，整体删除；缩放继续由 WorkspaceHostView 保持原宿主与兄弟尺寸。

锁屏限制已定位：正常启动的独立 AppKit 应用同样不能激活，系统前台为登录窗口。测试只注入 NSWindow 的 `isKeyWindow` 状态，窗口内容仍由生产 `TabWindowController` 创建；这证明焦点接线与控件行为，不证明系统激活。真实全屏、系统焦点、最终截图及 macOS 15 CI 仍为 S5 待验项。


### S5 原生离屏视觉记录与交叉回归

2026-10-29 已从生产窗口控制器创建的原生视图抓取七张图；终端文字来自真实本地 shell 的 `pwd` / `uname` 输出，文件面板使用隔离测试目录。界面切换深 / 浅色，终端保留当前默认配色。

| 布局 | 浅色界面 | 深色界面 |
|---|---|---|
| 侧边标签 | [查看](../design/tab-chrome-native/sidebar-light.png) | [查看](../design/tab-chrome-native/sidebar-dark.png) |
| 顶部标签 | [查看](../design/tab-chrome-native/top-light.png) | [查看](../design/tab-chrome-native/top-dark.png) |
| 精简侧栏 | [查看](../design/tab-chrome-native/compact-light.png) | [查看](../design/tab-chrome-native/compact-dark.png) |

另有[终端与文件混排](../design/tab-chrome-native/mixed-files.png)。已逐张检查导航互斥、唯一加号、精简侧栏的窗口按钮避让、窗格按钮和目标半区预览。截图包含窗口框架；原生红绿灯呈灰色，反映测试应用没有获得系统激活。

**证据边界**：这是离屏视觉记录。测试窗口的 key 状态受控，抓 Metal 帧前显式设置 surface 可见并请求绘制，不能据此声称已验证正常桌面激活或系统全屏。独立的真实桌面证据见下方系统验收记录。

复现命令（图片生成在工程 `tmp/tab-chrome-screenshots/`）：

```bash
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
GHOSTERM_AUTOMATION_OFFSCREEN=1 GHOSTERM_CAPTURE_TAB_CHROME=1 TMPDIR="$PWD/tmp" \
swift test --package-path apple/GhostermApp \
  --filter 'PaneActionsTests/testHoverPreview|PaneActionsTests/testFilePaneUses'
```

2026-10-29 本机交叉回归已完成，逐项日志为 `tmp/tab-chrome-s5-*.log`。最终结果：

| 检查 | 结果 |
|---|---|
| `tab-chrome` | 本轮七组分类共 39 组变异通过；S4 的 panes 22 组已通过，合计 61 组 |
| `sidebar-hover / chrome / settings-ui` | 分别 2 组变异、9 项检查、23 项检查通过 |
| `focus / split-dim / occlusion` | 分别 14 / 10 / 10 项检查通过 |
| `default-entry / workspace` | 4 / 21 组变异通过；workspace 已用统一 gt_timeout 复验 |
| `files / split-divider` | 15 / 8 项检查通过 |
| S4 已通过的 `tabs / pane-chrome / terminal` | 37 / 24 项检查与 24 项终端测试，相关源码未再改动 |
| 最终 `apple-app-gate.sh` | 336 项测试，1 项开发态随包主题资源跳过，零失败；隔离真实 sshd 已运行 |
| 最终 `swift build` | 原生构建通过；Apple ABI 为 69 项引用 / 2 项登记未用 |

工作区最终日志为 `tmp/tab-chrome-s5-workspace-timeout.log`，宿主与构建日志为
`tmp/tab-chrome-s5-app-gate-final.log`、`tmp/tab-chrome-s5-build-final.log`。
C gate 的 32 项测试 / 71 个符号证据沿用 S4，收尾未改 C 实现或 ABI。
截图复现的两项测试通过，七张图片已检查；不据此跳过真实系统焦点、全屏和 macOS 15 证据。


### S5 系统验收（macOS 26.6.2 已完成）

2026-10-29 桌面解锁后，已使用生产窗口控制器创建的普通 NSWindow 完成以下场景；未覆盖 `isKeyWindow`，未手动设置 fullScreen styleMask：

- 分别从顶部、侧边与精简侧栏进入系统全屏，再退出；检查终端顶边、唯一加号、窗口按钮避让及原窗口尺寸恢复。
- 在同一窗口中切换两条真实 shell 标签及导航布局，检查原有宿主 / surface / PID 保留、键盘输入落在当前窗格；点击非当前窗格分屏按钮后输入落到新窗格。
- 切到其他应用再切回，检查预览消失、按钮重新可用、焦点和终端渲染恢复；全屏中也执行一次。
- 记录实际提交、系统版本、通过 / 失败和屏幕截图。macOS 15 另以该提交的 full CI 结果登记，不能引用之前提交的通过结果。

本次实现基线为 `0086907`，27 条原始观测保存在 [desktop-observations.json](../design/tab-chrome-native/desktop-observations.json)。结果：

| 场景 | 实测结果 |
|---|---|
| 标准侧栏 / 顶部 / 精简侧栏 | 普通窗口和全屏均为真实 active / key；唯一加号，与可见系统按钮不重叠 |
| 终端列顶部预留 | 侧栏和精简模式 0pt；顶部模式 39pt（38pt 标签栏 + 1pt 分隔线） |
| 系统全屏往返 | 三种布局各一次，另一次跨应用切换；收到 4 次进入及 4 次退出完成通知 |
| 窗口尺寸 | 每次由 1000×600 进入 2560×1080 全屏，退出恢复原尺寸及位置 |
| 标签与会话 | 根视图、三个终端宿主保持同一对象；三个真实 shell PID 全部保留 |
| 系统失焦 / 重新激活 | 普通与全屏中预览均清除，切回后 key window、当前 Space、firstResponder 与窗格焦点恢复 |
| 按键与分屏 | NSEvent 经 NSApp 派发，仅当前窗格收到 desktop1；点击非当前窗格分屏后，新窗格独占 desktop2，旧窗格未收到 |

| 布局 | 普通窗口 | 系统全屏 |
|---|---|---|
| 侧边标签 | [查看](../design/tab-chrome-native/desktop-sidebar-normal.png) | [查看](../design/tab-chrome-native/desktop-sidebar-fullscreen.png) |
| 顶部标签 | [查看](../design/tab-chrome-native/desktop-top-normal.png) | [查看](../design/tab-chrome-native/desktop-top-fullscreen.png) |
| 精简侧栏 | [查看](../design/tab-chrome-native/desktop-compact-normal.png) | [查看](../design/tab-chrome-native/desktop-compact-fullscreen.png) |

**方法与边界**：桌面探针在工程 tmp 的隔离副本运行，复用生产 TabWindowController、根视图和本地 PTY；
窗口没有焦点替身，Metal 未被强制设为可见。鼠标悬停、按钮点击与字符按键为合成 NSEvent，
经过 AppKit 派发，未使用物理键盘自动化，也不宣称覆盖所有 IME / 辅助功能行为。
动画与跨 Space 切换必须异步等待系统完成；仅等 key 状态翻转而立即反向切换，会把尚未结束的
Space 过渡带进下一项。等待真实 isOnActiveSpace 后，上述焦点往返通过，未改生产焦点逻辑。

原始探针为 `tmp/TabDesktopProbeTests.swift`，最终日志为 `tmp/tab-desktop-validation.log`；
运行于 `tmp/workspace-mutations-7srjuix6/apple/GhostermApp`，命令为设置 Xcode DEVELOPER_DIR 和工程 TMPDIR 后
`swift test --package-path <隔离副本> --filter TabDesktopProbeTests`。临时测试未加入产品或常规测试集，
不会让 CI 抢占桌面；既有 336 项宿主测试和变异闸门保持不变。本轮只提交观测记录和截图。

macOS 15 CI 仍待运行；本机通过不替代跨系统证据。


### S5 交叉回归发现的修复

- **标题栏命中测试**：旧测试误把工作区局部坐标传给 `hitTest`，采到了窗口底部，放开拖动把手的移窗行为后仍假绿。改为父视图坐标，并先证明命中终端与文件标题的拖动把手；生产拖动行为不变。
- **文件双击动作**：`Coordinator.perform(_:)` 与 `NSObject.perform(_:)` 重载冲突，selector 实际成为 `performSelector:`，派发时报错。处理函数改名 `openClickedRow(_:)`。测试通过生产窗口安装的 action / target 验证目录实际打开；仅提供只读 clickedRow 的事件数据，不将其称作物理双击证据。
- **工作区验证脚本**：保留“安装缺失”变异，另加“错误 selector”变异，共 21 组。每组配独立对照，失败输出原始日志，超时统一走 `gt_timeout`。先前两条假绿不计为验收证据，以修复后的整组结果为准。

修复已提交为 `2efecae`，相关源码、测试、脚本与证据口径同步更新。文件、工作区及完整宿主回归均已通过；见本节结果表。


### 2026-10-29 视觉审核追加调整：Studio / Rail

按用户新要求替代第 1 节原来的加号位置与精简模式：顶部标签采用胶囊外观，关闭按钮放在胶囊右侧并继续悬停显示，唯一新建按钮固定窗口最右端；侧栏只在顶端预留红绿灯，标签采用左侧图标与标题、右侧状态，唯一「＋ 新建标签页」紧随最后一个标签并随列表滚动。

精简侧栏的偏好键、设置控件、布局分支、宽度常量与预览清理状态均删除；保留顶部 / 侧边切换、UUID 路由与终端宿主身份。旧精简断言随功能退役，保留偏好隔离测试；原来「两种布局加号同位置」的断言和变异替换为右端固定 / 列表末尾两条规则，另以真实标签边界验证红绿灯避让。

本机原生构建及完整 Apple 闸门通过：336 项测试、1 项开发态随包主题资源跳过、0 失败，包含隔离真 sshd；C 闸门 32 项测试和 71 个 ABI 符号通过。`tab-chrome` 的 navigation 9 组、preferences 2 组、sidebar 4 组变异均证明目标红、同变异对照绿、回滚绿及 SHA256 一致。

审核页面已更新为本轮侧栏 / 顶部 × 深 / 浅色四张截图，入口仍为 `docs/design/tabs-chrome-prototype.html`；记录见 `docs/design/tab-chrome-native/layout-review.json`。本轮桌面处于锁屏，使用生产 NSWindow、真实本地 PTY 与 full libghostty 的离屏渲染截图，显式唤醒 surface 以抓帧；这四张图只用于视觉审核，不冒充系统激活或全屏的新证据。历史桌面截图仅归档，不在当前审核页继续显示。macOS 15 CI 和用户视觉审核尚未作为本轮已通过项记账。


### 2026-10-29 视觉审核追加调整：统一配色与去边框

按最终选择合并 A / B：导航和窗格标题使用终端已生效的背景色，去掉窗格外框与标题底线，当前分屏只在标题前画 16×2pt 短线，保留非聚焦内容淡化。没有新增风格设置；现有「窗格间距」继续生效。未设置过间距时默认 0；用户保存过的 0 / 6 / 10 / 14pt 偏好保留。间距为 0 时画低对比的 1pt 分隔线，有留白时不再画分隔线，拖动命中区始终保持 6pt。

顶部加号仍固定在最右端，侧栏加号仍紧随最后一个标签；分屏、缩放、关闭按钮仍属于各窗格。主题切换与导航切换保留现有终端宿主。背景和前景读取 full libghostty 的配置；条件主题通过 `CONFIG_CHANGE` 缓存已解析颜色，原始配置继续保留条件选择规则。原生按钮同时更新图标与文字颜色，避免系统外观与终端主题相反时失去对比。

审核页 `docs/design/tabs-chrome-prototype.html` 展示顶部 / 侧栏 × 深 / 浅色终端 × 0 / 10pt 间距，共八张生产窗口截图。图像来自真实 PTY 与 full libghostty 的离屏抓帧；桌面锁屏，不能用来证明真实系统激活或全屏。Tern 四种参考图保持原样，A/B/C HTML 示意保留为历史探索。


本机验证（macOS 26.6.2）：`gate.sh` 的 32 项 C 测试与 71 个 ABI 符号通过；`terminal-gate.sh` 的 24 项测试通过；`apple-app-gate.sh` 原生构建与 339 项测试通过，1 项开发态随包主题资源跳过、0 失败，包含隔离真 sshd。`pane-chrome` 的 24 项检查和 `split-divider` 的 10 项检查全绿；`tab-chrome` appearance 七组以及配置警告、标签高亮两组均证明目标红、同变异独立对照绿、回滚绿与 SHA256 一致。像素取样先将固定 sRGB 期望值转换到抓帧的 ICC 色域，避免显示器颜色转换造成假红。

浏览器复验八种截图切换、Tern 四种参考切换与 390px 宽度布局通过；当前源码及图像哈希记在 `tab-chrome-native/layout-review.json`。本轮没有重跑真实桌面激活 / 全屏，也没有新增 macOS 15 CI 证据；用户视觉审核仍以当前页面为准。日志保存在工程 `tmp/chrome-ab/`。


### 2026-10-29 视觉审核追加调整：顶部焦点线与留白边界

用户反馈标题前的短蓝线不够自然，有间距时窗格边界不清晰；本轮替代上一节的短线与去外框决定。参考 [Tern 官方演示](https://stencil.so/tern) 的 Tiles / Rail：当前分屏使用贯穿顶部的 2pt 蓝线，有间距时辅以 1pt 淡蓝外框，其他格为中性细框；间距 0 时不画外框，只保留工作区已有的共享细分隔线。统一导航底色、标题无底线、现有间距设置与窗格内按钮保持。

焦点装饰由终端 / 文件共用的 PaneCard 绘制，不参与布局或命中；关闭标题栏仍可辨认当前格，单格不画焦点线。新像素断言用真实生产窗口的上下终端 + 文件混排，切换焦点、间距、标题开关，检查顶部三个位置与左右边界；变异覆盖删外框、缩短顶边、断开文件焦点接线，并保留独立对照。

审核页增加四种场景：左右分屏、上下分屏、混排终端聚焦、混排文件聚焦。每种均含顶部 / 侧边标签、深 / 浅色与 0 / 10pt 间距，共 32 张原生窗口截图。截图来源仍为生产 NSWindow、真实 PTY、full libghostty 与本地文件面板；桌面锁屏，使用离屏抓帧，不作为真实系统激活 / 全屏证据。


用户随后指出浅色侧栏「新建标签页」文字过浅。像素回归复现浅色文字被原生按钮重着色为白色；只检查 contentTintColor 和 attributedTitle 的属性值不足以发现它。新建按钮仍保留原生命中、键盘与 AX，只在 cell 的标题绘制中使用已保存的显式前景色并居中；图标同时使用显式 symbol palette。回归逐次检查浅 → 深 → 浅的实际文字像素，变异恢复原生标题绘制后必须目标红、原先的加号配色属性对照仍绿。


本轮本机验证（macOS 26.6.2）：C gate 的 32 项测试与 71 个 ABI 符号通过；`apple-app-gate.sh` 原生构建及 341 项测试通过，1 项开发态随包主题资源跳过、0 失败，包含隔离真 sshd。`pane-chrome` 为 24 项检查全绿；`tab-chrome` appearance 的 12 组分别证明目标红、同变异独立对照绿、复原绿及 SHA256 一致。「贴边不叠框」以间距几何作为独立对照，避免把同样受新增描边影响的顶边 / 中线像素误当独立对照。浏览器复验 32 张新图、Tern 四种布局与 390px 窄屏通过；最终图像及源码 SHA256 均与 `layout-review.json` 一致。日志在工程 `tmp/pane-border-study/`；未新增 macOS 15 CI 或真实桌面激活 / 全屏证据。


### 2026-10-29 补齐标签循环快捷键

主菜单「显示」与设置「键盘」新增 Ctrl+Tab（下一个标签页）和 Ctrl+Shift+Tab（上一个标签页）。按当前显示顺序首尾循环，拖动重排后跟随新顺序；空清单不操作，单标签保持原页。切换复用现有标签激活与键盘焦点接线，不重建终端宿主。AppKit 的 Shift+Tab 使用 Backtab（U+0019），反向菜单项绑定该字符。

`TabCycleTests` 经 `NSApp.sendEvent` 向生产窗口发送按键，使用三个真实 shell 验证顶部 / 侧边导航下的重排、双向循环、终端宿主身份与 firstResponder；另验空清单和单文件标签。`tab-chrome --group menu` 新增四组变异，分别破坏 Ctrl 绑定、反向动作、Backtab 字符与首尾循环；每组以导航选择 / 重排测试作为同变异独立对照。

本机 macOS 26.6.2 原生构建和六项定向测试通过；`gate.sh` 的 32 项 C 测试与 71 个 ABI 符号通过。`mutation-check-tab-chrome.py --group menu` 全部九组通过（含新增四组）：目标红、同变异对照绿、复原绿且 SHA256 一致。macOS 15 CI 与物理键盘操作尚未作为本轮已通过证据。
