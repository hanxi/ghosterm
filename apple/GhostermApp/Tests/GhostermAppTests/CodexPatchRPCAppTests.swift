import AppKit
import CryptoKit
import Foundation
import GhostermCore
import SwiftUI
import Vision
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

    private struct FixtureFailure: Error { let message: String }

    private func wait(_ label: String, seconds: Double = 30, until predicate: () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + seconds
        while !predicate() && ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(20))
        }
        if !predicate() { throw FixtureFailure(message: label) }
    }

    private func buttonPoint(_ root: NSView, label: String) throws -> NSPoint? {
        let window = try XCTUnwrap(root.window)
        window.alphaValue = 0
        guard NSScreen.screens.allSatisfy({ !$0.frame.intersects(window.frame) }) else {
            throw FixtureFailure(message: "Automation window must stay outside every screen: \(window.frame)")
        }
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        root.layoutSubtreeIfNeeded()
        guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(root.bounds.width * 2),
            pixelsHigh: Int(root.bounds.height * 2), bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
            isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return nil }
        bitmap.size = root.bounds.size
        root.cacheDisplay(in: root.bounds, to: bitmap)
        guard let image = bitmap.cgImage else { return nil }
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.minimumTextHeight = 0.002
        request.usesLanguageCorrection = false
        request.recognitionLanguages = ["zh-Hans", "en-US"]
        request.customWords = [label]
        try VNImageRequestHandler(cgImage: image).perform([request])
        let matches = (request.results ?? []).filter {
            $0.topCandidates(1).first?.string.replacingOccurrences(of: " ", with: "") == label
        }.sorted { $0.boundingBox.midY > $1.boundingBox.midY }
        guard let match = matches.first else {
            if let data = bitmap.representation(using: .png, properties: [:]) {
                try data.write(to: URL(fileURLWithPath: #filePath).deletingLastPathComponent()
                    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
                    .deletingLastPathComponent().appendingPathComponent("tmp/codex-patch-pane.png"))
            }
            return nil
        }
        let box = match.boundingBox
        return NSPoint(x: box.midX * root.bounds.width,
                       y: (root.isFlipped ? 1 - box.midY : box.midY) * root.bounds.height)
    }

    private func press(_ root: NSView, id: String) async throws {
        let labels = ["plugin-consent-allow": "批准并启动", "plugin-codex-start-thread": "启动Codex对话",
                      "plugin-codex-send": "发送", "plugin-codex-approval-allow": "批准一次", "plugin-stop": "停止并释放插件"]
        let label = id.hasPrefix("plugin-codex-read-change-") ? "读取完整补丁" : try XCTUnwrap(labels[id])
        var point: NSPoint?
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while point == nil && ProcessInfo.processInfo.systemUptime < deadline {
            point = try buttonPoint(root, label: label)
            if point == nil { try await Task.sleep(for: .milliseconds(100)) }
        }
        let location = root.convert(try XCTUnwrap(point, "Native rendered button missing: \(id)"), to: nil)
        let window = try XCTUnwrap(root.window)
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            let event = try XCTUnwrap(NSEvent.mouseEvent(with: type, location: location, modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber,
                context: nil, eventNumber: 1, clickCount: 1, pressure: type == .leftMouseDown ? 1 : 0))
            window.sendEvent(event)
        }
    }

    private func textViews(_ view: NSView) -> [NSTextView] {
        (view as? NSTextView).map { [$0] } ?? view.subviews.flatMap { textViews($0) }
    }

    private func expectedDiff(_ revision: Int) -> String {
        "@@ -1 +1 @@\n" + (0..<600).map { "+REV-\(revision) row-\($0) 中文😀\n" }.joined() + "+FINAL-REV-\(revision)\n"
    }

    private func run(useJump: Bool) async throws {
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
        let pane = NSHostingView(rootView: ExtensionsPane(model: model)
            .background(Color(nsColor: .windowBackgroundColor)))
        pane.frame = NSRect(x: 0, y: 0, width: 1440, height: 2200)
        let window = FixtureWindow(contentRect: pane.frame,
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = pane
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
        model.codexMessageDraft = "exercise the isolated patch protocol fixture"
        try await press(pane, id: "plugin-codex-send")
        try await wait("Patch and file approval did not arrive over Host RPC") {
            (model.codexPendingApprovals.count == 1 && model.codexTimeline.contains { $0.changeReference != nil } && !model.codexCallBusy) ||
                model.extensionRunIssue != nil
        }
        XCTAssertNil(model.extensionRunIssue)
        guard model.extensionRunIssue == nil else { return }
        let approval = try XCTUnwrap(model.codexPendingApprovals.first)
        let old = try XCTUnwrap(model.codexTimeline.first { $0.changeReference != nil }?.changeReference)
        XCTAssertEqual(approval.id, "s:file:审批")
        XCTAssertTrue(model.codexTimeline.first { $0.changeReference != nil }?.fileChanges.isEmpty == true)
        XCTAssertEqual(model.codexTimeline.first { $0.changeReference != nil }?.omittedFileCount, 10)
        XCTAssertNil(model.codexApprovalSnapshot(approval))
        try await press(pane, id: "plugin-codex-approval-allow")
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
        try await press(pane, id: "plugin-codex-approval-allow")
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
