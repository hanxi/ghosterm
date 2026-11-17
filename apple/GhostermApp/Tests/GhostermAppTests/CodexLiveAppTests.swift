import AppKit
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Opt-in selected-account model turn. No credential copies, alternate HOME,
/// synthetic backend, automatic approvals or foreground automation windows.
@MainActor
final class CodexLiveAppTests: XCTestCase {
    private struct Fixture: Decodable {
        let gterm: String, gtermSHA256: String, codex: String, codexSHA256: String
        let package: String, workspace: String, temporary: String, target: String, nonce: String
        let prefixArguments: [String]
        let prepareOnly: Bool
    }
    private struct Failure: Error { let code: String }

    private func require(_ condition: Bool, _ code: String) throws {
        if !condition { throw Failure(code: code) }
    }
    private func wait(_ code: String, seconds: Double = 35, until predicate: () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + seconds
        while !predicate() && ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(20))
        }
        try require(predicate(), code)
    }
    private func diagnose(_ model: AppModel) {
        guard let issue = model.extensionRunIssue else { return }
        let markers = ["审批响应失败", "事件序列", "Host 限额", "RPC", "30 秒", "未知", "invalid", "failed",
                       "denied", "closed", "CPU", "超时", "Runtime", "未接受审批", "envelope", "内存", "取消"]
        let matched = markers.filter { issue.contains($0) }
        print("CODEX LIVE APP ISSUE: markers=\(matched) events=\(model.extensionRun?.codexEvents.count ?? -1) pending=\(model.codexPendingApprovals.count) active=\(model.codexTurnActive)")
    }

    func testRealModelFileApprovalThroughNativePage() async throws {
        let env = ProcessInfo.processInfo.environment
        guard env["GHOSTERM_CODEX_APP_LIVE"] == "1", let fixturePath = env["GHOSTERM_CODEX_APP_FIXTURE"] else {
            throw XCTSkip("requires the explicit --live selected-account runner")
        }
        try require(ExtensionRuntime.supportStatus == 0, "PLUGIN_RUNTIME_UNAVAILABLE")
        let fixture = try JSONDecoder().decode(Fixture.self, from: Data(contentsOf: URL(fileURLWithPath: fixturePath)))
        let workspace = URL(fileURLWithPath: fixture.workspace)
        let target = URL(fileURLWithPath: fixture.target)
        let root = URL(fileURLWithPath: fixture.temporary).deletingLastPathComponent()
        let config = root.appendingPathComponent("empty-ssh-config")
        let known = root.appendingPathComponent("empty-known-hosts")
        try Data().write(to: config); try Data().write(to: known)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: known.path)
        let controller = try XCTUnwrap(model.extensionController)
        let pane = NSHostingView(rootView: PluginWorkspaceTestView(model: model)
            .background(Color(nsColor: .windowBackgroundColor)))
        let ui = ConversationUIProbe(root: pane, width: 1440, height: 2400, captureFailures: false)
        addTeardownBlock {
            await model.stopExtensionRun()
            _ = await controller.close()
            await MainActor.run { ui.close() }
        }
        let review = try await controller.review(directory: URL(fileURLWithPath: fixture.package))
        try require(review.id == "org.ghosterm.ai-codex", "OFFICIAL_CODEX_PACKAGE")
        let approved = try await controller.approve(id: review.id, digest: review.digest)
        try require(approved == 0, "EXPLICIT_PACKAGE_SOURCE_APPROVAL")
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest,
            command: "ai codex", arguments: ["--ephemeral"], sshAlias: "", executable: fixture.gterm,
            executorDirectory: fixture.package, executorDigest: review.digest,
            methods: ["codex.thread.start", "codex.turn.send", "codex.turn.interrupt", "codex.approval.respond", "codex.change.read"],
            hostMethods: ["codex.event"], scopedRoots: [.init(id: "workspace", directory: workspace.path)],
            processPrograms: [.init(name: "codex", executable: fixture.codex, version: "codex-cli 0.166.0",
                sha256: fixture.codexSHA256, prefixArguments: fixture.prefixArguments)],
            localLaunch: .init(executableSHA256: fixture.gtermSHA256, workingDirectory: workspace.path,
                temporaryDirectory: fixture.temporary, authorized: true))
        if let issue = model.extensionRunIssue {
            // This is before Codex is spawned or any model call: only Native
            // launch/handshake diagnostics, with selected account paths removed.
            let redacted = issue.replacingOccurrences(of: root.path, with: "<fixture>")
                .replacingOccurrences(of: env["HOME"] ?? "\u{0}", with: "<account-home>")
            print("CODEX LIVE APP LAUNCH: \(redacted)")
        }
        try require(model.extensionRunIssue == nil &&
            model.extensionRun?.session?.phase == ExtensionSession.Phase.consent.rawValue, "NATIVE_CONSENT_READY")
        try require(model.codexThreadID == nil && !FileManager.default.fileExists(atPath: target.path), "NO_PRECONSENT_EFFECT")
        try await ui.press("批准并启动")
        try await wait("NATIVE_RUNTIME_ACTIVE") {
            model.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
        }
        try await ui.press("启动Codex对话")
        try await wait("REAL_THREAD_READY") { model.codexThreadID != nil && !model.codexCallBusy }
        try require(model.extensionRunIssue == nil, "REAL_THREAD_START_FAILED")
        let thread = try XCTUnwrap(model.codexThreadID)
        // No model request before the real backend confirms the selected scope.
        try await wait("OBSERVED_BACKEND_POLICY") {
            model.codexTimeline.contains { entry in
                guard entry.title == "ghosterm/threadConfigured",
                      let value = try? JSONSerialization.jsonObject(with: Data(entry.detail.utf8)) as? [String: Any] else { return false }
                return value["threadId"] as? String == thread && value["sandboxType"] as? String == "workspaceWrite" &&
                    value["approvalPolicy"] as? String == "on-request" && value["approvalsReviewer"] as? String == "user"
            }
        }
        if fixture.prepareOnly {
            try await ui.press("停止并释放插件")
            try await wait("NATIVE_PREFLIGHT_RELEASED") { model.extensionRun == nil }
            let state = try await controller.localProcessState()
            print("CODEX LIVE APP STOP: reaped=\(state?.reaped == true) exit=\(state?.exitCode ?? -999) signal=\(state?.termSignal ?? -999) issue=\(model.extensionRunIssue != nil)")
            try require(model.extensionRunIssue == nil && state?.reaped == true &&
                        [Int32(0), Int32(130)].contains(state?.exitCode ?? -1) && state?.termSignal == nil,
                        "NATIVE_PREFLIGHT_STOP_FAILED")
            print("PASS CODEX LIVE APP PREFLIGHT: Native consent/thread/policy/stop; no model turn")
            return
        }
        print("CODEX LIVE APP STEP: model turn requested")
        model.codexMessageDraft = "Use apply_patch to add exactly one file at \(target.path), containing exactly \(fixture.nonce) followed by one newline. This isolated target is deliberately outside the writable current workspace, so request file-change approval. The user authorizes only this exact proposed file. Do not use shell commands, read other files, create other files or use subagents. Then reply with exactly \(fixture.nonce)."
        try await ui.press("发送")
        try await wait("REAL_FILE_APPROVAL", seconds: 180) {
            !model.codexPendingApprovals.isEmpty || model.extensionRunIssue != nil ||
                (!model.codexTurnActive && model.codexTimeline.contains { $0.title == "Turn 已完成" })
        }
        try require(model.extensionRunIssue == nil && model.codexPendingApprovals.count == 1, "ONE_FILE_APPROVAL_REQUIRED")
        print("CODEX LIVE APP STEP: one approval received")
        let approval = try XCTUnwrap(model.codexPendingApprovals.first)
        try require(approval.method == "item/fileChange/requestApproval" && approval.identity?.threadID == thread,
                    "EXACT_FILE_APPROVAL_METHOD_AND_THREAD")
        let parameters = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(approval.paramsJSON.utf8)) as? [String: Any])
        if let grantRoot = parameters["grantRoot"] as? String, !grantRoot.isEmpty {
            try require(URL(fileURLWithPath: grantRoot).standardizedFileURL.path == target.deletingLastPathComponent().path,
                        "ONLY_OWNED_FILE_GRANT_ROOT")
        }
        try require(!FileManager.default.fileExists(atPath: target.path) && model.codexApprovalSnapshot(approval) == nil,
                    "NO_WRITE_OR_ALLOW_BEFORE_FULL_REVIEW")
        try await wait("PROPOSED_CHANGE_REFERENCE") { model.codexTimeline.contains { $0.changeReference != nil } }
        try await ui.press("读取完整补丁")
        try await wait("COMPLETE_REAL_CHANGE_DOCUMENT") { model.codexChangeReview?.document != nil }
        let document = try XCTUnwrap(model.codexChangeReview?.document)
        try require(document.changes.count == 1 && document.threadId == thread &&
                    document.turnId == approval.identity?.turnID && document.itemId == approval.identity?.itemID,
                    "COMPLETE_CHANGE_IDENTITY")
        let change = document.changes[0]
        let path = URL(fileURLWithPath: change.path, relativeTo: workspace).standardizedFileURL.path
        try require(path == target.path && change.kind.type == "add" && change.diff.contains(fixture.nonce), "EXACT_OWNED_CHANGE")
        print("CODEX LIVE APP STEP: full owned patch read")
        // Pinned app-server format_file_change_diff returns the full content
        // for Add/Delete; only Update is a unified diff. Compare every byte.
        try require(change.diff.utf8.elementsEqual((fixture.nonce + "\n").utf8), "ONLY_AUTHORIZED_FILE_BYTES")
        try await wait("NATIVE_COMPLETE_PATCH_TEXT") {
            ui.textViews().contains { $0.identifier?.rawValue == "plugin-codex-full-diff-text" &&
                !$0.isEditable && $0.isSelectable && $0.string.utf8.elementsEqual(change.diff.utf8) }
        }
        try require(model.codexApprovalSnapshot(approval) == model.codexChangeReview?.reference.snapshotID,
                    "CURRENT_REVIEWED_APPROVAL_SNAPSHOT")
        try await ui.press("批准一次")
        try await wait("REAL_APPROVED_FILE_AND_TURN", seconds: 180) {
            model.extensionRunIssue != nil || (!model.codexTurnActive && !model.codexCallBusy &&
                model.codexPendingApprovals.isEmpty && FileManager.default.fileExists(atPath: target.path))
        }
        diagnose(model)
        try require(model.extensionRunIssue == nil, "NATIVE_TURN_FAILED")
        try require(try Data(contentsOf: target) == Data((fixture.nonce + "\n").utf8), "ACTUAL_EXACT_APPROVED_FILE")
        try require(model.codexTimeline.contains { $0.title == "Codex" && $0.detail.contains(fixture.nonce) } &&
            model.codexTimeline.contains { $0.title == "Turn 已完成" && $0.detail.hasSuffix("completed") }, "CORRELATED_SUCCESSFUL_MODEL_REPLY")
        try require(try FileManager.default.contentsOfDirectory(atPath: target.deletingLastPathComponent().path) == [target.lastPathComponent],
                    "ONLY_EXPECTED_TARGET_FILE")
        try require(try FileManager.default.contentsOfDirectory(atPath: workspace.path).isEmpty,
                    "NO_ADDITIONAL_WORKSPACE_FILES")
        try await ui.press("停止并释放插件")
        try await wait("NATIVE_SESSION_RELEASED") { model.extensionRun == nil }
        try require(model.extensionRunIssue == nil && model.codexChangeReview == nil, "NATIVE_STOP_FAILED")
        let state = try await controller.localProcessState()
        print("CODEX LIVE APP STOP: reaped=\(state?.reaped == true) exit=\(state?.exitCode ?? -999) signal=\(state?.termSignal ?? -999) issue=\(model.extensionRunIssue != nil)")
        // Explicit Native stop can return the CLI's cooperative cancellation
        // code 130. Signal death or an unreaped owner is still a failure.
        try require(state?.reaped == true && [Int32(0), Int32(130)].contains(state?.exitCode ?? -1) &&
                    state?.termSignal == nil, "LOCAL_EXECUTOR_REAPED_SUCCESSFULLY")
        print("PASS CODEX LIVE APP: Native consent/thread/send/full-read/approve/reply/stop; one owned file; selected account unchanged")
    }
}
