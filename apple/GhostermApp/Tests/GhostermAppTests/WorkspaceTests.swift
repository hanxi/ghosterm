import AppKit
import GhostermCore
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor final class WorkspaceTests: XCTestCase {
    override class func setUp() { _ = NSApplication.shared; NSApplication.shared.setActivationPolicy(.accessory) }
    func testDefaultAAndDockingPreserveAllPaneIdentities() throws {
        let tab = TerminalTab(kind: .ssh(.init(pattern: "a", user: "u", host: "a", port: 22)), pane: TerminalPane())
        let local = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend()))
        let remote = WorkspaceFilePane(remote: true, panel: FilesPanelModel(backend: WorkspaceBackend()))
        tab.addFilePane(remote); tab.addFilePane(local)
        let tree = try XCTUnwrap(tab.workspaceLayout)
        let g = tree.geometry(in: CGRect(x: 0, y: 0, width: 900, height: 600), minimum: { _ in CGSize(width: 100, height: 80) }).panes
        let t = try XCTUnwrap(g[tab.panes[0].id]), l = try XCTUnwrap(g[local.id]), r = try XCTUnwrap(g[remote.id])
        XCTAssertEqual(t.width, 600, accuracy: 1)
        XCTAssertEqual(l.minY, 0); XCTAssertEqual(r.minY, 300)
        for edge in [WorkspaceLayout.Edge.left, .right, .top, .bottom, .center] {
            let next = try XCTUnwrap(tree.docking(source: local.id, onto: tab.panes[0].id, edge: edge))
            XCTAssertEqual(Set(next.paneIDs), Set(tree.paneIDs))
            XCTAssertEqual(next.paneIDs.count, 3)
        }
        XCTAssertNil(tree.docking(source: local.id, onto: local.id, edge: .left))
        XCTAssertFalse(tab.dock(.init(tabID: UUID(), paneID: local.id), onto: remote.id, edge: .center,
                                available: CGSize(width: 1000, height: 700), minimum: { _ in CGSize(width: 260, height: 210) }))
        XCTAssertEqual(tab.workspaceLayout, tree)
        XCTAssertFalse(tab.dock(.init(tabID: tab.id, paneID: local.id), onto: remote.id, edge: .left,
                                available: CGSize(width: 400, height: 300), minimum: { _ in CGSize(width: 260, height: 210) }))
    }
    func testOpeningFilesDoesNotResetExistingTerminalSplits() {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        tab.split(axis: .vertical, with: TerminalPane())
        let before = tab.layout?.viewID
        let local = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend()))
        tab.addFilePane(local)
        XCTAssertEqual(tab.layout?.viewID, before, "打开文件窗格不得把既有上下分屏改为左右分屏")
        XCTAssertEqual(tab.workspacePanes.count, 3)
    }

    func testHideRestoreAndDuplicateFilesAreIndependent() throws {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        let local = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend(), initialPath: workspaceTestTmp))
        tab.addFilePane(local)
        let before = tab.workspaceLayout
        tab.filesPanelVisible = false
        XCTAssertEqual(tab.visibleWorkspaceLayout?.paneIDs, [tab.panes[0].id])
        tab.filesPanelVisible = true
        XCTAssertEqual(before, tab.workspaceLayout)
        tab.duplicateFilePane(local, axis: .horizontal)
        let copy = try XCTUnwrap(tab.filePanes.last)
        XCTAssertFalse(copy.panel === local.panel)
        XCTAssertTrue(copy.panel?.backend === local.panel?.backend)
        XCTAssertEqual(copy.endpointID, local.endpointID)
        XCTAssertEqual(copy.panel?.currentPath, local.panel?.currentPath, "异步列目录之前副本就必须指向源目录")
        copy.panel?.selection = ["copy"]
        XCTAssertTrue(local.panel?.selection.isEmpty == true)
        tab.toggleWorkspaceZoom(copy.id)
        XCTAssertEqual(tab.zoomedPaneID, copy.id)
        tab.removeFilePane(copy.id)
        XCTAssertNil(tab.zoomedPaneID)
        XCTAssertEqual(tab.workspacePanes.count, 2, "关闭文件副本不得移除其他窗格")
        XCTAssertTrue(tab.filePanes.contains { $0 === local })
        func assertNoEmptySplits(_ node: WorkspaceLayout) {
            if case .split(_, _, _, let children) = node {
                XCTAssertGreaterThanOrEqual(children.count, 2, "关闭后布局必须塌陷单子节点")
                children.forEach(assertNoEmptySplits)
            }
        }
        if let layout = tab.workspaceLayout { assertNoEmptySplits(layout) }
    }
    func testNativeHostIdentityAndSiblingGeometrySurviveMovesAndZoom() throws {
        let dir = workspaceTestTmp + "workspace-native-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/ssh_config", knownHostsPath: dir + "/known_hosts")
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        defer { model.tabs.removeAll() }
        let terminal = try XCTUnwrap(tab.panes.first)
        let realSurface = try TerminalSurface(app: TerminalApp.shared())
        let session = try SessionController.localShell(surface: realSurface, executable: "/bin/sh",
                                                       argv: ["-i"], cwd: dir, kind: .direct)
        terminal.attach(surface: realSurface, session: session)
        terminal.start()
        let local = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend(initialPath: dir)))
        tab.addFilePane(local)
        let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 900, height: 600))
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        window.contentView = view
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        let options = PaneChromeOptions(gap: 10)
        func update() {
            view.update(model: model, tab: tab, isActive: true, opts: options)
            view.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        }
        update()
        let original = try XCTUnwrap(view.hosts[terminal.id])
        let host = try XCTUnwrap(findTerminal(original))
        let surface = terminal.surface
        func waitFor(_ marker: String) -> Bool {
            let deadline = Date().addingTimeInterval(5)
            while Date() < deadline {
                if realSurface.readText()?.contains(marker) == true { return true }
                RunLoop.main.run(until: Date().addingTimeInterval(0.02))
            }
            return false
        }
        XCTAssertTrue(waitFor("$ "), "隔离 shell 必须进入交互提示符：\(session.state)，网格：\(realSurface.readText() ?? "无")")
        realSurface.sendCommandLine("printf 'GT_%s_%s\\n' WORKSPACE $$")
        let deadline = Date().addingTimeInterval(5)
        var shellID: String?
        while shellID == nil && Date() < deadline {
            if let text = realSurface.readText(), let range = text.range(of: "GT_WORKSPACE_[0-9]+", options: .regularExpression) {
                shellID = String(text[range]).replacingOccurrences(of: "GT_WORKSPACE_", with: "")
            }
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        let pid = try XCTUnwrap(shellID, "真实 shell 必须输出其 PID")
        for (index, edge) in [WorkspaceLayout.Edge.left, .bottom, .right, .center].enumerated() {
            XCTAssertTrue(tab.dock(.init(tabID: tab.id, paneID: local.id), onto: terminal.id, edge: edge,
                                   available: CGSize(width: 900, height: 600), minimum: view.minimum))
            update()
            XCTAssertIdentical(view.hosts[terminal.id], original)
            XCTAssertIdentical(findTerminal(original), host)
            XCTAssertIdentical(terminal.surface, surface)
            XCTAssertGreaterThan(host.bounds.width, 0)
            XCTAssertGreaterThan(host.bounds.height, 0)
            XCTAssertTrue(terminal.isRunning)
            realSurface.sendCommandLine("printf 'GT_%s_%s_%s\\n' MOVE \(index) $$")
            XCTAssertTrue(waitFor("GT_MOVE_\(index)_\(pid)"), "移动后字节仍进同一 PID 的终端网格")
        }
        host.setMarkedText("中文", selectedRange: NSRange(location: 2, length: 0),
                           replacementRange: NSRange(location: NSNotFound, length: 0))
        XCTAssertTrue(host.hasMarkedText())
        XCTAssertTrue(tab.dock(.init(tabID: tab.id, paneID: local.id), onto: terminal.id, edge: .center,
                               available: CGSize(width: 900, height: 600), minimum: view.minimum))
        update()
        XCTAssertTrue(host.hasMarkedText(), "布局移动不得丢失输入法组合态")
        host.unmarkText()
        let siblingFrame = original.frame
        tab.toggleWorkspaceZoom(local.id); update()
        XCTAssertEqual(original.frame, siblingFrame)
        tab.toggleWorkspaceZoom(local.id); update()
        XCTAssertEqual(original.frame, siblingFrame)
    }
    func testEndpointRoutingRejectsStaleSourceAndDoesNotUseTargetBackend() throws {
        let tab = TerminalTab(kind: .localFiles)
        let a = WorkspaceBackend(), b = WorkspaceBackend()
        let local = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend()))
        let remoteA = WorkspaceFilePane(remote: true, panel: FilesPanelModel(backend: a))
        let remoteB = WorkspaceFilePane(remote: true, panel: FilesPanelModel(backend: b))
        [local, remoteA, remoteB].forEach { tab.addFilePane($0) }
        let manager = TransferManager(pump: CoreEventPump())
        let drag = WorkspaceFileDrag(paneID: remoteA.id, endpointID: remoteA.endpointID,
                                     entries: [.init(path: "/a", name: "a", isDirectory: false), .init(path: "/b", name: "b", isDirectory: false)])
        XCTAssertTrue(WorkspaceFileRouter.perform(drag, onto: local, directory: workspaceTestTmp, in: tab, manager: manager))
        XCTAssertEqual(a.downloads, ["/a", "/b"])
        XCTAssertTrue(b.downloads.isEmpty)
        XCTAssertFalse(WorkspaceFileRouter.perform(drag, onto: remoteB, directory: "/", in: tab, manager: manager))
        tab.removeFilePane(remoteA.id)
        XCTAssertFalse(WorkspaceFileRouter.perform(drag, onto: local, directory: workspaceTestTmp, in: tab, manager: manager))
        XCTAssertEqual(manager.jobs.count, 2)
    }
    func testRetryNeverBorrowsAnotherEndpoint() {
        let manager = TransferManager(pump: CoreEventPump()), a = WorkspaceBackend(), b = WorkspaceBackend()
        manager.enqueueDownload(a, remote: "/a", local: workspaceTestTmp + "retry-a", name: "a", isDirectory: false)
        let id = manager.jobs[0].coreJob
        manager.handle(CoreEvent(kind: .jobDone, sessionID: 0, jobID: id, code: 1, message: "fail"))
        manager.enqueueDownload(b, remote: "/b", local: workspaceTestTmp + "retry-b", name: "b", isDirectory: false)
        manager.retry(manager.jobs[0])
        XCTAssertEqual(a.downloads, ["/a", "/a"])
        XCTAssertEqual(b.downloads, ["/b"])
    }
    func testLocalFilesEntryDoesNotStartShellAndExitKeepsFiles() throws {
        let dir = workspaceTestTmp + "workspace-command-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll() }
        model.openLocalFiles()
        XCTAssertTrue(model.tabs.activeTab?.panes.isEmpty == true)
        XCTAssertNotNil(model.tabs.activeTab?.localFiles)
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab); model.openLocalFiles()
        model.handlePaneExit(tab.panes[0])
        XCTAssertTrue(model.tabs.tabs.contains { $0 === tab })
        XCTAssertTrue(tab.panes.isEmpty)
        XCTAssertNotNil(tab.localFiles)
    }
    func testUnifiedCardsGeometryAndAppearance() throws {
        let dir = workspaceTestTmp + "workspace-cards-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll(); NSApp.appearance = nil }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        let local = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend(initialPath: dir)))
        let remote = WorkspaceFilePane(remote: true, panel: FilesPanelModel(backend: WorkspaceBackend()))
        tab.addFilePane(remote); tab.addFilePane(local)
        for dark in [false, true] {
            NSApp.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
            let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 1000, height: 660))
            let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .resizable], backing: .buffered, defer: false)
            window.contentView = view
            TestWindows.presentOffscreen(window)
            defer { window.orderOut(nil) }
            view.update(model: model, tab: tab, isActive: true, opts: PaneChromeOptions(gap: 10))
            view.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.5))
            let a = try XCTUnwrap(view.hosts[local.id]), b = try XCTUnwrap(view.hosts[remote.id])
            XCTAssertEqual(a.frame.minX, b.frame.minX, accuracy: 1)
            XCTAssertEqual(a.frame.maxY, b.frame.minY, accuracy: 1)
            XCTAssertEqual(a.frame.width, b.frame.width, accuracy: 1)
            let rep = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
            view.cacheDisplay(in: view.bounds, to: rep)
            let image = try XCTUnwrap(rep.representation(using: .png, properties: [:]))
            try image.write(to: URL(fileURLWithPath: workspaceTestTmp + "workspace-preview-\(dark ? "dark" : "light").png"))
        }
    }

    func testWindowPaneDragUsesOwnTypeAndMoveMask() {
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        let provider = workspaceDragProvider(tab: tab, paneID: tab.panes[0].id)
        XCTAssertTrue(provider.hasItemConformingToTypeIdentifier(workspacePaneType))
        XCTAssertFalse(provider.hasItemConformingToTypeIdentifier(workspaceLocalFileType))
        XCTAssertFalse(provider.hasItemConformingToTypeIdentifier(workspaceRemoteFileType))
        XCTAssertEqual(WorkspaceDragSourceView.operationMask(for: .withinApplication), .move)
        XCTAssertEqual(WorkspaceDragSourceView.operationMask(for: .outsideApplication), [])
        let expectedTab = tab.id, expectedPane = tab.panes[0].id
        let decoded = expectation(description: "原生拖动源可解码专用载荷")
        provider.loadDataRepresentation(forTypeIdentifier: workspacePaneType) { data, error in
            XCTAssertNil(error)
            guard let data, let drag = try? JSONDecoder().decode(WorkspacePaneDrag.self, from: data) else {
                XCTFail("NSItemProvider 必须真正提供可加载的窗格载荷")
                decoded.fulfill(); return
            }
            XCTAssertEqual(drag.tabID, expectedTab)
            XCTAssertEqual(drag.paneID, expectedPane)
            decoded.fulfill()
        }
        wait(for: [decoded], timeout: 5)
    }

    /// 焦点必须能双向走：文件窗格当前时键盘在它的表格上，切回终端时键盘
    /// 回到终端宿主；另一个标签页（后台、仍挂载）的更新不得抢走键盘。
    func testFileAndTerminalFocusRoundTripAndBackgroundDoesNotSteal() throws {
        let dir = workspaceTestTmp + "workspace-focus-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll() }
        let opts = PaneChromeOptions(gap: 10)
        func pump(_ view: WorkspaceHostView, _ tab: TerminalTab, active: Bool) {
            view.update(model: model, tab: tab, isActive: active, opts: opts)
            view.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        }

        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        let terminal = try XCTUnwrap(tab.panes.first)
        terminal.attachForTesting(surface: try TerminalSurface(app: TerminalApp.shared()))
        let file = WorkspaceFilePane(remote: false,
                                     panel: FilesPanelModel(backend: LocalFileBackend(initialPath: dir)))
        tab.addFilePane(file)

        let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 900, height: 600))
        let window = WorkspaceTestWindow(contentRect: view.frame, styleMask: [.titled, .resizable],
                                         backing: .buffered, defer: false)
        window.stubbedKey = true
        let container = NSView(frame: view.frame)
        container.addSubview(view)
        window.contentView = container
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }

        tab.activeWorkspacePaneID = terminal.id
        pump(view, tab, active: true)
        let terminalHost = try XCTUnwrap(findTerminal(try XCTUnwrap(view.hosts[terminal.id])))
        XCTAssertTrue(window.firstResponder === terminalHost,
                      "终端是当前窗格时键盘必须在终端宿主上")

        tab.activeWorkspacePaneID = file.id
        pump(view, tab, active: true)
        let fileHost = try XCTUnwrap(view.hosts[file.id])
        let fileResponder = window.firstResponder as? NSView
        XCTAssertTrue(fileResponder?.isDescendant(of: fileHost) == true,
                      "文件窗格成为当前后键盘必须交给它的列表")
        XCTAssertTrue(fileResponder is NSTableView || fileResponder is NSOutlineView,
                      "落点必须是文件列表（表格/大纲），不是窗口或终端")

        // 后台标签页仍挂在视图树上；它的更新不能把键盘抢过去。
        let background = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(background)
        let backgroundTerminal = try XCTUnwrap(background.panes.first)
        backgroundTerminal.attachForTesting(surface: try TerminalSurface(app: TerminalApp.shared()))
        let backgroundView = WorkspaceHostView(frame: view.frame)
        container.addSubview(backgroundView)
        pump(backgroundView, background, active: false)
        XCTAssertTrue(window.firstResponder === fileResponder,
                      "后台标签页不得抢走当前窗格的键盘")

        tab.activeWorkspacePaneID = terminal.id
        pump(view, tab, active: true)
        XCTAssertTrue(window.firstResponder === terminalHost,
                      "文件→终端回切后键盘必须回到终端，否则敲字进不了终端")
    }

    /// 纯文件标签页首次上屏时 `firstResponder` 是窗口自身（不是任何 NSView）。
    /// 这条路径也必须把键盘交给列表，否则方向键/回车一开始不起作用。
    func testPureFileTabClaimsKeyboardWhenWindowIsFirstResponder() throws {
        let dir = workspaceTestTmp + "workspace-focus-file-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localFiles)
        model.tabs.add(tab)
        let panel = FilesPanelModel(backend: LocalFileBackend(initialPath: dir))
        panel.start()
        let file = WorkspaceFilePane(remote: false, panel: panel)
        tab.addFilePane(file)

        let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 900, height: 600))
        let window = WorkspaceTestWindow(contentRect: view.frame, styleMask: [.titled, .resizable],
                                         backing: .buffered, defer: false)
        window.stubbedKey = true
        window.contentView = view
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        XCTAssertTrue(window.makeFirstResponder(nil))
        XCTAssertTrue(window.firstResponder === window,
                      "前置条件：窗口自身是 firstResponder（不是 NSView）")

        view.update(model: model, tab: tab, isActive: true, opts: PaneChromeOptions(gap: 10))
        view.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))

        let fileHost = try XCTUnwrap(view.hosts[file.id])
        let responder = window.firstResponder as? NSView
        XCTAssertTrue(responder?.isDescendant(of: fileHost) == true,
                      "窗口自身是 firstResponder 时也必须把键盘交给文件列表")
        XCTAssertTrue(responder is NSTableView || responder is NSOutlineView,
                      "落点必须是文件列表（表格/大纲），不是窗口或宿主")
    }

    /// 四边/中央的判定与预览矩形是纯函数：目标窗口内 25% 边缘停靠，
    /// 其余中央交换；自身与窗口外必须拒绝。
    func testDockingGeometryResolvesEdgesAndCenter() {
        let source = UUID(), target = UUID()
        let frames = [(source, CGRect(x: 0, y: 0, width: 300, height: 200)),
                      (target, CGRect(x: 300, y: 0, width: 600, height: 200))]
        func resolve(_ x: CGFloat, _ y: CGFloat)
            -> (target: UUID, edge: WorkspaceLayout.Edge, highlight: CGRect)? {
            WorkspaceDockingGeometry.resolve(point: CGPoint(x: x, y: y), frames: frames,
                                             excluding: source)
        }
        XCTAssertEqual(resolve(310, 100)?.edge, .left)
        XCTAssertEqual(resolve(890, 100)?.edge, .right)
        XCTAssertEqual(resolve(600, 10)?.edge, .top)
        XCTAssertEqual(resolve(600, 190)?.edge, .bottom)
        XCTAssertEqual(resolve(600, 100)?.edge, .center)
        XCTAssertEqual(resolve(450, 100)?.edge, .center,
                       "恰在 25% 边界上属于中央，不算边缘停靠")
        XCTAssertNil(resolve(1000, 100), "窗口外不得给出落点")
        XCTAssertNil(resolve(100, 100), "自身矩形不得成为落点")

        let left = resolve(310, 100)
        XCTAssertEqual(left?.target, target)
        XCTAssertEqual(left?.highlight, CGRect(x: 306, y: 6, width: 288, height: 188))
        XCTAssertEqual(resolve(600, 100)?.highlight,
                       CGRect(x: 306, y: 6, width: 588, height: 188),
                       "中央预览盖住整个目标窗格")
    }

    /// 拖动会话结束（含 Esc 取消）必须清掉落点预览；取消路径由
    /// `NSDraggingSource` 的结束回调统一通知宿主。
    func testCancelledPaneDragClearsPreview() throws {
        let dir = workspaceTestTmp + "workspace-drag-cancel-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        let file = WorkspaceFilePane(remote: false,
                                     panel: FilesPanelModel(backend: LocalFileBackend(initialPath: dir)))
        tab.addFilePane(file)
        let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 900, height: 600))
        let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .resizable],
                              backing: .buffered, defer: false)
        window.contentView = view
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        view.update(model: model, tab: tab, isActive: true, opts: PaneChromeOptions(gap: 10))
        view.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        let terminal = try XCTUnwrap(tab.panes.first)
        let target = try XCTUnwrap(view.hosts[terminal.id]?.frame)
        let center = CGPoint(x: target.midX, y: target.midY)
        let drag = WorkspacePaneDrag(tabID: tab.id, paneID: file.id)

        XCTAssertTrue(view.updateDropPreview(drag, at: center), "文件窗格拖到终端中央应有落点")
        XCTAssertTrue(view.hasDropPreview)
        NotificationCenter.default.post(name: .workspacePaneDragEnded, object: nil)
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        XCTAssertFalse(view.hasDropPreview, "Esc / 会话取消后高亮必须消失")

        XCTAssertFalse(view.updateDropPreview(.init(tabID: UUID(), paneID: file.id), at: center),
                       "跨标签页载荷不产生预览")
        XCTAssertFalse(view.hasDropPreview)

        // 结束回调本身要发通知（AppKit 在 Esc 时也走这里）。
        let ended = expectation(forNotification: .workspacePaneDragEnded, object: nil)
        WorkspaceDragSourceView(frame: .zero).finishDrag()
        wait(for: [ended], timeout: 1)
    }

    /// 文件窗格最小高度不能写死：工具条会随宽度折行、搜索栏会显隐。
    /// 断言的是同一条路径——宿主 `minimum()` 读到的实测头部高度。
    func testFilePaneMinimumHeightTracksMeasuredChrome() throws {
        let dir = workspaceTestTmp + "workspace-min-height-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        for i in 0..<20 {
            try Data("x".utf8).write(to: URL(fileURLWithPath: dir + "/file-\(i).txt"))
        }
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll() }
        let gap = CGFloat(PaneGap.standard)
        var opts = PaneChromeOptions(gap: PaneGap.standard)
        func makeHost(_ tab: TerminalTab) -> (WorkspaceHostView, NSWindow) {
            let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 900, height: 620))
            let window = NSWindow(contentRect: view.frame, styleMask: [.titled, .resizable],
                                  backing: .buffered, defer: false)
            window.contentView = view
            TestWindows.presentOffscreen(window)
            return (view, window)
        }
        func pump(_ view: WorkspaceHostView, _ tab: TerminalTab) {
            view.update(model: model, tab: tab, isActive: true, opts: opts)
            view.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        }
        func measured(_ view: WorkspaceHostView, _ tab: TerminalTab,
                      _ file: WorkspaceFilePane, after old: CGFloat? = nil) -> CGFloat {
            let deadline = Date().addingTimeInterval(5)
            while Date() < deadline {
                pump(view, tab)
                if let h = file.chromeHeight, h > 0,
                   old == nil || abs(h - old!) > 0.5 { return h }
            }
            XCTFail("文件窗格头部高度必须被实测上报（旧值 \(String(describing: old))）")
            return 0
        }
        func expected(_ chrome: CGFloat, title: CGFloat = PaneTitleBar.height) -> CGFloat {
            title + chrome + WorkspaceHostView.fileContentMinimum + gap
        }

        // 宽例：纯文件标签页占满 900pt，工具条一行。
        let wideTab = TerminalTab(kind: .localFiles)
        model.tabs.add(wideTab)
        let widePanel = FilesPanelModel(backend: LocalFileBackend(initialPath: dir))
        widePanel.start()
        let wideFile = WorkspaceFilePane(remote: false, panel: widePanel)
        wideTab.addFilePane(wideFile)
        let (wideView, wideWindow) = makeHost(wideTab)
        defer { wideWindow.orderOut(nil) }
        let wideChrome = measured(wideView, wideTab, wideFile)
        XCTAssertEqual(wideView.minimum(wideFile.id).height, expected(wideChrome), accuracy: 0.5,
                       "最小高度必须由实测头部 + 64pt 内容区组成")

        // 窄例：默认 A 布局（终端 : 文件 = 2:1），文件窗格约 300pt，工具条折行。
        let narrowTab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(narrowTab)
        let narrowPanel = FilesPanelModel(backend: LocalFileBackend(initialPath: dir))
        narrowPanel.start()
        let narrowFile = WorkspaceFilePane(remote: false, panel: narrowPanel)
        narrowTab.addFilePane(narrowFile)
        let (narrowView, narrowWindow) = makeHost(narrowTab)
        defer { narrowWindow.orderOut(nil) }
        let narrowChrome = measured(narrowView, narrowTab, narrowFile, after: wideChrome)
        XCTAssertGreaterThan(narrowChrome, wideChrome + 1,
                             "窄面板工具条折行必须抬高头部，否则内容区被挤掉")
        XCTAssertEqual(narrowView.minimum(narrowFile.id).height, expected(narrowChrome), accuracy: 0.5)

        narrowPanel.search("no-such-file-\(UUID())")
        let searchChrome = measured(narrowView, narrowTab, narrowFile, after: narrowChrome)
        XCTAssertGreaterThan(searchChrome, narrowChrome + 1,
                             "搜索栏出现必须计入最小高度")
        XCTAssertEqual(narrowView.minimum(narrowFile.id).height, expected(searchChrome), accuracy: 0.5)

        narrowPanel.cancelSearch()
        let cancelledChrome = measured(narrowView, narrowTab, narrowFile, after: searchChrome)
        XCTAssertEqual(cancelledChrome, narrowChrome, accuracy: 0.5,
                       "收起搜索栏后最小高度要回落")

        opts = PaneChromeOptions(gap: PaneGap.standard, showsTitleBar: false)
        pump(narrowView, narrowTab)
        XCTAssertEqual(narrowView.minimum(narrowFile.id).height,
                       expected(cancelledChrome, title: 6), accuracy: 0.5,
                       "关掉标题栏只省标题条，头部实测值不变")
    }

    // MARK: - 标题栏带劫持（「拖窗格把整个窗口拖走了」回归）

    /// 侧栏展开时右列没有顶栏（TabStripVisibility），最上排窗格顶到窗口
    /// y=0，正好落进系统标题栏带（顶部约 28pt；fullSizeContentView 下这条
    /// 带仍在，且保留「这里拖动=移窗」语义）。AppKit 的判据是**命中视图**的
    /// `mouseDownCanMoveWindow`（非不透明视图默认 YES）：YES → mouseDown
    /// 被劫持成窗口拖动，视图收不到 mouseDragged——窗格拖动把手恰恰靠
    /// mouseDragged（4pt 阈值）起拖，于是「拖窗格」变成「拖窗口」（用户
    /// 实测）。不变量：**工作区子树里任何视图都不参与移窗**；移窗只归
    /// 顶带（WindowDragArea 的显式 performDrag）。
    /// 取 gap=0 + 标题栏开：标题栏 26pt 完全在带内，最恶劣几何。
    func testWorkspaceSubtreeRefusesWindowDragInTitleBarStrip() throws {
        let dir = workspaceTestTmp + "workspace-strip-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localShell, pane: TerminalPane())
        model.tabs.add(tab)
        let terminal = try XCTUnwrap(tab.panes.first)
        terminal.attachForTesting(surface: try TerminalSurface(app: TerminalApp.shared()))
        let file = WorkspaceFilePane(remote: false,
                                     panel: FilesPanelModel(backend: LocalFileBackend(initialPath: dir)))
        tab.addFilePane(file)

        let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 900, height: 600))
        // fullSizeContentView：与生产一致，内容从窗口 y=0 开始（工作区上沿
        // 即系统标题栏带下沿）。
        let window = NSWindow(contentRect: view.frame,
                              styleMask: [.titled, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.contentView = view
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        view.update(model: model, tab: tab, isActive: true, opts: PaneChromeOptions(gap: 0))
        view.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date().addingTimeInterval(0.2))

        XCTAssertFalse(view.hosts.isEmpty, "前置条件：窗格宿主已挂载")
        XCTAssertFalse(view.dividerViews.isEmpty, "前置条件：两格之间要有分隔条")

        // 操作性不变量：**标题栏带内任何一点的命中视图**都不参与移窗。
        // AppKit 只问命中的那个视图（最深者），所以逐点采样比全树遍历更
        // 贴近事实——SwiftUI/AppKit 内部视图返回什么无关紧要，只要它们
        // 不是带内的命中落点。带高按系统标题栏口径取 28pt，采三档深度。
        var offenders: [String] = []
        for x in stride(from: 4.0, to: view.bounds.width - 4, by: 13.0) {
            for y in [3.0, 13.0, 25.0] {
                guard let hit = view.hitTest(NSPoint(x: x, y: y)) else { continue }
                if hit.mouseDownCanMoveWindow {
                    offenders.append("(\(Int(x)),\(Int(y)))→\(type(of: hit))")
                }
            }
        }
        XCTAssertTrue(offenders.isEmpty,
                      "系统标题栏带内这些落点仍会「拖动=移窗」（窗格拖动会被劫持成拖窗口）：\(offenders)")
    }

    /// 文件窗格「点击没高亮」回归探针。合成点击驱动不了 SwiftUI List 的行
    /// 选中（哨兵 testSyntheticClicksCannotDriveSwiftUIListSelection 三种派发
    /// 配方全部实证无效——点击路由需要真实事件流），所以本探针在表层驱动
    /// 选中，断言我们负责的接线：tag↔绑定同步、键盘归属（活动色高亮的
    /// 前提）、双击打开通道（doubleAction，替代被移除的行级 tap 手势——
    /// 那是吞掉单击选中的文档化元凶）、以及 clickedRow→条目映射。
    func testFilePaneSelectionSyncKeyboardAndDoubleClickChannel() throws {
        let dir = workspaceTestTmp + "workspace-click-\(UUID())"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(atPath: dir) }
        for name in ["alpha.txt", "beta.txt"] {
            try Data("x".utf8).write(to: URL(fileURLWithPath: dir + "/" + name))
        }
        let model = isolatedAppModel(configPath: dir + "/config", knownHostsPath: dir + "/known_hosts")
        defer { model.tabs.removeAll() }
        let tab = TerminalTab(kind: .localFiles)
        model.tabs.add(tab)
        let panel = FilesPanelModel(backend: LocalFileBackend(initialPath: dir))
        panel.start()
        let file = WorkspaceFilePane(remote: false, panel: panel)
        tab.addFilePane(file)

        let view = WorkspaceHostView(frame: CGRect(x: 0, y: 0, width: 900, height: 600))
        let window = WorkspaceTestWindow(contentRect: view.frame, styleMask: [.titled, .resizable],
                                         backing: .buffered, defer: false)
        window.stubbedKey = true
        window.contentView = view
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        func pump() {
            view.update(model: model, tab: tab, isActive: true, opts: PaneChromeOptions(gap: 10))
            view.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.2))
        }
        pump()
        // 目录列表是异步加载的：等到有行为止（带截止，不假装通过）。
        let deadline = Date().addingTimeInterval(5)
        while panel.visibleEntries.isEmpty && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.05))
            pump()
        }
        XCTAssertEqual(panel.visibleEntries.count, 2, "前置条件：两个文件都列出来了")

        let fileHost = try XCTUnwrap(view.hosts[file.id])
        func table(in v: NSView) -> NSTableView? {
            if let t = v as? NSTableView { return t }
            return v.subviews.lazy.compactMap(table(in:)).first
        }
        let tableView = try XCTUnwrap(table(in: fileHost), "文件窗格里必须有 NSTableView")
        XCTAssertEqual(tableView.numberOfRows, 2)

        // 第 0 行中心 → 窗口坐标：命中链必须落在表内（排除遮挡/几何错乱）。
        let rowRect = tableView.rect(ofRow: 0)
        let point = tableView.convert(NSPoint(x: rowRect.midX, y: rowRect.midY), to: nil)
        let hit = window.contentView?.hitTest(point)
        XCTAssertTrue(hit?.isDescendant(of: tableView) == true,
                      "行上的命中点必须落在文件列表内，实际命中 \(String(describing: hit.map { type(of: $0) }))")

        // 表层驱动选中（合成点击驱动不了，见哨兵测试）。
        tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))

        let expected = panel.visibleEntries[0].path
        XCTAssertEqual(tableView.selectedRowIndexes, IndexSet(integer: 0),
                       "表层：第 0 行应处于选中态")
        XCTAssertEqual(panel.selection, [expected],
                       "模型层：表选中必须经 tag 绑定回流（表有这里没有=绑定/tag 接线断）")
        let responder = window.firstResponder as? NSView
        XCTAssertTrue(responder === tableView || responder?.isDescendant(of: tableView) == true,
                      "键盘必须留在列表上：NSTableView 是 firstResponder 才画活动色高亮，实际 \(String(describing: window.firstResponder))")

        // 双击打开走 AppKit 通道：doubleAction 必须已装到这张表上。
        XCTAssertNotEqual(tableView.doubleAction, NSSelectorFromString(""),
                          "文件列表必须装有双击通道（doubleAction 为空=ListDoubleClick 没钩上表）")
        XCTAssertNotNil(tableView.target, "doubleAction 的 target 必须已安装")

        // clickedRow→visibleEntries 映射：错误行占第 0 行时整体后移一位。
        XCTAssertEqual(FilesPanelView.entryIndex(clickedRow: 0, errorVisible: false, count: 2), 0)
        XCTAssertNil(FilesPanelView.entryIndex(clickedRow: 0, errorVisible: true, count: 2))
        XCTAssertEqual(FilesPanelView.entryIndex(clickedRow: 1, errorVisible: true, count: 2), 0)
        XCTAssertNil(FilesPanelView.entryIndex(clickedRow: 2, errorVisible: false, count: 2))
        XCTAssertNil(FilesPanelView.entryIndex(clickedRow: -1, errorVisible: false, count: 2))
    }

    /// 哨兵（测试框架边界）：合成点击驱动不了 SwiftUI List 的行选中。
    ///
    /// 三种派发配方（window.sendEvent / NSApp.sendEvent / 全入队+runloop
    /// 泵）实测都不能让裸 List（无任何手势/拖放，零生产代码参与）选中行
    /// ——SwiftUI 的点击选中需要真实事件流，XCTest 下不可达。文件窗格探针
    /// 因此一律在表层（selectRowIndexes）驱动选中再断言接线。本哨兵防止
    /// 后人重蹈覆辙；若某天它红了=合成点击可用了，应把探针升级为真实
    /// 点击路径。
    func testSyntheticClicksCannotDriveSwiftUIListSelection() throws {
        final class Box: ObservableObject { @Published var selection: Set<String> = [] }

        func table(in v: NSView) -> NSTableView? {
            if let t = v as? NSTableView { return t }
            return v.subviews.lazy.compactMap(table(in:)).first
        }

        /// 一轮配方实验：全新裸 List 窗口，按给定配方合成一次「点击第 0 行」，
        /// 返回（表层选中，绑定选中）。每轮独立窗口/Box，避免互相污染。
        func attempt(_ recipe: (NSWindow, NSTableView, (NSEvent.EventType) throws -> NSEvent) throws -> Void)
                throws -> (IndexSet, Set<String>) {
            let box = Box()
            let host = NSHostingView(rootView:
                List(selection: Binding(get: { box.selection }, set: { box.selection = $0 })) {
                    Text("alpha.txt").tag("alpha.txt")
                    Text("beta.txt").tag("beta.txt")
                }
                .listStyle(.inset)
            )
            host.frame = CGRect(x: 0, y: 0, width: 400, height: 300)
            let window = WorkspaceTestWindow(contentRect: host.frame, styleMask: [.titled, .resizable],
                                             backing: .buffered, defer: false)
            window.stubbedKey = true
            window.contentView = host
            TestWindows.presentOffscreen(window)
            defer { window.orderOut(nil) }
            host.layoutSubtreeIfNeeded()
            RunLoop.main.run(until: Date().addingTimeInterval(0.3))

            let tableView = try XCTUnwrap(table(in: host), "裸 List 里必须有 NSTableView")
            XCTAssertEqual(tableView.numberOfRows, 2)
            let rowRect = tableView.rect(ofRow: 0)
            let point = tableView.convert(NSPoint(x: rowRect.midX, y: rowRect.midY), to: nil)
            func mouse(_ type: NSEvent.EventType) throws -> NSEvent {
                try XCTUnwrap(NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                                                 timestamp: ProcessInfo.processInfo.systemUptime,
                                                 windowNumber: window.windowNumber, context: nil,
                                                 eventNumber: 0, clickCount: 1, pressure: 1))
            }
            try recipe(window, tableView, mouse)
            RunLoop.main.run(until: Date().addingTimeInterval(0.3))
            // 清掉队列里可能剩余的事件，别污染后续轮次/测试。
            while NSApp.nextEvent(matching: .any, until: Date(), inMode: .default, dequeue: true) != nil {}
            return (tableView.selectedRowIndexes, box.selection)
        }

        // mouseDown 的处理会进入嵌套事件跟踪（在 NSApp 队列里等释放），且
        // SwiftUI 的行选中经由 CellHostingView 的手势机制——同步 sendEvent
        // 的成对事件驱动不了它。逐个试可用的派发配方。
        var report: [String] = []
        // 配方 A：mouseUp 先入队，mouseDown 走 window.sendEvent。
        let a = try attempt { window, _, mouse in
            NSApp.postEvent(try mouse(.leftMouseUp), atStart: false)
            window.sendEvent(try mouse(.leftMouseDown))
        }
        report.append("A(up入队+window.sendEvent)=\(a.0)/\(a.1)")
        // 配方 B：mouseDown 走 NSApp.sendEvent（应用层完整派发，设置 currentEvent）。
        let b = try attempt { _, _, mouse in
            NSApp.postEvent(try mouse(.leftMouseUp), atStart: false)
            NSApp.sendEvent(try mouse(.leftMouseDown))
        }
        report.append("B(up入队+NSApp.sendEvent)=\(b.0)/\(b.1)")
        // 配方 C：两个事件都入队，靠 runloop 泵让 NSApp 自己派发。
        let c = try attempt { _, _, mouse in
            NSApp.postEvent(try mouse(.leftMouseDown), atStart: false)
            NSApp.postEvent(try mouse(.leftMouseUp), atStart: false)
        }
        report.append("C(全入队+泵)=\(c.0)/\(c.1)")

        // 哨兵断言现状：三种点击派发配方都驱动不了。若这条红了=框架限制
        // 解除（合成点击可用了），文件窗格探针应升级为真实点击路径。
        let worked = [a, b, c].contains { $0.0 == IndexSet(integer: 0) && $0.1 == ["alpha.txt"] }
        XCTAssertFalse(worked,
                       "合成点击竟能驱动裸 List 行选中了（\(report.joined(separator: "；"))）——框架限制解除，请升级文件窗格探针为真实点击路径")

        // 对照配方 D：表级 selectRowIndexes 必须走得通（选中→绑定回流），
        // 证明 List 机制在测试环境不是全死，只是点击路由不可达。
        let d = try attempt { _, table, _ in
            table.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        }
        XCTAssertEqual(d.0, IndexSet(integer: 0))
        XCTAssertEqual(d.1, ["alpha.txt"],
                       "表级选中应回流绑定——这条红=探针方法论整体失效：\(report.joined(separator: "；"))")
    }

    private func findTerminal(_ root: NSView) -> TerminalHostView? {
        if let host = root as? TerminalHostView { return host }
        return root.subviews.lazy.compactMap(findTerminal).first
    }
}

