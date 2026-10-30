// TerminalSurfaceView — SwiftUI ⇄ libghostty surface 的视图桥。
//
// 职责限定在「把 surface 贴进视图树 + 把尺寸/焦点/可见性转给 libghostty +
// 把键盘（含输入法）转成终端输入」：字节流、transport、会话生命周期都在别处
// （铁律 #1）。键盘这半见 `TerminalHostView`（它实现 `NSTextInputClient`）。
import AppKit
import SwiftUI

/// 把 `TerminalSurface` 的 NSView 嵌进 SwiftUI。
public struct TerminalSurfaceView: NSViewRepresentable {
    public let surface: TerminalSurface
    /// 这个窗格是不是**当前窗格**（当前标签页里被选中的那个）。
    ///
    /// 只报「布局层面是不是当前窗格」；真正下发给 libghostty 的焦点还要再过
    /// 一道「窗口是不是 key」——那由 `TerminalHostView` 自己判（见下）。
    public let isFocused: Bool
    /// 这个窗格所在**标签页**是不是当前显示的那一个。
    ///
    /// 后台标签页的 surface 保持挂载（只 `opacity(0)`），但我们要把它报成
    /// 「不可见」让引擎停掉绘制（省 CPU/GPU）。与「窗口是否真在屏幕上」一起
    /// 决定生效可见性——那由 `TerminalHostView` 自己判（见下）。
    public let isVisible: Bool

    public init(surface: TerminalSurface, isFocused: Bool, isVisible: Bool) {
        self.surface = surface
        self.isFocused = isFocused
        self.isVisible = isVisible
    }

    public func makeNSView(context: Context) -> TerminalHostView {
        TerminalHostView(surface: surface)
    }

    public func updateNSView(_ nsView: TerminalHostView, context: Context) {
        nsView.paneFocused = isFocused
        nsView.paneVisible = isVisible
    }
}

/// 承载 libghostty surface 的 NSView。
///
/// libghostty 自己往这个 view 上挂 Metal 层，所以这里**不**设 wantsLayer
/// （设为 layer-hosting 会与它争 layer 所有权）。
///
/// **键盘输入必须走 `NSTextInputClient`**（这是本类实现它的理由）：macOS 的
/// 输入法（中文/日文/韩文…）不是「按下一个键就给一段文本」，而是由**系统输入
/// 上下文**驱动——`keyDown` 里得先 `interpretKeyEvents` 把事件交给输入法，输入法
/// 再经 `setMarkedText`（组合中，内联显示）/ `insertText`（提交）回调回来。
/// 早先的版本在 `keyDown` 里直接把 `event.characters` 转发给终端（**完全没碰
/// 输入法**），于是中文输入法根本不被唤起：没有候选窗、没组合、敲了没反应。
/// 上游 Ghostty 的 `SurfaceView`（AppKit）实现的是同一套协议，这里按同一口径复用。
public final class TerminalHostView: NSView, NSTextInputClient {
    private let surface: TerminalSurface
    private var observers: [NSObjectProtocol] = []

    /// 输入法**组合中**（marked）的文本。空 = 不在组合态。用来告诉输入法
    /// 「本视图在组字」，并在 `keyDown` 末尾判断这次按键是否已被输入法消费。
    private var markedText = NSMutableAttributedString()

    /// `keyDown` 期间**临时**非 nil：此时系统输入栈若回调 `insertText`
    /// （输入法提交 / 死键 / 听写），我们把提交的文本**攒在这里**，等在
    /// `keyDown` 末尾统一以**键入语义**下发——而不是当场发出去。
    /// nil = 当前不在 keyDown 处理中（例如输入法在别处提交）。
    private var keyTextAccumulator: [String]?

    /// SwiftUI 侧认定的「当前窗格」。与「窗口是否 key」一起决定生效焦点。
    public var paneFocused: Bool = false {
        didSet {
            guard paneFocused != oldValue else { return }
            applyFocus()
            claimFirstResponder()
        }
    }

    /// SwiftUI 侧认定的「当前显示的标签页」。与「窗口是否真在屏幕上」一起
    /// 决定生效可见性（不可见 = 让引擎停画，省 CPU/GPU）。
    public var paneVisible: Bool = false {
        didSet {
            guard paneVisible != oldValue else { return }
            applyVisible()
        }
    }

