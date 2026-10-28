import AppKit
import GhostermAppModel
import XCTest
@testable import GhostermApp

@MainActor final class TabNavigationContextTests: XCTestCase {
    func testBothNavigationsUseTheSameMenuAndTargetTheClickedTab() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let target = TerminalTab(kind: .localShell, pane: TerminalPane())
        let current = TerminalTab(kind: .localShell, pane: TerminalPane())
        target.title = "Duplicate"; current.title = "Duplicate"
        f.model.tabs.add(target); f.model.tabs.add(current)
        var expected: [String]?
        for placement in TabPlacement.allCases {
            f.model.preferences.tabPlacement = placement
            f.model.tabs.activate(current); f.settle()
            let menu = try f.menu(for: target.id)
            let items = menu.items.filter { !$0.isSeparatorItem }
            let description = items.map { $0.title + " " + ShortcutCatalog.describe(keyEquivalent: $0.keyEquivalent, modifiers: $0.keyEquivalentModifierMask) }
            if let expected { XCTAssertEqual(description, expected, "顶部与侧边共用菜单及快捷键") }
            else { expected = description }
            let files = try XCTUnwrap(menu.item(withTitle: "显示文件面板"))
            XCTAssertEqual(files.keyEquivalent, "e")
            XCTAssertEqual(files.keyEquivalentModifierMask.intersection(.deviceIndependentFlagsMask), [.command, .shift])
            XCTAssertTrue(NSApp.sendAction(try XCTUnwrap(files.action), to: files.target, from: files))
            XCTAssertTrue(target.filesPanelVisible, "文件面板必须作用于右键的 UUID")
            XCTAssertFalse(current.filesPanelVisible)
            f.model.toggleFilesPanel(for: target)
        }
        f.model.tabs.activate(current); f.settle()
        let split = try XCTUnwrap(f.menu(for: target.id).item(withTitle: "向右分屏"))
        XCTAssertTrue(NSApp.sendAction(try XCTUnwrap(split.action), to: split.target, from: split))
        XCTAssertEqual(target.panes.count, 2, "右键分屏不能操作当前的另一标签")
        XCTAssertEqual(current.panes.count, 1)
        f.model.tabs.activate(current); f.settle()
        let zoom = try XCTUnwrap(f.menu(for: target.id).item(withTitle: "缩放当前分屏"))
        XCTAssertTrue(NSApp.sendAction(try XCTUnwrap(zoom.action), to: zoom.target, from: zoom))
        XCTAssertEqual(target.zoomedPaneID, target.activePaneID, "右键缩放必须操作所属标签")
        XCTAssertNil(current.zoomedPaneID)
    }
}
