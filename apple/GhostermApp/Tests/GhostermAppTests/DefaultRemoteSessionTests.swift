import AppKit
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel

@MainActor final class DefaultRemoteSessionTests: XCTestCase {
    private struct WaitTimeout: Error, CustomStringConvertible {
        let description: String
    }

    private func until(_ description: String, _ predicate: () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(20)
        while !predicate() {
            guard Date() < deadline else { throw WaitTimeout(description: description) }
            try await Task.sleep(nanoseconds: 10_000_000)
        }
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
        try await until("opening the original SSH pane") { original.status == .ready || original.status.isFailed }
        XCTAssertEqual(original.status, .ready)
        XCTAssertTrue(original.isRunning)
        let terminal = try TerminalApp.shared()
        func requestProcess(_ surface: TerminalSurface) {
            surface.setSize(width: 640, height: 400)
            surface.sendCommandLine("printf 'ORDINARY-%s:%s\\n' SSH \"$$\"")
        }
        func process(_ pane: TerminalPane) -> Int32? {
            terminal.tick()
            guard let text = pane.surface?.readText(),
                  let range = text.range(of: "ORDINARY-SSH:[0-9]+", options: .regularExpression) else { return nil }
            return Int32(text[range].dropFirst("ORDINARY-SSH:".count))
        }
        requestProcess(try XCTUnwrap(original.surface))
        try await until("reading the original SSH process marker") { process(original) != nil }
        let originalPID = try XCTUnwrap(process(original))
        model.splitActiveTab(axis: .horizontal)
        guard let next = model.activePane else { return XCTFail("missing SSH split") }
        XCTAssertFalse(original === next)
        try await until("opening the SSH split pane") { next.status == .ready || next.status.isFailed }
        XCTAssertEqual(next.status, .ready)
        requestProcess(try XCTUnwrap(next.surface))
        try await until("reading the split SSH process marker") { process(next) != nil }
        XCTAssertNotEqual(try XCTUnwrap(process(next)), originalPID)
        XCTAssertEqual(process(original), originalPID)
        model.openSFTP(for: "ordinary")
        try await until("opening SFTP with the accepted SSH host key") { model.remoteFiles?.panel != nil }
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
