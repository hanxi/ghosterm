import AppKit
import CryptoKit
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Real Native/SSH/QuickJS RPC; the explicitly mapped app-server is a no-model protocol fixture.
@MainActor
final class CodexPatchRPCAppTests: XCTestCase {
    private final class FixtureWindow: NSWindow {
        override var occlusionState: NSWindow.OcclusionState { [.visible] }
        override func constrainFrameRect(_ frameRect: NSRect, to screen: NSScreen?) -> NSRect {
            var result = frameRect; result.origin = TestWindows.offscreenOrigin; return result
        }
        override func setFrame(_ frameRect: NSRect, display flag: Bool) {
            var result = frameRect; result.origin = TestWindows.offscreenOrigin
            super.setFrame(result, display: flag)
        }
        override func setFrame(_ frameRect: NSRect, display flag: Bool, animate: Bool) {
            var result = frameRect; result.origin = TestWindows.offscreenOrigin
            super.setFrame(result, display: flag, animate: animate)
        }
    }
    override func setUp() { TestWindows.enterAutomationMode() }

    func testPatchAndApprovalOverDirectSSH() async throws { try await run(useJump: false) }
    func testPatchAndApprovalOverProxyJump() async throws { try await run(useJump: true) }

    func testLostTurnReplyOverDirectSSH() async throws { try await run(useJump: false, dropTurnReply: true) }
    func testLostTurnReplyOverProxyJump() async throws { try await run(useJump: true, dropTurnReply: true) }

    func testInterruptWaitsForTerminalEventOverDirectSSH() async throws { try await run(useJump: false, interruptTurn: true) }

    private struct FixtureFailure: Error { let message: String }

