# Apple 端 UI 已知坑（SwiftUI / AppKit）

> 只收**踩过、且留下断言或注释**的坑——每条给「现象 / 根因 / 规避 / 证据」。
> 反直觉但没证据的传闻不进这里。证据一律指到测试与变异脚本。
> 相关：`apple/README.md`（宿主结构）、`terminal/apple/README.md`（渲染面）、
> `AGENTS.md`（变异验证清单）。

## 1. `Menu` 的 **label 与条目只认一个视图**——多给的兄弟视图被 AppKit 吃掉

**现象**：设置页「外观」里两个配色下拉**右边那一小块配色预览**不显示
（用户实测报的「主题预览没正常显示」）。

**根因**：`Menu` 在 macOS 上不是「任意视图包一层」——它下面的
`label:` 会被 AppKit 收编成**按钮**的内容，只认**一个**视图。多给一个
兄弟视图（哪怕只是 `HStack` 里的第二个元素），被吃掉的往往是**文字或色块**
中的一个，且**不报错**（数据面全对，就是画不出来）。

**踩过的两种写法（都不要回去）**：

```swift
// ❌ 色块进 Menu 的条目：条目被压成真 NSMenuItem，普通视图撑不住
ForEach(themes) { name in
    Button(name) { … } label: {
        HStack { Text(name); ThemeSwatch(…) }   // 塌成字面量 "$_" + 几个色块，名字全丢
    }
}

// ❌ 色块进 Menu 的 label：label 只认一个视图，色块整个不画
Menu { … } label: {
    HStack { PopupLabel(title: …); ThemeSwatch(…) }   // 色块消失（用户报的就是这个）
}
```

**规避**：`label` 里**只放一个视图**；其余控件当 `Menu` 的**兄弟**放外面：

```swift
// ✅ 落点：SettingsView.slotPicker
HStack(spacing: DS.Padding.s) {
    Menu { … } label: { PopupLabel(title: selected ?? "（不设置）") }
    if let preview = preview(of: selected) { ThemeSwatch(preview: preview) }
}
```

iGhostVT 在 Mac/Catalyst 上踩的是**条目**那一半，它的注释原话是
*AppKit draws no image on a Catalyst popup menu's items, so they cannot be
in it*——它也是把色块挪到按钮**旁边**。

**证据**：

- 断言：`SettingsUITests.testThemeSwatchRendersBesidePicker`——塞一套**纯红底**
  的临时配色，数页面里色块背景色的像素（选不中就是 0）；带对照（不选配色时
  红像素必须≈0，证明认的是色块而不是别处的红）。
- 变异：`scripts/mutation-check-settings-ui.sh` 的 **U7**（把色块塞回 label →
  断言变红 + 指向性对照仍绿）。
- 位图证据：把真视图抓帧后，`label` 写成 `HStack { Text; 方块 }` 时**只剩文字**
  （方块被吃掉，且顺序无关——`Text` 在前在后都一样）；改成把方块当 `Menu` 的
  兄弟后，两者都正常。真页面同口径：修前外观页只有两个下拉、右边空白；修后
  两档都画出了「底色块 + 八色调色板条」。

## 2. （测试）抓帧的 `cgImage` 必须在 `cacheDisplay` **之后**取

**现象**：像素断言恒为 0（「什么都没画」），但肉眼看视图是对的。

**根因**：`bitmapImageRepForCachingDisplay` 返回的是**空** rep，内容要等
`cacheDisplay(in:to:)` 才被画进去。**先**取 `rep.cgImage` 拿到的是那张空帧。

**规避**：

```swift
let rep = host.bitmapImageRepForCachingDisplay(in: host.bounds)!
host.cacheDisplay(in: host.bounds, to: rep)   // 先抓帧
let cg = rep.cgImage                           // 再取位图
```

**证据**：`PixelRenderTests` / `SettingsUITests` 都按这个顺序取帧；反过来写
时 `testThemeSwatchRendersBesidePicker` 的红像素恒为 0（曾经真栽过一次）。

