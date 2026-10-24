// TabStripVisibilityTests — 「标签页只列一处」与「只有一个加号」。
//
// 用户实测带出：把新建标签页的 `+` 从侧栏列表行挪到顶带后，侧栏展开时
// 屏幕上同时出现了**两个加号**（侧栏一个、顶部标签条一个）——
// 「你没理解意思，现在出现 2 个加号了，我只要一个加号。」
// 同一份标签清单也一样：侧栏展开时顶部标签条再排一遍就是画两处。
//
// 怎么量：SwiftUI 的 `Menu` 在 AppKit 侧会落成一个真的 `SwiftUIPopupButton`
// 视图，所以数视图树里的 popup 就能数出屏幕上有几个菜单按钮
// （无障碍树在离屏时不生成，验证过拿不到，只能走视图树）。
import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

@MainActor
final class TabStripVisibilityTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    // MARK: - 纯判据

    /// 顶部标签条的显隐是一条纯函数（规则写死，偏好已弃用），两态钉死
    /// （视图只是照着它画）。
    func testStripVisibilityRule() {
        XCTAssertFalse(TabStripVisibility.shows(sidebarVisible: true),
                       "侧栏展开 → 顶部标签条隐藏（清单只列一处）")
        XCTAssertTrue(TabStripVisibility.shows(sidebarVisible: false),
                      "侧栏收起时顶部标签条必须在——否则标签页一个都看不见")
    }

    // MARK: - 屏幕上真的只有一个加号

    /// 侧栏展开：整窗只有**一个**菜单按钮（顶带里的 `+`），且它在侧栏那一段。
    func testSidebarOpenShowsExactlyOneNewTabMenu() throws {
        let host = try renderWindow(sidebar: true)
        let menus = popupFrames(host)
        XCTAssertEqual(menus.count, 1,
                       "侧栏展开时整窗应只有一个菜单按钮（新建标签页的 `+`）；"
                       + "得到 \(menus.count) 个，位置 \(menus.map(\.minX))")
        XCTAssertLessThan(try XCTUnwrap(menus.first).minX, DS.sidebarWidth,
                          "这个 `+` 应在侧栏那一段")
    }

    /// 侧栏收起：还是只有一个 `+`，而且停在**同一个 x**。
    ///
    /// 顶栏右端那个 `⋯` 溢出菜单已经删了（里面每条都是副本，窗口级的那几条
    /// 在 macOS 全局菜单条里，见 `AppMenuCoverageTests`），所以这里只该数到
    /// 一个菜单按钮。
    func testSidebarClosedKeepsTheSingleNewTabMenuAtTheSameSpot() throws {
        let open = popupFrames(try renderWindow(sidebar: true))
        let closed = popupFrames(try renderWindow(sidebar: false))
        XCTAssertEqual(closed.count, 1,
                       "收起时也只该有一个菜单按钮（`⋯` 已删）；"
                       + "得到 \(closed.map(\.minX))")

        let a = try XCTUnwrap(open.first), b = try XCTUnwrap(closed.first)
        XCTAssertGreaterThanOrEqual(b.minX, WindowChrome.trafficLightsWidth,
                                    "`+` 不该压在红绿灯上（x=\(b.minX)）")
        // 这条是这次改版的要害：按钮组贴红绿灯左对齐，于是开合两态同一个 x，
        // 用户开合侧栏后不用重新找按钮（先前开关从 x≈200 跳到 x≈94）。
        XCTAssertEqual(a.minX, b.minX, accuracy: 1,
                       "`+` 在侧栏开 / 关两态必须是同一个 x："
                       + "展开 \(a.minX) vs 收起 \(b.minX)")
    }

    /// 顺序钉死：侧栏开关 ▯ 在左、`+` 在右，两个都跳开红绿灯。
    func testToggleSitsLeftOfThePlusInBothStates() throws {
        for sidebar in [true, false] {
            let host = try renderWindow(sidebar: sidebar)
            let plus = try XCTUnwrap(popupFrames(host).first)
            let toggle = try XCTUnwrap(bandButtonFrames(host).first,
                                       "顶带里找不到侧栏开关（sidebar=\(sidebar)）")
            XCTAssertGreaterThanOrEqual(toggle.minX, WindowChrome.trafficLightsWidth,
                                        "开关压在红绿灯上了（x=\(toggle.minX)）")
            XCTAssertLessThan(toggle.maxX, plus.maxX,
                              "开关应在 `+` 左边（sidebar=\(sidebar)）："
                              + "开关 \(toggle.minX) vs 加号 \(plus.minX)")
        }
    }

    /// 终端配置坏了 → 顶带右边多一个 ⚠，但**不许把前面两个按钮推走**。
    ///
    /// 这个 ⚠ 是侧栏底部那条 footer 删掉后唯一的报错出口（iron law：不能
    /// 悄悄降级）；挂在最右是为了它的出现 / 消失不改变其它按钮的位置。
    func testConfigWarningAppendsOnTheRightWithoutMovingAnything() throws {
        let plain = try renderWindow(sidebar: true)
        let plainPlus = try XCTUnwrap(popupFrames(plain).first)
        let plainButtons = bandButtonFrames(plain)

        let warned = try renderWindow(sidebar: true) { model in
            model.setTerminalConfigErrorForTesting("配置无效：bad key")
        }
        let warnedPlus = try XCTUnwrap(popupFrames(warned).first)
        let warnedButtons = bandButtonFrames(warned)

        XCTAssertEqual(warnedButtons.count, plainButtons.count + 1,
                       "配置无效时顶带应多出一个 ⚠（用户得知道配置坏了）；"
                       + "位置 \(warnedButtons.map(\.minX))")
        XCTAssertGreaterThan(try XCTUnwrap(warnedButtons.last).minX, warnedPlus.maxX,
                             "⚠ 必须挂在 `+` 右边，不能插队")
        XCTAssertEqual(warnedPlus.minX, plainPlus.minX, accuracy: 1,
                       "⚠ 出现后 `+` 不该挪位置")
        XCTAssertEqual(try XCTUnwrap(warnedButtons.first).minX,
                       try XCTUnwrap(plainButtons.first).minX, accuracy: 1,
                       "⚠ 出现后侧栏开关不该挪位置")
    }

    /// 侧栏收起：顶部标签条回来——方向性对照，证明上面几条数到的东西确实
    /// 跟着布局动，而不是恒为真。
    ///
    /// 用顶带的**宽度**量：每条顶带都铺一层 `WindowDragArea`（空白处能拖
    /// 窗口）。侧栏展开时只有侧栏那条（宽 = 侧栏宽）；收起时侧栏整列移除、
    /// 标签条那条顶上（横跨整个终端列，远宽于侧栏）。「把规则关掉」的偏好
    /// 已弃用（关掉它就是用户实测报过的「清单画两处 + 两个加号」形态），
    /// 两条顶带共存的组合不再存在。
    func testClosingTheSidebarBringsTheStripBack() throws {
        let opened = try renderWindow(sidebar: true)
        let openedAreas = dragAreaFrames(opened)
        XCTAssertEqual(openedAreas.count, 1, "侧栏展开时只该有侧栏那一条顶带")
        XCTAssertLessThan(try XCTUnwrap(openedAreas.first).width,
                          DS.sidebarWidth + 1,
                          "这条顶带应是侧栏的（宽 ≤ 侧栏宽）")

        let closed = try renderWindow(sidebar: false)
        let closedAreas = dragAreaFrames(closed)
        XCTAssertEqual(closedAreas.count, 1, "收起后换成标签条那条顶带（仍只有一条）")
        XCTAssertGreaterThan(try XCTUnwrap(closedAreas.first).width,
                             DS.sidebarWidth + 100,
                             "标签条顶带应横跨终端列（远宽于侧栏）")
        // 状态切换时最容易出的错：两条顶带都画按钮组 → 屏幕上两个加号
        // （用户实测报过「现在出现 2 个加号了，我只要一个加号」）。
        XCTAssertEqual(popupFrames(closed).count, 1,
                       "收起态也只能有一个 `+`；得到 "
                       + "\(popupFrames(closed).map(\.minX))")
    }

    // MARK: - 辅助

    private func renderWindow(sidebar: Bool,
                              configure: (AppModel) -> Void = { _ in }) throws
        -> NSHostingView<RootView>
    {
        let dir = NSTemporaryDirectory() + "tab-strip-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/config", atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: dir + "/config",
                             knownHostsPath: dir + "/known_hosts")
        model.tabs.add(TerminalTab(kind: .localShell))
        model.tabs.add(TerminalTab(kind: .localShell))
        configure(model)

        let host = makeRootHostingView(model: model,
                                       initialShowsSidebar: sidebar)
        host.frame = NSRect(x: 0, y: 0, width: 900, height: 600)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable,
                                          .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        addTeardownBlock { @MainActor in window.orderOut(nil) }
        for _ in 0..<16 { RunLoop.main.run(until: Date().addingTimeInterval(0.05)) }
        return host
    }

    /// 视图树里所有 SwiftUI `Menu` 落下来的 popup 按钮（窗口坐标）。
    private func popupFrames(_ root: NSView) -> [CGRect] {
        var out: [CGRect] = []
        func walk(_ v: NSView) {
            if String(describing: type(of: v)).contains("PopupButton") {
                out.append(v.convert(v.bounds, to: nil))
            }
            for s in v.subviews { walk(s) }
        }
        walk(root)
        return out
    }

    /// 顶带那一行里的普通按钮（侧栏开关 / ⚠），从左到右。
    ///
    /// SwiftUI 的 `Button` 在 AppKit 侧没有自己的控件类，但会留下一个
    /// 同尺寸的 `KeyViewProxy`（键盘焦点用）——拿它当句柄量位置。只取顶带
    /// 那条横带里的，免得把侧栏行尾的关闭按钮、标签胶囊上的叉也数进来。
    private func bandButtonFrames(_ root: NSView) -> [CGRect] {
        let band = root.bounds.height - WindowChrome.barHeight
        var out: [CGRect] = []
        func walk(_ v: NSView) {
            let f = v.convert(v.bounds, to: nil)
            if String(describing: type(of: v)).contains("KeyViewProxy"),
               f.minY >= band, f.width >= DS.controlSize - 1,
               f.minX < DS.sidebarWidth {
                out.append(f)
            }
            for s in v.subviews { walk(s) }
        }
        walk(root)
        return out.sorted { $0.minX < $1.minX }
    }

    /// 每条顶带的范围：顶带都铺一层 `WindowDragArea`（空白处拖窗口），
    /// 数它 / 量它的宽就知道哪条顶带在场。
    private func dragAreaFrames(_ root: NSView) -> [CGRect] {
        var out: [CGRect] = []
        func walk(_ v: NSView) {
            if String(describing: type(of: v)) == "DragView" {
                out.append(v.convert(v.bounds, to: nil))
            }
            for s in v.subviews { walk(s) }
        }
        walk(root)
        return out
    }
}

