import AppKit
import GhostermAppModel
import GhostermTerminal
import XCTest
@testable import GhostermApp

@MainActor final class PaneActionsTests: XCTestCase {
    // 锁屏 / CI 下不能激活窗口。只替换 OS 焦点输入，内容仍由生产控制器创建。
    // 本组证明接线；真实窗口激活和全屏另验，不把此状态当作系统行为证据。
    private final class FocusWindow: NSWindow {
        var keyState = true
        override var isKeyWindow: Bool { keyState }
    }
    private func splitFixture() throws -> (TabWindowFixture, TerminalTab, TerminalPane, TerminalPane, WorkspaceHostView) {
        _ = NSApplication.shared
        let window = FocusWindow(contentRect: .zero, styleMask: [.titled, .closable, .resizable, .miniaturizable, .fullSizeContentView], backing: .buffered, defer: false)
        let f = try TabWindowFixture(window: window)
        let (tab, _) = try f.shell("Preview")
        let first = try XCTUnwrap(tab.activePane)
        let second = TerminalPane()
        let surface = try TerminalSurface(app: TerminalApp.shared())
        let session = try SessionController.localShell(surface: surface, executable: "/bin/sh", argv: ["-i"], cwd: f.directory.path, kind: .direct)
        second.attach(surface: surface, session: session); second.start()
        tab.split(axis: .horizontal, with: second)
        tab.activeWorkspacePaneID = first.id
        XCTAssertTrue(f.until { f.views(TerminalHostView.self).count == 2 })
        let workspace = try XCTUnwrap(f.views(WorkspaceHostView.self).first)
        f.settle()
        XCTAssertTrue(f.window.isKeyWindow)
        return (f, tab, first, second, workspace)
    }
    private func toolbar(_ f: TabWindowFixture, _ id: UUID) throws -> PaneActionsHost {
        try XCTUnwrap(f.views(PaneActionsHost.self).first { $0.buttons.first?.identifier?.rawValue.hasPrefix("pane-\(id)-") == true })
    }
    private func event(_ f: TabWindowFixture, _ type: NSEvent.EventType) throws -> NSEvent {
        try XCTUnwrap(NSEvent.enterExitEvent(with: type, location: .zero, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                            windowNumber: f.window.windowNumber, context: nil, eventNumber: 0, trackingNumber: 0, userData: nil))
    }
    private func hover(_ f: TabWindowFixture, _ id: UUID, enter: Bool) throws {
        let tracking = try XCTUnwrap(f.views(HoverTrackingView.self).first { $0.identifier?.rawValue == "pane-hover-\(id)" })
        if enter { tracking.mouseEntered(with: try event(f, .mouseEntered)) }
        else { tracking.mouseExited(with: try event(f, .mouseExited)) }
        f.settle()
    }
    private func button(_ f: TabWindowFixture, _ id: UUID, _ action: String) throws -> PaneActionButton {
        try XCTUnwrap(toolbar(f, id).buttons.first { $0.descriptor?.id == action })
    }