    private func wait(_ label: String, seconds: Double = 30, until predicate: () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + seconds
        while !predicate() && ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(20))
        }
        if !predicate() { throw FixtureFailure(message: label) }
    }

    private func control(_ root: NSView, id: String) -> NSObject? {
        root.layoutSubtreeIfNeeded()
        return PluginTableRendering.elements(root).first {
            PluginTableRendering.attribute($0, "accessibilityIdentifier") == id
        }
    }

    private func press(_ root: NSView, id: String) async throws {
        try await wait("Native button unavailable: \(id)", seconds: 5) {
            guard let button = self.control(root, id: id) else { return false }
            return self.isEnabled(button)
        }
        XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(control(root, id: id))), id)
    }

    private func isEnabled(_ button: NSObject) -> Bool {
        let selector = NSSelectorFromString("isAccessibilityEnabled")
        guard button.responds(to: selector), let implementation = button.method(for: selector) else { return false }
        typealias Enabled = @convention(c) (AnyObject, Selector) -> Bool
        return unsafeBitCast(implementation, to: Enabled.self)(button, selector)
    }

    private func assertApprovalDisabled(_ root: NSView) async throws {
        try await wait("Native approval did not become disabled", seconds: 5) {
            guard let button = self.control(root, id: "plugin-codex-approval-allow") else { return false }
            return !self.isEnabled(button)
        }
    }

    private func textViews(_ view: NSView) -> [NSTextView] {
        (view as? NSTextView).map { [$0] } ?? view.subviews.flatMap { textViews($0) }
    }

    private func expectedDiff(_ revision: Int) -> String {
        "@@ -1 +1 @@\n" + (0..<600).map { "+REV-\(revision) row-\($0) 中文😀\n" }.joined() + "+FINAL-REV-\(revision)\n"
    }

    private func run(useJump: Bool, dropTurnReply: Bool = false, interruptTurn: Bool = false) async throws {
        let env = ProcessInfo.processInfo.environment
        guard ExtensionRuntime.supportStatus == 0, let gterm = env["GHOSTERM_TEST_GTERM"],
              let program = env["GHOSTERM_TEST_CODEX_PATCH_FIXTURE"], let packagePath = env["GHOSTERM_TEST_CODEX_PACKAGE"],
              let host = env["GHOSTERM_SSH_HOST"], let port = env["GHOSTERM_SSH_PORT"],
              let user = env["GHOSTERM_SSH_USER"], let key = env["GHOSTERM_SSH_KEY"] else {
            throw XCTSkip("requires the explicit no-model protocol fixture and owned SSH runner")
        }
        if useJump && (env["GHOSTERM_SSH_JUMP_HOST"] == nil || env["GHOSTERM_SSH_JUMP_PORT"] == nil) {
            throw XCTSkip("requires the owned forwarding-only jump sshd")
        }
        let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let root = repo.appendingPathComponent("tmp/codex-patch-" + UUID().uuidString)
        for name in ["workspace", "home", "codex", "config"] {
            try FileManager.default.createDirectory(at: root.appendingPathComponent(name), withIntermediateDirectories: true)
        }
        let workspace = root.appendingPathComponent("workspace")
        if dropTurnReply { try Data().write(to: workspace.appendingPathComponent("drop-turn-reply")) }
        if interruptTurn { try Data().write(to: workspace.appendingPathComponent("interrupt-turn")) }
        try Data(#"{"fixture":"ghosterm-patch-rpc","version":1}"#.utf8).write(to: workspace.appendingPathComponent("fixture.json"))
        func quote(_ text: String) -> String { "'" + text.replacingOccurrences(of: "'", with: "'\"'\"'") + "'" }
        let childEnvironment = ["HOME": root.appendingPathComponent("home").path,
            "CODEX_HOME": root.appendingPathComponent("codex").path, "XDG_CONFIG_HOME": root.appendingPathComponent("config").path,
            "TMPDIR": repo.appendingPathComponent("tmp").path, "PATH": env["PATH"] ?? "/usr/bin:/bin"]
        let assignments = childEnvironment.sorted { $0.key < $1.key }.map { quote("\($0.key)=\($0.value)") }.joined(separator: " ")
        let launcher = root.appendingPathComponent("gterm-fixture")
        let diagnostics = root.appendingPathComponent("executor.log")
        try Data("#!/bin/sh\nexec /usr/bin/env -i \(assignments) \(quote(gterm)) \"$@\" 2>\(quote(diagnostics.path))\n".utf8).write(to: launcher)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: launcher.path)
        var config = "Host patch-fixture\n HostName \(host)\n Port \(port)\n User \(user)\n IdentityFile \(key)\n"
        if useJump {
            config += " ProxyJump patch-jump\nHost patch-jump\n HostName \(env["GHOSTERM_SSH_JUMP_HOST"]!)\n Port \(env["GHOSTERM_SSH_JUMP_PORT"]!)\n User \(user)\n IdentityFile \(key)\n"
        }
        let configURL = root.appendingPathComponent("ssh_config"), known = root.appendingPathComponent("known_hosts")
        try Data(config.utf8).write(to: configURL); try Data().write(to: known)
        let model = isolatedAppModel(configPath: configURL.path, knownHostsPath: known.path)
        let controller = try XCTUnwrap(model.extensionController)
        let interaction = CodexRouteInteraction(); model.interaction = interaction
        let pane = NSHostingView(rootView: PluginWorkspaceTestView(model: model)
            .background(Color(nsColor: .windowBackgroundColor)))
        pane.frame = NSRect(x: 0, y: 0, width: 1440, height: 2200)
        let window = FixtureWindow(contentRect: pane.frame,
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = pane
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        TestWindows.presentOffscreen(window)
        addTeardownBlock {
            await model.stopExtensionRun(); _ = await controller.close()
            await MainActor.run { window.close() }; try? FileManager.default.removeItem(at: root)
        }
        let package = URL(fileURLWithPath: packagePath)
        let review = try await controller.review(directory: package)
        let approved = try await controller.approve(id: review.id, digest: review.digest)
        XCTAssertEqual(approved, 0)
        let digest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: program)))
            .map { String(format: "%02x", $0) }.joined()
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest,
            command: "ai codex", arguments: [], sshAlias: "patch-fixture", executable: launcher.path,
            executorDirectory: package.path, executorDigest: review.digest,
            methods: ["codex.thread.start", "codex.turn.send", "codex.turn.interrupt", "codex.approval.respond", "codex.change.read"],
            hostMethods: ["codex.event"], scopedRoots: [.init(id: "workspace", directory: workspace.path)],
            processPrograms: [.init(name: "codex", executable: program, version: "codex-cli 0.166.0", sha256: digest)])
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertEqual(model.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
        XCTAssertEqual(interaction.hosts.count, useJump ? 2 : 1)
        try await press(pane, id: "plugin-consent-allow")
        try await wait("Client Runtime did not activate") { model.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        try await press(pane, id: "plugin-codex-start-thread")
        try await wait("Fixture thread did not reach the Native UI") { model.codexThreadID == "thread-fixture" && !model.codexCallBusy }
        if !useJump && !dropTurnReply && !interruptTurn {
            let invalidDraft = String(repeating: "x", count: 2049)
            model.codexMessageDraft = invalidDraft
            await model.sendCodexMessage()
            XCTAssertFalse(model.extensionInstance.codexSendUnconfirmed)
            XCTAssertFalse(model.codexTurnActive)
            XCTAssertEqual(model.codexMessageDraft, invalidDraft)
            XCTAssertTrue(model.extensionRunIssue?.contains("Codex 未发送消息") == true)
        }
        let previousSendIssue = model.extensionRunIssue
        model.codexMessageDraft = "exercise the isolated patch protocol fixture"
        try await press(pane, id: "plugin-codex-send")
        if dropTurnReply {
            try await wait("missing turn reply was not reported") {
                model.extensionInstance.codexSendUnconfirmed && !model.codexCallBusy &&
                    self.control(pane, id: "plugin-codex-send-unconfirmed") != nil
            }
            XCTAssertTrue(FileManager.default.fileExists(atPath: workspace.appendingPathComponent("turn-received.json").path))
            XCTAssertFalse(model.codexTurnActive)
            XCTAssertFalse(isEnabled(try XCTUnwrap(control(pane, id: "plugin-codex-send"))))
            XCTAssertEqual(model.codexMessageDraft, "exercise the isolated patch protocol fixture")
            let issue = model.extensionRunIssue
            await model.sendCodexMessage()
            XCTAssertEqual(model.extensionRunIssue, issue)
            XCTAssertTrue(model.extensionInstance.codexSendUnconfirmed)
            XCTAssertFalse(model.codexCallBusy)
            return
        }
        if interruptTurn {
            try await wait("interruption fixture turn did not start") { model.codexTurnActive && !model.codexCallBusy }
            try await press(pane, id: "plugin-codex-interrupt")
            let receipt = workspace.appendingPathComponent("interrupt-received.json")
            try await wait("interrupt acknowledgement did not return") {
                FileManager.default.fileExists(atPath: receipt.path) && !model.codexCallBusy
            }
            XCTAssertNil(model.extensionRunIssue)
            XCTAssertTrue(model.codexTurnActive, "acknowledgement must not end the turn")
            try Data().write(to: workspace.appendingPathComponent("complete-interrupt"))
            try await wait("terminal event did not end the turn") { !model.codexTurnActive }
            XCTAssertNil(model.extensionRunIssue)
            return
        }
        try await wait("Patch and file approval did not arrive over Host RPC") {
            (model.codexPendingApprovals.count == 1 && model.codexTimeline.contains { $0.changeReference != nil } && !model.codexCallBusy) ||
                (model.extensionRunIssue != nil && model.extensionRunIssue != previousSendIssue)
        }
        XCTAssertNil(model.extensionRunIssue)
        guard model.extensionRunIssue == nil else { return }
        let approval = try XCTUnwrap(model.codexPendingApprovals.first)
        let old = try XCTUnwrap(model.codexTimeline.first { $0.changeReference != nil }?.changeReference)
        XCTAssertEqual(approval.id, "s:file:审批")
        XCTAssertTrue(model.codexTimeline.first { $0.changeReference != nil }?.fileChanges.isEmpty == true)
        XCTAssertEqual(model.codexTimeline.first { $0.changeReference != nil }?.omittedFileCount, 10)
        XCTAssertNil(model.codexApprovalSnapshot(approval))
        try await assertApprovalDisabled(pane)
        XCTAssertNil(model.extensionRunIssue, "Disabled unread approval must not invoke the response handler")
        XCTAssertFalse(FileManager.default.fileExists(atPath: workspace.appendingPathComponent("approved.json").path))
        try await press(pane, id: "plugin-codex-read-change-\(old.snapshotID)")
        try await wait("Complete revision one was not published") { model.codexChangeReview?.document != nil }
        let first = try XCTUnwrap(model.codexChangeReview?.document)
        XCTAssertEqual(first.changes.count, 10)
        XCTAssertEqual(first.changes[0].path, "nested/" + String(repeating: "x", count: 4096) + "/source.swift")
        XCTAssertTrue(first.changes[0].diff.utf8.elementsEqual(expectedDiff(1).utf8))
        try await wait("Production Native text view did not receive revision one") {
            pane.layoutSubtreeIfNeeded()
            return self.textViews(pane).contains { $0.identifier?.rawValue == "plugin-codex-full-diff-text" &&
                $0.string.utf8.elementsEqual(self.expectedDiff(1).utf8) }
        }
        let fullText = try XCTUnwrap(textViews(pane).first { $0.identifier?.rawValue == "plugin-codex-full-diff-text" })
        XCTAssertFalse(fullText.isEditable); XCTAssertTrue(fullText.isSelectable)
        XCTAssertEqual(model.codexApprovalSnapshot(approval), old.snapshotID)
        try Data().write(to: workspace.appendingPathComponent("request-update"))
        try await wait("Replacement patch did not invalidate the old review") {
            model.codexTimeline.first { $0.changeReference != nil }?.changeReference?.snapshotID != old.snapshotID &&
            model.codexChangeReview == nil
        }
        let current = try XCTUnwrap(model.codexTimeline.first { $0.changeReference != nil }?.changeReference)
        try await assertApprovalDisabled(pane)
        XCTAssertNil(model.extensionRunIssue, "A changed unread patch must keep Native approval disabled")
        let input = try JSONSerialization.data(withJSONObject: ["requestID": approval.id, "decisionJSON": "\"accept\"", "snapshotID": old.snapshotID])
        let admission = try await controller.requestExecutorRPC(method: "codex.approval.respond", input: input)
        XCTAssertEqual(admission.status, .ok)
        let handle = try XCTUnwrap(admission.handle)
        var rejected: ExtensionSession.RPCResult?
        let deadline = ProcessInfo.processInfo.systemUptime + 15
        while ProcessInfo.processInfo.systemUptime < deadline {
            let (status, value) = try await controller.executorRPCResult(handle: handle)
            XCTAssertEqual(status, .ok)
            if value?.confirmed == true { rejected = value; break }
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertEqual(rejected?.state, .succeeded, "A deterministic business refusal must have a confirmed RPC reply")
        XCTAssertTrue(rejected?.confirmed == true)
        let refusal = try XCTUnwrap(rejected)
        let envelope = try XCTUnwrap(JSONSerialization.jsonObject(with: refusal.outcome) as? [String: Any])
        XCTAssertEqual(envelope["kind"] as? String, "success")
        XCTAssertEqual((envelope["value"] as? [String: Any])?["accepted"] as? Bool, false,
                       "Executor must reject stale approval over the real RPC transport")
        let forgotten = try await controller.forgetExecutorRPC(handle: handle); XCTAssertEqual(forgotten, .ok)
        XCTAssertFalse(FileManager.default.fileExists(atPath: workspace.appendingPathComponent("approved.json").path))
        try await press(pane, id: "plugin-codex-read-change-\(current.snapshotID)")
        try await wait("Complete revision two was not published") { model.codexChangeReview?.reference == current && model.codexChangeReview?.document != nil }
        try await wait("Production Native text view did not update to revision two") {
            pane.layoutSubtreeIfNeeded()
            return self.textViews(pane).contains { $0.string.utf8.elementsEqual(self.expectedDiff(2).utf8) }
        }
        try await press(pane, id: "plugin-codex-approval-allow")
        try await wait("Native approval did not reach the child and complete its turn") {
            FileManager.default.fileExists(atPath: workspace.appendingPathComponent("approved.json").path) &&
            model.codexPendingApprovals.isEmpty && !model.codexTurnActive && !model.codexCallBusy
        }
        let receipt = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: workspace.appendingPathComponent("approved.json"))) as? [String: Any])
        XCTAssertEqual(receipt["revision"] as? Int, 2)
        let request = try XCTUnwrap(receipt["request"] as? [String: Any])
        XCTAssertEqual(request["id"] as? String, "file:审批")
        XCTAssertEqual((request["result"] as? [String: Any])?["decision"] as? String, "accept")
        XCTAssertNil(request["snapshotID"], "Native version tokens must not leak into the official approval response")
        XCTAssertNil(model.extensionRunIssue)
        try await press(pane, id: "plugin-stop")
        try await wait("Explicit Native stop did not release the Session") { model.extensionRun == nil }
        XCTAssertNil(model.codexChangeReview)
        XCTAssertNil(model.extensionRunIssue)
    }
}
