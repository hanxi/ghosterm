import AppKit
import Foundation
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel
@testable import GhostermApp

@MainActor final class SessionSetupTransportTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }
    private func until(timeout: TimeInterval = 15, _ predicate: () -> Bool) async throws {
        let end = Date().addingTimeInterval(timeout)
        while !predicate(), Date() < end { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), "native Linux setup flow did not reach its observable state")
    }
    func testRemotePathsDefaultToPrivateHomeAndPreserveOverrides() {
        let contract = "0123456789abcdef" + String(repeating: "a", count: 48)
        let paths = SessionSetupTransport.remotePaths(home: "/home/fixture", contract: contract, installRoot: nil, runtimeDirectory: nil)
        XCTAssertEqual(paths.installRoot, "/home/fixture/.ghosterm/sessiond")
        XCTAssertEqual(paths.runtimeDirectory, "/home/fixture/.ghosterm/runtime/0123456789abcdef")
        let savedRuntime = "/home/fixture/.local/state/ghosterm/0123456789abcdef"
        let overridden = SessionSetupTransport.remotePaths(home: "/home/fixture", contract: contract,
            installRoot: "/fixture/install", runtimeDirectory: savedRuntime)
        XCTAssertEqual(overridden.installRoot, "/fixture/install")
        XCTAssertEqual(overridden.runtimeDirectory, savedRuntime)
    }
    func testCancellationBeforeStartNeverRequestsCredentials() async throws {
        var calls = 0, completions = 0
        var result: Result<SessionSetupTransport.Installed, Swift.Error>?
        let task = SessionSetupTransport(options: .init(host: "127.0.0.1", username: "fixture"), bundles: [:], contract: String(repeating: "a", count: 64), prompts: .init(
            credential: { _, _, _ in calls += 1 }, hostkey: { _, _, _, _ in calls += 1 })) { result = $0; completions += 1 }
        task.cancel(); task.start(); task.cancel(); task.start()
        try await until { result != nil }
        guard case .failure(let error)? = result, let failure = error as? SessionSetupTransport.Failure,
              case .cancelled = failure else { return XCTFail("cancelled setup must not authenticate") }
        XCTAssertEqual(calls, 0); XCTAssertEqual(completions, 1)
    }
    func testRealLinuxInstallNativePaneResumeAndExplicitStop() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_REPLICA_SSH_PORT"].flatMap(UInt16.init),
              let key = env["GHOSTERM_REPLICA_SSH_KEY"],
              let hex = env["GHOSTERM_REPLICA_CONTRACT"], let bundle = env["GHOSTERM_SESSION_LINUX_BUNDLE"],
              let root = env["GHOSTERM_SESSION_SETUP_ROOT"], let runtime = env["GHOSTERM_REPLICA_RUNTIME"] else { throw XCTSkip("requires the dedicated Mac-to-Linux native setup fixture") }
        _ = NSApplication.shared
        let arch = env["GHOSTERM_SESSION_LINUX_ARCH"] ?? "aarch64"
        var project = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { project.deleteLastPathComponent() }
        let directory = project.appendingPathComponent("tmp/native-linux-setup-records-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        let known = directory.appendingPathComponent("known_hosts").path
        XCTAssertTrue(FileManager.default.createFile(atPath: known, contents: Data(), attributes: [.posixPermissions: 0o600]))
        let pem = try String(contentsOfFile: key, encoding: .utf8)
        let ssh = SSHOptions(host: "127.0.0.1", port: port, username: env["GHOSTERM_REPLICA_SSH_USER"] ?? NSUserName(), knownHostsPath: known)
        var credentialCalls = 0, hostkeyCalls = 0, failures: [String] = [], progress: [String] = []
        let prompts = SessionAuthenticationBroker.Prompts(credential: { _, _, done in
            XCTAssertTrue(Thread.isMainThread); credentialCalls += 1; done(.success(.privateKey(pem: pem, passphrase: nil))); done(.success(.password("duplicate-must-not-replace-key")))
        }, hostkey: { _, _, fingerprint, done in
            XCTAssertTrue(Thread.isMainThread)
            let accepted = fingerprint == env["GHOSTERM_REPLICA_FINGERPRINT"]
            hostkeyCalls += 1
            XCTAssertTrue(accepted); done(accepted ? .accept : .reject); done(accepted ? .accept : .reject)
        })
        func installer(_ completion: @escaping (Result<SessionSetupTransport.Installed, Swift.Error>) -> Void) -> SessionSetupTransport {
            SessionSetupTransport(options: ssh, bundles: [arch: URL(fileURLWithPath: bundle)], contract: hex,
                installRoot: root, runtimeDirectory: runtime, prompts: prompts, progress: { progress.append($0) }, completion: completion)
        }
        var invalid: Result<SessionSetupTransport.Installed, Swift.Error>?
        let mismatch = SessionSetupTransport(options: ssh, bundles: [arch: URL(fileURLWithPath: bundle)], contract: String(repeating: "b", count: 64),
            installRoot: root, runtimeDirectory: runtime, prompts: prompts) { invalid = $0 }
        mismatch.start(); defer { mismatch.cancel() }
        try await until(timeout: 30) { invalid != nil }
        guard case .failure(let error)? = invalid, let invalidBundle = error as? SessionSetupTransport.Failure,
              case .invalidBundle = invalidBundle else { return XCTFail("mismatched application/bundle contract must fail before installation") }
        let damaged = directory.appendingPathComponent("damaged-linux-bundle")
        try FileManager.default.copyItem(at: URL(fileURLWithPath: bundle), to: damaged)
        let table = damaged.appendingPathComponent("manifest.tsv")
        var lines = try String(contentsOf: table, encoding: .utf8).components(separatedBy: "\n")
        var entry = lines[1].components(separatedBy: "\t")
        entry[2] = String(repeating: "0", count: 64); lines[1] = entry.joined(separator: "\t")
        try lines.joined(separator: "\n").write(to: table, atomically: true, encoding: .utf8)
        var damagedResult: Result<SessionSetupTransport.Installed, Swift.Error>?
        let rejectDamaged = SessionSetupTransport(options: ssh, bundles: [arch: damaged], contract: hex,
            installRoot: root, runtimeDirectory: runtime, prompts: prompts) { damagedResult = $0 }
        rejectDamaged.start(); defer { rejectDamaged.cancel() }
        try await until(timeout: 150) { damagedResult != nil }
        guard case .failure(let damagedError)? = damagedResult, let error = damagedError as? SessionSetupTransport.Failure,
              case .installation(let rejected) = error else { return XCTFail("failed verification must not be reported as installed") }
        XCTAssertNotEqual(rejected.status, 0); XCTAssertFalse(rejected.published); XCTAssertFalse(rejected.publicationUnknown)
        var installed: Result<SessionSetupTransport.Installed, Swift.Error>?
        let initial = installer { installed = $0 }; initial.start()
        defer { initial.cancel() }
        try await until(timeout: 150) { installed != nil }
        guard case .success(let ready)? = installed else { return XCTFail("Linux installation failed: \(String(describing: installed))") }
        XCTAssertEqual(ready.linux.arch, arch); XCTAssertEqual(ready.installation.status, 0)
        XCTAssertTrue(ready.installation.published || ready.installation.reused); XCTAssertFalse(ready.installation.publicationUnknown)
        XCTAssertTrue(ready.endpoint.daemon.hasPrefix(root + "/releases/")); XCTAssertEqual(ready.endpoint.runtime, runtime)
        XCTAssertFalse(progress.isEmpty); XCTAssertEqual(ready.acceptedAttempt, 1)
        let nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
        let target = SessionRecord.Target(alias: "linux-fixture", host: ssh.host, user: ssh.username, port: port, knownHosts: known, identityPaths: [key])
        let record = SessionRecord(kind: .ssh, endpoint: ready.endpoint, target: target, nonce: nonce)
        let hash = stride(from: 0, to: hex.count, by: 2).compactMap { n -> UInt8? in
            let a = hex.index(hex.startIndex, offsetBy: n)
            return UInt8(hex[a..<hex.index(a, offsetBy: 2)], radix: 16)
        }
        var options = RecoverableSession.Options(kind: .ssh, proxy: ready.endpoint.proxy, daemon: ready.endpoint.daemon, runtime: runtime, compatibility: hash, format: 0x47545643,
            create: .init(nonce: nonce, executable: "/bin/sh", cwd: ready.linux.home,
                arguments: ["-c", "printf 'SETUP-PID:%s\\n' \"$$\"; IFS= read -r one; printf 'FIRST:%s\\n' \"$one\"; IFS= read -r two; printf 'SECOND:%s\\n' \"$two\"; IFS= read -r last"], size: .init(cols: 80, rows: 24), direct: true))
        let app = try TerminalApp.shared(), pane = TerminalPane()
        let model = isolatedAppModel(configPath: directory.path + "/ssh-config", knownHostsPath: known)
        let tab = TerminalTab(kind: .ssh(.init(pattern: target.alias, user: target.user, host: target.host, port: target.port)), pane: pane)
        model.tabs.add(tab)
        let host = makeRootHostingView(model: model, initialShowsSidebar: false)
        host.frame = NSRect(x: 0, y: 0, width: 800, height: 600)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled, .closable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.contentView = host; TestWindows.presentOffscreenAsKey(window)
        defer { pane.stop(); window.orderOut(nil) }
        func auth(_ transport: RecoverableSurfaceTransport) -> RecoverableSession.Authentication {
            SessionAuthenticationBroker(owner: transport.owner, options: ssh, prompts: prompts,
                endpoint: { [weak transport] body in transport?.withSession(body) }, failure: { failures.append($0) }).callbacks
        }
        try pane.attachRecoverable(app: app, record: record, directory: directory, options: options, authentication: auth)
        try await until { app.tick(); return pane.status == .ready && (pane.sessionRecord?.session ?? 0) > 0 || pane.status.isFailed }
        guard !pane.status.isFailed, let first = pane.surface, let saved = pane.sessionRecord, saved.session > 0 else { return XCTFail("installed Linux Source did not attach: \(pane.status)") }
        try await until { app.tick(); return first.readText()?.contains("SETUP-PID:") == true && first.view.window === window }
        let pid = first.readText()?.components(separatedBy: .newlines).first { $0.contains("SETUP-PID:") }
        try await until { app.tick(); return pane.sourceDirectory == ready.linux.home && pane.childState == .idle }
        XCTAssertNotNil(pid); XCTAssertGreaterThanOrEqual(credentialCalls, 2, "setup and long-lived SSH must authenticate independently")
        var reused: Result<SessionSetupTransport.Installed, Swift.Error>?
        let repeatInstall = installer { reused = $0 }; repeatInstall.start()
        defer { repeatInstall.cancel() }
        try await until(timeout: 150) { app.tick(); return reused != nil }
        guard case .success(let existing)? = reused else { return XCTFail("existing release probe failed") }
        XCTAssertTrue(existing.installation.reused); XCTAssertEqual(existing.endpoint, ready.endpoint)
        first.sendCommittedText("after-install\r")
        try await until { app.tick(); return first.readText()?.contains("FIRST:after-install") == true }
        XCTAssertTrue(first.readText()?.contains(pid ?? "missing PID") == true)
        pane.stop(); options.create = nil; options.instance = saved.instance; options.session = saved.session
        try pane.attachRecoverable(app: app, record: saved, directory: directory, options: options, authentication: auth)
        try await until { app.tick(); return pane.surface !== first && pane.status == .ready || pane.status.isFailed }
        guard !pane.status.isFailed, let second = pane.surface, second !== first else { return XCTFail("Linux pane did not resume") }
        try await until { app.tick(); return second.view.window === window && first.view.window == nil }
        XCTAssertTrue(second.readText()?.contains(pid ?? "missing PID") == true)
        XCTAssertEqual(model.tabs.activeTab?.id, tab.id); XCTAssertEqual(tab.activePane?.id, pane.id)
        first.sendCommittedText("stale\r"); second.sendCommittedText("after-resume\r")
        try await until { app.tick(); return second.readText()?.contains("SECOND:after-resume") == true }
        XCTAssertFalse(second.readText()?.contains("SECOND:stale") == true)
        pane.stop()
        var stopped: Result<SessionControlTransport.Outcome, Swift.Error>?
        let stop = SessionControlTransport(record: saved, directory: directory, operation: .terminate) { stopped = $0 }
        let stopAuth = SessionAuthenticationBroker(owner: stop.owner, options: ssh, prompts: prompts,
            endpoint: { [weak stop] body in stop?.withSession(body) }, failure: { failures.append($0) })
        stop.start(authentication: stopAuth.callbacks)
        try await until { stopped != nil }
        guard case .success(.termination(let receipt, let result))? = stopped else { return XCTFail("Linux Source stop failed: \(String(describing: stopped))") }
        XCTAssertEqual(receipt.state, .known); XCTAssertEqual(receipt.status, 0); XCTAssertEqual(result?.hasResult, true)
        XCTAssertEqual(failures, []); XCTAssertGreaterThanOrEqual(credentialCalls, 7); XCTAssertEqual(hostkeyCalls, 1)
        if testRun?.failureCount == 0 { print("PASS Mac-to-Linux native setup: probe/prebuilt install, fresh SSH, live reinstall preserves Source, durable RootView resume and explicit observed stop") }
    }
}
