import AppKit
import GhostermAppModel
import GhostermTerminal
import XCTest
@testable import GhostermApp

@MainActor final class TabWindowTests: XCTestCase {
    func testNavigationChangesKeepLiveHostsProcessesAndKeyboardFocus() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let window = f.window, content = f.root
        let (first, surface) = try f.shell("First")
        let (second, secondSurface) = try f.shell("Second")
        XCTAssertTrue(f.until { f.views(TerminalHostView.self).count == 2 })
        let firstHost = try f.terminalHost(for: XCTUnwrap(first.activePane))
        let secondHost = try f.terminalHost(for: XCTUnwrap(second.activePane))
        surface.sendCommandLine("printf 'GT_TAB_PID_%s\\n' $$")
        XCTAssertTrue(f.until { surface.readText()?.range(of: "GT_TAB_PID_[0-9]+", options: .regularExpression) != nil })
        let text = try XCTUnwrap(surface.readText())
        let range = try XCTUnwrap(text.range(of: "GT_TAB_PID_[0-9]+", options: .regularExpression))
        let pid = String(text[range]).replacingOccurrences(of: "GT_TAB_PID_", with: "")
        for placement in [TabPlacement.sidebar, .top, .sidebar, .top] {
            f.model.preferences.tabPlacement = placement
            f.model.preferences.compactSidebar.toggle()
            f.model.tabs.activate(first)
            XCTAssertTrue(f.until { firstHost.paneFocused && !secondHost.paneFocused && window.firstResponder === firstHost },
                          "切到第一页时键盘应落在该页终端")
            f.model.tabs.activate(second)
            XCTAssertTrue(f.until { secondHost.paneFocused && !firstHost.paneFocused && window.firstResponder === secondHost },
                          "切到第二页时键盘应落在该页终端")
            XCTAssertTrue(f.window === window && f.root === content, "导航不得替换窗口或根宿主")
            XCTAssertTrue(try f.terminalHost(for: XCTUnwrap(first.activePane)) === firstHost, "布局切换不得重建终端宿主")
            XCTAssertTrue(try f.terminalHost(for: XCTUnwrap(second.activePane)) === secondHost)
            XCTAssertTrue(first.activePane?.surface === surface)
            XCTAssertTrue(second.activePane?.surface === secondSurface)
        }
        surface.sendCommandLine("printf 'GT_TAB_AGAIN_%s\\n' $$")
        XCTAssertTrue(f.until { surface.readText()?.contains("GT_TAB_AGAIN_\(pid)") == true }, "布局切换后仍须为同一 shell PID")
    }

    func testBothNavigationsSelectAndReorderByIdentity() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let a = TerminalTab(kind: .localFiles), b = TerminalTab(kind: .localFiles), c = TerminalTab(kind: .localFiles)
        for tab in [a, b, c] { tab.title = "Duplicate"; f.model.tabs.add(tab) }
        for placement in TabPlacement.allCases {
            f.model.preferences.tabPlacement = placement; f.settle()
            let target = try f.tabView(a.id)
            try f.click(target.convert(NSPoint(x: target.bounds.midX, y: target.bounds.midY), to: nil))
            XCTAssertEqual(f.model.tabs.activeTabID, a.id, "点击实际标签必须按 UUID 选中")
            TabDropDelegate.move(c.id, onto: a.id, in: f.model.tabs); f.settle()
            XCTAssertEqual(f.model.tabs.tabs.map(\.id).first, c.id, "重排必须更新同一标签清单")
            let views = try f.model.tabs.tabs.map { tab -> (UUID, CGRect) in
                let view = try f.tabView(tab.id)
                return (tab.id, view.convert(view.bounds, to: nil))
            }
            let ordered = views.sorted { placement == .top ? $0.1.minX < $1.1.minX : $0.1.maxY > $1.1.maxY }
            XCTAssertEqual(ordered.map(\.0), f.model.tabs.tabs.map(\.id), "真实导航顺序必须跟随模型")
            let before = f.model.tabs.tabs.map(\.id)
            TabDropDelegate.move(UUID(), onto: a.id, in: f.model.tabs)
            XCTAssertEqual(f.model.tabs.tabs.map(\.id), before, "失效拖动不得回落到当前标签")
            f.model.tabs.move(a, to: 0)
        }
    }

    func testCloseButtonConfirmsTheTargetAndKeepsAnEmptyWindow() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let target = TerminalTab(kind: .sftp(RemoteTarget(pattern: "test", user: "u", host: "example.test", port: 22)))
        let other = TerminalTab(kind: .localFiles)
        target.title = "Duplicate"; other.title = "Duplicate"
        f.model.tabs.add(target); f.model.tabs.add(other)
        for placement in TabPlacement.allCases {
            f.model.preferences.tabPlacement = placement; f.settle()
            let view = try f.tabView(target.id)
            let entered = try XCTUnwrap(NSEvent.enterExitEvent(with: .mouseEntered, location: .zero, modifierFlags: [],
                                                              timestamp: 0, windowNumber: f.window.windowNumber, context: nil,
                                                              eventNumber: 0, trackingNumber: 0, userData: nil))
            view.mouseEntered(with: entered); f.settle()
            let local = NSPoint(x: placement == .top ? 16 : view.bounds.maxX - 12, y: view.bounds.midY)
            try f.click(view.convert(local, to: nil))
            XCTAssertEqual(f.model.tabs.closeRequest?.id, target.id, "关闭必须询问被点击的标签")
            XCTAssertEqual(f.model.tabs.activeTabID, other.id, "关闭后台标签不能顺便激活它")
            XCTAssertNotNil(f.window.attachedSheet, "目标确认必须在唯一窗口呈现")
            f.model.tabs.cancelClose(); f.settle()
            XCTAssertEqual(f.model.tabs.tabs.count, 2)
        }
        f.model.tabs.requestClose(target)
        f.model.tabs.confirmClose(); f.settle()
        XCTAssertEqual(f.model.tabs.tabs.map(\.id), [other.id])
        f.model.tabs.requestClose(other); f.settle()
        XCTAssertTrue(f.model.tabs.tabs.isEmpty)
        XCTAssertTrue(f.window.isVisible, "最后一页关闭后保留空工作区")
        XCTAssertEqual(f.views(NSButton.self).filter { $0.identifier?.rawValue == "new-connection" }.count, 1)
    }

    func testTmuxSheetStaysOnTheSameWindowAcrossNavigationChanges() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let first = TerminalTab(kind: .localFiles), second = TerminalTab(kind: .localFiles)
        f.model.tabs.add(first); f.model.tabs.add(second)
        f.model.showTmux(for: .localShell)
        XCTAssertTrue(f.until { f.window.attachedSheet != nil })
        let sheet = try XCTUnwrap(f.window.attachedSheet)
        f.model.tabs.activate(first)
        f.model.preferences.tabPlacement = .top
        f.settle(0.4)
        XCTAssertNotNil(f.model.tmuxChooser, "切换标签不能取消 tmux 操作")
        XCTAssertTrue(f.window.attachedSheet === sheet, "切换布局和标签不重开或迁移 sheet")
    }

    func testWindowCloseUsesTheApplicationExitRoute() throws {
        var requested = 0
        let f = try TabWindowFixture(requestClose: { requested += 1 }); defer { f.close() }
        f.window.performClose(nil)
        XCTAssertEqual(requested, 1, "窗口关闭必须走统一的退出确认入口")
        XCTAssertTrue(f.window.isVisible, "退出裁决前不能先销毁工作区")
    }
}