    func testHoverPreviewIsPassiveAndControlsReserveTheirWidth() throws {
        let (f, tab, first, second, workspace) = try splitFixture(); defer { f.close() }
        let a = try f.terminalHost(for: first), b = try f.terminalHost(for: second)
        let frameA = a.convert(a.bounds, to: nil), frameB = b.convert(b.bounds, to: nil)
        let responder = f.window.firstResponder
        let layout = tab.workspaceLayout
        let controls = try toolbar(f, second.id), frame = controls.frame
        XCTAssertFalse(controls.controlsVisible)
        let center = b.convert(NSPoint(x: b.bounds.midX, y: b.bounds.midY), to: workspace)
        let hit = workspace.hitTest(workspace.convert(center, to: workspace.superview))
        XCTAssertNotNil(hit)
        XCTAssertFalse(hit === workspace.splitPreview, "覆盖层不得成为终端内容的命中目标")
        try hover(f, second.id, enter: true)
        XCTAssertTrue(controls.controlsVisible, "悬停非当前窗格必须显示操作")
        XCTAssertEqual(controls.frame, frame, "按钮出现时标题区域不能跳动")
        let right = try button(f, second.id, "split-right")
        right.mouseEntered(with: try event(f, .mouseEntered))
        let horizontal = try XCTUnwrap(workspace.splitPreview.path?.boundingBoxOfPath, "悬停分屏按钮必须绘制预览")
        let card = try XCTUnwrap(workspace.hosts[second.id]).frame.insetBy(dx: 5, dy: 5)
        XCTAssertEqual(horizontal.minX, card.midX + 1, accuracy: 1, "向右预览必须只占目标窗格右半边")
        XCTAssertEqual(horizontal.maxY, card.maxY - 1, accuracy: 1)
        XCTAssertEqual(workspace.splitPreview.paneID, second.id)
        XCTAssertEqual(tab.activeWorkspacePaneID, first.id, "悬停预览不得激活目标窗格")
        XCTAssertTrue(f.window.firstResponder === responder, "预览不得抢走终端键盘")
        XCTAssertEqual(tab.workspaceLayout, layout, "预览不能改变布局或创建会话")
        XCTAssertEqual(tab.panes.count, 2)
        XCTAssertEqual(a.convert(a.bounds, to: nil), frameA)
        XCTAssertEqual(b.convert(b.bounds, to: nil), frameB, "预览不能触发终端尺寸变化")
        XCTAssertTrue(workspace.hitTest(workspace.convert(center, to: workspace.superview)) === hit, "预览不能拦截终端输入")
        right.mouseExited(with: try event(f, .mouseExited))
        XCTAssertNil(workspace.splitPreview.path, "离开按钮必须清除预览")
        let down = try button(f, second.id, "split-down")
        down.mouseEntered(with: try event(f, .mouseEntered))
        let vertical = try XCTUnwrap(workspace.splitPreview.path?.boundingBoxOfPath)
        XCTAssertEqual(vertical.minX, card.minX + 1, accuracy: 1)
        XCTAssertEqual(vertical.minY, (card.minY + PaneTitleBar.height + card.maxY) / 2 + 1, accuracy: 1)
        down.mouseExited(with: try event(f, .mouseExited))
        try hover(f, second.id, enter: false)
        XCTAssertFalse(controls.controlsVisible, "离开非当前窗格应隐藏按钮")
        XCTAssertNil(controls.hitTest(controls.frame.origin), "隐藏按钮不能接鼠标")
        if ProcessInfo.processInfo.environment["GHOSTERM_CAPTURE_TAB_CHROME"] == "1" {
            tab.title = "Workspace"
            _ = try f.shell("Shell")
            let files = TerminalTab(kind: .localFiles)
            let panel = FilesPanelModel(backend: LocalFileBackend(initialPath: f.directory.path))
            files.addFilePane(WorkspaceFilePane(remote: false, panel: panel)); panel.start()
            files.title = "Files"; f.model.tabs.add(files)
            f.model.tabs.activate(tab)
            var project = URL(fileURLWithPath: #filePath)
            for _ in 0..<5 { project.deleteLastPathComponent() }
            let quotedProject = "'" + project.path.replacingOccurrences(of: "'", with: "'\\''") + "'"
            for pane in [first, second] {
                pane.surface?.sendCommandLine("export PS1='ghosterm $ '; cd " + quotedProject + "; clear; pwd; uname -sm")
            }
            f.settle(1)
            for (name, appearance) in [("light", NSAppearance.Name.aqua), ("dark", .darkAqua)] {
                f.window.appearance = NSAppearance(named: appearance)
                for (layout, placement, compact) in [("sidebar", TabPlacement.sidebar, false), ("top", .top, false), ("compact", .sidebar, true)] {
                    f.model.preferences.tabPlacement = placement
                    f.model.preferences.compactSidebar = compact
                    f.settle()
                    try hover(f, second.id, enter: true)
                    try button(f, second.id, "split-right").mouseEntered(with: event(f, .mouseEntered))
                    for pane in [first, second] { pane.surface?.setVisible(true); pane.surface?.draw() }
                    f.settle()
                    try f.capture("review-\(layout)-\(name)")
                }
            }
        }
    }

    func testNativeClickSplitsItsOwnPaneAndFocusesTheNewTerminal() throws {
        let (f, tab, first, second, workspace) = try splitFixture(); defer { f.close() }
        try hover(f, second.id, enter: true)
        let button = try button(f, second.id, "split-down")
        button.mouseEntered(with: try event(f, .mouseEntered))
        let firstFrame = try XCTUnwrap(workspace.hosts[first.id]).frame
        try f.clickNative(button)
        XCTAssertEqual(tab.panes.count, 3, "真实按钮点击应执行分屏")
        let created = try XCTUnwrap(tab.activePane)
        XCTAssertFalse(created === first || created === second, "点击结束后焦点必须留在新窗格")
        XCTAssertNil(workspace.splitPreview.path)
        XCTAssertEqual(try XCTUnwrap(workspace.hosts[first.id]).frame, firstFrame, "操作 B 不应改变 A 的区域")
        XCTAssertTrue(f.until { (try? f.terminalHost(for: created)) === f.window.firstResponder }, "新窗格必须取得实际键盘焦点")
    }

    func testKeyboardFocusRevealsActionsWithoutChangingTheActivePane() throws {
        let (f, tab, first, second, workspace) = try splitFixture(); defer { f.close() }
        let controls = try toolbar(f, second.id), split = try button(f, second.id, "split-right")
        XCTAssertFalse(controls.controlsVisible)
        XCTAssertTrue(f.window.makeFirstResponder(split), "原生按钮必须支持键盘焦点")
        XCTAssertTrue(controls.controlsVisible, "键盘聚焦时必须显现操作")
        XCTAssertEqual(tab.activeWorkspacePaneID, first.id)
        XCTAssertEqual(workspace.splitPreview.paneID, second.id, "键盘聚焦分屏按钮须预览所属窗格")
        XCTAssertTrue(f.window.makeFirstResponder(try f.terminalHost(for: first)))
        XCTAssertNil(workspace.splitPreview.path, "键盘离开按钮后清预览")
        XCTAssertFalse(controls.controlsVisible)
    }

    func testPreviewClearsOnNavigationZoomDragEscapeAndTargetRemoval() throws {
        let (f, tab, first, second, workspace) = try splitFixture(); defer { f.close() }
        func show() throws {
            let split = try button(f, second.id, "split-right")
            split.mouseEntered(with: try event(f, .mouseEntered))
            XCTAssertNotNil(workspace.splitPreview.path)
        }
        try show()
        f.model.preferences.tabPlacement = .top; f.settle()
        XCTAssertNil(workspace.splitPreview.path, "切换导航必须取消预览")
        try show()
        f.model.preferences.compactSidebar.toggle(); f.settle()
        XCTAssertNil(workspace.splitPreview.path)
        try show()
        f.model.toggleZoom(tabID: tab.id, paneID: first.id); f.settle()
        XCTAssertNil(workspace.splitPreview.path, "缩放必须取消预览")
        f.model.toggleZoom(tabID: tab.id, paneID: first.id); f.settle()
        try show()
        let drag = WorkspaceDragSourceView()
        drag.beginDrag()
        XCTAssertNil(workspace.splitPreview.path, "拖动开始必须取消分屏预览")
        try button(f, second.id, "split-right").mouseEntered(with: event(f, .mouseEntered))
        XCTAssertNil(workspace.splitPreview.path, "拖动期间不能重新出现分屏预览")
        drag.finishDrag()
        try show()
        let escape = try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                                                   windowNumber: f.window.windowNumber, context: nil, characters: "\u{1b}",
                                                   charactersIgnoringModifiers: "\u{1b}", isARepeat: false, keyCode: 53))
        NSApp.sendEvent(escape)
        XCTAssertNil(workspace.splitPreview.path, "Escape 必须取消预览")
        try show()
        f.model.closePane(tabID: tab.id, paneID: second.id); f.settle()
        XCTAssertNil(workspace.splitPreview.path, "目标消失后不得残留预览")
    }

    func testPreviewTracksResizeAndStopsWhenWindowOrTabLosesFocus() throws {
        let (f, tab, first, second, workspace) = try splitFixture(); defer { f.close() }
        let split = try button(f, second.id, "split-right")
        func show() throws {
            split.mouseEntered(with: try event(f, .mouseEntered))
            XCTAssertNotNil(workspace.splitPreview.path)
        }
        try show()
        let before = try XCTUnwrap(workspace.splitPreview.path?.boundingBoxOfPath)
        f.window.setContentSize(NSSize(width: 1200, height: 700)); f.settle()
        let after = try XCTUnwrap(workspace.splitPreview.path?.boundingBoxOfPath)
        XCTAssertGreaterThan(after.width, before.width, "窗口变宽后预览必须重新计算")
        XCTAssertGreaterThan(after.height, before.height)
        let card = try XCTUnwrap(workspace.hosts[second.id]).frame.insetBy(dx: 5, dy: 5)
        XCTAssertEqual(after.minX, card.midX + 1, accuracy: 1)
        let window = try XCTUnwrap(f.window as? FocusWindow)
        window.keyState = false
        NotificationCenter.default.post(name: NSWindow.didResignKeyNotification, object: window)
        XCTAssertNil(workspace.splitPreview.path, "窗口失焦必须取消预览")
        split.mouseEntered(with: try event(f, .mouseEntered))
        XCTAssertNil(workspace.splitPreview.path, "后台窗口不能创建预览")
        window.keyState = true
        try show()
        _ = try f.shell("Other"); f.settle()
        XCTAssertNil(workspace.splitPreview.path, "切换标签必须取消旧预览")
        XCTAssertFalse(split.isEnabled, "后台标签的窗格按钮不可操作")
        XCTAssertFalse(split.acceptsFirstResponder, "后台按钮不能进入键盘焦点")
        f.model.tabs.activate(tab); f.settle()
        try show()
        tab.activeWorkspacePaneID = second.id; f.settle()
        XCTAssertNil(workspace.splitPreview.path, "当前窗格改变后取消预览")
        XCTAssertEqual(tab.panes.map(\.id), [first.id, second.id])
    }

    func testFilePaneUsesTheSamePreviewAndNativeActions() throws {
        let (f, tab, first, _, workspace) = try splitFixture(); defer { f.close() }
        let panel = FilesPanelModel(backend: LocalFileBackend(), initialPath: f.directory.path)
        let file = WorkspaceFilePane(remote: false, panel: panel)
        tab.addFilePane(file)
        tab.activeWorkspacePaneID = first.id; f.settle()
        let controls = try toolbar(f, file.id)
        XCTAssertFalse(controls.controlsVisible)
        try hover(f, file.id, enter: true)
        XCTAssertTrue(controls.controlsVisible, "文件窗格悬停时同样显示按钮")
        let split = try button(f, file.id, "split-down")
        split.mouseEntered(with: try event(f, .mouseEntered))
        XCTAssertEqual(workspace.splitPreview.paneID, file.id, "文件预览必须指向所属文件窗格")
        XCTAssertEqual(tab.activeWorkspacePaneID, first.id)
        let terminalFrames = Dictionary(uniqueKeysWithValues: tab.panes.map { ($0.id, workspace.hosts[$0.id]!.frame) })
        XCTAssertTrue(f.window.makeFirstResponder(split))
        f.settle()
        XCTAssertTrue(f.window.firstResponder === split, "被动聚焦按钮时不能抢回文件列表")
        try f.clickNative(split)
        let copy = try XCTUnwrap(tab.activeWorkspacePane?.files)
        XCTAssertNotEqual(copy.id, file.id, "文件按钮必须创建独立文件窗格")
        XCTAssertTrue(copy.panel?.backend === panel.backend)
        XCTAssertEqual(copy.panel?.currentPath, panel.currentPath)
        XCTAssertEqual(tab.filePanes.count, 2)
        XCTAssertNil(workspace.splitPreview.path)
        XCTAssertTrue(f.until {
            guard let host = workspace.hosts[copy.id], let responder = f.window.firstResponder as? NSView else { return false }
            return responder.isDescendant(of: host)
        }, "文件分屏后键盘必须交给新文件列表")
        if ProcessInfo.processInfo.environment["GHOSTERM_CAPTURE_TAB_CHROME"] == "1" {
            for pane in tab.panes { pane.surface?.setVisible(true); pane.surface?.draw() }
            f.settle()
            try f.capture("review-mixed-files")
        }
        for (id, frame) in terminalFrames {
            XCTAssertEqual(workspace.hosts[id]?.frame, frame, "文件分屏不改变终端区域")
        }
        let zoom = try button(f, file.id, "zoom")
        XCTAssertTrue(f.window.makeFirstResponder(zoom))
        tab.title = "File actions"; f.settle()
        XCTAssertTrue(f.window.firstResponder === zoom, "文件刷新不得抢走非当前窗格按钮的键盘")
        try hover(f, file.id, enter: true)
        try f.clickNative(zoom)
        XCTAssertEqual(tab.zoomedPaneID, file.id, "文件缩放按钮按所属 UUID 操作")
        try f.clickNative(button(f, file.id, "zoom"))
        XCTAssertNil(tab.zoomedPaneID)
        try f.clickNative(button(f, file.id, "close"))
        XCTAssertEqual(tab.filePanes.map(\.id), [copy.id], "关闭文件按钮不得删除其他窗格")
        XCTAssertEqual(tab.panes.count, 2)
    }

    func testPreviewIsActuallyPaintedOverTheWorkspace() throws {
        let (f, _, _, second, workspace) = try splitFixture(); defer { f.close() }
        try hover(f, second.id, enter: true)
        func pixels() throws -> NSBitmapImageRep {
            let bitmap = try XCTUnwrap(NSBitmapImageRep(bitmapDataPlanes: nil,
                pixelsWide: Int(workspace.bounds.width), pixelsHigh: Int(workspace.bounds.height),
                bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0))
            let context = try XCTUnwrap(NSGraphicsContext(bitmapImageRep: bitmap))
            try XCTUnwrap(workspace.layer).render(in: context.cgContext)
            return bitmap
        }
        let before = try pixels()
        try button(f, second.id, "split-right").mouseEntered(with: event(f, .mouseEntered))
        f.settle()
        let after = try pixels()
        var changed = 0
        for y in stride(from: 0, to: before.pixelsHigh, by: 4) {
            for x in stride(from: 0, to: before.pixelsWide, by: 4) {
                if before.colorAt(x: x, y: y) != after.colorAt(x: x, y: y) { changed += 1 }
            }
        }
        XCTAssertGreaterThan(changed, 1000, "预览层必须真正绘制到工作区，不能只有 path 数据")
    }

    func testShiftShortcutsReachTheSplitAndFileCommands() throws {
        let (f, tab, _, second, _) = try splitFixture()
        defer { f.close(); NSApp.mainMenu = nil }
        AppMenu.install(model: f.model)
        tab.activeWorkspacePaneID = second.id; f.settle()
        func press(_ character: String, code: UInt16) throws -> Bool {
            let event = try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [.command, .shift],
                timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: f.window.windowNumber, context: nil,
                characters: character, charactersIgnoringModifiers: character, isARepeat: false, keyCode: code))
            return NSApp.mainMenu?.performKeyEquivalent(with: event) == true
        }
        XCTAssertTrue(try press("D", code: 2), "⇧⌘D 必须匹配真实带 Shift 的按键事件")
        XCTAssertEqual(tab.panes.count, 3, "分屏快捷键必须执行模型命令")
        XCTAssertTrue(try press("E", code: 14), "⇧⌘E 必须匹配真实带 Shift 的按键事件")
        XCTAssertEqual(tab.filePanes.count, 1, "文件快捷键必须打开文件窗格")
    }

    func testTitleAndHiddenHandleMenusKeepThePaneTargetAndShortcuts() throws {
        let saved = UserDefaults.standard.object(forKey: PaneTitleBar.visibleKey)
        defer { UserDefaults.standard.set(saved, forKey: PaneTitleBar.visibleKey) }
        let (f, tab, first, second, workspace) = try splitFixture(); defer { f.close() }
        for visible in [true, false] {
            UserDefaults.standard.set(visible, forKey: PaneTitleBar.visibleKey); f.settle()
            tab.activeWorkspacePaneID = first.id; f.settle()
            if !visible { XCTAssertTrue(f.views(PaneActionsHost.self).isEmpty, "隐藏标题栏后不应残留窗格按钮") }
            let frame = try XCTUnwrap(workspace.hosts[second.id]).frame
            let point = workspace.convert(NSPoint(x: frame.midX, y: frame.minY + 5 + (visible ? PaneTitleBar.height / 2 : 3)), to: nil)
            let menu = try f.menu(at: point)
            let items = menu.items.filter { !$0.isSeparatorItem }
            XCTAssertEqual(items.map(\.title), ["向右分屏", "向下分屏", "缩放当前分屏", "关闭这个窗格"], "标题和隐藏把手必须提供同一组窗格动作")
            let down = try XCTUnwrap(items.first { $0.title == "向下分屏" })
            XCTAssertEqual(ShortcutCatalog.describe(keyEquivalent: down.keyEquivalent, modifiers: down.keyEquivalentModifierMask), "⇧⌘D", "窗格菜单必须显示真实分屏快捷键")
            let firstFrame = try XCTUnwrap(workspace.hosts[first.id]).frame
            menu.performActionForItem(at: menu.index(of: down)); f.settle()
            XCTAssertEqual(tab.panes.count, 3, "窗格菜单必须执行分屏")
            XCTAssertEqual(workspace.hosts[first.id]?.frame, firstFrame, "非当前窗格右键不得分屏当前窗格")
            let created = try XCTUnwrap(tab.activeWorkspacePaneID)
            XCTAssertFalse(created == first.id || created == second.id)
            f.model.closePane(tabID: tab.id, paneID: created); f.settle()
        }
    }

    func testRemoteFileActionRejectsHiddenOrRemovedTargets() throws {
        let f = try TabWindowFixture(); defer { f.close() }
        let target = TerminalTab(kind: .localShell, pane: TerminalPane())
        let remote = WorkspaceFilePane(remote: true, panel: nil)
        target.addFilePane(remote)
        let other = TerminalTab(kind: .localShell, pane: TerminalPane())
        f.model.tabs.add(target); f.model.tabs.add(other)
        let actions = PaneActionsView(model: f.model, tab: target, paneID: remote.id, revealed: true, preview: { _ in })
        let action = try XCTUnwrap(actions.items.first { $0.id == "local-files" })
        target.filesPanelVisible = false
        action.perform()
        XCTAssertEqual(f.model.tabs.activeTabID, other.id, "隐藏文件窗格的旧动作不得激活标签")
        XCTAssertNil(target.localFiles, "隐藏目标不得新建本地文件窗格")
        target.filesPanelVisible = true
        f.model.tabs.remove(target)
        action.perform()
        XCTAssertNil(other.localFiles, "已移除标签的动作不得回落到当前标签")
        f.model.tabs.add(target); f.model.tabs.activate(other)
        action.perform()
        XCTAssertEqual(f.model.tabs.activeTabID, target.id)
        XCTAssertNotNil(target.localFiles, "有效远端文件动作必须打开所属标签的本地文件窗格")
    }
}
