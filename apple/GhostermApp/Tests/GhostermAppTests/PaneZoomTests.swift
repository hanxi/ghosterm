// PaneZoomTests — 缩放（⇧⌘↩）的语义与「兄弟窗格不被动过」。
//
// 缩放最容易做坏的两件事，都不报错：
//   1. 状态悬空——关掉被缩放那格后 zoomedPaneID 还指着它，屏幕上盖出一片空白；
//   2. 顺手把兄弟窗格重排了——它们会收到一轮 SIGWINCH，回卷里的内容被揉烂
//      （所以实现是「盖一层」而不是「改布局」，见 RootView.terminalContent）。
// 第 2 条在这里用**真的视图树**量：缩放前后兄弟窗格的 TerminalHostView
// 宽度必须一模一样。
import AppKit
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class PaneZoomTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "pane-zoom-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/ssh_config", atomically: true,
                               encoding: .utf8)
        return isolatedAppModel(configPath: dir + "/ssh_config",
                        knownHostsPath: dir + "/known_hosts")
    }

    // MARK: - 语义

    /// 没分屏时缩放**不留状态**：单格本来就铺满，留个 true 只会让下次分屏
    /// 莫名其妙带着缩放。
    func testZoomIsNoOpWithOnlyOnePane() {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        tab.toggleZoom(tab.panes[0])
        XCTAssertNil(tab.zoomedPaneID, "只有一格时不该进入缩放")
    }

    /// 分屏后来回切换；缩放时顺带把焦点给它（铺满了却不是焦点说不通）。
    func testToggleZoomSetsAndClearsWithFocus() {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        tab.split(axis: .horizontal, with: TerminalPane())
        let first = tab.panes[0]

        tab.toggleZoom(first)
        XCTAssertEqual(tab.zoomedPaneID, first.id)
        XCTAssertEqual(tab.activePaneID, first.id, "缩放的那格应同时成为当前窗格")
        XCTAssertIdentical(tab.zoomedPane, first)

        tab.toggleZoom(first)
        XCTAssertNil(tab.zoomedPaneID, "再按一次还原")
    }

    /// 关掉**被缩放**那格 → 退出缩放（否则状态指向一个不在布局里的窗格）。
    func testClosingTheZoomedPaneExitsZoom() {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        tab.split(axis: .horizontal, with: TerminalPane())
        tab.split(axis: .vertical, with: TerminalPane())      // 三格
        let zoomed = tab.panes[0]
        tab.toggleZoom(zoomed)
        XCTAssertEqual(tab.zoomedPaneID, zoomed.id)

        _ = tab.closePane(zoomed)
        XCTAssertNil(tab.zoomedPaneID, "被缩放那格没了，缩放状态必须跟着清掉")
        XCTAssertNil(tab.zoomedPane)
    }

    /// 关掉**别的**格直到只剩一格 → 也要退出缩放（一格没有「缩放」可言）。
    func testClosingDownToOnePaneExitsZoom() {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        tab.split(axis: .horizontal, with: TerminalPane())
        let kept = tab.panes[0], other = tab.panes[1]
        tab.toggleZoom(kept)
        _ = tab.closePane(other)
        XCTAssertEqual(tab.panes.count, 1)
        XCTAssertNil(tab.zoomedPaneID, "只剩一格时不该还处在缩放态")
    }

    /// 缩放中再分屏 → 退出缩放（又多一格，「铺满」不成立了）。
    func testSplittingWhileZoomedExitsZoom() {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        tab.split(axis: .horizontal, with: TerminalPane())
        tab.toggleZoom(tab.panes[0])
        tab.split(axis: .vertical, with: TerminalPane())
        XCTAssertNil(tab.zoomedPaneID)
    }

    /// 菜单 / ⇧⌘↩ 的落点真的接到当前标签页的当前窗格上。
    func testAppModelTogglesZoomOnActivePane() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        tab.split(axis: .horizontal, with: TerminalPane())
        let active = try XCTUnwrap(tab.activePane)

        model.toggleZoomActivePane()
        XCTAssertEqual(tab.zoomedPaneID, active.id, "⇧⌘↩ 应缩放当前窗格")
        model.toggleZoomActivePane()
        XCTAssertNil(tab.zoomedPaneID)
    }

    // MARK: - 兄弟窗格的几何不被动过

    /// 缩放 → 还原的整个过程里，**没被缩放**的那格尺寸一个点都不该变。
    ///
    /// 这条抓的是「把缩放实现成改布局」那种做法：无论是把兄弟摘掉还是把它
    /// 压成 1×1，这里量到的宽度都会变，而终端那边对应的是一轮 SIGWINCH。
    func testSiblingGeometryIsUntouchedByZoom() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        tab.split(axis: .horizontal, with: TerminalPane())
        let app = try TerminalApp.shared()
        for p in tab.panes { p.attachForTesting(surface: try TerminalSurface(app: app)) }
        let sibling = tab.panes[0], zoomed = tab.panes[1]
        tab.activePaneID = sibling.id

        let host = renderWindow(model)
        let before = hostFrames(host)
        XCTAssertEqual(before.count, 2, "两格各挂一个终端面")
        // 兄弟是左边那格（panes[0]），按 x 认人——不能按宽度排序取，
        // 左右两格的宽度本来就可能差 1 点（NSSplitView 取整）。
        let siblingWidthBefore = try XCTUnwrap(before.min(by: { $0.minX < $1.minX })?.width)
        XCTAssertGreaterThan(siblingWidthBefore, 200)

        tab.toggleZoom(zoomed)
        pump(20)
        let during = hostFrames(host)
        XCTAssertEqual(during.count, 2,
                       "缩放是盖一层，不卸载任何窗格——兄弟的 surface 必须还在树上")
        let widest = try XCTUnwrap(during.map(\.width).max())
        XCTAssertGreaterThan(widest, siblingWidthBefore * 1.5,
                             "被缩放那格应铺满整块终端区（得到 \(during.map(\.width))）")
        let stillThere = try XCTUnwrap(during.filter { $0.width < widest }.first?.width)
        // 容差 1 点只兜 NSSplitView 的取整抖动；真把兄弟重排了（摘掉、压扁、
        // 重新等分）差的是上百点。
        XCTAssertEqual(stillThere, siblingWidthBefore, accuracy: 1.5,
                       "没被缩放那格的宽度不该变（\(siblingWidthBefore) → \(stillThere)）")

        tab.toggleZoom(zoomed)
        pump(20)
        let after = hostFrames(host).map(\.width).sorted()
        XCTAssertEqual(after.count, 2)
        XCTAssertEqual(after[0], after[1], accuracy: 6.0,
                       "还原后两格回到等分（得到 \(after)）")
    }

    /// 还原之后，**用户拖出来的分隔比例**必须原样回来。
    ///
    /// 上一条是从等分起步的，所以「保持比例」与「被重新等分」给出同一个
    /// 结果——它看不见这个 bug。这里先把分隔线拖到 70/30 再来回缩放：
    /// 比例丢了就会塌回等分（450/450），差上百点。
    ///
    /// 根因是叶子那层的 `if 被缩放 { 空卡片 } else { PaneView }`：两个分支
    /// 是不同的 `_ConditionalContent`，SwiftUI 会把 `NSSplitView` 的那个
    /// arranged subview 整个换掉，而 `NSSplitView` 在子视图集合变化后只增删、
    /// 不重算比例（同一文件里关窗格那条注释讲的是同一件事）。
    func testZoomRoundTripKeepsDraggedDividerRatio() throws {
        let model = try makeModel()
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        tab.split(axis: .horizontal, with: TerminalPane())
        let app = try TerminalApp.shared()
        for p in tab.panes { p.attachForTesting(surface: try TerminalSurface(app: app)) }
        let zoomed = tab.panes[1]

        let host = renderWindow(model)
        let split = try XCTUnwrap(splitView(in: host), "没找到分屏容器")

        // 把分隔线拖到约 70/30（模拟用户手动调过比例）。
        split.setPosition(split.bounds.width * 0.7, ofDividerAt: 0)
        pump(20)

        let before = hostFrames(host).sorted { $0.minX < $1.minX }.map(\.width)
        XCTAssertEqual(before.count, 2)
        XCTAssertGreaterThan(before[0], before[1] * 1.8,
                             "前置条件：分隔线应已被拖成明显非等分（得到 \(before)）")

        tab.toggleZoom(zoomed)
        pump(20)
        tab.toggleZoom(zoomed)
        pump(20)

        let after = hostFrames(host).sorted { $0.minX < $1.minX }.map(\.width)
        XCTAssertEqual(after.count, 2)
        XCTAssertEqual(after[0], before[0], accuracy: 2.0,
                       "缩放往返后左格宽度应原样回来（\(before) → \(after)）")
        XCTAssertEqual(after[1], before[1], accuracy: 2.0,
                       "缩放往返后右格宽度应原样回来（\(before) → \(after)）")
    }

    // MARK: - 辅助

    private func pump(_ rounds: Int) {
        for _ in 0..<rounds {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
    }

    private func renderWindow(_ model: AppModel) -> NSHostingView<RootView> {
        model.preferences.tabPlacement = .top
        let host = makeRootHostingView(model: model)
        host.frame = NSRect(x: 0, y: 0, width: 900, height: 600)
        let window = NSWindow(contentRect: host.frame,
                              styleMask: [.titled, .closable, .resizable,
                                          .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        addTeardownBlock { @MainActor in window.orderOut(nil) }
        pump(20)
        return host
    }

    /// 找到分屏容器（`HSplitView`/`VSplitView` 背后的那个 `NSSplitView`）。
    /// 只认有两个 arranged subview 的那一个——两格分屏下它唯一。
    private func splitView(in root: NSView) -> WorkspaceHostView? {
        var found: WorkspaceHostView?
        func walk(_ v: NSView) {
            if let s = v as? WorkspaceHostView, s.hosts.count == 2, found == nil {
                found = s
            }
            for sub in v.subviews { walk(sub) }
        }
        walk(root)
        return found
    }

    private func hostFrames(_ root: NSView) -> [CGRect] {
        var out: [CGRect] = []
        func walk(_ v: NSView) {
            // 转到窗口坐标：要按 x 认出「左边那格」，bounds.minX 恒为 0。
            if v is TerminalHostView { out.append(v.convert(v.bounds, to: nil)) }
            for s in v.subviews { walk(s) }
        }
        walk(root)
        return out
    }
}
