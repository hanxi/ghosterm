import AppKit
import Foundation
import GhostermTerminal
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor final class PluginWorkspaceTests: XCTestCase {
    /// The prohibited background test process has no OS key window. Match the
    /// existing WorkspaceTests fixture while exercising real responder routing.
    private final class KeyWindow: NSWindow {
        override var isKeyWindow: Bool { true }
    }
    override class func setUp() { _ = NSApplication.shared; TestWindows.enterAutomationMode() }

    private func until(_ check: () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 3
        while !check(), ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(for: .milliseconds(10)) }
        XCTAssertTrue(check())
    }
    private func app() throws -> AppModel {
        let root = sessionTestDirectory()
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let config = root.appendingPathComponent("config")
        try Data().write(to: config)
        addTeardownBlock { try? FileManager.default.removeItem(at: root) }
        return isolatedAppModel(configPath: config.path, knownHostsPath: root.appendingPathComponent("known").path,
                                pluginResourceDirectory: root.appendingPathComponent("resources"))
    }

    func testExtensionLeavesSurviveFileVisibilityDefaultResetDockingAndZoom() throws {
        let terminal = TerminalPane(), first = ExtensionInstanceModel(controller: nil), second = ExtensionInstanceModel(controller: nil)
        let tab = TerminalTab(kind: .localShell, pane: terminal)
        tab.addExtensionPane(first); tab.addExtensionPane(second)
        let file = WorkspaceFilePane(remote: false, panel: FilesPanelModel(backend: LocalFileBackend()))
        tab.addFilePane(file)
        tab.filesPanelVisible = false
        let expected = Set([terminal.id, first.id, second.id])
        XCTAssertEqual(Set(try XCTUnwrap(tab.visibleWorkspaceLayout).paneIDs), expected, "Hiding files must keep native extensions visible")
        tab.activeWorkspacePaneID = first.id
        XCTAssertNil(tab.activePane, "Plugin focus must not fall back to a terminal")
        tab.restoreDefaultWorkspace()
        XCTAssertEqual(Set(try XCTUnwrap(tab.workspaceLayout).paneIDs), expected.union([file.id]))
        tab.toggleWorkspaceZoom(first.id); XCTAssertEqual(tab.zoomedPaneID, first.id)
        XCTAssertTrue(tab.dock(.init(tabID: tab.id, paneID: second.id), onto: terminal.id, edge: .bottom,
                               available: CGSize(width: 1800, height: 1200), minimum: { _ in CGSize(width: 120, height: 80) }))
        XCTAssertEqual(tab.activeWorkspacePaneID, second.id); XCTAssertNil(tab.zoomedPaneID)
        XCTAssertEqual(Set(try XCTUnwrap(tab.workspaceLayout).paneIDs), expected.union([file.id]))
        XCTAssertTrue(tab.workspacePanes.first { $0.id == first.id }?.extensionInstance === first)
    }

    func testWorkspaceHostsKeepTheirIdentityAndClaimFocusFromOtherPanes() throws {
        let app = try app(), first = app.extensionInstance, second = ExtensionInstanceModel(controller: nil)
        let tab = TerminalTab(kind: .workspace)
        tab.addExtensionPane(first); tab.addExtensionPane(second); app.tabs.add(tab)
        let host = WorkspaceHostView(frame: NSRect(x: 0, y: 0, width: 1200, height: 800))
        let window = KeyWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = host
        TestWindows.presentOffscreenAsKey(window)
        defer { window.orderOut(nil); window.contentView = nil; window.close() }
        host.update(model: app, tab: tab, isActive: true, opts: .init()); host.layoutSubtreeIfNeeded()
        let original = try XCTUnwrap(host.hosts[first.id])
        tab.activeWorkspacePaneID = first.id
        host.update(model: app, tab: tab, isActive: true, opts: .init())
        XCTAssertTrue(window.firstResponder === original, "Extension focus must leave the previous pane")
        tab.activeWorkspacePaneID = second.id
        host.update(model: app, tab: tab, isActive: false, opts: .init())
        XCTAssertTrue(window.firstResponder === original, "Background extension updates must not steal focus")
        tab.activeWorkspacePaneID = first.id
        tab.toggleWorkspaceZoom(first.id)
        host.update(model: app, tab: tab, isActive: true, opts: .init()); host.layoutSubtreeIfNeeded()
        tab.toggleWorkspaceZoom(first.id)
        _ = tab.dock(.init(tabID: tab.id, paneID: first.id), onto: second.id, edge: .bottom,
                     available: host.bounds.size, minimum: { _ in CGSize(width: 120, height: 80) })
        host.update(model: app, tab: tab, isActive: true, opts: .init()); host.layoutSubtreeIfNeeded()
        XCTAssertTrue(host.hosts[first.id] === original, "Reordering must keep the original native host")
        XCTAssertTrue(original.rootView.pane.extensionInstance === first)
    }

    func testPaneCloseWaitsForItsCapturedOwnerAndKeepsTheOtherPane() async throws {
        let app = try app(), instance = app.extensionInstance
        let other = ExtensionInstanceModel(controller: nil), tab = TerminalTab(kind: .workspace)
        tab.addExtensionPane(instance); tab.addExtensionPane(other); app.tabs.add(tab)
        instance.beginInspection()
        app.closePane(tabID: tab.id, paneID: instance.id)
        XCTAssertEqual(app.extensionPaneCloseRequest?.paneID, instance.id)
        XCTAssertFalse(instance.closeRequested, "Opening confirmation must not cancel an active invocation")
        app.cancelExtensionPaneClose()
        XCTAssertNil(app.extensionPaneCloseRequest); XCTAssertFalse(instance.closeRequested)
        app.closePane(tabID: tab.id, paneID: instance.id)
        tab.activeWorkspacePaneID = other.id
        app.confirmExtensionPaneClose()
        XCTAssertNil(app.extensionPaneCloseRequest)
        try await until { instance.closeRequested }
        XCTAssertTrue(tab.workspacePanes.contains { $0.id == instance.id }, "Pending cleanup must retain its pane")
        tab.activeWorkspacePaneID = other.id
        instance.endInspection()
        try await until { !tab.workspacePanes.contains { $0.id == instance.id } }
        XCTAssertEqual(tab.workspacePanes.map(\.id), [other.id]); XCTAssertEqual(tab.activeWorkspacePaneID, other.id)
        XCTAssertFalse(other.closeRequested, "Closing one invocation must not close the newly focused one")
    }

    func testStaleCloseChoiceDoesNotCloseAnotherPluginPane() throws {
        let app = try app(), first = app.extensionInstance, other = ExtensionInstanceModel(controller: nil)
        let tab = TerminalTab(kind: .workspace)
        tab.addExtensionPane(first); tab.addExtensionPane(other); app.tabs.add(tab)
        first.beginInspection()
        app.closePane(tabID: tab.id, paneID: first.id)
        XCTAssertEqual(app.extensionPaneCloseRequest?.paneID, first.id)
        first.endInspection(); tab.removeExtensionPane(first.id)
        tab.activeWorkspacePaneID = other.id
        app.confirmExtensionPaneClose()
        XCTAssertNil(app.extensionPaneCloseRequest)
        XCTAssertFalse(first.closeRequested); XCTAssertFalse(other.closeRequested)
        XCTAssertEqual(tab.workspacePanes.map(\.id), [other.id])
    }

    func testIdlePluginPaneClosesWithoutConfirmation() async throws {
        let app = try app(), first = app.extensionInstance, other = ExtensionInstanceModel(controller: nil)
        let tab = TerminalTab(kind: .workspace)
        tab.addExtensionPane(first); tab.addExtensionPane(other); app.tabs.add(tab)
        XCTAssertFalse(first.closeNeedsConfirmation)
        app.closePane(tabID: tab.id, paneID: first.id)
        XCTAssertNil(app.extensionPaneCloseRequest)
        try await until { !tab.workspacePanes.contains { $0.id == first.id } }
        XCTAssertFalse(other.closeRequested)
    }

    func testClosingFocusedPluginReturnsKeyboardToTerminal() async throws {
        let app = try app()
        let terminal = TerminalPane()
        terminal.attachForTesting(surface: try TerminalSurface(app: TerminalApp.shared()))
        let tab = TerminalTab(kind: .localShell, pane: terminal)
        let plugin = app.extensionInstance
        tab.addExtensionPane(plugin)
        app.tabs.add(tab)
        let host = WorkspaceHostView(frame: NSRect(x: 0, y: 0, width: 1200, height: 800))
        let window = KeyWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        TestWindows.presentOffscreenAsKey(window)
        defer { window.orderOut(nil); window.contentView = nil; window.close() }

        tab.activeWorkspacePaneID = plugin.id
        host.update(model: app, tab: tab, isActive: true, opts: .init())
        host.layoutSubtreeIfNeeded()
        let pluginHost = try XCTUnwrap(host.hosts[plugin.id])
        XCTAssertTrue(window.firstResponder === pluginHost)

        app.closePane(tabID: tab.id, paneID: plugin.id)
        try await until { !tab.workspacePanes.contains { $0.id == plugin.id } }
        XCTAssertEqual(tab.activeWorkspacePaneID, terminal.id)
        host.update(model: app, tab: tab, isActive: true, opts: .init())
        host.layoutSubtreeIfNeeded()
        try await until { window.firstResponder is TerminalHostView }
    }

    func testTabCloseDefersRemovalUntilPluginCleanupFinishes() async throws {
        let app = try app(); app.showExtensionWorkspace()
        let tab = try XCTUnwrap(app.tabs.activeTab), instance = app.extensionInstance
        XCTAssertEqual(tab.kind, .workspace); XCTAssertEqual(tab.workspacePanes.map(\.id), [instance.id])
        app.showExtensionWorkspace(); XCTAssertEqual(app.tabs.tabs.count, 1)
        instance.beginInspection()
        XCTAssertTrue(tab.closeNeedsConfirmation)
        XCTAssertFalse(app.shouldTerminate(), "An active plugin must participate in application close confirmation")
        app.tabs.close(tab)
        try await until { instance.closeRequested }
        XCTAssertTrue(app.tabs.tabs.contains { $0.id == tab.id })
        instance.endInspection()
        try await until { app.tabs.tabs.isEmpty }
        XCTAssertTrue(instance.canDiscard)
    }

    func testFailedCleanupKeepsThePaneAndItsIssueAvailableForRetry() async throws {
        let app = try app(), instance = ExtensionInstanceModel(controller: nil)
        let tab = TerminalTab(kind: .workspace)
        tab.addExtensionPane(instance); tab.addExtensionPane(app.extensionInstance); app.tabs.add(tab)
        instance.recordInspectionIssue("owned cleanup failure")
        let closed = await instance.requestClose()
        XCTAssertFalse(closed)
        tab.removeExtensionPane(instance.id)
        XCTAssertTrue(tab.workspacePanes.contains { $0.id == instance.id }, "Failed cleanup must retain its owner")
        XCTAssertEqual(instance.extensionRunIssue, "owned cleanup failure")
    }

    func testDraftAndApprovalEventsBelongToTheInvocation() {
        let first = ExtensionInstanceModel(controller: nil), second = ExtensionInstanceModel(controller: nil)
        first.codexMessageDraft = "first"; second.codexMessageDraft = "second"
        let approval = AppModel.CodexPendingApproval(id: "1", method: "item/commandExecution/requestApproval",
            paramsJSON: "{}", itemID: nil, identity: nil, permissionReviewID: nil)
        first.applyCodexApprovalEvent(kind: "approval", added: approval)
        second.applyCodexApprovalEvent(kind: "protocol-error")
        XCTAssertEqual(first.codexPendingApprovals, [approval]); XCTAssertTrue(second.codexPendingApprovals.isEmpty)
        XCTAssertEqual(first.codexMessageDraft, "first"); XCTAssertEqual(second.codexMessageDraft, "second")
    }

    func testFailedObservationWithdrawsOnlyItsApprovalsAndRetainsDraftAndCause() {
        let first = ExtensionInstanceModel(controller: nil), second = ExtensionInstanceModel(controller: nil)
        first.codexMessageDraft = "retained draft"
        let approval = AppModel.CodexPendingApproval(id: "1", method: "item/commandExecution/requestApproval",
            paramsJSON: "{}", itemID: nil, identity: nil, permissionReviewID: nil)
        first.applyCodexApprovalEvent(kind: "approval", added: approval)
        second.applyCodexApprovalEvent(kind: "approval", added: approval)
        first.runtimeObservationFailed("owned observation failure")
        XCTAssertEqual(first.runtimeObservationIssue, "owned observation failure")
        XCTAssertFalse(first.canInteractWithRuntime)
        XCTAssertTrue(first.codexPendingApprovals.isEmpty)
        XCTAssertEqual(second.codexPendingApprovals, [approval]); XCTAssertNil(second.runtimeObservationIssue)
        XCTAssertEqual(first.codexMessageDraft, "retained draft")
        XCTAssertTrue(first.extensionRunIssue?.contains("未知") == true)
        first.runtimeObservationFailed("later cleanup failure")
        XCTAssertEqual(first.runtimeObservationIssue, "owned observation failure", "A cleanup retry must not overwrite the original loss observation")
    }
}