## 3. 抓**自己进程内**的视图不需要「屏幕录制」权限

**澄清一条记错的结论**：早先记为「无屏幕录制权限时抓像素会 SIGSEGV」，据此
闸门只到「渲染层已挂」。复核后：**会崩的是 `CGWindowListCreateImage`（抓别人
的窗口，需要权限）**；`NSView.bitmapImageRepForCachingDisplay` + `cacheDisplay`
抓**自己进程内**的视图**不需要**该权限（本机实测无权限下正常出图）。

> 唯一的注意点：抓到的可能是**全 0 的帧**（没内容），那是「这一帧没东西」，
> 不是权限拦下——所以像素判据要**做内容判别**并配对照，别把空帧当通过。
> 见 `PixelRenderTests`（同一视图换转义序列，证明判据认的是绘制结果）。

## 4. 动态 `NSColor` 的 `.cgColor` 会**按 `NSAppearance.current` 求死**——不是按窗口的外观

**现象**：用户报「外观切成深色、重启后设置界面看不清了」——设置窗口是**白底**
配**浅色字**；主窗口却是正常的深色。

**根因**：`SettingsContainerController` 早先在 `viewDidLoad` 里写

```swift
view.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
```

`NSColor.windowBackgroundColor` 是**动态色**，`.cgColor` 会把它求成一个固定的
`CGColor`——求值时用**当前绘制上下文的外观** `NSAppearance.current`。问题在于
`viewDidLoad` **不在一次绘制里**，此时的 `NSAppearance.current` 是**系统**外观
（本机浅色），**不等于**窗口的 `effectiveAppearance`（用户选的深色）。于是底色
被求成**白**，而 SwiftUI 的文字按窗口的深色外观渲染成浅色 → 白底浅字，看不清。
（更坑的是它只求一次：切外观也不会跟着变。）

**规避**：让 AppKit 在**绘制期间**来问颜色——那时 `NSAppearance.current` 就是
本视图的 `effectiveAppearance`。用 `wantsUpdateLayer` + `updateLayer`，并在
`viewDidChangeEffectiveAppearance` 里标脏重画：

```swift
final class SettingsBackgroundView: NSView {
    override var wantsUpdateLayer: Bool { true }
    override func updateLayer() { layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor }
    override func viewDidChangeEffectiveAppearance() { super.viewDidChangeEffectiveAppearance(); needsDisplay = true }
}
```

> 同理：外观偏好的变更要在 `didSet` 里**同时**喂 `NSApp.appearance`（界面）与
> libghostty 的色板事件（终端）；只喂一处就会出现「界面/终端不一致」，而只喂
> 启动那一次就是用户报的「切了界面不动、要重启才变」。

**证据**：

- 断言：`SettingsUITests.testSettingsBackgroundTracksAppearance`（深色下量窗口底
  的像素必须偏暗、浅色下偏亮；对照是深浅两档一亮一暗）与
  `testAppearancePreferenceAppliesToApplicationLive`（改偏好后 `NSApp.appearance`
  必须立刻跟着走）。
- 变异：`scripts/mutation-check-settings-ui.sh` 的 **U8**（删掉 `didSet` 里落
  AppKit 那半截 → 实时刷新断言变红）与 **U9**（底色回到 `viewDidLoad` 里
  `.cgColor` 求死 → 深色下底色断言变红）；两条都带指向性对照仍绿。

## 5. `gt_ghostty_surface_free` **必须在主线程**——否则 join 自己 → panic

**现象**：`apple-app-gate` 的 SSH e2e 偶发 panic（`HostKeyDialogTests`
最显眼，但不是它独有）：`reached unreachable code` +
`std.Thread.zig: .DEADLK => unreachable, // two threads tried to join each other`。

