// TabStripScrollTests — 顶栏标签条「标签多了要能横向滚」的断言。
//
// 为什么值得断言：这条**不报错**，只在标签多到超出顶栏时「右边那些够不着」。
// iGhostVT 在 Mac 上踩过同一个坑（`TabStripBar` 注释原话：the strip's hidden
// chips were out of reach），根因是**普通鼠标的滚轮只有纵向 delta，横向
// `ScrollView` 会忽略它**。
//
// 第一版把 `scrollWheel` 写在滚动内容里的子视图上——**没用**：SwiftUI 的
// `NSHostingView` 会把 `hitTest` 收成它自己（实测命中视图就是 `NSHostingView`），
// 子视图根本收不到滚轮。所以现在拦在**事件分发层**（`NSEvent` 本地监视器），
// 判定逻辑落在可断言的 `WheelScrollsHorizontally.WheelView.wheelScroll(...)`。
// （`NSEvent` 造不出「带 window」的真事件，故这里直接驱动那条判定函数；真机
// 事件带 window，`handle` 只是薄薄一层转发。）
import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

final class TabStripScrollTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    @MainActor
    func testQuartzScreenPointUsesAppKitVerticalAxis() throws {
        let height = try XCTUnwrap(NSScreen.screens.first?.frame.height)
        let point = WheelScrollsHorizontally.WheelView.appKitScreenPoint(CGPoint(x: 123, y: 75))
        XCTAssertEqual(point.x, 123)
        XCTAssertEqual(point.y, height - 75, "Quartz 坐标必须转换为 AppKit 的纵轴")
    }

    private func findScroll(_ v: NSView) -> NSScrollView? {
        if let s = v as? NSScrollView { return s }
        for sub in v.subviews { if let s = findScroll(sub) { return s } }
        return nil
    }
    private func findWheel(_ v: NSView) -> WheelScrollsHorizontally.WheelView? {
        if let w = v as? WheelScrollsHorizontally.WheelView { return w }
        for sub in v.subviews { if let w = findWheel(sub) { return w } }
        return nil
    }

    @MainActor
    private func makeStrip(tabs n: Int) throws -> (NSHostingView<RootView>, NSWindow) {
        let dir = NSTemporaryDirectory() + "gt-strip-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let cfg = dir + "/config"
        try "Host web\n  HostName h\n".write(toFile: cfg, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: cfg, knownHostsPath: dir + "/known_hosts")
        for i in 0..<n {
            let t = TerminalTab(kind: .localShell)
            t.title = "标签页-\(i)-很长很长"
            model.tabs.add(t)
        }
        // 侧栏收起 → 顶栏就是那条要横向滚的标签条。
        model.preferences.tabPlacement = .top
        let host = makeRootHostingView(model: model)
        host.frame = NSRect(x: 0, y: 0, width: 640, height: 480)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        addTeardownBlock { @MainActor in window.orderOut(nil) }
        for _ in 0..<20 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        return (host, window)
    }

    /// 一条滚动事件，位置给的是 **AppKit 屏坐标**（`window.convertPoint(toScreen:)`
    /// 那种）。`CGEvent.location` 是 Quartz 屏坐标（y 向下），这里先翻成 Quartz
    /// 再塞进去——跟真机 Quartz 事件一致（`handle` 会再翻回 AppKit）。
    private func wheelEvent(_ dy: Int32, atAppKitScreenPoint p: NSPoint) -> NSEvent? {
        guard let cg = CGEvent(scrollWheelEvent2Source: nil, units: .line,
                               wheelCount: 1, wheel1: dy, wheel2: 0, wheel3: 0)
        else { return nil }
        let primaryHeight = NSScreen.screens.first?.frame.height ?? 0
        cg.location = CGPoint(x: p.x, y: primaryHeight - p.y)   // AppKit → Quartz
        return NSEvent(cgEvent: cg)
    }

    /// 标签超多 → 内容溢出视口（可滚），普通鼠标滚轮真的推得动它。
    @MainActor
    func testManyTabsOverflowAndWheelScrolls() throws {
        let (host, window) = try makeStrip(tabs: 15)
        guard let sv = findScroll(host) else { return XCTFail("顶栏里应有横向 NSScrollView") }
        let docWidth = sv.documentView?.frame.width ?? 0
        XCTAssertGreaterThan(docWidth, sv.contentView.bounds.width,
                             "标签超出顶栏时，滚动内容应比视口宽（可横向滚）")
        guard let wheel = findWheel(host) else {
            return XCTFail("WheelScrollsHorizontally 没挂到滚动区上——普通鼠标就滚不动")
        }
        XCTAssertTrue(wheel.isMonitoring, "滚轮监视器应已安装（否则真机收不到滚轮）")

        // 标签条在窗口坐标里的中间点（监视器正是按这个点判「指针在不在条上」）。
        let stripPoint = sv.convert(NSPoint(x: sv.bounds.midX, y: sv.bounds.midY), to: nil)
        let stripScreen = window.convertPoint(toScreen: stripPoint)

        // 归位到最左，再发一个「向下」的滚轮（普通鼠标）。
        sv.contentView.scroll(to: .zero)
        sv.reflectScrolledClipView(sv.contentView)
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        XCTAssertTrue(wheel.wheelScroll(deltaX: 0, deltaY: -3, atWindowPoint: stripPoint),
                      "落在标签条上的纵向滚轮应被接住")
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        let afterDown = sv.contentView.bounds.origin.x
        XCTAssertGreaterThan(afterDown, 0,
                             "普通鼠标向下滚应把横向标签条往右推（得到 \(afterDown)）")

        // 方向性：向上滚应往左退回。
        XCTAssertTrue(wheel.wheelScroll(deltaX: 0, deltaY: 3, atWindowPoint: stripPoint))
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        let afterUp = sv.contentView.bounds.origin.x
        XCTAssertLessThan(afterUp, afterDown, "向上滚应往左退回（得到 \(afterUp)）")

        // 指针不在标签条上（条下方的空白）→ 不该被我们消费，也不该动。
        let below = NSPoint(x: stripPoint.x, y: stripPoint.y - 200)
        XCTAssertFalse(wheel.wheelScroll(deltaX: 0, deltaY: -3, atWindowPoint: below),
                       "指针不在标签条上时不该消费滚轮")

        // 横向为主的手势（触控板两指横滑）→ 放行给 ScrollView 自己。
        XCTAssertFalse(wheel.wheelScroll(deltaX: -30, deltaY: -1, atWindowPoint: stripPoint),
                       "横向手势应原样放行给 ScrollView")

        // ---- 真机路径（监视器入口 handle）：屏幕坐标 → 窗口坐标 → 滚动 ----
        sv.contentView.scroll(to: .zero)
        sv.reflectScrolledClipView(sv.contentView)
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        guard let downEv = wheelEvent(-3, atAppKitScreenPoint: stripScreen) else {
            return XCTFail("造不出滚轮事件")
        }
        XCTAssertTrue(wheel.handle(downEv), "指针在标签条上的滚轮应被监视器接住")
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        XCTAssertGreaterThan(sv.contentView.bounds.origin.x, 0,
                             "handle 走完后标签条应已横向滚动")

        // 指针在条下方的空白 → 监视器不该接（不同窗口坐标点判定）。
        let belowScreen = window.convertPoint(toScreen: NSPoint(x: stripPoint.x, y: stripPoint.y - 200))
        if let off = wheelEvent(-3, atAppKitScreenPoint: belowScreen) {
            XCTAssertFalse(wheel.handle(off), "指针不在标签条上时不该消费滚轮")
        }
    }

    /// 装得下（标签不多）时不该有东西溢出——否则「可滚」那条会在空场景假绿。
    @MainActor
    func testFewTabsDoNotOverflow() throws {
        let (host, _) = try makeStrip(tabs: 2)
        guard let sv = findScroll(host) else { return XCTFail("应有滚动区") }
        let docWidth = sv.documentView?.frame.width ?? 0
        XCTAssertLessThanOrEqual(docWidth, sv.contentView.bounds.width + 1,
                                 "标签装得下时内容不该溢出")
        let wheel = findWheel(host)
        let stripPoint = sv.convert(NSPoint(x: sv.bounds.midX, y: sv.bounds.midY), to: nil)
        // 装得下时即使接住也不该动（maxX == 0）。
        XCTAssertFalse(wheel?.wheelScroll(deltaX: 0, deltaY: -3, atWindowPoint: stripPoint) ?? false,
                       "内容装得下时不该消费滚轮（没有可滚的余量）")
    }
}
