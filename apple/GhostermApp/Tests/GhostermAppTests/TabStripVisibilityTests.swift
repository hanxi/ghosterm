import AppKit
import GhostermAppModel
import XCTest
@testable import GhostermApp

@MainActor final class TabStripVisibilityTests: XCTestCase {
    func testNavigationGeometryForEmptySingleAndMultipleTabs() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        for count in [0, 1, 3] {
            f.model.tabs.removeAll()
            for _ in 0..<count { f.model.tabs.add(TerminalTab(kind: .localShell, pane: TerminalPane())) }
            for placement in TabPlacement.allCases {
                f.model.preferences.tabPlacement = placement
                f.settle()
                let plus = f.views(NSButton.self).filter { $0.identifier?.rawValue == "new-connection" }
                XCTAssertEqual(plus.count, 1, "每种布局、空态和多标签都只能有一个加号")
                let button = try XCTUnwrap(plus.first)
                let rect = button.convert(button.bounds, to: nil)
                for kind in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] {
                    let light = try XCTUnwrap(f.window.standardWindowButton(kind))
                    let lightRect = light.convert(light.bounds, to: nil)
                    XCTAssertFalse(rect.intersects(lightRect), "加号不能遮住系统窗口按钮")
                    if placement == .top, let first = f.model.tabs.tabs.first {
                        let row = try f.tabView(first.id)
                        XCTAssertFalse(row.convert(row.bounds, to: nil).intersects(lightRect), "顶部标签不能遮住系统窗口按钮")
                    }
                }
                XCTAssertEqual(f.window.tabbingMode, .disallowed)
                XCTAssertNil(f.window.toolbar, "不保留会占位的旧原生 toolbar")
                XCTAssertFalse(f.window.tabGroup?.isTabBarVisible ?? false, "不能出现第二条系统标签栏")
                let navigation = navigationButtons(f, placement: placement)
                XCTAssertEqual(navigation.count, 1, "导航区只保留加号，不再放切换或搜索按钮")
                if count > 0 {
                    let rootTop = f.root.convert(f.root.bounds, to: nil).maxY
                    let workspace = try XCTUnwrap(f.views(WorkspaceHostView.self).first)
                    let bounds = workspace.convert(workspace.bounds, to: nil)
                    if placement == .sidebar {
                        XCTAssertEqual(bounds.maxY, rootTop, accuracy: 1, "侧栏模式不能留下顶部空带")
                        XCTAssertEqual(bounds.minX, DS.sidebarWidth, accuracy: 2)
                    } else {
                        XCTAssertEqual(rootTop - bounds.maxY, WindowChrome.barHeight, accuracy: 2, "顶部模式只预留一行标签栏")
                        XCTAssertEqual(bounds.minX, 0, accuracy: 1)
                    }
                }
            }
        }
    }

    func testPlusFollowsTheLastSidebarTabAndStaysAtTheTopRight() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        func plus() throws -> NSButton { try XCTUnwrap(f.views(NSButton.self).first { $0.identifier?.rawValue == "new-connection" }) }
        for count in [0, 1, 3] {
            f.model.tabs.removeAll()
            for _ in 0..<count { f.model.tabs.add(TerminalTab(kind: .localFiles)) }
            for placement in TabPlacement.allCases {
                f.model.preferences.tabPlacement = placement
                for width in [780.0, 1200.0] {
                    f.window.setContentSize(NSSize(width: width, height: 600)); f.settle()
                    let button = try plus(), rect = button.convert(button.bounds, to: nil)
                    if placement == .top {
                        XCTAssertEqual(rect.maxX, f.root.bounds.maxX - DS.Padding.s, accuracy: 2,
                                       "顶部加号必须固定在最右端")
                    } else {
                        XCTAssertLessThan(rect.maxX, DS.sidebarWidth, "侧栏加号必须位于侧栏内")
                        if let last = f.model.tabs.tabs.last {
                            let view = try f.tabView(last.id), lastRect = view.convert(view.bounds, to: nil)
                            XCTAssertLessThanOrEqual(rect.maxY, lastRect.minY + 1, "侧栏加号必须位于最后一个标签下方")
                            XCTAssertLessThan(lastRect.minY - rect.maxY, 24, "侧栏加号应紧随标签而不是钉在底部")
                        } else {
                            let top = f.root.convert(f.root.bounds, to: nil).maxY
                            XCTAssertLessThanOrEqual(rect.maxY, top - WindowChrome.barHeight, "空侧栏加号应位于红绿灯下方")
                        }
                    }
                }
            }
        }
        let second = try plus()
        second.performClick(nil)
        XCTAssertTrue(f.until { f.window.attachedSheet != nil }, "唯一加号必须打开连接选择器")
    }

    func testResizePreservesNavigationAndWorkspaceGeometry() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        f.model.tabs.add(TerminalTab(kind: .localShell, pane: TerminalPane()))
        for placement in TabPlacement.allCases {
            f.model.preferences.tabPlacement = placement
            for size in [NSSize(width: 780, height: 460), NSSize(width: 1200, height: 800)] {
                f.window.setContentSize(size); f.settle()
                let workspace = try XCTUnwrap(f.views(WorkspaceHostView.self).first)
                let top = f.root.convert(f.root.bounds, to: nil).maxY
                let bounds = workspace.convert(workspace.bounds, to: nil)
                XCTAssertEqual(top - bounds.maxY, placement == .top ? WindowChrome.barHeight : 0, accuracy: 2,
                               "resize 不能重新塞入顶栏占位")
                XCTAssertGreaterThan(bounds.width, size.width - DS.sidebarWidth - 2)
            }
        }
    }

    func testConfigurationWarningIsVisibleWithoutMovingThePlus() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        f.model.tabs.add(TerminalTab(kind: .localShell, pane: TerminalPane()))
        for placement in TabPlacement.allCases {
            f.model.preferences.tabPlacement = placement
            f.model.setTerminalConfigErrorForTesting(nil); f.settle()
            let button = try XCTUnwrap(f.views(NSButton.self).first { $0.identifier?.rawValue == "new-connection" })
            let before = button.convert(button.bounds, to: nil)
            func orange() throws -> Int {
                let image = try XCTUnwrap(f.root.bitmapImageRepForCachingDisplay(in: f.root.bounds))
                f.root.cacheDisplay(in: f.root.bounds, to: image)
                var count = 0
                for y in 0..<image.pixelsHigh {
                    for x in stride(from: 0, to: image.pixelsWide, by: 2) {
                        guard let c = image.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                        if c.redComponent > 0.7 && c.greenComponent > 0.2 && c.greenComponent < 0.8 && c.redComponent > c.greenComponent * 1.2 && c.blueComponent < c.greenComponent * 0.8 { count += 1 }
                    }
                }
                return count
            }
            let plain = try orange()
            f.model.setTerminalConfigErrorForTesting("bad key"); f.settle()
            try f.capture("error-" + placement.rawValue)
            XCTAssertGreaterThan(try orange(), plain + 1000, "终端配置错误必须显示可见警告")
            let after = button.convert(button.bounds, to: nil)
            XCTAssertEqual(after, before, "配置警告不能移动加号")
            XCTAssertEqual(navigationButtons(f, placement: placement).count, 1)
        }
    }

    private func navigationButtons(_ f: TabWindowFixture, placement: TabPlacement) -> [CGRect] {
        let height = placement == .sidebar ? f.root.bounds.height : WindowChrome.barHeight
        let width = placement == .sidebar ? (DS.sidebarWidth) : f.root.bounds.width
        let top = f.root.convert(f.root.bounds, to: nil).maxY
        var frames: [CGRect] = []
        for view in f.views(NSView.self) {
            let name = String(describing: type(of: view)), rect = view.convert(view.bounds, to: nil)
            if (view is NSButton || name.contains("KeyViewProxy") || name.contains("FocusRingView")),
               rect.width >= DS.controlSize - 1, rect.height >= 20, rect.minY >= top - height,
               rect.maxX <= width + 1, !frames.contains(where: { abs($0.midX - rect.midX) < 1 && abs($0.midY - rect.midY) < 1 }) {
                frames.append(rect)
            }
        }
        return frames
    }
}