    public init(surface: TerminalSurface) {
        self.surface = surface
        super.init(frame: surface.view.frame)
        surface.view.frame = bounds
        surface.view.autoresizingMask = [.width, .height]
        addSubview(surface.view)

        // 切到别的 App / 别的窗口（如设置窗）时，窗口会失去 key——那一刻必须
        // 把「失焦」报给 libghostty，否则终端游标会在后台继续闪（见 applyFocus）。
        let center = NotificationCenter.default
        let focusNames: [Notification.Name] = [
            NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification,
            NSApplication.didBecomeActiveNotification,
            NSApplication.didResignActiveNotification,
        ]
        // 窗口被最小化 / 隐藏 / 被别的窗口完全遮住时，系统会改
        // occlusionState——那一刻要把「不可见」报给引擎停画。
        //
        // 另外挂 didExpose：窗口**第一次真正画出来**时再同步一次。这是给
        // occlusionState 的兜底——启动早期它可能还是「空」，若那一刻正好没
        // 收到遮挡变化通知，画面就会停在「不可见」上。
        let visibilityNames: [Notification.Name] = [
            NSWindow.didChangeOcclusionStateNotification,
            NSWindow.didExposeNotification,
            NSWindow.didMiniaturizeNotification,
            NSWindow.didDeminiaturizeNotification,
            NSApplication.didHideNotification,
            NSApplication.didUnhideNotification,
        ]
        observers = focusNames.map { name in
            center.addObserver(forName: name, object: nil, queue: .main) {
                [weak self] _ in self?.applyFocus()
            }
        } + visibilityNames.map { name in
            center.addObserver(forName: name, object: nil, queue: .main) {
                [weak self] _ in self?.applyVisible()
            }
        }
        applyFocus()
        applyVisible()
    }

    deinit {
        for o in observers { NotificationCenter.default.removeObserver(o) }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) 未实现") }