/// 测试进程里造不出真 key 窗口（没有用户点击激活），可驱动 `isKeyWindow`。
private final class WorkspaceTestWindow: NSWindow {
    var stubbedKey = false
    override var isKeyWindow: Bool { stubbedKey }
}

final class WorkspaceBackend: TransferBackend {
    var displayName = "test-endpoint"
    var initialPath = "/"
    var downloads: [String] = [], uploads: [String] = []
    var id: UInt64 = UInt64.random(in: 1000...100000)
    func list(_ path: String) throws -> [FileEntry] { [] }
    func mkdir(_ path: String, mode: UInt32) throws {}
    func remove(_ path: String, recursive: Bool) throws {}
    func rename(_ from: String, _ to: String) throws {}
    func chmod(_ path: String, mode: UInt32) throws {}
    func realpath(_ path: String) throws -> String { path }
    func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {}
    func exists(_ path: String) -> Bool { false }
    func download(remote: String, local: String) throws -> UInt64 { downloads.append(remote); id += 1; return id }
    func upload(local: String, remote: String) throws -> UInt64 { uploads.append(remote); id += 1; return id }
    func pause(jobID: UInt64, _ p: Bool) throws {}
    func cancel(jobID: UInt64) throws {}
    func search(root: String, pattern: String, maxDepth: Int, onHit: @escaping (FileEntry) -> Bool,
                onScanned: @escaping (Int) -> Void) throws -> Bool { true }
}