**根因**：`TerminalSurface.deinit` 里调 `gt_ghostty_surface_free` →
libghostty `Surface.deinit` 会 `renderer_thr.join()` / `io_thr.join()`。而
libghostty 的回调是在 **io 线程**上派发的；若**最后一个强引用**恰好在一次
io 线程回调（实测是 `onResize`）里被释放（ARC 的临时引用），`deinit` 就在 io
线程上执行 → `io_thr.join()` = **join 自己** → `EDEADLK` → panic。

**规避**：把释放搬回**主线程**（本就是 libghostty 的线程契约），并把回调
`box` 一起带过去——C 侧 userdata 是 `passUnretained(box)`，free 完成前必须让
box 活着，否则 io 线程若再回调就是 use-after-free：

```swift
deinit {
    let raw = self.raw, box = self.box
    if Thread.isMainThread { gt_ghostty_surface_free(raw) }
    else { DispatchQueue.main.async { gt_ghostty_surface_free(raw); _ = box } }
}
```

**证据**：

- 注释：`TerminalSurface.swift` 的 `deinit`（长注释写明线程契约与 UAF 考量）。
- 复现（`tmp/` 里的一次性脚本，起真 sshd 反复跑 `HostKeyDialogTests`）：
  **改前 20 跑 1 中，改后 60 跑 0 中**。
- **没有**确定性断言/变异：这是时序竞态（1/N），造不出稳定红——故只用复现
  循环 + 注释取证（诚实标注：这一条不像前四条那样有常驻闸门）。

## 6. 浅/深色切换要**逐个 surface** 下发——app 级事件改不了已有 surface

**现象**：用户报「切到深色时，终端颜色没及时刷新，**新建**终端是新的了」——
已开的标签页停在旧配色，新开的才对。

**根因**：libghostty 的 `theme = light:X,dark:Y` 靠**条件状态**
（`configpkg.ConditionalState.theme`）选边。每个 **surface 各自持有一份**
条件状态（`Surface.init` 时从 `app.config_conditional_state` **拷贝**），此后
`ghostty_app_set_color_scheme`（app 级）只改 app 自己那份，**不会**回头改已有
surface 那份。所以：

- 已有的 surface：自己那份还是旧档 → 不换色；
- 新开的 surface：创建时拷贝到的是新档 → 换色。

「只有新开的才对」就是这么来的。另外，`colorSchemeCallback`（surface 级）只是
**把「该重读配置」当 `reload_config` 动作丢回嵌入侧**——嵌入侧不消费它，
条件状态改了也不会重派生配置，同样不换色。

**规避**（两处一起做，缺一不可）：

1. `gt_ghostty_app_set_color_scheme` 除了 app 级事件，还要**遍历已有 surface**
   逐个报 `ghostty_surface_set_color_scheme`（对齐上游 GTK apprt 的
   `handleStyleManagerDark`：`colorSchemeEvent` + 遍历 `surfaces` 调
   `colorSchemeCallback`）。所以 shim 的 app 结构里记着一份 surface 清单。
2. `cb_action` 要**消费** `GHOSTTY_ACTION_RELOAD_CONFIG`：app 目标 →
   `ghostty_app_update_config`，surface 目标 → `ghostty_surface_update_config`
   （上游 macOS 在 `Ghostty.App.swift` 的 `configReload` 里就是这么落的）。

**证据**：

- 断言：`TerminalSurfaceTests.testColorSchemeSwitchReachesExistingSurface`——用
  **DEC mode 2031（配色方案上报）**观测：程序开启后，切档会让**已有** surface 吐
  `CSI ?997;1n`（深）/`;2n`（浅）。不依赖窗口与像素，确定性。
- 变异：`scripts/mutation-check-terminal.sh` 的 **T3**（砍掉逐个 surface 下发 →
  红）与 **T4**（不消费 `reload_config` → 红）；两条都带指向性对照仍绿。

## 7. 终端视图不实现 `NSTextInputClient` → 中文输入法根本不被唤起

**现象**：用户报「终端无法输入中文，没有弹出中文输入法」——切到拼音等输入法后
敲键，既没有候选窗、也没有组合串，终端直接把原始 ASCII（`a`/`b`/`c`…）当文本收下。

