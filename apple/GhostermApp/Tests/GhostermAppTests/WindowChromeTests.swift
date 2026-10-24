// WindowChromeTests — 「顶栏即标题栏」这件事的几何断言。
//
// 为什么值得单独断言：这类改动**没有报错**，只在视觉上不对——红绿灯挂在
// 系统标题栏的高度上（约 28/32pt），而我们的顶栏是 38pt，差那几磅肉眼看
// 得出来却说不出哪里怪。抓不了像素的 CI 里，几何是唯一能验的东西。
import AppKit
import GhostermAppModel
import XCTest

final class WindowChromeTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func makeWindow() -> NSWindow {
        let w = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 900, height: 600),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered, defer: false)
        TestWindows.presentOffscreen(w)
        addTeardownBlock { @MainActor in w.orderOut(nil) }
        return w
    }

    /// `apply` 之后：内容铺满整窗、标题字隐藏、分隔线去掉。
    @MainActor
    func testApplyMakesContentFullSize() {
        let w = makeWindow()
        WindowChrome.apply(to: w)
        XCTAssertTrue(w.styleMask.contains(.fullSizeContentView),
                      "内容必须铺满整窗，否则顶栏被压在系统标题栏下面")
        XCTAssertTrue(w.titlebarAppearsTransparent, "标题栏得透，不然顶栏上面多一条底色")
        XCTAssertEqual(w.titleVisibility, .hidden, "标题字会被顶栏内容压住，得藏掉")
        if #available(macOS 11.0, *) {
            XCTAssertEqual(w.titlebarSeparatorStyle, .none, "去掉标题栏那条分隔线")
        }
    }

    /// `apply` 必须把标题**清空**：AppKit 会在分屏 / 重新 tile 标题栏时
    /// 把标题字画出来（用户实测「分屏时左上角冒出 Ghosterm」），而默认标题
    /// 是 CFBundleName——只靠 `titleVisibility = .hidden` 挡不住，空标题才是
    /// 「连 AppKit 都别想画」。
    @MainActor
    func testApplyClearsWindowTitle() {
        let w = makeWindow()
        w.title = "Ghosterm"
        WindowChrome.apply(to: w)
        XCTAssertEqual(w.title, "",
                       "apply 必须清空标题字（分屏时 AppKit 会把它显示出来）")
    }

    /// 红绿灯的**垂直中心**要落在我们那条顶栏的中心上。
    ///
    /// 这条是这次改动的关键几何：系统的默认位置按系统标题栏高度算，
    /// 我们的条是 38pt——不挪就会明显偏上。
    @MainActor
    func testTrafficLightsCenteredOnBar() {
        let w = makeWindow()
        WindowChrome.apply(to: w)
        let expectedCenterFromWindowTop = WindowChrome.barHeight / 2
        for kind in [NSWindow.ButtonType.closeButton,
                     .miniaturizeButton, .zoomButton] {
            guard let button = w.standardWindowButton(kind),
                  let superview = button.superview else {
                return XCTFail("拿不到 \(kind) 或其 superview")
            }
            let f = button.frame
            // superview 不翻转（实测本机不翻转）→ 距离窗口顶 = 高度 - (y + h)
            let fromTop = superview.bounds.height - (f.origin.y + f.height)
            XCTAssertEqual(fromTop + f.height / 2, expectedCenterFromWindowTop,
                           accuracy: 1.5,
                           "\(kind) 的垂直中心应贴在顶栏中心"
                           + "（距顶 \(fromTop + f.height / 2)，期望 "
                           + "\(expectedCenterFromWindowTop)）")
        }
    }

    /// `.fullSizeContentView` 必须在**创建窗口时**就在 styleMask 里。
    ///
    /// 这条是真踩过的：事后 `insert` 不触发重新布局，标题栏照样占一条——
    /// 用户看到的就是「红绿灯在一条灰带上、标签条在它下面」。
    @MainActor
    func testWindowCreatedWithFullSizeContent() {
        // 与 GhostermApp.swift 的 AppDelegate 同样的 styleMask。
        let w = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 900, height: 600),
            styleMask: [.titled, .closable, .resizable, .miniaturizable,
                        .fullSizeContentView],
            backing: .buffered, defer: false)
        addTeardownBlock { @MainActor in w.orderOut(nil) }
        XCTAssertTrue(w.styleMask.contains(.fullSizeContentView),
                      "创建时就该有 fullSizeContentView（事后 insert 不重排布局）")
        WindowChrome.apply(to: w)
        XCTAssertTrue(w.titlebarAppearsTransparent)
    }

    /// 顶栏左侧要给红绿灯让出位置：让出的宽度必须够三个按钮住下。
    @MainActor
    func testTrafficLightsInsetCoversButtons() {
        let w = makeWindow()
        WindowChrome.apply(to: w)
        guard let zoom = w.standardWindowButton(.zoomButton) else {
            return XCTFail("拿不到 zoom 按钮")
        }
        XCTAssertGreaterThanOrEqual(WindowChrome.trafficLightsWidth,
                                    zoom.frame.maxX,
                                    "顶栏左侧留白要盖住最后一个按钮的右边缘，"
                                    + "否则侧栏切换按钮会压在红绿灯上")
    }

    /// 双击顶栏 = 系统的「双击窗口标题栏以…」行为，**按系统设置走**。
    ///
    /// 用户实测「双击右边的标题栏无法放大缩小窗口」——顶栏里那片当前标签
    /// 胶囊 / 标签条有自己的命中区，把双击吃掉了，后面那层 WindowDragArea
    /// 收不到。修法是给这些控件挂 `.titleBarDoubleClick()`（见 TabStrip），
    /// 而它的动作由本函数决定：必须尊重 AppleActionOnDoubleClick，
    /// 否则用户设成「最小化」时会觉得「双击不听话」。
    @MainActor
    func testDoubleClickHonorsSystemPreference() {
        let key = "AppleActionOnDoubleClick"
        let saved = UserDefaults.standard.string(forKey: key)
        addTeardownBlock {
            if let saved { UserDefaults.standard.set(saved, forKey: key) }
            else { UserDefaults.standard.removeObject(forKey: key) }
        }

        // None：用户显式关掉了 → 什么都不做，窗口不缩放、不最小化。
        let w = makeWindow()
        UserDefaults.standard.set("None", forKey: key)
        let before = w.frame
        WindowChrome.performDoubleClickAction(on: w)
        XCTAssertEqual(w.frame, before, "设为「无」时双击不该改变窗口")
        XCTAssertFalse(w.isMiniaturized, "设为「无」时双击不该最小化")

        // Minimize：按系统设置走最小化分支。
        UserDefaults.standard.set("Minimize", forKey: key)
        WindowChrome.performDoubleClickAction(on: w)
        XCTAssertTrue(w.isMiniaturized, "设为「最小化」时双击应最小化窗口")

        // nil 传入：不崩、无副作用（标题栏可能还没来得及挂上）。
        WindowChrome.performDoubleClickAction(on: nil)
    }
}
