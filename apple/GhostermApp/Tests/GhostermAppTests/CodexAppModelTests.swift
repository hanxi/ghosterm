import CryptoKit
import Foundation
import GhostermCore
@testable import GhostermAppModel
import XCTest

final class CodexAppModelTests: XCTestCase {
    @MainActor
    func testCodexThreadThroughAppModelAndIsolatedSSH() async throws {
        let environment = ProcessInfo.processInfo.environment
        guard ExtensionRuntime.supportStatus == 0,
              let gterm = environment["GHOSTERM_TEST_GTERM"],
              let codexPath = environment["GHOSTERM_TEST_CODEX"],
              let packagePath = environment["GHOSTERM_TEST_CODEX_PACKAGE"],
              let host = environment["GHOSTERM_SSH_HOST"],
              let port = environment["GHOSTERM_SSH_PORT"],
              let key = environment["GHOSTERM_SSH_KEY"],
              let user = environment["GHOSTERM_SSH_USER"] else {
            throw XCTSkip("requires the explicitly configured Codex package, pinned CLI and isolated SSH fixture")
        }
        let repository = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let root = repository.appendingPathComponent("tmp/ca-" + UUID().uuidString.prefix(8))
        for directory in ["w", "h", "c", "x"] {
            try FileManager.default.createDirectory(at: root.appendingPathComponent(directory), withIntermediateDirectories: true)
        }
        var cleanupOnReturn = true
        defer { if cleanupOnReturn { try? FileManager.default.removeItem(at: root) } }
        let workspace = root.appendingPathComponent("w")
        let codex = URL(fileURLWithPath: codexPath).resolvingSymlinksInPath()
        let childEnvironment = ["HOME": root.appendingPathComponent("h").path,
            "CODEX_HOME": root.appendingPathComponent("c").path,
            "XDG_CONFIG_HOME": root.appendingPathComponent("x").path,
            "TMPDIR": repository.appendingPathComponent("tmp").path,
            "PATH": environment["PATH"] ?? "/usr/bin:/bin"]
        let version = Process()
        let output = Pipe(), diagnostics = Pipe()
        version.executableURL = codex
        version.arguments = ["--version"]
        version.environment = childEnvironment
        version.currentDirectoryURL = workspace
        version.standardOutput = output
        version.standardError = diagnostics
        try version.run()
        version.waitUntilExit()
        XCTAssertEqual(version.terminationStatus, 0)
        let reported = String(decoding: output.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        XCTAssertEqual(reported, "codex-cli 0.166.0")
        guard version.terminationStatus == 0, reported == "codex-cli 0.166.0" else { return }
        let executableDigest = SHA256.hash(data: try Data(contentsOf: codex)).map { String(format: "%02x", $0) }.joined()
        func quote(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "'\"'\"'") + "'" }
        // sshd uses the test account's shell. Reset the Executor environment before
        // gterm runs so the real CLI cannot read that account's Codex settings.
        let launcher = root.appendingPathComponent("g")
        let executorLog = root.appendingPathComponent("executor.log")
        let assignments = childEnvironment.sorted { $0.key < $1.key }.map { quote("\($0.key)=\($0.value)") }.joined(separator: " ")
        try Data("#!/bin/sh\nexec /usr/bin/env -i \(assignments) \(quote(gterm)) \"$@\" 2>\(quote(executorLog.path))\n".utf8).write(to: launcher)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: launcher.path)
        let config = root.appendingPathComponent("ssh_config")
        let knownHosts = root.appendingPathComponent("known_hosts")
        try Data("""
        Host codex-fixture
          HostName \(host)
          Port \(port)
          User \(user)
          IdentityFile \(key)
        """.utf8).write(to: config)
        try Data().write(to: knownHosts)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: knownHosts.path)
        let controller = try XCTUnwrap(model.extensionController)
        addTeardownBlock {
            await model.stopExtensionRun()
            _ = await controller.close()
            try? FileManager.default.removeItem(at: root)
        }
        cleanupOnReturn = false
        model.interaction = AcceptAllInteraction()
        let package = URL(fileURLWithPath: packagePath)
        let review = try await controller.review(directory: package)
        XCTAssertEqual(review.id, "org.ghosterm.ai-codex")
        let approved = try await controller.approve(id: review.id, digest: review.digest)
        XCTAssertEqual(approved, 0)
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest,
            command: "ai codex", arguments: [], sshAlias: "codex-fixture", executable: launcher.path,
            executorDirectory: package.path, executorDigest: review.digest,
            methods: ["codex.thread.start", "codex.turn.send", "codex.turn.interrupt", "codex.approval.respond"],
            hostMethods: ["codex.event"], scopedRoots: [.init(id: "workspace", directory: workspace.path)],
            processPrograms: [.init(name: "codex", executable: codex.path, version: reported, sha256: executableDigest)])
        XCTAssertNil(model.extensionRunIssue)
        guard model.extensionRun?.session?.phase == ExtensionSession.Phase.consent.rawValue else {
            XCTFail(model.extensionRunIssue ?? "Codex Session did not reach consent")
            return
        }
        XCTAssertNil(model.codexThreadID)
        XCTAssertTrue(model.extensionRun?.codexEvents.isEmpty == true)
        XCTAssertEqual(Set(model.extensionRun?.requiredCapabilities ?? []),
                       Set(["session.rpc", "process.spawn", "process.pipes"]),
                       "A process working directory must not add file/resource capabilities")
        await model.decideExtensionConsent(allow: true)
        let deadline = ProcessInfo.processInfo.systemUptime + 35
        while model.extensionRun?.runtime?.phase != ExtensionRuntime.Phase.active.rawValue &&
              ProcessInfo.processInfo.systemUptime < deadline && model.extensionRunIssue == nil {
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertEqual(model.extensionRun?.runtime?.phase, ExtensionRuntime.Phase.active.rawValue)
        guard model.extensionRunIssue == nil,
              model.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue else { return }
        await model.startCodexThread()
        if let issue = model.extensionRunIssue {
            let diagnostics = (try? Data(contentsOf: executorLog)).map { String(decoding: $0.prefix(4096), as: UTF8.self) } ?? ""
            XCTFail("\(issue); isolated Executor diagnostics: \(diagnostics)")
            return
        }
        XCTAssertFalse(model.codexThreadID?.isEmpty ?? true)
        func receivedThreadStarted() -> Bool {
            (model.extensionRun?.codexEvents ?? []).contains { text in
                guard let event = try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any],
                      event["kind"] as? String == "notification",
                      let notification = event["notification"] as? [String: Any] else { return false }
                return notification["method"] as? String == "thread/started"
            }
        }
        while !receivedThreadStarted() && ProcessInfo.processInfo.systemUptime < deadline && model.extensionRunIssue == nil {
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertTrue(receivedThreadStarted(), "The actual thread/started notification must reach AppModel")
        XCTAssertFalse(model.codexTimeline.isEmpty)
        XCTAssertFalse(model.codexTurnActive)
        XCTAssertTrue(model.codexPendingApprovals.isEmpty)
        await model.stopExtensionRun()
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertNil(model.extensionRun)
        XCTAssertNil(model.codexThreadID)
        XCTAssertTrue(model.codexTimeline.isEmpty)
        let closed = await controller.close()
        XCTAssertTrue(closed)
    }
}
