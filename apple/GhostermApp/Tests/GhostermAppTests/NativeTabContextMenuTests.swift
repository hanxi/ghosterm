import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

@MainActor final class NativeTabContextMenuTests: XCTestCase {
    override class func setUp() {
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    func testNativeMenuTargetsRightClickedTab() throws {
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: dir.appendingPathComponent("config").path,
                                     knownHostsPath: dir.appendingPathComponent("known_hosts").path)
        let window = NativeTitlebarWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 500),
                                          styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                                          backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        TestWindows.presentOffscreen(window)
        let coordinator = NativeTabWindows(model: model, anchor: window)
        defer {
            model.tabs.removeAll()
            for w in window.tabGroup?.windows ?? [window] { w.orderOut(nil) }
            withExtendedLifetime(coordinator) {}
        }
        func settle() { RunLoop.main.run(until: Date().addingTimeInterval(0.4)) }
        let target = TerminalTab(kind: .localShell, pane: TerminalPane()); target.title = "Target"
        let current = TerminalTab(kind: .localShell, pane: TerminalPane()); current.title = "Current"
        model.tabs.add(target); model.tabs.add(current); settle()
        let group = try XCTUnwrap(window.tabGroup)
        let currentWindow = try XCTUnwrap(group.windows.first { $0.title == "Current" } as? NativeTitlebarWindow)
        currentWindow.makeMain(); settle()
        func all(_ view: NSView, matching: (NSView) -> Bool) -> [NSView] {
            (matching(view) ? [view] : []) + view.subviews.flatMap { all($0, matching: matching) }
        }
        func menuForTarget() throws -> NSMenu {
            let root = try XCTUnwrap(currentWindow.contentView?.superview)
            let nativeTabs = all(root) { String(describing: type(of: $0)) == "NSTabButton" }
            let tabView = try XCTUnwrap(nativeTabs.first { tab in
                !all(tab) { ($0 as? NSTextField)?.stringValue == "Target" }.isEmpty
            })
            let point = tabView.convert(NSPoint(x: tabView.bounds.midX, y: tabView.bounds.midY), to: nil)
            let event = try XCTUnwrap(NSEvent.mouseEvent(with: .rightMouseDown, location: point, modifierFlags: [],
                                                          timestamp: 0, windowNumber: currentWindow.windowNumber,
                                                          context: nil, eventNumber: 1, clickCount: 1, pressure: 1))
            return try XCTUnwrap(currentWindow.nativeContextMenu(for: event)?.0, "原生标签右键必须提供菜单")
        }
        let outside = try XCTUnwrap(NSEvent.mouseEvent(with: .rightMouseDown, location: NSPoint(x: 20, y: 20),
                                                        modifierFlags: [], timestamp: 0, windowNumber: currentWindow.windowNumber,
                                                        context: nil, eventNumber: 2, clickCount: 1, pressure: 1))
        XCTAssertNil(currentWindow.nativeContextMenu(for: outside), "标签栏外的右键不得被截获")
        let menu = try menuForTarget()
        menu.delegate?.menuNeedsUpdate?(menu)
        let comparison = NSHostingView(rootView:
            Rectangle().fill(Color.gray).frame(width: 120, height: 80)
                .contextMenu { TabContextMenu(model: model, tabs: model.tabs, tab: target) })
        comparison.frame = NSRect(x: 0, y: 0, width: 120, height: 80)
        let comparisonWindow = NSWindow(contentRect: comparison.frame, styleMask: [.titled], backing: .buffered, defer: false)
        comparisonWindow.isReleasedWhenClosed = false
        comparisonWindow.contentView = comparison
        TestWindows.presentOffscreen(comparisonWindow)
        defer { comparisonWindow.orderOut(nil) }
        settle()
        let comparisonEvent = try XCTUnwrap(NSEvent.mouseEvent(with: .rightMouseDown, location: NSPoint(x: 60, y: 40),
                                                                modifierFlags: [], timestamp: 0, windowNumber: comparisonWindow.windowNumber,
                                                                context: nil, eventNumber: 0, clickCount: 1, pressure: 1))
        let currentRoot = try XCTUnwrap(currentWindow.contentView?.superview)
        let targetView = try XCTUnwrap(all(currentRoot) { String(describing: type(of: $0)) == "NSTabButton" }.first { tab in
            !all(tab) { ($0 as? NSTextField)?.stringValue == "Target" }.isEmpty
        })
        let otherPoint = targetView.convert(NSPoint(x: targetView.bounds.midX, y: targetView.bounds.midY), to: nil)
        let otherWindowEvent = try XCTUnwrap(NSEvent.mouseEvent(with: .rightMouseDown, location: otherPoint,
                                                                modifierFlags: [], timestamp: 0, windowNumber: comparisonWindow.windowNumber,
                                                                context: nil, eventNumber: 3, clickCount: 1, pressure: 1))
        XCTAssertNil(currentWindow.nativeContextMenu(for: otherWindowEvent), "其他窗口的右键不得被截获")
        func sidebarMenu(_ view: NSView) -> NSMenu? {
            if let candidate = view.menu(for: comparisonEvent), !candidate.items.isEmpty { return candidate }
            return view.subviews.lazy.compactMap { sidebarMenu($0) }.first
        }
        let sidebarItems = try XCTUnwrap(sidebarMenu(comparison)).items.filter { !$0.isSeparatorItem }
        let nativeItems = menu.items.filter { !$0.isSeparatorItem }
        XCTAssertEqual(nativeItems.map(\.title), sidebarItems.map(\.title), "原生菜单必须与侧边栏菜单一致")
        for (native, sidebar) in zip(nativeItems, sidebarItems) {
            XCTAssertEqual(ShortcutCatalog.describe(keyEquivalent: native.keyEquivalent, modifiers: native.keyEquivalentModifierMask),
                           ShortcutCatalog.describe(keyEquivalent: sidebar.keyEquivalent, modifiers: sidebar.keyEquivalentModifierMask),
                           "原生菜单快捷键必须与侧边栏一致：\(native.title)")
        }
        let files = try XCTUnwrap(menu.item(withTitle: "显示文件面板"))
        XCTAssertEqual(files.keyEquivalent, "e")
        XCTAssertEqual(files.keyEquivalentModifierMask.intersection(.deviceIndependentFlagsMask), [.command, .shift])
        XCTAssertTrue(NSApp.sendAction(try XCTUnwrap(files.action), to: files.target, from: files))
        XCTAssertTrue(target.filesPanelVisible, "文件面板应作用于被右键的标签")
        XCTAssertFalse(current.filesPanelVisible)
        model.toggleFilesPanel(for: target)
        model.tabs.activate(current); settle()
        let refreshed = try menuForTarget()
        refreshed.delegate?.menuNeedsUpdate?(refreshed)
        let split = try XCTUnwrap(refreshed.item(withTitle: "向右分屏"))
        XCTAssertEqual(split.keyEquivalent, "d")
        XCTAssertTrue(NSApp.sendAction(try XCTUnwrap(split.action), to: split.target, from: split))
        XCTAssertEqual(target.panes.count, 2, "分屏应作用于被右键的标签")
        XCTAssertEqual(current.panes.count, 1)
    }
}
