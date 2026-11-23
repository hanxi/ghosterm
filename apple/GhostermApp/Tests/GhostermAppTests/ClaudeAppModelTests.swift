import AppKit
import CryptoKit
import Darwin
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Native Claude controls over owned SSH and the Lua package with a no-model CLI fixture.
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
              let gterm = env["GHOSTERM_TEST_GTERM"], let program = env["GHOSTERM_TEST_CLAUDE_PROGRAM"],
              let packagePath = env["GHOSTERM_TEST_CLAUDE_PACKAGE"], let scratch = env["GHOSTERM_APP_TEST_TMP"],
              let host = env["GHOSTERM_SSH_HOST"], let port = env["GHOSTERM_SSH_PORT"],
              let user = env["GHOSTERM_SSH_USER"], let key = env["GHOSTERM_SSH_KEY"] else {
            throw XCTSkip("requires explicitly enabled Core and the owned Claude CLI/SSH fixture")
        }
        if jump && env["GHOSTERM_SSH_JUMP_HOST"] == nil { throw XCTSkip("owned jump fixture is required") }
        var template = URL(fileURLWithPath: scratch).appendingPathComponent("claude-XXXXXX").path.utf8CString
        let fixturePath = template.withUnsafeMutableBufferPointer { buffer in
            buffer.baseAddress.flatMap { mkdtemp($0) }.map { String(cString: $0) }
        }
        let root = URL(fileURLWithPath: try XCTUnwrap(fixturePath))
        for name in ["w", "h"] {
            try FileManager.default.createDirectory(at: root.appendingPathComponent(name), withIntermediateDirectories: true)
        }
        let workspace = root.appendingPathComponent("w"), package = URL(fileURLWithPath: packagePath)
        let selectedEnvironment = env["GHOSTERM_TEST_CLAUDE_SELECTED_ENV"] == "1"
        let disconnect = env["GHOSTERM_TEST_CLAUDE_DISCONNECT"] == "1"
        let disconnectMarker = root.appendingPathComponent("disconnect")
        let environmentMarker = workspace.appendingPathComponent("environment-approved")
        func quote(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        let executable: URL
        if selectedEnvironment {
            executable = root.appendingPathComponent("p")
            let check = """
                #!/bin/sh
                if [ "${1-}" != --version ]; then
                    [ "${GT_SELECTED-}" = fixture-only ] && [ -z "${GT_AMBIENT+x}" ] || exit 91
                    : > \(quote(environmentMarker.path))
                fi
                exec \(quote(program)) "$@"

                """
            try Data(check.utf8).write(to: executable)
            try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: executable.path)
        } else {
            executable = URL(fileURLWithPath: program)
        }
        let digest = SHA256.hash(data: try Data(contentsOf: executable))
            .map { String(format: "%02x", $0) }.joined()
        let launcher = root.appendingPathComponent("g")
        let policy = "(version 1)(allow default)(deny network*)(deny mach-lookup (global-name \"com.apple.securityd\"))"
        let relay = "/usr/bin/env -i HOME=" + quote(root.appendingPathComponent("h").path) +
            " PATH=" + quote(env["PATH"] ?? "/usr/bin:/bin") + " TMPDIR=" + quote(scratch) +
            (selectedEnvironment ? " GT_SELECTED=fixture-only GT_AMBIENT=must-not-cross" : "") +
            " /usr/bin/sandbox-exec -p " + quote(policy) + " " + quote(gterm) + " \"$@\""
        let script: String
        if disconnect {
            script = """
                #!/bin/sh
                exec 3<&0
                \(relay) <&3 3<&- &
                child=$!
                exec 3<&-
                trap 'kill -TERM "$child" 2>/dev/null; wait "$child"; exit' HUP INT TERM
                while kill -0 "$child" 2>/dev/null; do
                    if [ -f \(quote(disconnectMarker.path)) ]; then
                        kill -TERM "$child" 2>/dev/null
                        break
                    fi
                    /bin/sleep 0.05
                done
                wait "$child"

                """
        } else {
            script = "#!/bin/sh\nexec " + relay + "\n"
        }
        try Data(script.utf8).write(to: launcher); try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: launcher.path)
        var config = "Host claude-fixture\n HostName \(host)\n Port \(port)\n User \(user)\n IdentityFile \(key)\n"
        if jump { config += " ProxyJump claude-jump\nHost claude-jump\n HostName \(env["GHOSTERM_SSH_JUMP_HOST"]!)\n Port \(env["GHOSTERM_SSH_JUMP_PORT"]!)\n User \(user)\n IdentityFile \(key)\n" }
        let configURL = root.appendingPathComponent("ssh_config"), known = root.appendingPathComponent("known_hosts")
        try Data(config.utf8).write(to: configURL); try Data().write(to: known)
        let model = isolatedAppModel(configPath: configURL.path, knownHostsPath: known.path)
        let controller = try XCTUnwrap(model.extensionController)
        let interaction = CodexRouteInteraction(); model.interaction = interaction
        let pane = NSHostingView(rootView: PluginWorkspaceTestView(model: model).background(Color(nsColor: .windowBackgroundColor)))
        let probe = ConversationUIProbe(root: pane, width: 1440, height: 2600)
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        func button(_ id: String, enabled: Bool = true) async throws -> NSObject {
            var found: NSObject?
            try await wait("Native control unavailable: \(id)") {
                pane.layoutSubtreeIfNeeded()
                found = PluginTableRendering.elements(pane).first {
                    PluginTableRendering.attribute($0, "accessibilityIdentifier") == id
                }
                guard let control = found else { return false }
                let selector = NSSelectorFromString("isAccessibilityEnabled")
                guard control.responds(to: selector), let implementation = control.method(for: selector) else { return false }
                typealias Enabled = @convention(c) (AnyObject, Selector) -> Bool
                return unsafeBitCast(implementation, to: Enabled.self)(control, selector) == enabled
            }
            return try XCTUnwrap(found, id)
        }
        func press(_ id: String) async throws {
            let control = try await button(id)
            XCTAssertTrue(PluginTableRendering.press(control), id)
        }

        addTeardownBlock {
            await model.stopExtensionRun(); _ = await controller.close()
            await MainActor.run { probe.close() }; try? FileManager.default.removeItem(at: root)
        }
        let review = try await controller.review(directory: package)
        let approval = try await controller.approve(id: review.id, digest: review.digest); XCTAssertEqual(approval, 0)
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "ai claude", arguments: [],
            sshAlias: "claude-fixture", executable: launcher.path, executorDirectory: package.path, executorDigest: review.digest,
            methods: ClaudeRPCClient.methods, hostMethods: [], scopedRoots: [.init(id: "workspace", directory: workspace.path)],
            processPrograms: [.init(name: "claude", executable: executable.path, version: "2.1.323", sha256: digest)],
            processEnvironmentNames: selectedEnvironment ? ["GT_SELECTED"] : [])
        XCTAssertNil(model.extensionRunIssue)
        guard model.extensionRunIssue == nil else { return }
        XCTAssertEqual(interaction.hosts.count, jump ? 2 : 1)
        try await press("plugin-consent-allow")
        try await wait("Native Claude adapter was not attached to the active Runtime") { model.claudeConversation != nil }
        let conversation = try XCTUnwrap(model.claudeConversation)
        try await press("plugin-claude-start")
        try await wait("Claude did not reach ready") { conversation.phase == .ready || conversation.issue != nil }
        XCTAssertNil(conversation.issue)
        guard conversation.issue == nil else { return }
        conversation.draft = "explicit CLI fixture 中文😀"
        try await press("plugin-claude-send")
        if selectedEnvironment && env["GHOSTERM_TEST_CLAUDE_ENV_PROBE"] == "1" {
            try await wait("Scoped process environment did not produce an outcome") {
                FileManager.default.fileExists(atPath: environmentMarker.path) || conversation.issue != nil ||
                    conversation.entries.contains { $0.title == "本轮已完成" || $0.title == "本轮未成功" }
            }
            XCTAssertTrue(FileManager.default.fileExists(atPath: environmentMarker.path),
                          "Approved CLI did not receive the scoped environment")
            return
        }
        if env["GHOSTERM_TEST_CLAUDE_LOGGED_OUT"] == "1" {
            try await wait("Official CLI authentication outcome did not arrive") {
                conversation.entries.contains { $0.title == "本轮未成功" } || conversation.issue != nil
            }
            XCTAssertNil(conversation.issue)
            XCTAssertTrue(conversation.entries.contains { $0.title == "本轮未成功" && $0.text.contains("Not logged in") })
            XCTAssertFalse(conversation.entries.contains { $0.title == "本轮已完成" })
            XCTAssertNil(conversation.activeMessageID)
            return
        }
        try await wait("Correlated result did not arrive") {
            conversation.entries.contains { $0.title == "本轮已完成" || (selectedEnvironment && $0.title == "本轮未成功") } ||
                conversation.issue != nil
        }
        if selectedEnvironment {
            let approved = FileManager.default.fileExists(atPath: environmentMarker.path)
            XCTAssertTrue(approved, "Approved CLI did not receive the scoped environment")
            guard approved else { return }
        }
        XCTAssertNil(conversation.issue)
        guard conversation.issue == nil else { return }
        XCTAssertTrue(conversation.entries.contains { $0.title == "Claude" && $0.text == String(repeating: "完整响应😀", count: 400) })
        XCTAssertTrue(conversation.activeMessageID == nil && conversation.pending.isEmpty)
        conversation.draft = "interrupt this owned fixture"
        try await press("plugin-claude-send")
        try await wait("Fixture did not receive the second input") {
            FileManager.default.fileExists(atPath: workspace.appendingPathComponent("waiting").path)
        }
        try await press("plugin-claude-interrupt")
        try await wait("Interrupt did not settle") { conversation.phase == .ready || conversation.issue != nil }
        XCTAssertNil(conversation.issue)
        XCTAssertNil(conversation.activeMessageID)
        XCTAssertTrue(conversation.entries.contains { $0.title == "本轮未成功" && $0.text == "Claude CLI interrupted" })
        conversation.draft = "stop this owned fixture"
        try await press("plugin-claude-send")
        try await wait("Fixture did not receive the input to stop") {
            FileManager.default.fileExists(atPath: workspace.appendingPathComponent("waiting-stop").path)
                && conversation.canInterrupt
        }
        if disconnect {
            let pendingID = try XCTUnwrap(conversation.activeMessageID)
            try Data().write(to: disconnectMarker, options: .atomic)
            try await wait("Disconnect did not settle the pending input") {
                conversation.phase == .unknown || conversation.phase == .lost
            }
            XCTAssertEqual(conversation.activeMessageID, pendingID)
            XCTAssertTrue(conversation.issue?.contains("未知") == true || conversation.issue?.contains("未确认") == true)
            XCTAssertFalse(conversation.canInterrupt)
            await model.stopExtensionRun()
            XCTAssertNil(model.extensionRun)
            XCTAssertNil(model.extensionRunIssue)
            return
        }
        try await press("plugin-claude-stop")
        try await wait("Explicit Claude stop did not finish") { conversation.phase == .closed || conversation.issue != nil }
        XCTAssertEqual(conversation.phase, .closed)
        XCTAssertTrue(conversation.issue?.contains("结果尚未确认") == true)
        try await press("plugin-stop")
        try await wait("Native plugin owners were not released") { model.extensionRun == nil || model.extensionRunIssue != nil }
        XCTAssertNil(model.extensionRunIssue)
    }
}
