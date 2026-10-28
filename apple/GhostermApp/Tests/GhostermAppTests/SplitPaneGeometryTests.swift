// SplitPaneGeometryTests — **结构改动后**分屏窗格的尺寸。
//
// 用户实测带出：三窗格（嵌套）关掉中间那个后，剩下的两个停在上一次布局给的
// **不相等**宽度（163.5 vs 735.5，而不是各半 450）。根因是 `HSplitView`/
// `VSplitView` 背后的 `NSSplitView` 在子视图集合变化后**只增删、不重算比例**。
// 修法：分屏容器按结构 id（`PaneLayout.viewID`）强制重建（见 RootView）。
// 这条断言抓的是「关窗格后剩下的两个等分」，未修时会差好几百。
import AppKit
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class SplitPaneGeometryTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "split-geom-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/ssh_config", atomically: true,
                               encoding: .utf8)
        return isolatedAppModel(configPath: dir + "/ssh_config",
                        knownHostsPath: dir + "/known_hosts")
    }

    /// 关掉一个窗格后，剩下的两个应**等分**——不残留上一次布局的宽度。
    ///
    /// 复现「三窗格（嵌套）关中间」：`[[p0|p2]|p1]` 去掉 p2 → `[p0|p1]`。
    /// 未修时 `NSSplitView` 沿用旧比例 → 剩两个 163.5 vs 735.5；修后各半。
    func testClosingPaneRelayoutsEqually() throws {
        let model = try makeModel()
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        tab.split(axis: .horizontal, with: TerminalPane())     // [p0 | p1]
        tab.activePaneID = tab.panes[0].id
        tab.split(axis: .horizontal, with: TerminalPane())     // [[p0 | p2] | p1]
        XCTAssertEqual(tab.panes.count, 3)
        if case .split(let id, _, _, _) = tab.workspaceLayout {
            tab.workspaceLayout = tab.workspaceLayout?.settingWeights(node: id, weights: [7, 3])
        }

        // 每个窗格都挂一个真 terminal 面（几何断言要能抓到 TerminalHostView）。
        let app = try TerminalApp.shared()
        for p in tab.panes { p.attachForTesting(surface: try TerminalSurface(app: app)) }

        let host = renderWindow(model)
        XCTAssertEqual(hostWidths(host).count, 3, "三窗格应各挂一个终端面")

        _ = tab.closePane(tab.panes[1])                        // 去掉中间那个
        pump(20)

        let widths = hostWidths(host)
        XCTAssertEqual(widths.count, 2, "关掉中间窗格后应剩两个终端面（得到 \(widths.count)）")
        XCTAssertGreaterThan(widths[0], 200, "剩下的窗格该有像样的宽度（得到 \(widths)）")
        XCTAssertEqual(widths[0], widths[1], accuracy: 6.0,
                       "剩下的两个窗格应等分（得到 \(widths)）——"
                       + "NSSplitView 沿用旧比例时会差好几百")
    }

    // MARK: - 辅助

    private func pump(_ rounds: Int) {
        for _ in 0..<rounds {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        }
    }

    private func renderWindow(_ model: AppModel) -> NSHostingView<RootView> {
        let size = NSSize(width: 900, height: 600)
        model.preferences.tabPlacement = .top
        let host = makeRootHostingView(model: model)
        host.frame = NSRect(origin: .zero, size: size)
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

    /// 视图树里所有 `TerminalHostView` 的宽度（升序）。
    private func hostWidths(_ root: NSView) -> [CGFloat] {
        var out: [CGFloat] = []
        func walk(_ v: NSView) {
            if v is TerminalHostView { out.append(v.bounds.width) }
            for s in v.subviews { walk(s) }
        }
        walk(root)
        return out.sorted()
    }
}