    public override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        applyFocus()
        applyVisible()
        // 新建标签页 / 分屏时，`paneFocused` 是在视图**进树之前**被置上的
        // （NSViewRepresentable 先 make 再 update，最后才挂进窗口），那一刻
        // window 还是 nil、要不到键盘。所以进树时补一次。
        claimFirstResponder()
    }

    /// 生效焦点 = 布局上「是当前窗格」**且**窗口真的在收键盘。
    ///
    /// 只看「SwiftUI 的 activePaneID」是不够的：切到别的 App、或打开设置窗
    /// 之后，终端窗口不再是 key，此时不该继续闪（上游 macOS 的口径是
    /// `window.isKeyWindow && surfaceView.isFirstResponder`）。
    private func effectiveFocus() -> Bool {
        paneFocused && (window?.isKeyWindow ?? false)
    }

    private func applyFocus() {
        surface.setFocused(effectiveFocus())
    }

    /// 成为当前窗格时**把键盘要过来**。
    ///
    /// 「报给 libghostty 的焦点位」（游标闪、DEC 1004）与「AppKit 的
    /// firstResponder」（`keyDown` 送到谁手里）是两件事。只做前者的话：新开的
    /// 标签页 / 刚分出来的窗格光标在闪，敲字却进了上一个终端——它还占着
    /// firstResponder（后台标签页的 surface 是保持挂载的，没人把它撤下来）。
    private func claimFirstResponder() {
        guard paneFocused, let window, window.firstResponder !== self else { return }
        window.makeFirstResponder(self)
    }

    /// 生效可见性 = 「当前显示的标签页」**且**「窗口真在屏幕上」。
    ///
    /// 窗口那半的判据照抄上游 macOS（`BaseTerminalController` 的
    /// `syncSurfaceTreeOcclusionState`）：`occlusionState` 含 `.visible` 才算
    /// 上屏——不含（含空）就是「整个窗口被挡住」。上游同口径，故也照抄了它的
    /// 兜底办法：窗口遮挡状态一变就重新同步（见 observers），否则一旦误判为
    /// 不可见就再也没人把它叫回来，画面会停死。
    static func effectiveVisibility(paneVisible: Bool,
                                    window: NSWindow?) -> Bool {
        guard paneVisible, let window else { return false }
        return window.isVisible && !window.isMiniaturized
            && window.occlusionState.contains(.visible)
    }

    private func applyVisible() {
        surface.setVisible(Self.effectiveVisibility(paneVisible: paneVisible,
                                                    window: window))
    }

    public override func layout() {
        super.layout()
        surface.view.frame = bounds
        // 视图尺寸 → libghostty 网格 → onResize → transport.resize。
        // 必须用像素（backing scale 后的尺寸），否则 Retina 上网格偏大。
        let scale = window?.backingScaleFactor ?? 2.0
        surface.setSize(width: UInt32(bounds.width * scale),
                        height: UInt32(bounds.height * scale))
    }

    public override var acceptsFirstResponder: Bool { true }

    /// 与 `TerminalSurface` 的渲染锚点同一口径：终端区域永不参与「拖动=移窗」
    /// （上沿可能顶进系统标题栏带，见 SurfaceAnchorView 的说明）。
    public override var mouseDownCanMoveWindow: Bool { false }

    // 标准编辑菜单经 firstResponder 路由到当前终端，不向 shell 发送 ⌘C/⌘V。
    @objc public func copy(_ sender: Any?) {
        guard let text = surface.selectedText(), !text.isEmpty else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    @objc public func paste(_ sender: Any?) {
        guard let text = NSPasteboard.general.string(forType: .string) else { return }
        surface.sendText(text)
    }

    // MARK: - 鼠标事件追踪区域
    //
    // libghostty 需要收到鼠标移动事件来支持程序鼠标捕获（vim / less / tmux
    // 鼠标模式等）和悬停效果（如链接高亮）。AppKit 默认**不**发送 mouseMoved，
    // 必须通过 NSTrackingArea 显式开启。

    public override func updateTrackingAreas() {
        super.updateTrackingAreas()
        // 先清掉旧的再建新的（视图尺寸变化时系统会调这里）。
        trackingAreas.forEach { removeTrackingArea($0) }
        addTrackingArea(NSTrackingArea(
            rect: frame,
            options: [
                .mouseEnteredAndExited,
                .mouseMoved,
                .inVisibleRect,   // 只在未被遮挡的区域触发
                .activeAlways,    // 即使不是 key window 也要收鼠标事件
            ],
            owner: self,
            userInfo: nil))
    }

    /// 鼠标是否在视图区域内。mouseExited 时置 false，阻止多余的 pos 事件。
    private var mouseOverSurface = false

    /// Force Touch 上一次 stage（用于只在**首次**进入 stage 2 时触发 Quick Look）。
    private var prevPressureStage: Int = 0

    // MARK: - 坐标转换

    /// 把 NSEvent 的窗口坐标转成 libghostty 要的视图坐标（左上原点，点单位）。
    /// AppKit 视图坐标是左下原点，libghostty 是左上原点，需要翻转 y。
    private func ghosttyMousePos(from event: NSEvent) -> (x: Double, y: Double) {
        let pos = convert(event.locationInWindow, from: nil)
        return (Double(pos.x), Double(frame.height - pos.y))
    }

    // MARK: - 鼠标按键

    public override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
        surface.sendMouseButton(.press, button: .left, modifiers: event.modifierFlags)
    }

    public override func mouseUp(with event: NSEvent) {
        prevPressureStage = 0
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
        surface.sendMouseButton(.release, button: .left, modifiers: event.modifierFlags)
        surface.sendMousePressure(stage: 0, pressure: 0)
    }

    public override func rightMouseDown(with event: NSEvent) {
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
        let consumed = surface.sendMouseButton(.press, button: .right,
                                               modifiers: event.modifierFlags)
        if !consumed { super.rightMouseDown(with: event) }
    }

    public override func rightMouseUp(with event: NSEvent) {
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
        let consumed = surface.sendMouseButton(.release, button: .right,
                                               modifiers: event.modifierFlags)
        if !consumed { super.rightMouseUp(with: event) }
    }

    public override func otherMouseDown(with event: NSEvent) {
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
        let button: TerminalSurface.MouseButton = switch event.buttonNumber {
        case 2: .middle
        case 3: .left   // GHOSTTY_MOUSE_FOUR = 4, 但 enum 没映射，按 left 降级
        default: .unknown
        }
        surface.sendMouseButton(.press, button: button, modifiers: event.modifierFlags)
    }

    public override func otherMouseUp(with event: NSEvent) {
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
        let button: TerminalSurface.MouseButton = switch event.buttonNumber {
        case 2: .middle
        case 3: .left
        default: .unknown
        }
        surface.sendMouseButton(.release, button: button, modifiers: event.modifierFlags)
    }

    // MARK: - 鼠标移动

    public override func mouseEntered(with event: NSEvent) {
        mouseOverSurface = true
        super.mouseEntered(with: event)
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
    }

    public override func mouseExited(with event: NSEvent) {
        mouseOverSurface = false
        // 如果有按键被按住，不报退出——drag 事件仍然会送到（mouseDragged）。
        if NSEvent.pressedMouseButtons != 0 { return }
        // 负坐标告诉 libghostty 鼠标已离开视口。
        surface.sendMousePos(x: -1, y: -1, modifiers: event.modifierFlags)
    }

    public override func mouseMoved(with event: NSEvent) {
        let p = ghosttyMousePos(from: event)
        surface.sendMousePos(x: p.x, y: p.y, modifiers: event.modifierFlags)
    }

    public override func mouseDragged(with event: NSEvent) {
        mouseMoved(with: event)
    }

    public override func rightMouseDragged(with event: NSEvent) {
        mouseMoved(with: event)
    }

    public override func otherMouseDragged(with event: NSEvent) {
        mouseMoved(with: event)
    }

    // MARK: - 滚轮

    public override func scrollWheel(with event: NSEvent) {
        var dx = event.scrollingDeltaX
        var dy = event.scrollingDeltaY
        let precision = event.hasPreciseScrollingDeltas
        if precision {
            // 触控板 / Magic Mouse 加 2× 速度（与上游同口径）。
            dx *= 2
            dy *= 2
        }
        let momentum = UInt8(min(event.momentumPhase.rawValue, 6))
        surface.sendMouseScroll(dx: dx, dy: dy, precision: precision,
                                momentum: momentum)
    }

    // MARK: - Force Touch

    public override func pressureChange(with event: NSEvent) {
        surface.sendMousePressure(stage: UInt32(event.stage),
                                  pressure: Double(event.pressure))
        // stage 2 = force click（只在首次进入时触发，避免重复）。
        guard prevPressureStage < 2 else { return }
        prevPressureStage = event.stage
        guard event.stage == 2 else { return }
        // Force click QuickLook 留给后续按需实现。
    }

    /// 键盘 → （先过输入法）→ libghostty 输入编码 → host write 回调 → transport。
    ///
    /// 流程（上游 Ghostty 同口径）：
    ///   1. 置 `keyTextAccumulator = []`，标记「正在一次 keyDown 里」，并把事件
    ///      交给 `interpretKeyEvents`——这一步才会**唤起输入法**：组合中的字经
    ///      `setMarkedText` 回来做内联显示，提交的字经 `insertText` 攒进累加器；
    ///   2. 结束后把组合态同步给引擎（有组合 → 显示预编辑；已清除 → 清掉）；
    ///   3. 结算：
    ///      - 有输入法**提交**的文本 → 以**键入语义**下发（非粘贴）；
    ///      - 仍在**组合**中（这次按键属于输入法，如选字 / 退格改拼音）→ 吞掉，
    ///        不进终端；
    ///      - 否则是**普通按键**（功能键 / 组合键 / 未产生文本的键）→ 走原来的
    ///        编码路径。
    public override func keyDown(with event: NSEvent) {
        keyTextAccumulator = []
        let markedTextBefore = markedText.length > 0
        interpretKeyEvents([event])
        let accumulated = keyTextAccumulator
        keyTextAccumulator = nil

        // 把组合态变化同步给引擎（内联显示预编辑串 / 提交或取消时清掉）。
        syncPreedit()

        // 输入法提交的文本（含「拼音选字后回车/空格」这种刚结束组合的提交）：
        // 以键入语义下发，程序看到的是「一个字一个字敲出来」，不是一次粘贴。
        if let accumulated, !accumulated.isEmpty {
            for text in accumulated { surface.sendCommittedText(text) }
            return
        }

        // 仍在组合态（本次按键被输入法消费：选字 / 翻页 / 改拼音 / 取消…）：
        // 不能进终端——组合中的拼音不该被程序看到。
        if markedText.length > 0 || markedTextBefore { return }

        // 普通按键。注意：可打印的 ASCII 也会经 insertText 攒进累加器（上面已
        // 处理），这里兜的是**未产生文本**的键（回车 / 退格 / 方向键 / 组合键…）。
        surface.sendKeyEvent(nativeKeycode: UInt32(event.keyCode),
                             text: event.characters ?? "",
                             codepoint: event.charactersIgnoringModifiers?
                                 .unicodeScalars.first?.value ?? 0,
                             modifiers: event.modifierFlags)
    }

    public override func keyUp(with event: NSEvent) {}

    /// 把当前组合态（`markedText`）同步给 libghostty：非空 = 显示预编辑串，
    /// 空 = 清除。**只影响显示，不发给程序。**
    private func syncPreedit() {
        surface.setPreedit(markedText.length > 0 ? markedText.string : "")
    }

    // MARK: - NSTextInputClient
    //
    // 输入法经这条协议把组合 / 提交的文本交给我们。只实现「终端输入法」真正需要
    // 的那几项（对照上游 Ghostty 的 SurfaceView 同名实现）。

    public func hasMarkedText() -> Bool { markedText.length > 0 }

    public func markedRange() -> NSRange {
        guard markedText.length > 0 else { return NSRange() }
        return NSRange(location: 0, length: markedText.length)
    }

    /// 终端没有「可编辑文本选区」这个概念，输入法也不依赖它做替换，故返回空。
    public func selectedRange() -> NSRange { NSRange(location: 0, length: 0) }

    /// 输入法更新组合串（逐键）。**不产生输出**，只做显示（见 syncPreedit）。
    public func setMarkedText(_ string: Any, selectedRange: NSRange,
                              replacementRange: NSRange) {
        switch string {
        case let v as NSAttributedString:
            markedText = NSMutableAttributedString(attributedString: v)
        case let v as String:
            markedText = NSMutableAttributedString(string: v)
        default:
            return
        }
        // 若不在 keyDown 里（例如切输入法布局时系统重放组合态），立即同步显示；
        // 在 keyDown 里则由 keyDown 末尾统一同步（见上）。
        if keyTextAccumulator == nil { syncPreedit() }
    }

    /// 输入法**提交**文本。在 keyDown 里则攒进累加器（末尾以键入语义下发）；
    /// 否则（听写等）直接下发。
    public func insertText(_ string: Any, replacementRange: NSRange) {
        let chars: String
        switch string {
        case let v as NSAttributedString: chars = v.string
        case let v as String: chars = v
        default: return
        }
        // 提交即组合结束：清掉预编辑显示。
        unmarkText()
        if keyTextAccumulator != nil {
            keyTextAccumulator?.append(chars)
            return
        }
        surface.sendCommittedText(chars)
    }

    public func unmarkText() {
        if markedText.length > 0 {
            markedText.mutableString.setString("")
            syncPreedit()
        }
    }

    public func validAttributesForMarkedText() -> [NSAttributedString.Key] { [] }

    /// 输入法偶尔会来取「已有文本」，终端不提供，返回 nil。
    public func attributedSubstring(forProposedRange range: NSRange,
                                    actualRange: NSRangePointer?) -> NSAttributedString? {
        nil
    }

    public func characterIndex(for point: NSPoint) -> Int { 0 }

    /// 候选窗锚点：贴在**光标处**。libghostty 给出光标（含预编辑宽度）的视图坐标
    /// （左上原点），这里换算成屏幕坐标给输入法（AppKit 要左下原点 + 屏幕坐标）。
    public func firstRect(forCharacterRange range: NSRange,
                          actualRange: NSRangePointer?) -> NSRect {
        let p = surface.imePoint
        let cellHeight = max(p.height, 1)
        let viewRect = NSRect(x: p.x, y: frame.size.height - p.y,
                              width: p.width, height: cellHeight)
        let winRect = convert(viewRect, to: nil)
        guard let window else { return winRect }
        return window.convertToScreen(winRect)
    }

    /// 必须实现：否则 `interpretKeyEvents` 遇到我们没有显式处理的命令
    /// （方向键 / 回车 / 退格 / escape…）会发出系统警示音（NSBeep）。
    /// 我们**不在这里处理任何命令**——这些键要么已被输入法消费，要么会落到
    /// `keyDown` 末尾的普通按键路径。
    public override func doCommand(by selector: Selector) {}
}
