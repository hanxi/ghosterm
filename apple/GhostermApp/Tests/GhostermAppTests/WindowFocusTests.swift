// WindowFocusTests — 「分屏只有当前窗格的光标在闪」这一步的**接线**断言。
//
// 为什么单独盯这条：焦点从没从 SwiftUI 下发到 libghostty 时，**屏幕上不会报
// 任何错**——每个窗格的 surface 都停在「已聚焦」的默认值上一起闪，只有肉眼
// 看得出来。观测手段是 DEC 1004（焦点上报）：焦点得/失会让 libghostty 经
// host write 回调吐出 `CSI I` / `CSI O`，而那个焦点位正是游标闪烁所用的一位
// （Surface.focusCallback → terminal.flags.focused）；终端包那边有一条同法
// 的引擎侧断言（TerminalSurfaceTests.testFocusIsReportedToProgramThroughHostWrite）。

// 同法覆盖 **可见性**（后台标签页停画，省 CPU/GPU）：DEC 2033 上报
// `CSI ?999;1n`（可见）/ `;2n`（不可见），而那个位就是渲染线程决定画不画的
// `visible`（Surface.occlusionCallback → renderer.mailbox）。
import AppKit
import GhostermTerminal
import XCTest

final class WindowFocusTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    /// 测试进程里造不出真的 key 窗口（没有用户点击去激活 app），于是用一个
    /// 可驱动的 `isKeyWindow`：这样「窗口是不是 key」这一档也能被断言。
    private final class KeyWindow: NSWindow {
        var stubbedKey = false
        override var isKeyWindow: Bool { stubbedKey }

        /// 测试进程里的窗口既没真上屏、遮挡状态也不稳，故把「可见性」那几档
        /// 也做成可驱动（生产路径读的就是这几个属性）。
        var stubbedVisible = true
        var stubbedMiniaturized = false
        var stubbedOcclusionState: NSWindow.OcclusionState = .visible
        override var isVisible: Bool { stubbedVisible }
        override var isMiniaturized: Bool { stubbedMiniaturized }
        override var occlusionState: NSWindow.OcclusionState {
            stubbedOcclusionState
        }
    }

    /// 焦点变化是 io 线程异步送出的字节，收集要跨线程，故自带锁。
    private final class Recorder {
        private var data = Data()
        private let lock = NSLock()
        func append(_ d: Data) { lock.lock(); data.append(d); lock.unlock() }
        func snapshot() -> Data { lock.lock(); defer { lock.unlock() }; return data }
        func reset() { lock.lock(); data.removeAll(); lock.unlock() }
        var text: String { String(decoding: snapshot(), as: UTF8.self) }
    }

    private func waitUntil(_ timeout: TimeInterval = 5,
                           _ cond: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if cond() { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        return cond()
    }

    @MainActor
    private func makeReportingSurface() throws -> (TerminalSurface, Recorder) {
        let surface = try TerminalSurface(app: try TerminalApp.shared())
        let rec = Recorder()
        surface.onInput = { rec.append($0) }
        // 开焦点上报（1004）与可见性上报（2033）：两种事件都会从 host write
        // 回调吐出字节，是这两条接线的观测通道。
        surface.feed(Data("\u{1b}[?1004h".utf8))
        surface.feed(Data("\u{1b}[?2033h".utf8))
        return (surface, rec)
    }

    @MainActor
    private func makeHost(_ surface: TerminalSurface)
        -> (TerminalHostView, KeyWindow) {
        let host = TerminalHostView(surface: surface)
        let window = KeyWindow(
            contentRect: NSRect(x: 100, y: 100, width: 400, height: 300),
            styleMask: [.titled], backing: .buffered, defer: false)
        // 先把窗口设成 key，再挂上去：这样下面的 paneFocused 翻转都是真跳变。
        window.stubbedKey = true
        window.contentView = host
        return (host, window)
    }

    // MARK: - 后台标签页不画（省 CPU/GPU）

    /// 只有「当前显示的标签页」**且**「窗口真在屏幕上」时，才报「可见」。
    @MainActor
    func testOnlyVisiblePaneInOnScreenWindowReportsVisible() throws {
        let (surface, rec) = try makeReportingSurface()
        rec.reset()
        let host = TerminalHostView(surface: surface)
        let window = KeyWindow(
            contentRect: NSRect(x: 100, y: 100, width: 400, height: 300),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        defer { window.orderOut(nil) }

        // 挂上去时还不是「当前标签页」也不是 key → 应报「不可见」（默认 false）。
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[?999;2n") },
                      "窗口一上屏就先按「不可见」起步（得到 \(rec.text.debugDescription)）")

        // 成为当前标签页 → 报「可见」（引擎该画了）。
        rec.reset()
        host.paneVisible = true
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[?999;1n") },
                      "当前标签页 + 窗口在屏上应报「可见」"
                      + "（得到 \(rec.text.debugDescription)）")

        // 切成后台标签页 → 报「不可见」（引擎停画）。
        rec.reset()
        host.paneVisible = false
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[?999;2n") },
                      "后台标签页必须报「不可见」——否则它会在没人看时继续画"
                      + "（得到 \(rec.text.debugDescription)）")
    }

    /// 窗口被最小化 / 被完全遮挡时，即便它是当前标签页也要报「不可见」。
    @MainActor
    func testOccludedWindowReportsNotVisible() throws {
        let (surface, rec) = try makeReportingSurface()
        rec.reset()
        let host = TerminalHostView(surface: surface)
        let window = KeyWindow(
            contentRect: NSRect(x: 100, y: 100, width: 400, height: 300),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        host.paneVisible = true
        defer { window.orderOut(nil) }
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[?999;1n") }, "先可见（前置条件）")

        // 系统判定「完全被遮挡」→ 停画。
        rec.reset()
        window.stubbedOcclusionState = []
        NotificationCenter.default.post(name: NSWindow.didChangeOcclusionStateNotification,
                                        object: window)
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[?999;2n") },
                      "被完全遮挡时该报「不可见」（得到 \(rec.text.debugDescription)）")

        // 取消遮挡 → 重新可见并重画。
        rec.reset()
        window.stubbedOcclusionState = .visible
        NotificationCenter.default.post(name: NSWindow.didChangeOcclusionStateNotification,
                                        object: window)
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[?999;1n") },
                      "取消遮挡后该重新报「可见」（得到 \(rec.text.debugDescription)）")

        // 最小化 → 停画。
        rec.reset()
        window.stubbedMiniaturized = true
        NotificationCenter.default.post(name: NSWindow.didMiniaturizeNotification,
                                        object: window)
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[?999;2n") },
                      "最小化时该报「不可见」（得到 \(rec.text.debugDescription)）")
    }

    /// 只有「当前窗格」**且**「窗口在收键盘」时，焦点才报给引擎。
    @MainActor
    func testOnlyFocusedPaneInKeyWindowReportsFocus() throws {
        let (surface, rec) = try makeReportingSurface()
        let (host, window) = makeHost(surface)
        defer { window.orderOut(nil) }

        // 窗口是 key、但还不是当前窗格 → 报失焦（前置态）。
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[O") },
                      "非当前窗格应报失焦（得到 \(rec.text.debugDescription)）")

        // 1) 成为当前窗格 → 聚焦（游标该闪了）。
        rec.reset()
        host.paneFocused = true
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[I") },
                      "当前窗格应报聚焦（得到 \(rec.text.debugDescription)）")

        // 2) 不再是当前窗格 → 报失焦。这就是「分屏只有选中的格子才闪」。
        rec.reset()
        host.paneFocused = false
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[O") },
                      "非当前窗格必须报失焦——否则它会跟着一起闪"
                      + "（得到 \(rec.text.debugDescription)）")
    }

    // MARK: - 键盘真的落在当前窗格上（firstResponder）

    /// 成为当前窗格 → 必须把 AppKit 的 firstResponder 要过来。
    ///
    /// 这条与上面那组是**两件事**：上面盯的是报给 libghostty 的焦点位（游标
    /// 闪 / DEC 1004），这条盯的是 `keyDown` 送到谁手里。只做前者时屏幕上同样
    /// 不报错——新窗格光标在闪，敲字却进了上一个终端（后台标签页的 surface
    /// 保持挂载，firstResponder 还在它身上）。这正是用户报的
    /// 「开终端 / 分屏后键盘没定位到最新终端」。
    @MainActor
    func testFocusedPaneTakesKeyboardFromPreviousPane() throws {
        let (a, _) = try makeReportingSurface()
        let (b, _) = try makeReportingSurface()
        let hostA = TerminalHostView(surface: a)
        let hostB = TerminalHostView(surface: b)
        let window = KeyWindow(
            contentRect: NSRect(x: 100, y: 100, width: 400, height: 300),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.stubbedKey = true
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 400, height: 300))
        container.addSubview(hostA)
        container.addSubview(hostB)
        window.contentView = container
        defer { window.orderOut(nil) }

        hostA.paneFocused = true
        XCTAssertTrue(window.firstResponder === hostA,
                      "当前窗格必须拿到键盘，否则 keyDown 根本不到它手里")

        // 分屏 / 切窗格：键盘要跟着当前窗格走，不能留在上一个上。
        hostA.paneFocused = false
        hostB.paneFocused = true
        XCTAssertTrue(window.firstResponder === hostB,
                      "切到新窗格后键盘必须跟过去——留在旧窗格上就是「敲字进了上一个终端」")
    }

    /// 新建的窗格是「先置当前、后进视图树」（NSViewRepresentable 先 make 再
    /// update，最后才挂进窗口）。置 `paneFocused` 的那一刻 window 还是 nil，
    /// 要不到键盘——所以进树时必须补要一次。
    @MainActor
    func testPaneCreatedAlreadyFocusedClaimsKeyboardOnMount() throws {
        let (surface, _) = try makeReportingSurface()
        let host = TerminalHostView(surface: surface)
        // 还没进窗口就被标成当前窗格（新建标签页 / 分屏的真实时序）。
        host.paneFocused = true

        let window = KeyWindow(
            contentRect: NSRect(x: 100, y: 100, width: 400, height: 300),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.stubbedKey = true
        window.contentView = host
        defer { window.orderOut(nil) }

        XCTAssertTrue(window.firstResponder === host,
                      "进树时没补要键盘 → 新开的终端在闪、键盘还在旧终端上")
    }

    /// 窗口失去 key（切到别的 App / 别的窗口）：当前窗格也必须报失焦，
    /// 不能让游标在后台继续闪。
    @MainActor
    func testKeyWindowLosingKeyReportsUnfocused() throws {
        let (surface, rec) = try makeReportingSurface()
        let (host, window) = makeHost(surface)
        defer { window.orderOut(nil) }

        rec.reset()
        host.paneFocused = true
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[I") }, "先聚焦（前置条件）")

        rec.reset()
        window.stubbedKey = false
        NotificationCenter.default.post(name: NSWindow.didResignKeyNotification,
                                        object: window)
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[O") },
                      "窗口失去 key 必须报失焦——切到别的 App 后不该继续闪"
                      + "（得到 \(rec.text.debugDescription)）")

        // 再切回来 → 重新聚焦。
        rec.reset()
        window.stubbedKey = true
        NotificationCenter.default.post(name: NSWindow.didBecomeKeyNotification,
                                        object: window)
        XCTAssertTrue(waitUntil { rec.text.contains("\u{1b}[I") },
                      "切回来后当前窗格应重新聚焦（得到 \(rec.text.debugDescription)）")
    }
}
