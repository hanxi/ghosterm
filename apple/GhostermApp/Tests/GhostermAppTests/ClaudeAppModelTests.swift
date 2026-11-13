import AppKit
import CryptoKit
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Owned SSH -> production gterm/package/Node -> real SDK with an explicitly
/// in-memory CLI control fixture. No model or actual tool execution.
@MainActor
final class ClaudeAppModelTests: XCTestCase {
    func testClaudeConversationOverDirectSSH() async throws { try await run(jump: false) }
    func testClaudeConversationOverProxyJump() async throws { try await run(jump: true) }

    private func wait(_ label: String, _ predicate: @escaping () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 45
        while !predicate() && ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(nanoseconds: 20_000_000) }
        guard predicate() else { XCTFail(label); throw ClaudeRPCClient.Failure.invalidResponse }
    }
    private func run(jump: Bool) async throws {
        let env = ProcessInfo.processInfo.environment
        guard ExtensionRuntime.supportStatus == 0,
              let gterm = env["GHOSTERM_TEST_GTERM"], let component = env["GHOSTERM_TEST_CLAUDE_COMPONENT"],
              let packagePath = env["GHOSTERM_TEST_CLAUDE_PACKAGE"], let nodePath = env["GHOSTERM_TEST_CLAUDE_NODE"],
              let host = env["GHOSTERM_SSH_HOST"], let port = env["GHOSTERM_SSH_PORT"],
              let user = env["GHOSTERM_SSH_USER"], let key = env["GHOSTERM_SSH_KEY"] else {
            throw XCTSkip("requires explicitly enabled Core and the owned Claude SDK/SSH fixture")
        }
        if jump && env["GHOSTERM_SSH_JUMP_HOST"] == nil { throw XCTSkip("owned jump fixture is required") }
        var repository = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { repository.deleteLastPathComponent() }
        let root = repository.appendingPathComponent("tmp/cv-" + UUID().uuidString.prefix(5))
        for directory in ["w", "h", "c"] { try FileManager.default.createDirectory(at: root.appendingPathComponent(directory), withIntermediateDirectories: true) }
        let workspace = root.appendingPathComponent("w"), package = root.appendingPathComponent("k")
        try FileManager.default.copyItem(at: URL(fileURLWithPath: packagePath), to: package)
        let entry = root.appendingPathComponent("e.mjs")
        try FileManager.default.createSymbolicLink(at: entry, withDestinationURL: repository.appendingPathComponent("scripts/fixtures/claude-node-sdk.mjs"))
        #if arch(arm64)
        let arch = "arm64"
        #else
        let arch = "x64"
        #endif
        let cli = URL(fileURLWithPath: component).appendingPathComponent("node_modules/@anthropic-ai/claude-agent-sdk-darwin-\(arch)/claude")
        func sha(_ url: URL) throws -> String { SHA256.hash(data: try Data(contentsOf: url)).map { String(format: "%02x", $0) }.joined() }
        let profile = root.appendingPathComponent("p")
        let selected = ["HOME", "PATH", "TMPDIR", "CLAUDE_CONFIG_DIR"]
        let profileBytes = try JSONSerialization.data(withJSONObject: ["version": 1, "cwd": workspace.path,
            "executable": cli.path, "cliVersion": "2.1.323", "sha256": try sha(cli), "environmentNames": selected, "tools": []])
        try profileBytes.write(to: profile)
        let node = URL(fileURLWithPath: nodePath)
        let version = Process(), output = Pipe()
        version.executableURL = node; version.arguments = ["--version"]; version.standardOutput = output
        version.environment = ["HOME": root.appendingPathComponent("h").path, "PATH": env["PATH"] ?? "/usr/bin:/bin", "TMPDIR": repository.appendingPathComponent("tmp").path]
        try version.run(); version.waitUntilExit(); XCTAssertEqual(version.terminationStatus, 0)
        let nodeVersion = String(decoding: output.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        func quote(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        let launcher = root.appendingPathComponent("g")
        let policy = "(version 1)(allow default)(deny network*)(deny mach-lookup (global-name \"com.apple.securityd\"))"
        let script = "#!/bin/sh\nexec /usr/bin/env -i HOME=" + quote(root.appendingPathComponent("h").path) +
            " CLAUDE_CONFIG_DIR=" + quote(root.appendingPathComponent("c").path) +
            " PATH=" + quote(env["PATH"] ?? "/usr/bin:/bin") + " TMPDIR=" + quote(repository.appendingPathComponent("tmp").path) +
            " /usr/bin/sandbox-exec -p " + quote(policy) + " " + quote(gterm) + " \"$@\"\n"
        try Data(script.utf8).write(to: launcher); try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: launcher.path)
        var config = "Host claude-fixture\n HostName \(host)\n Port \(port)\n User \(user)\n IdentityFile \(key)\n"
        if jump { config += " ProxyJump claude-jump\nHost claude-jump\n HostName \(env["GHOSTERM_SSH_JUMP_HOST"]!)\n Port \(env["GHOSTERM_SSH_JUMP_PORT"]!)\n User \(user)\n IdentityFile \(key)\n" }
        let configURL = root.appendingPathComponent("ssh_config"), known = root.appendingPathComponent("known_hosts")
        try Data(config.utf8).write(to: configURL); try Data().write(to: known)
        let model = isolatedAppModel(configPath: configURL.path, knownHostsPath: known.path)
        let controller = try XCTUnwrap(model.extensionController)
        let interaction = CodexRouteInteraction(); model.interaction = interaction
        let pane = NSHostingView(rootView: ExtensionsPane(model: model).background(Color(nsColor: .windowBackgroundColor)))
        let probe = ConversationUIProbe(root: pane, width: 1440, height: 2600)
        addTeardownBlock {
            await model.stopExtensionRun(); _ = await controller.close()
            await MainActor.run { probe.close() }; try? FileManager.default.removeItem(at: root)
        }
        let review = try await controller.review(directory: package)
        let approval = try await controller.approve(id: review.id, digest: review.digest); XCTAssertEqual(approval, 0)
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "ai claude", arguments: [],
            sshAlias: "claude-fixture", executable: launcher.path, executorDirectory: package.path, executorDigest: review.digest,
            methods: ClaudeRPCClient.methods, hostMethods: [], scopedRoots: [.init(id: "workspace", directory: workspace.path)],
            processPrograms: [.init(name: "claude-node", executable: nodePath, version: nodeVersion, sha256: try sha(node),
                prefixArguments: [entry.path, "--profile", profile.path])], processEnvironmentNames: ["CLAUDE_CONFIG_DIR"])
        XCTAssertNil(model.extensionRunIssue)
        guard model.extensionRunIssue == nil else { return }
        XCTAssertEqual(interaction.hosts.count, jump ? 2 : 1)
        try await probe.press("批准并启动")
        try await wait("Native Claude adapter was not attached to the active Runtime") { model.claudeConversation != nil }
        let conversation = try XCTUnwrap(model.claudeConversation)
        defer { print("CLAUDE_FIXTURE_STATE phase=\(conversation.phase.rawValue) busy=\(conversation.busy) ownerBusy=\(model.extensionRunBusy) runtime=\(model.extensionRun?.runtime?.phase ?? 999) issue=\(conversation.issue ?? "none") ownerIssue=\(model.extensionRunIssue ?? "none")") }
        try await probe.press("启动Claude对话")
        try await wait("SDK control initialization did not reach ready") { conversation.phase == .ready || conversation.issue != nil }
        let failureFile = workspace.appendingPathComponent("sdk-fixture-failure.txt")
        let sdkDiagnostic = (try? String(contentsOf: failureFile, encoding: .utf8)) ?? "no fixture diagnostic"
        XCTAssertNil(conversation.issue, sdkDiagnostic); guard conversation.issue == nil else { return }
        conversation.draft = "explicit Native SDK fixture 中文😀\n" + String(repeating: "long input 中文😀", count: 500)
        try await probe.press("发送")
        try await wait("Real SDK permission callback did not reach Native controls") { conversation.pending.count == 1 || conversation.issue != nil }
        XCTAssertNil(conversation.issue); guard conversation.issue == nil else { return }
        let tool = conversation.pending[0]
        XCTAssertEqual(tool.toolName, "Bash")
        try await wait("Native tool parameters were not rendered completely") { probe.textViews().contains { $0.identifier?.rawValue == "plugin-claude-tool-input" && $0.string.utf8.elementsEqual(tool.inputJSON.utf8) } }
        try await probe.press("批准一次")
        try await wait("SDK question callback did not reach Native form") { conversation.pending.first?.isQuestion == true || conversation.issue != nil }
        XCTAssertNil(conversation.issue); guard conversation.issue == nil else { return }
        let question = conversation.pending[0]; XCTAssertEqual(question.questions.count, 4)
        try await probe.press("提交回答") // disabled: no answer has been selected
        XCTAssertTrue(conversation.pending.contains { $0.reference == question.reference })
        for index in 0..<4 { try await probe.press("One", occurrence: index) }
        try await probe.press("提交回答")
        try await wait("Correlated fixture result was not observed") { conversation.phase == .ready || conversation.issue != nil }
        XCTAssertNil(conversation.issue)
        XCTAssertTrue(conversation.pending.isEmpty && conversation.activeMessageID == nil)
        XCTAssertTrue(conversation.records.contains { $0.json.contains("fixture-audit") })
        XCTAssertTrue(conversation.entries.contains { $0.title == "本轮已完成" })
        try await probe.press("停止Claude会话")
        try await wait("Explicit Node stop/reap did not finish") { conversation.phase == .closed || conversation.issue != nil }
        XCTAssertNil(conversation.issue)
        try await probe.press("停止并释放插件")
        try await wait("Native plugin owners were not released") { model.extensionRun == nil || model.extensionRunIssue != nil }
        XCTAssertNil(model.extensionRunIssue)
    }
}
