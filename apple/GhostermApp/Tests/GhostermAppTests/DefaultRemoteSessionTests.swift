import AppKit
import Darwin
import Foundation
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

@MainActor final class DefaultRemoteSessionTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }
    private func until(timeout: TimeInterval = 20, file: StaticString = #filePath, line: UInt = #line, _ predicate: () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), "default Linux flow did not settle", file: file, line: line)
    }
    private func relay(_ command: String, path: String) async throws {
        try await withCheckedThrowingContinuation { (completion: CheckedContinuation<Void, Error>) in
            DispatchQueue.global().async {
                let fd = Darwin.socket(AF_UNIX, SOCK_STREAM, 0)
                guard fd >= 0 else { completion.resume(throwing: NSError(domain: "relay socket", code: Int(errno))); return }
                defer { Darwin.close(fd) }
                var timeout = timeval(tv_sec: 2, tv_usec: 0)
                _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout.size(ofValue: timeout)))
                _ = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, socklen_t(MemoryLayout.size(ofValue: timeout)))
                var address = sockaddr_un(); address.sun_family = sa_family_t(AF_UNIX)
                address.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
                let bytes = Array(path.utf8) + [0]
                guard bytes.count <= MemoryLayout.size(ofValue: address.sun_path) else { completion.resume(throwing: NSError(domain: "relay path", code: 1)); return }
                withUnsafeMutableBytes(of: &address.sun_path) { $0.copyBytes(from: bytes + Array(repeating: 0, count: $0.count - bytes.count)) }
                let connected = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) } }
                guard connected == 0 else { completion.resume(throwing: NSError(domain: "relay connect", code: Int(errno))); return }
                let sent = command.withCString { Darwin.write(fd, $0, strlen($0)) }
                var reply = [UInt8](repeating: 0, count: 2)
                let received = reply.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
                guard sent == command.utf8.count, received == 2, reply == Array("OK".utf8) else { completion.resume(throwing: NSError(domain: "relay acknowledgement", code: Int(errno))); return }
                completion.resume()
            }
        }
    }
    private func pid(_ surface: TerminalSurface?) -> Int32? {
        for text in surface?.readText()?.components(separatedBy: .newlines) ?? [] {
            let line = text.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("REMOTE-PID:"), let pid = Int32(line.dropFirst(11)) { return pid }
        }
        return nil
    }
    func testLinuxDefaultEntryInstallDetachStartupRestoreAndStop() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_REPLICA_SSH_PORT"].flatMap(UInt16.init), let key = env["GHOSTERM_REPLICA_SSH_KEY"],
              let root = env["GHOSTERM_SESSION_SETUP_ROOT"], let runtime = env["GHOSTERM_REPLICA_RUNTIME"],
              let expectedFingerprint = env["GHOSTERM_REPLICA_FINGERPRINT"] else { throw XCTSkip("requires dedicated Linux default-entry fixture") }
        _ = NSApplication.shared
        let directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let config = directory.appendingPathComponent("ssh_config"), known = directory.appendingPathComponent("known_hosts")
        let user = env["GHOSTERM_REPLICA_SSH_USER"] ?? NSUserName()
        try "Host linux\n HostName 127.0.0.1\n Port \(port)\n User \(user)\n IdentityFile \(key)\n".write(to: config, atomically: true, encoding: .utf8)
        XCTAssertTrue(FileManager.default.createFile(atPath: known.path, contents: Data(), attributes: [.posixPermissions: 0o600]))
        let records = directory.appendingPathComponent("records")
        let interaction = DefaultLinuxInteraction(expectedFingerprint)
        func model(runtimeOverride: String? = nil) -> AppModel {
            let model = isolatedAppModel(configPath: config.path, knownHostsPath: known.path, sessionDirectory: records,
                remoteInstallRoot: root, remoteRuntimeDirectory: runtimeOverride ?? runtime)
            model.interaction = interaction; return model
        }
        var first: AppModel? = model()
        let host = makeRootHostingView(model: first!, initialShowsSidebar: true)
        host.frame = NSRect(x: 0, y: 0, width: 900, height: 600)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.contentView = host; TestWindows.presentOffscreenAsKey(window)
        defer { window.orderOut(nil); window.contentView = nil }
        first!.connect(to: .host("linux"))
        try await until(timeout: 150) { first!.activePane?.status == .ready || first!.activePane?.status.isFailed == true }
        guard first!.activePane?.status == .ready, let original = first!.surface, let pane = first!.activePane else { return XCTFail("default Linux launch: \(first!.status)") }
        try await until { original.view.window === window }
        original.sendCommittedText("cd /usr; printf 'REMOTE-PID:%s\\n' \"$$\"\r")
        try await until { pid(original) != nil && pane.currentDirectory == "/usr" && (pane.sessionRecord?.session ?? 0) > 0 }
        guard let originalPID = pid(original), let saved = pane.sessionRecord, let tab = first!.tabs.activeTab else { return XCTFail("original Linux identity missing") }
        XCTAssertEqual(saved.target?.identityPaths, [key]); XCTAssertEqual(saved.endpoint.runtime, runtime)
        XCTAssertTrue(saved.endpoint.proxy.hasPrefix(root + "/releases/")); XCTAssertTrue(saved.admitted)
        original.sendCommittedText("printf 'REMOTE-SHELL:%s\\n' \"$0\"; shopt -q login_shell && printf 'REMOTE-LOGIN:yes\\n'\r")
        try await until {
            let lines = original.readText()?.components(separatedBy: .newlines).map { $0.trimmingCharacters(in: .whitespaces) } ?? []
            return lines.contains("REMOTE-SHELL:/bin/bash") && lines.contains("REMOTE-LOGIN:yes")
        }
        XCTAssertTrue(original.readText()?.components(separatedBy: .newlines).contains { $0.trimmingCharacters(in: .whitespaces) == "REMOTE-LOGIN:yes" } == true, "dedicated fixture account's bash must be a login shell")
        XCTAssertTrue(first!.shouldTerminate(), "GUI quit retains the remote Source")
        if let control = env["GT_TEST_RELAY_CONTROL"] {
            try await relay("reset", path: control)
            try await until(timeout: 40) { pane.status == .ready && pane.surface !== original }
            guard let afterReset = pane.surface else { return XCTFail("reset surface missing") }
            XCTAssertEqual(pid(afterReset), originalPID)
            XCTAssertEqual(pane.sessionRecord?.session, saved.session)
            try await relay("blackhole", path: control)
            try await until(timeout: 40) { if case .recovering = pane.status { return true }; return false }
            afterReset.sendCommittedText("echo NETWORK-STALE-MUST-NOT-EXECUTE\r")
            try await relay("online", path: control)
            try await until(timeout: 40) { pane.status == .ready && pane.surface !== afterReset }
            XCTAssertEqual(pid(pane.surface), originalPID)
            XCTAssertEqual(pane.sessionRecord?.instance, saved.instance)
            XCTAssertFalse(pane.surface?.readText()?.contains("NETWORK-STALE-MUST-NOT-EXECUTE") == true)
            print("PASS default application SSH reset and blackhole: original PID/session, fresh Surface and stale input rejected")
        }
        first!.detachKeepingSession(tab)
        try await until { first!.tabs.tabs.isEmpty && first!.savedSessions.contains { $0.id == saved.id } }
        window.contentView = nil; first = nil
        // A changed default for new sessions must not redirect a saved session to a different socket.
        let second = model(runtimeOverride: runtime + "/new-session"); defer { second.disconnectForTesting() }
        let restored = makeRootHostingView(model: second, initialShowsSidebar: true)
        restored.frame = host.frame; window.contentView = restored
        second.startInitialSessions(); second.startInitialSessions()
        try await until(timeout: 150) { second.activePane?.status == .ready || second.activePane?.status.isFailed == true }
        guard second.activePane?.status == .ready, let surface = second.surface else { return XCTFail("default Linux restore: \(second.status)") }
        try await until { surface.view.window === window && surface !== original && second.activePane?.currentDirectory == "/usr" }
        XCTAssertEqual(second.activePane?.sessionRecord?.instance, saved.instance); XCTAssertEqual(second.activePane?.sessionRecord?.session, saved.session)
        XCTAssertEqual(pid(surface), originalPID)
        original.sendCommittedText("echo STALE-MUST-NOT-EXECUTE\r")
        surface.sendCommittedText("printf 'DEFAULT-RESTORED\\n'\r")
        try await until { surface.readText()?.components(separatedBy: .newlines).contains { $0.trimmingCharacters(in: .whitespaces) == "DEFAULT-RESTORED" } == true }
        XCTAssertFalse(surface.readText()?.contains("STALE-MUST-NOT-EXECUTE") == true)
        let contents = try String(contentsOf: records.appendingPathComponent("sessions.json"), encoding: .utf8)
        XCTAssertFalse(contents.contains("PRIVATE KEY")); XCTAssertFalse(contents.contains("REMOTE-PID"))
        XCTAssertTrue(contents.contains(saved.id.uuidString), "终止前这条记录必须还在存储上（清理的对拍基线）")
        XCTAssertEqual(interaction.hostkeys, 1); XCTAssertEqual(interaction.credentials, 0)
        second.disconnectForTesting()
        // 远端 Source 被回收之后，这条记录既打不开也不该留着：列表与存储都要空。
        func durableGone() -> Bool {
            let url = records.appendingPathComponent("sessions.json")
            guard let text = try? String(contentsOf: url, encoding: .utf8) else {
                return !FileManager.default.fileExists(atPath: url.path)   // 读不出又不在盘上＝已清掉
            }
            return !text.contains(saved.id.uuidString)
        }
        try await until(timeout: 40) { !second.savedSessions.contains { $0.id == saved.id } && durableGone() }
        XCTAssertNil(second.sessionError)
        print("PASS default Linux entry: application connect/prebuilt installation, nested cold runtime, real RootView, original PID/cwd after startup restore, stale input rejected and observed termination")
    }
}
private final class DefaultLinuxInteraction: AppInteraction {
    let expected: String
    var hostkeys = 0, credentials = 0
    init(_ expected: String) { self.expected = expected }
    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision {
        hostkeys += 1; XCTAssertTrue(Thread.isMainThread); XCTAssertEqual(prompt.fingerprint, expected)
        return prompt.fingerprint == expected ? .accept : .reject
    }
    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer? { credentials += 1; XCTFail("unencrypted fixture key must not prompt"); return nil }
}
