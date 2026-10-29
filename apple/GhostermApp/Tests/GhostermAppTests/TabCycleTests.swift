import AppKit
import GhostermAppModel
import GhostermTerminal
import XCTest
@testable import GhostermApp

@MainActor final class TabCycleTests: XCTestCase {
    private func pressTab(_ f: TabWindowFixture, backwards: Bool = false) throws {
        let event = try XCTUnwrap(NSEvent.keyEvent(
            with: .keyDown, location: .zero,
            modifierFlags: backwards ? [.control, .shift] : [.control],
            timestamp: ProcessInfo.processInfo.systemUptime,
            windowNumber: f.window.windowNumber, context: nil,
            characters: backwards ? "\u{19}" : "\t", charactersIgnoringModifiers: backwards ? "\u{19}" : "\t",
            isARepeat: false, keyCode: 48))
        NSApp.sendEvent(event)
        f.settle()
    }

    func testControlTabCyclesLiveTabsInDisplayOrderAndMovesKeyboardFocus() throws {
        let f = try TabWindowFixture(); defer { f.close(); NSApp.mainMenu = nil }
        let (a, _) = try f.shell("A")
        let (b, _) = try f.shell("B")
        let (c, _) = try f.shell("C")
        AppMenu.install(model: f.model)
        XCTAssertTrue(f.until { f.views(TerminalHostView.self).count == 3 })
        let hosts = try [a, b, c].map { try f.terminalHost(for: XCTUnwrap($0.activePane)) }
        for placement in TabPlacement.allCases {
            f.model.preferences.tabPlacement = placement
            f.model.tabs.move(c, to: 0) // 显示顺序 C A B，不是创建顺序。
            f.model.tabs.activate(b); f.settle()
            for (backwards, expected) in [(false, c), (false, a), (true, c), (true, b)] {
                try pressTab(f, backwards: backwards)
                XCTAssertEqual(f.model.tabs.activeTabID, expected.id, "Ctrl+Tab 必须按显示顺序首尾循环，Shift 反向：backwards=\(backwards)，目标=\(expected.title)，实际=\(f.model.tabs.activeTab?.title ?? "nil")")
                let host = try f.terminalHost(for: XCTUnwrap(expected.activePane))
                XCTAssertTrue(f.until { f.window.firstResponder === host && host.paneFocused },
                              "快捷键切标签后键盘必须落到新终端")
            }
            for (tab, host) in zip([a, b, c], hosts) {
                XCTAssertTrue(try f.terminalHost(for: XCTUnwrap(tab.activePane)) === host,
                              "快捷键切换不得重建终端宿主")
            }
        }
    }

    func testEmptyAndSingleTabStayStable() throws {
        let f = try TabWindowFixture(); defer { f.close(); NSApp.mainMenu = nil }
        AppMenu.install(model: f.model)
        try pressTab(f); try pressTab(f, backwards: true)
        XCTAssertNil(f.model.tabs.activeTabID)
        let tab = TerminalTab(kind: .localFiles)
        f.model.tabs.add(tab)
        try pressTab(f); try pressTab(f, backwards: true)
        XCTAssertEqual(f.model.tabs.activeTabID, tab.id)
        XCTAssertEqual(f.model.tabs.tabs.count, 1)
    }
}
