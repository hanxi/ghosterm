import AppKit
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel

@MainActor final class DefaultRemoteSessionTests: XCTestCase {
    private func until(_ predicate: () -> Bool, file: StaticString = #filePath, line: UInt = #line) async throws {
        let deadline = Date().addingTimeInterval(20)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), file: file, line: line)
    }
    func testOrdinarySSHEntryAndSplitUseExistingAuthenticationWithoutInstallation() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let host = env["GHOSTERM_SSH_HOST"], let port = env["GHOSTERM_SSH_PORT"],
              let user = env["GHOSTERM_SSH_USER"], let key = env["GHOSTERM_SSH_KEY"] else {
            throw XCTSkip("requires real sshd GHOSTERM_SSH_* fixture")
        }
        _ = NSApplication.shared
        let directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let config = directory.appendingPathComponent("config"), known = directory.appendingPathComponent("known_hosts")
        try "Host ordinary\n HostName \(host)\n Port \(port)\n User \(user)\n IdentityFile \(key)\n".write(to: config, atomically: true, encoding: .utf8)
        try Data().write(to: known)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: known.path)
        defer { model.disconnectForTesting() }
        let interaction = OrdinarySSHInteraction(); model.interaction = interaction
        model.connect(to: .host("ordinary"))
        guard let original = model.activePane else { return XCTFail("missing SSH pane") }
        try await until { original.status == .ready || original.status.isFailed }
        XCTAssertEqual(original.status, .ready)
        XCTAssertTrue(original.isRunning)
        let terminal = try TerminalApp.shared()
        func requestProcess(_ pane: TerminalPane) {
            pane.surface?.sendCommandLine("printf 'ORDINARY-%s:%s\\n' SSH \"$$\"")
        }
        func process(_ pane: TerminalPane) -> Int32? {
            terminal.tick()
            guard let text = pane.surface?.readText(),
                  let range = text.range(of: "ORDINARY-SSH:[0-9]+", options: .regularExpression) else { return nil }
            return Int32(text[range].dropFirst("ORDINARY-SSH:".count))
        }
        requestProcess(original)
        try await until { process(original) != nil }
        let originalPID = process(original)
        model.splitActiveTab(axis: .horizontal)
        guard let next = model.activePane else { return XCTFail("missing SSH split") }
        try await until { next.status == .ready || next.status.isFailed }
        XCTAssertEqual(next.status, .ready)
        requestProcess(next)
        try await until { process(next) != nil }
        XCTAssertNotEqual(process(next), originalPID); XCTAssertEqual(process(original), originalPID)
        model.openSFTP(for: "ordinary")
        try await until { model.remoteFiles?.panel != nil }
        XCTAssertEqual(interaction.hostkeys, 1); XCTAssertEqual(interaction.credentials, 0)
        XCTAssertTrue(FileManager.default.fileExists(atPath: known.path), "复用已接受的 known_hosts")
        print("PASS ordinary SSH/default split/SFTP: real libssh2 PTY, independent processes, shared hostkey/auth and no installer")
    }
}

private final class OrdinarySSHInteraction: AppInteraction {
    var hostkeys = 0, credentials = 0
    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision {
        XCTAssertTrue(Thread.isMainThread); hostkeys += 1; return .accept
    }
    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer? {
        credentials += 1; XCTFail("unencrypted fixture key must not prompt"); return nil
    }
}