**根因**：`TerminalHostView` 早先只在 `keyDown` 里读 `event.characters` 直接编码
转发，**完全没碰系统输入法**。macOS 的 IME 不是「按下一个键就给一段文本」，而是由
`NSTextInputContext` 驱动——视图必须

1. 声明自己实现 `NSTextInputClient`（AppKit 才会给它建立输入上下文、挂上输入法）；
2. 在 `keyDown` 里调 `interpretKeyEvents([event])`，把事件交给输入法；

输入法再经 `setMarkedText`（组合中）/ `insertText`（提交）/ `firstRect`（候选窗
定位）回调回来。**缺任一环，整条输入法链路静默断掉，屏幕上不报任何错。**

**规避**（照上游 Ghostty 的 `SurfaceView`（AppKit）同口径）：

```swift
public final class TerminalHostView: NSView, NSTextInputClient {
    private var markedText = NSMutableAttributedString()
    private var keyTextAccumulator: [String]?

    public override func keyDown(with event: NSEvent) {
        keyTextAccumulator = []
        let markedBefore = markedText.length > 0
        interpretKeyEvents([event])                 // ← 不调它，输入法不会被唤起
        let committed = keyTextAccumulator; keyTextAccumulator = nil
        syncPreedit()                               // 组合串 → 引擎内联显示
        if let committed, !committed.isEmpty {      // 输入法提交：键入语义下发
            for t in committed { surface.sendCommittedText(t) }
            return
        }
        if markedText.length > 0 || markedBefore { return }   // 组合中：键归输入法
        surface.sendKeyEvent(/* 普通按键走原编码路径 */)
    }
    public func setMarkedText(_:selectedRange:replacementRange:) { …; syncPreedit() }
    public func insertText(_:replacementRange:) { … }
    public func firstRect(forCharacterRange:actualRange:) -> NSRect { /* surface.imePoint */ }
    public override func doCommand(by selector: Selector) {}   // 防 NSBeep
}
```

三个**配套**入口（都补在 `CGhostty/shim.c`，缺一不可）：

- 组合串**内联显示**走 `ghostty_surface_preedit`——**不发给程序**（不是提交）；
- 候选窗**锚点**走 `ghostty_surface_ime_point`（光标处、含预编辑宽度的矩形）；
- **提交**的文本走 **key 事件**（`keycode=0` + text），**不是** `ghostty_surface_text`
  ——后者是**粘贴**语义，对端开了 bracketed paste 会被 `ESC[200~ … ESC[201~` 包裹，
  你敲的中文就被程序当成一次「粘贴」。

**证据**：

- 断言：`TerminalIMETests`（7 条）——视图 `is NSTextInputClient` 且 `inputContext`
  非 nil；`setMarkedText` 后 libghostty 的 `ime_point` 宽度 > 0（组合串真进了引擎）、
  `unmarkText` 后归 0；`insertText("中文")` 在**开着 bracketed paste** 下字节原样是
  `中文`（不是 `ESC[200~…`）；合成 NSEvent 的 `keyDown` 经 `interpretKeyEvents` 把
  按键送到 transport。带两条**对照**（`sendText` 粘贴语义在同条件下确实被包裹）。
- 变异：`scripts/mutation-check-terminal.sh` 的 **T5**（`setPreedit` 不下发 → 预编辑
  断言红）、**T6**（提交误走粘贴 → 提交断言红）、**T7**（`keyDown` 不路由 → 按键
  断言红）；三条都带指向性对照仍绿。

## 8. 带 Shift 的菜单字母键必须验实际事件

macOS 26 上，`NSMenuItem(keyEquivalent: "d")` 加 `.command, .shift` 的文字能显示为
⇧⌘D，但真实 `characters = "D"` 的按键没有匹配。向下分屏和文件面板已改为 `D` / `E`，
搜索标签用 `A`。快捷键目录的文字对拍仍保留，另用 `performKeyEquivalent(with:)` 驱动
真实菜单并检查窗格实际创建。`PaneActionsTests.testShiftShortcutsReachTheSplitAndFileCommands`
与 `tab-chrome --group panes` 的两组变异护这条链。

