// SplitDividerTests — 有留白时不再叠加分隔线，拖动命中与调比例始终保留。
// 默认贴边时需要的 1pt 细线由 ChromePaletteTests 另验像素，不能套用旧卡片规则。
import AppKit
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor
final class SplitDividerTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    /// 横分屏：线子层被清、divider view 本体仍可交互。
    func testHorizontalDividerLineIsCleared() throws {
        try assertDividerCleared(axis: .horizontal)
    }

    /// 竖分屏采用同一策略，另一个轴也必须保留透明的拖动区域。
    func testVerticalDividerLineIsCleared() throws {
        try assertDividerCleared(axis: .vertical)
    }

    // MARK: - 断言主体

    private func assertDividerCleared(axis: PaneLayout.Axis) throws {
        let oldGap = UserDefaults.standard.object(forKey: PaneGap.key)
        UserDefaults.standard.set(10, forKey: PaneGap.key)
        defer { UserDefaults.standard.set(oldGap, forKey: PaneGap.key) }
        let model = try makeModel()
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        tab.split(axis: axis, with: TerminalPane())

        let app = try TerminalApp.shared()
        for p in tab.panes { p.attachForTesting(surface: try TerminalSurface(app: app)) }

        let host = renderWindow(model)
        pump(30)

        guard let sv = findSplitView(host) else {
            return XCTFail("找不到 \(axis == .vertical ? "竖" : "横")分屏的 WorkspaceHostView")
        }
        let dividers = sv.dividerViews
        XCTAssertFalse(dividers.isEmpty, "WorkspaceHostView 里没有 divider view——前提不成立")

        let before = tab.workspaceLayout
        let first = try XCTUnwrap(dividers.first)
        XCTAssertIdentical(sv.hitTest(NSPoint(x: first.frame.midX, y: first.frame.midY)), first,
                           "透明手柄仍须参与命中测试")
        sv.setPosition((axis == .horizontal ? sv.bounds.width : sv.bounds.height) * 0.65, ofDividerAt: 0)
        pump(10)
        XCTAssertNotEqual(tab.workspaceLayout, before, "分隔手柄必须真实修改比例")
        for d in dividers {
            // 拖拽保留：divider view 本体**不得**被藏（藏了就收不到拖拽）。
            XCTAssertFalse(d.isHidden, "divider view 被藏了——拖拽调比例会失灵")
            XCTAssertNil(d.layer?.backgroundColor, "分隔手柄不能绘制背景线")
            XCTAssertGreaterThan(d.alphaValue, 0.01,
                                 "divider view 透明到不参与 hitTest——拖拽会失灵")
            // 线不得画：背衬层里不许再有「可见且带背景色」的细线子层。
            for sl in d.layer?.sublayers ?? []
            where min(sl.frame.width, sl.frame.height) <= 1.5 {
                let lineVisible = !sl.isHidden && (sl.backgroundColor?.alpha ?? 0) > 0
                XCTAssertFalse(lineVisible,
                               "分隔线子层还在画（frame=\(sl.frame)）——中间会看到一条 1pt 灰线")
            }
        }
    }

    // MARK: - 辅助（与 SplitPaneGeometryTests 同套路）

    private func makeModel() throws -> AppModel {
        let dir = NSTemporaryDirectory() + "split-divider-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        try "Host web\n".write(toFile: dir + "/ssh_config", atomically: true,
                               encoding: .utf8)
        return isolatedAppModel(configPath: dir + "/ssh_config",
                        knownHostsPath: dir + "/known_hosts")
    }

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
        return host
    }

    private func findSplitView(_ root: NSView) -> WorkspaceHostView? {
        var found: WorkspaceHostView?
        func walk(_ v: NSView) {
            // 本测试的布局里只有一个分屏容器，找到即是。
            if found == nil, let sv = v as? WorkspaceHostView { found = sv }
            for s in v.subviews { walk(s) }
        }
        walk(root)
        return found
    }
}
