import AppKit
import GhostermAppModel
import GhostermTerminal
import XCTest
@testable import GhostermApp

@MainActor final class TabSearchTests: XCTestCase {
    func testCommandKStillClearsTheLiveTerminalWithoutOpeningSearch() throws {
        let f = try TabWindowFixture(); defer { f.close(); NSApp.mainMenu = nil }
        let (tab, surface) = try f.shell("Clear screen")
        AppMenu.install(model: f.model)
        XCTAssertTrue(f.until { !f.views(TerminalHostView.self).isEmpty })
        let host = try f.terminalHost(for: XCTUnwrap(tab.activePane))
        XCTAssertTrue(f.until { f.window.firstResponder === host })
        surface.sendCommandLine("printf 'CLEAR_SCREEN_%s\\n' MARKER")
        XCTAssertTrue(f.until { surface.readText()?.contains("CLEAR_SCREEN_MARKER") == true })
        let event = try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .command,
                                                 timestamp: ProcessInfo.processInfo.systemUptime,
                                                 windowNumber: f.window.windowNumber, context: nil,
                                                 characters: "k", charactersIgnoringModifiers: "k", isARepeat: false, keyCode: 40))
        let consumed = NSApp.mainMenu?.performKeyEquivalent(with: event) == true
        if !consumed { f.window.sendEvent(event) }
        f.settle()
        XCTAssertFalse(consumed, "⌘K 必须继续交给终端引擎")
        XCTAssertNil(f.window.attachedSheet, "清屏不能打开标签搜索")
        XCTAssertTrue(f.until { surface.readText()?.contains("CLEAR_SCREEN_MARKER") == false }, "⌘K 必须真正清除终端网格内容")
    }

    func testMenuShortcutSearchesSubtitlesAndSelectsDuplicateTitleByIdentity() throws {
        let f = try TabWindowFixture(); defer { f.close(); NSApp.mainMenu = nil }
        let target = TerminalTab(kind: .sftp(RemoteTarget(pattern: "server-a", user: "u", host: "needle.example", port: 22)))
        let other = TerminalTab(kind: .localFiles)
        target.title = "Duplicate"; other.title = "Duplicate"
        f.model.tabs.add(target); f.model.tabs.add(other)
        AppMenu.install(model: f.model)
        f.settle()
        for query in ["needle.example", "Renamed-title"] {
            f.model.tabs.activate(other); f.settle()
            let key = try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [.command, .shift],
                                                   timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: f.window.windowNumber, context: nil,
                                                   characters: "A", charactersIgnoringModifiers: "A", isARepeat: false, keyCode: 0))
            XCTAssertTrue(NSApp.mainMenu?.performKeyEquivalent(with: key) == true, "搜索快捷键必须走真实主菜单")
            XCTAssertTrue(f.until { f.window.attachedSheet != nil }, "搜索菜单必须打开搜索面板")
            let sheet = try XCTUnwrap(f.window.attachedSheet)
            let content = try XCTUnwrap(sheet.contentView)
            let field = try XCTUnwrap(f.views(NSTextField.self, in: content).first { $0.isEditable }, "搜索输入框必须可编辑")
            sheet.makeFirstResponder(field)
            let editor = try XCTUnwrap(field.currentEditor() as? NSTextView)
            editor.selectAll(nil)
            editor.insertText(query, replacementRange: editor.selectedRange())
            f.settle()
            if query == "Renamed-title" {
                target.title = query; f.settle()
            }
            editor.insertNewline(nil)
            XCTAssertTrue(f.until { f.window.attachedSheet == nil }, "回车应确认搜索结果")
            XCTAssertEqual(f.model.tabs.activeTabID, target.id, "搜索副标题后必须按 UUID 激活同名标签")
            XCTAssertEqual(f.model.tabs.tabs.count, 2, "搜索不创建或关闭会话")
        }
    }

    func testTitlesRefreshInBothNavigationViewsWithoutReplacingTheirRows() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let target = TerminalTab(kind: .localFiles), control = TerminalTab(kind: .localFiles)
        target.title = "iiii"; control.title = "Control"
        f.model.tabs.add(target); f.model.tabs.add(control)
        func pixels(_ id: UUID) throws -> Data {
            let row = try f.tabView(id)
            let rect = row.convert(row.bounds, to: f.root)
            let image = try XCTUnwrap(f.root.bitmapImageRepForCachingDisplay(in: rect))
            f.root.cacheDisplay(in: rect, to: image)
            return Data(bytes: try XCTUnwrap(image.bitmapData), count: image.bytesPerRow * image.pixelsHigh)
        }
        for placement in TabPlacement.allCases {
            target.title = "iiii"; f.model.preferences.tabPlacement = placement; f.settle()
            let row = try f.tabView(target.id)
            let before = try pixels(target.id), stable = try pixels(control.id)
            target.title = "WWWW"; f.settle()
            WindowChrome.redress(f.window); f.settle()
            XCTAssertNotEqual(try pixels(target.id), before, "真实标签标题必须立即重绘且 resize 不清空标题")
            XCTAssertEqual(try pixels(control.id), stable, "未改标题的标签不应随之改变")
            XCTAssertTrue(try f.tabView(target.id) === row, "改标题不替换标签身份")
        }
    }
}