## 9. 预览 path 存在，不等于覆盖层已经画到窗格上

本机离屏验证发现，挂在工作区 backing layer 上的两个 `CAShapeLayer` 仍排在
AppKit 管理的窗格 backing layers 之前；仅断言 path 和矩形全部通过，像素却只变了按钮背景。
提高 `zPosition` 不能作为离屏绘制正确的证据：Apple 的
[render(in:) 文档](https://developer.apple.com/documentation/quartzcore/calayer/render(in:))
明确指出这一路绘制不支持完整合成模型。

分屏预览改用最上层 `SplitPreview: NSView`，由 AppKit 排视图顺序，绘制时解析动态强调色。
`hitTest` 返回 nil，不可获取键盘焦点，不能吞终端选择、文件输入与拖动。
`PaneActionsTests` 同时验证工作区像素变化、底层命中对象保持不变、目标半区的几何和取消路径。
变异分别移除挂载、错误拦截命中、翻转方向，不能用一条 path 断言代替三种证据。

## 10. 离屏窗口的 firstResponder 与系统 key window 是两件事

锁屏时 `makeKeyAndOrderFront`、`activate`、完整 AppKit run loop 乃至正常应用启动都不能
保证 `isKeyWindow == true`。2026-10-29 的本机探针确认前台是登录窗口且会话处于锁屏，
这不是应该通过删掉生产端失焦保护来修的产品问题。

确定性窗格测试只注入 NSWindow 的焦点状态，生产窗口控制器与根视图仍是同一条路径；
覆盖真实 NSButton 点击、firstResponder、失焦通知与重新进入的保护。该证据只证明接线，
系统激活和全屏仍须在可交互桌面单独验收。记录中不得把 `paneFocused` 或可控制的
`isKeyWindow` 写成已经验证了系统激活。


## 11. 命中采样须先转换到父视图坐标，并证明命中了目标

`NSView.hitTest(_:)` 接收父视图坐标。工作区 `isFlipped == true`，窗口框架未翻转；
把工作区顶部的局部 y 直接传入，会采到窗口底部。旧标题栏带测试因此在拖动把手的
`mouseDownCanMoveWindow` 被故意改成 true 后仍然通过。

`WorkspaceTests.testWorkspaceSubtreeRefusesWindowDragInTitleBarStrip` 先用
`view.convert(point, to: view.superview)` 转换，再采样真实命中视图；同时要求命中的
拖动把手分别属于终端与文件窗格，避免“没有命中对象”也让拒绝移窗断言通过。
`mutation-check-workspace.py` 保留对应变异与独立拖动载荷对照。


## 12. 双击动作须验派发，避免 NSObject.perform 的重载陷阱

`NSTableView.doubleAction` 是可选值，`nil != NSSelectorFromString("")` 为真。
旧断言只排除了一个空 selector，没有验证动作能否执行；禁用安装逻辑后仍假绿。
改成非 nil 并经 `NSApp.sendAction` 派发后，又发现实际方法选择错误：
`#selector(Coordinator.perform(_:))` 解析为 `performSelector:`，命中了继承的
`NSObject.perform(_:)`。将 NSTableView 作为 sender 交给它会抛出未识别 selector 异常。

处理函数改名 `openClickedRow(_:)`，避免重载冲突。SwiftUI 默认 action 也能返回派发成功，
故最终测试通过生产窗口安装的 action / target 验证目录实际打开；用 NSTableView 子类只提供
只读 clickedRow 的事件数据，不替换生产目标，也不把它称作物理双击证据。文件索引映射
及选中绑定仍独立验证。工作区变异分别关闭动作安装、
把 selector 改回 `NSObject.perform(_:)`，并各配独立文件焦点对照。
