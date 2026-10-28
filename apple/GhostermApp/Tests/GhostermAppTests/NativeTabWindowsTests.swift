import AppKit
import GhostermAppModel
import GhostermTerminal
import XCTest
@testable import GhostermApp

@MainActor
final class NativeTabWindowsTests: XCTestCase {
    override class func setUp() {
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    func testSwitchingNativeTabsKeepsLiveTerminalHostsAndProcesses() throws {
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: dir.appendingPathComponent("config").path,
                                     knownHostsPath: dir.appendingPathComponent("known_hosts").path)
        let anchor = NativeTitlebarWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 500),
                              styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        anchor.isReleasedWhenClosed = false
        TestWindows.presentOffscreen(anchor)
        let coordinator = NativeTabWindows(model: model, anchor: anchor)
        defer {
            model.tabs.removeAll()
            for window in anchor.tabGroup?.windows ?? [anchor] { window.orderOut(nil) }
            withExtendedLifetime(coordinator) {}
        }
        func makeTab(_ title: String) throws -> (TerminalTab, TerminalSurface) {
            let pane = TerminalPane()
            let surface = try TerminalSurface(app: TerminalApp.shared())
            let session = try SessionController.localShell(surface: surface, executable: "/bin/sh",
                                                           argv: ["-i"], cwd: dir.path, kind: .direct)
            pane.attach(surface: surface, session: session)
            pane.start()
            let tab = TerminalTab(kind: .localShell, pane: pane)
            tab.title = title
            return (tab, surface)
        }
        func waitFor(_ condition: () -> Bool) -> Bool {
            let deadline = Date().addingTimeInterval(5)
            while Date() < deadline {
                if condition() { return true }
                RunLoop.main.run(until: Date().addingTimeInterval(0.05))
            }
            return condition()
        }
        func findHost(_ view: NSView) -> TerminalHostView? {
            if let host = view as? TerminalHostView { return host }
            return view.subviews.lazy.compactMap { findHost($0) }.first
        }
        let (first, firstSurface) = try makeTab("First shell")
        let (second, secondSurface) = try makeTab("Second shell")
        model.tabs.add(first); model.tabs.add(second)
        XCTAssertTrue(waitFor { anchor.tabGroup?.windows.count == 2 })
        let group = try XCTUnwrap(anchor.tabGroup)
        let firstWindow = try XCTUnwrap(group.windows.first { $0.title == "First shell" })
        let secondWindow = try XCTUnwrap(group.windows.first { $0.title == "Second shell" })
        XCTAssertTrue(waitFor { findHost(firstWindow.contentView!) != nil && findHost(secondWindow.contentView!) != nil })
        let firstHost = try XCTUnwrap(findHost(try XCTUnwrap(firstWindow.contentView)))
        let secondHost = try XCTUnwrap(findHost(try XCTUnwrap(secondWindow.contentView)))
        firstSurface.sendCommandLine("printf 'GT_NATIVE_FIRST_%s\\n' $$")
        XCTAssertTrue(waitFor { firstSurface.readText()?.contains("GT_NATIVE_FIRST_") == true })
        let before = try XCTUnwrap(firstSurface.readText())
        let pid = try XCTUnwrap(before.range(of: "GT_NATIVE_FIRST_[0-9]+", options: .regularExpression))
        let marker = String(before[pid]).replacingOccurrences(of: "GT_NATIVE_FIRST_", with: "")
        model.tabs.activate(first)
        XCTAssertTrue(waitFor { group.selectedWindow === firstWindow })
        XCTAssertTrue(waitFor { firstHost.paneFocused && !secondHost.paneFocused && firstWindow.firstResponder === firstHost },
                      "切到第一页时键盘应落在该页终端")
        model.tabs.activate(second)
        XCTAssertTrue(waitFor { group.selectedWindow === secondWindow })
        XCTAssertTrue(waitFor { secondHost.paneFocused && !firstHost.paneFocused && secondWindow.firstResponder === secondHost },
                      "切到第二页时键盘应落在该页终端")
        XCTAssertTrue(findHost(firstWindow.contentView!) === firstHost)
        XCTAssertTrue(findHost(secondWindow.contentView!) === secondHost)
        firstSurface.sendCommandLine("printf 'GT_NATIVE_AGAIN_%s\\n' $$")
        XCTAssertTrue(waitFor { firstSurface.readText()?.contains("GT_NATIVE_AGAIN_\(marker)") == true },
                      "切换后应仍是同一 shell 进程")
        XCTAssertTrue(first.panes[0].surface === firstSurface)
        XCTAssertTrue(second.panes[0].surface === secondSurface)
    }

    func testNativeWindowsFollowModelAndCloseTheRequestedTab() throws {
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: dir.appendingPathComponent("config").path,
                                     knownHostsPath: dir.appendingPathComponent("known_hosts").path)
        let anchor = NativeTitlebarWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 500),
                              styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        anchor.isReleasedWhenClosed = false
        TestWindows.presentOffscreen(anchor)
        let coordinator = NativeTabWindows(model: model, anchor: anchor)
        defer {
            model.tabs.removeAll()
            for window in anchor.tabGroup?.windows ?? [anchor] { window.orderOut(nil) }
            withExtendedLifetime(coordinator) {}
        }
        func settle() { RunLoop.main.run(until: Date().addingTimeInterval(0.3)) }
        let first = TerminalTab(kind: .localFiles); first.title = "First"
        let second = TerminalTab(kind: .localFiles); second.title = "Second"
        model.tabs.add(first); model.tabs.add(second); settle()
        let group = try XCTUnwrap(anchor.tabGroup, "标签必须属于 AppKit 窗口标签组")
        XCTAssertEqual(group.windows.count, 2)
        XCTAssertTrue(group.isTabBarVisible, "多个标签时必须显示原生标签栏")
        let firstWindow = try XCTUnwrap(group.windows.first { $0.title == "First" })
        let secondWindow = try XCTUnwrap(group.windows.first { $0.title == "Second" })
        secondWindow.makeMain(); settle()
        func findBar(_ view: NSView) -> NSView? {
            if String(describing: type(of: view)) == "NSTabBar" { return view }
            return view.subviews.lazy.compactMap { findBar($0) }.first
        }
        func findWorkspace(_ view: NSView) -> WorkspaceHostView? {
            if let workspace = view as? WorkspaceHostView { return workspace }
            return view.subviews.lazy.compactMap { findWorkspace($0) }.first
        }
        let frame = try XCTUnwrap(secondWindow.contentView?.superview)
        let bar = try XCTUnwrap(findBar(frame), "原生标签栏必须实际在窗口视图树中")
        do {
            func descendants(_ view: NSView, matching: (NSView) -> Bool) -> [NSView] {
                (matching(view) ? [view] : []) + view.subviews.flatMap { descendants($0, matching: matching) }
            }
            let tabButtons = descendants(bar) { String(describing: type(of: $0)) == "NSTabButton" }
            XCTAssertEqual(tabButtons.count, 2, "系统标签栏必须显示两个标签")
            for button in tabButtons {
                let close = try XCTUnwrap(descendants(button) { $0 is NSButton }.first as? NSButton,
                                          "系统标签应包含关闭按钮")
                let title = try XCTUnwrap(descendants(button) { $0 is NSTextField }.first)
                XCTAssertLessThan(close.convert(close.bounds, to: nil).maxX,
                                  title.convert(title.bounds, to: nil).minX, "关闭按钮应在标题左边")
                XCTAssertEqual(close.alphaValue, 0, accuracy: 0.01, "未悬停时关闭按钮应隐藏")
            }
        }
        let workspace = try XCTUnwrap(findWorkspace(frame))
        do {
            model.setTerminalConfigErrorForTesting("test")
            settle()
            let close = try XCTUnwrap(secondWindow.standardWindowButton(.closeButton))
            let barRect = bar.convert(bar.bounds, to: nil)
            let closeRect = close.convert(close.bounds, to: nil)
            XCTAssertTrue(barRect.minY <= closeRect.midY && closeRect.midY <= barRect.maxY,
                          "原生标签栏应与红绿灯位于同一行")
            XCTAssertGreaterThan(barRect.width, secondWindow.frame.width / 2,
                                 "原生标签栏应占据标题栏可用宽度")
            XCTAssertGreaterThan(barRect.minX, closeRect.maxX, "标签栏不得覆盖窗口控件")
            var bandButtons: [CGRect] = []
            func collectButtons(_ view: NSView) {
                let rect = view.convert(view.bounds, to: nil)
                let name = String(describing: type(of: view))
                if (name.contains("FocusRingView") || name.contains("KeyViewProxy") || view is NSButton),
                   rect.width >= DS.controlSize - 1, rect.midY >= secondWindow.frame.height - WindowChrome.barHeight,
                   !bandButtons.contains(where: { abs($0.midX - rect.midX) < 1 }) {
                    bandButtons.append(rect)
                }
                view.subviews.forEach(collectButtons)
            }
            collectButtons(try XCTUnwrap(secondWindow.contentView))
            XCTAssertGreaterThanOrEqual(bandButtons.count, 2, "侧栏切换和配置错误提示必须存在")
            XCTAssertLessThanOrEqual(bandButtons.map(\.maxX).max() ?? .infinity, barRect.minX,
                                     "标签栏不得覆盖配置错误提示")
        }
        XCTAssertLessThanOrEqual(workspace.convert(workspace.bounds, to: nil).maxY,
                                 bar.convert(bar.bounds, to: nil).minY + 1,
                                 "原生标签栏不得遮住工作区")
        XCTAssertTrue(group.selectedWindow === secondWindow)
        model.tabs.activate(first); settle()
        XCTAssertTrue(group.selectedWindow === firstWindow)
        do {
            firstWindow.makeMain(); settle()
            let firstFrame = try XCTUnwrap(firstWindow.contentView?.superview)
            let switchedBar = try XCTUnwrap(findBar(firstFrame))
            XCTAssertGreaterThan(switchedBar.convert(switchedBar.bounds, to: nil).width,
                                 firstWindow.frame.width / 2, "切换标签后标题栏必须保持完整宽度")
        }
        group.selectedWindow = secondWindow
        secondWindow.makeKey()
        settle()
        XCTAssertEqual(model.tabs.activeTabID, second.id)
        XCTAssertTrue(group.selectedWindow === secondWindow)
        second.title = "Renamed"; settle()
        XCTAssertEqual(secondWindow.title, "Renamed")
        secondWindow.setContentSize(NSSize(width: 850, height: 520)); settle()
        XCTAssertEqual(secondWindow.title, "Renamed", "调整尺寸不得清空原生标签标题")
        do {
            let resizedFrame = try XCTUnwrap(secondWindow.contentView?.superview)
            let resizedBar = try XCTUnwrap(findBar(resizedFrame))
            XCTAssertGreaterThan(resizedBar.convert(resizedBar.bounds, to: nil).width,
                                 secondWindow.frame.width / 2, "调整尺寸后标签栏仍需铺满可用宽度")
        }
        XCTAssertFalse(coordinator.windowShouldClose(firstWindow))
        settle()
        XCTAssertEqual(model.tabs.tabs.map(\.id), [second.id])
        XCTAssertTrue(model.tabs.activeTabID == second.id)
        XCTAssertTrue(secondWindow.isVisible, "关掉原始窗口后，其他标签必须继续存活")
        XCTAssertFalse(coordinator.windowShouldClose(secondWindow))
        settle()
        XCTAssertTrue(model.tabs.tabs.isEmpty)
        XCTAssertTrue(secondWindow.isVisible, "最后一页关闭后应保留空工作区")
    }

    func testNativeCloseRequestsConfirmationForTheTarget() throws {
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: dir.appendingPathComponent("config").path,
                                     knownHostsPath: dir.appendingPathComponent("known_hosts").path)
        let anchor = NativeTitlebarWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 500),
                              styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        anchor.isReleasedWhenClosed = false
        TestWindows.presentOffscreen(anchor)
        let coordinator = NativeTabWindows(model: model, anchor: anchor)
        defer {
            model.tabs.removeAll()
            for window in anchor.tabGroup?.windows ?? [anchor] { window.orderOut(nil) }
            withExtendedLifetime(coordinator) {}
        }
        func settle() { RunLoop.main.run(until: Date().addingTimeInterval(0.4)) }
        let target = TerminalTab(kind: .sftp(RemoteTarget(pattern: "test", user: "u", host: "h", port: 22)))
        target.title = "Pending"
        let other = TerminalTab(kind: .localFiles); other.title = "Other"
        model.tabs.add(target); model.tabs.add(other); settle()
        let group = try XCTUnwrap(anchor.tabGroup)
        let targetWindow = try XCTUnwrap(group.windows.first { $0.title == "Pending" })
        let currentWindow = try XCTUnwrap(group.windows.first { $0.title == "Other" })
        group.insertWindow(targetWindow, at: 1); settle()
        XCTAssertEqual(model.tabs.tabs.map(\.id), [other.id, target.id], "重排后侧栏顺序必须同步")
        do {
            currentWindow.makeMain(); settle()
            func descendants(_ view: NSView, matching: (NSView) -> Bool) -> [NSView] {
                (matching(view) ? [view] : []) + view.subviews.flatMap { descendants($0, matching: matching) }
            }
            let root = try XCTUnwrap(currentWindow.contentView?.superview)
            let nativeTabs = descendants(root) { String(describing: type(of: $0)) == "NSTabButton" }
            let target = try XCTUnwrap(nativeTabs.first { tab in
                descendants(tab) { ($0 as? NSTextField)?.stringValue == "Pending" }.isEmpty == false
            })
            let button = try XCTUnwrap(descendants(target) { $0 is NSButton }.first as? NSButton)
            XCTAssertTrue(NSApp.sendAction(try XCTUnwrap(button.action), to: button.target, from: button))
        }
        settle()
        XCTAssertEqual(model.tabs.closeRequest?.id, target.id, "原生关闭必须询问指定的标签")
        XCTAssertEqual(model.tabs.tabs.count, 2, "确认前不得关闭")
        XCTAssertEqual(model.tabs.activeTabID, other.id, "关闭后台标签不能切走当前标签")
        XCTAssertNotNil(currentWindow.attachedSheet, "确认对话框必须出现在当前窗口")
        model.tabs.cancelClose(); settle()
        XCTAssertNil(model.tabs.closeRequest)
        XCTAssertEqual(model.tabs.tabs.count, 2, "取消后两个标签都应保留")
        targetWindow.performClose(nil); settle()
        XCTAssertEqual(model.tabs.closeRequest?.id, target.id)
        model.tabs.confirmClose(); settle()
        XCTAssertEqual(model.tabs.tabs.map(\.id), [other.id])
    }

    func testTmuxChooserIsPresentedOnlyOnCurrentNativeTab() throws {
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: dir.appendingPathComponent("config").path,
                                     knownHostsPath: dir.appendingPathComponent("known_hosts").path)
        let anchor = NativeTitlebarWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 500),
                              styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        anchor.isReleasedWhenClosed = false
        TestWindows.presentOffscreen(anchor)
        let coordinator = NativeTabWindows(model: model, anchor: anchor)
        defer {
            model.dismissTmux(); model.tabs.removeAll()
            for window in anchor.tabGroup?.windows ?? [anchor] { window.orderOut(nil) }
            withExtendedLifetime(coordinator) {}
        }
        let first = TerminalTab(kind: .localFiles); first.title = "First"
        let second = TerminalTab(kind: .localFiles); second.title = "Second"
        model.tabs.add(first); model.tabs.add(second)
        RunLoop.main.run(until: Date().addingTimeInterval(0.4))
        XCTAssertEqual(model.tabs.activeTabID, second.id, "新标签必须保持选中")
        let group = try XCTUnwrap(anchor.tabGroup)
        let current = try XCTUnwrap(group.windows.first { $0.title == "Second" })
        let other = try XCTUnwrap(group.windows.first { $0.title == "First" })
        model.showTmux(for: .localShell)
        RunLoop.main.run(until: Date().addingTimeInterval(0.6))
        XCTAssertNotNil(current.attachedSheet, "tmux 对话框应在当前标签窗口")
        XCTAssertNil(other.attachedSheet, "后台标签窗口不得重复弹出同一个对话框")
        model.tabs.activate(first)
        RunLoop.main.run(until: Date().addingTimeInterval(0.6))
        XCTAssertNotNil(model.tmuxChooser, "切换标签不能取消已有 tmux 操作")
        XCTAssertLessThanOrEqual(group.windows.filter { $0.attachedSheet != nil }.count, 1,
                                 "切换标签后不得重复显示对话框")
    }

    func testNativeAndSidebarOrderStayInSync() throws {
        let dir = sessionTestDirectory()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: dir.appendingPathComponent("config").path,
                                     knownHostsPath: dir.appendingPathComponent("known_hosts").path)
        let anchor = NativeTitlebarWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 500),
                              styleMask: [.titled, .closable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        anchor.isReleasedWhenClosed = false
        TestWindows.presentOffscreen(anchor)
        let coordinator = NativeTabWindows(model: model, anchor: anchor)
        defer {
            model.tabs.removeAll()
            for window in anchor.tabGroup?.windows ?? [anchor] { window.orderOut(nil) }
            withExtendedLifetime(coordinator) {}
        }
        func settle() { RunLoop.main.run(until: Date().addingTimeInterval(0.5)) }
        let a = TerminalTab(kind: .localFiles); a.title = "A"
        let b = TerminalTab(kind: .localFiles); b.title = "B"
        let c = TerminalTab(kind: .localFiles); c.title = "C"
        [a, b, c].forEach(model.tabs.add); settle()
        let group = try XCTUnwrap(anchor.tabGroup)
        XCTAssertEqual(group.windows.map(\.title), ["A", "B", "C"])
        let last = try XCTUnwrap(group.windows.first { $0.title == "C" })
        group.insertWindow(last, at: 0); settle()
        XCTAssertEqual(model.tabs.tabs.map(\.id), [c.id, a.id, b.id], "拖动原生标签必须更新侧边栏顺序")
        model.tabs.move(b, to: 0); settle()
        XCTAssertEqual(group.windows.map(\.title), ["B", "C", "A"], "模型重排必须更新原生标签顺序")
    }
}
