import AppKit
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

private final class RouteKeyRequests: @unchecked Sendable {
    private let lock = NSLock(); private var paths: [String] = []
    func append(_ path: String) { lock.lock(); paths.append(path); lock.unlock() }
    var values: [String] { lock.lock(); defer { lock.unlock() }; return paths }
}
private final class RouteInteraction: AppInteraction {
    var hosts: [HostKeyPrompt] = []
    var decision: HostKeyDecision = .accept
    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision { hosts.append(prompt); return decision }
    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer? { XCTFail("Unexpected credential UI"); return nil }
}
final class SSHRouteAppTests: XCTestCase {
    override class func setUp() { super.setUp(); _ = NSApplication.shared; TestWindows.enterAutomationMode() }
    @MainActor private func wait(_ predicate: () -> Bool, seconds: Double = 10) -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end { if predicate() { return true }; RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
        return predicate()
    }
    func testCancelledDNSCannotAllocateRoute() {
        let owner = DispatchQueue(label: "route.cancelled-dns")
        let complete = expectation(description: "cancelled DNS")
        owner.async {
            var callback: ((Result<SSHAddress, SSHAddressResolver.Failure>) -> Void)?
            var results = 0, lookups = 0, allocations = 0
            let prompt = SSHAuthenticationPrompts(credential: { _, _, _ in XCTFail("credential after cancel") }, hostkey: { _, _, _, _ in XCTFail("hostkey after cancel") })
            let flow = SSHRouteAuthenticationFlow(owner: owner, options: [.init(host: "first.invalid", username: "fixture"), .init(host: "inner.invalid", username: "fixture")], prompts: [prompt, prompt], lookup: { host, _, _, done in
                XCTAssertEqual(host, "first.invalid"); lookups += 1; callback = done; return nil
            }, makeRoute: { _, _, _ in allocations += 1; throw GhostermError.failed("unexpected allocation") }, nextID: { XCTFail("request after cancel"); return nil }, completion: { result in
                results += 1
                if case .failure(let failure) = result { XCTAssertEqual(failure.status, 8) } else { XCTFail("cancelled route succeeded") }
            })
            flow.start(); flow.cancel()
            callback?(.success(SSHAddress(family: 4, bytes: [127,0,0,1] + Array(repeating: 0, count: 12))))
            flow.start(); flow.cancel()
            XCTAssertEqual(lookups, 1); XCTAssertEqual(allocations, 0); XCTAssertEqual(results, 1)
            complete.fulfill()
        }
        wait(for: [complete], timeout: 3)
    }
    @MainActor func testChangedDestinationCannotKeepOldLabel() async throws {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("route-target-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("config").path
        let text = "Host destination\n HostName original.invalid\n ProxyJump gateway\nHost gateway\n HostName gateway.invalid\n"
        try text.write(toFile: path, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: path, knownHostsPath: directory.appendingPathComponent("known_hosts").path)
        defer { model.disconnectForTesting() }
        let old = model.sshTarget(for: "destination")
        try text.replacingOccurrences(of: "original.invalid", with: "replacement.invalid").write(toFile: path, atomically: true, encoding: .utf8)
        model.reload()
        let owner = DispatchQueue(label: "route.changed-target")
        do {
            let connection = try await model.authenticate(old, id: UUID(), owner: owner)
            owner.async { connection.close() }; XCTFail("Changed destination was admitted")
        } catch { XCTAssertEqual(connectionErrorMessage(error), "SSH 目标配置已改变，请重新连接") }
    }
    func testDNSFailureIsConnectionError() {
        let owner = DispatchQueue(label: "route.dns-error")
        let complete = expectation(description: "DNS failure")
        owner.async {
            let prompt = SSHAuthenticationPrompts(credential: { _, _, _ in XCTFail("credential after DNS error") }, hostkey: { _, _, _, _ in XCTFail("hostkey after DNS error") })
            let flow = SSHRouteAuthenticationFlow(owner: owner, options: [.init(host: "first.invalid", username: "fixture"), .init(host: "inner.invalid", username: "fixture")], prompts: [prompt, prompt], lookup: { _, _, _, done in done(.failure(.unavailable(-1))); return nil }, nextID: { nil }, completion: { result in
                if case .failure(let failure) = result { XCTAssertEqual(failure.status, 3) } else { XCTFail("DNS failure succeeded") }
                complete.fulfill()
            })
            flow.start()
        }
        wait(for: [complete], timeout: 3)
    }
    func testRouteDeadlineIncludesDNS() {
        let owner = DispatchQueue(label: "route.deadline")
        let complete = expectation(description: "DNS deadline")
        var retained: SSHRouteAuthenticationFlow?
        owner.async {
            var instant: TimeInterval = 100
            let prompt = SSHAuthenticationPrompts(credential: { _, _, _ in XCTFail("credential while resolving") }, hostkey: { _, _, _, _ in XCTFail("hostkey while resolving") })
            let flow = SSHRouteAuthenticationFlow(owner: owner, options: [.init(host: "first.invalid", username: "fixture"), .init(host: "inner.invalid", username: "fixture")], prompts: [prompt, prompt], lookup: { _, _, _, _ in nil }, now: { instant }, nextID: { nil }, completion: { result in
                if case .failure(let failure) = result { XCTAssertEqual(failure.status, 3) } else { XCTFail("DNS timeout succeeded") }
                complete.fulfill()
            })
            retained = flow; flow.start(); instant = 161
        }
        wait(for: [complete], timeout: 3)
        owner.sync { retained = nil }
    }
    @MainActor func testConfiguredRouteProducts() throws {
        let env = ProcessInfo.processInfo.environment
        guard let raw = env["GHOSTERM_ROUTE_PORTS"], let known = env["GHOSTERM_ROUTE_KNOWN"],
              let user = env["GHOSTERM_ROUTE_USER"], let mode = env["GHOSTERM_ROUTE_MODE"] else { throw XCTSkip("Owned route fixture required") }
        let ports = raw.split(separator: ",").compactMap { UInt16($0) }
        let pem = String(decoding: FileHandle.standardInput.readDataToEndOfFile(), as: UTF8.self)
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("route-app-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        var hostkeys = try String(contentsOfFile: known, encoding: .utf8)
        for index in ports.indices { hostkeys = hostkeys.replacingOccurrences(of: "route-hop-\(index).invalid", with: "127.0.0.1") }
        let knownPath = directory.appendingPathComponent("known_hosts").path
        if ["pty-idle", "cancel"].contains(mode) { hostkeys = "" }
        try hostkeys.write(toFile: knownPath, atomically: true, encoding: .utf8)
        var text = ""
        for (index, port) in ports.enumerated() {
            text += "Host \(index == ports.count - 1 ? "destination" : "jump\(index)")\n HostName 127.0.0.1\n User \(user)\n Port \(port)\n IdentityFile /fixture/identity-\(index)\n"
            if index == ports.count - 1 { text += " ProxyJump " + (mode == "pty-cancel" ? "none" : (0..<index).map { "jump\($0)" }.joined(separator: ",")) + "\n" }
        }
        let config = directory.appendingPathComponent("config").path
        try text.write(toFile: config, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: config, knownHostsPath: knownPath)
        defer { model.disconnectForTesting(); _ = wait({ model.tabs.tabs.isEmpty }, seconds: 1) }
        let interaction = RouteInteraction(); model.interaction = interaction
        let keys = RouteKeyRequests()
        let releaseKey = DispatchSemaphore(value: 0)
        defer { releaseKey.signal() }
        if mode == "cancel" { interaction.decision = .reject }
        model.sshCredentialStorage = .init(key: { path in
                keys.append(path)
                if mode == "open-cancel" && path == "/fixture/identity-1" { _ = releaseKey.wait(timeout: .now() + 10) }
                return path.hasPrefix("/fixture/identity-") ? pem : nil },
            secret: { _, _ in nil }, save: { _, _, _ in XCTFail("Unexpected credential save") }, remove: { _, _ in })
        if mode == "read-wait-error" {
            let targets = try model.configuredSSHRoute(for: ["destination"])
            let owner = DispatchQueue(label: "route.replay")
            var seen = Array(repeating: 0, count: targets.count), accepted = seen
            var result: Result<SSHAuthenticationFlow.Authenticated, SSHAuthenticationFlow.Failure>?
            var request: UInt64 = 0
            let prompts = targets.indices.map { index in
                SSHAuthenticationPrompts(credential: { _, attempt, done in
                    XCTAssertEqual(attempt, 1); seen[index] += 1
                    done(.success(.privateKey(pem: pem, passphrase: nil)))
                    done(.success(.privateKey(pem: pem, passphrase: nil)))
                }, hostkey: { _, _, _, _ in XCTFail("Unexpected trust prompt") },
                    accepted: { _, attempt in XCTAssertEqual(attempt, 1); accepted[index] += 1 })
            }
            let flow = SSHRouteAuthenticationFlow(owner: owner, options: targets.map {
                SSHOptions(host: $0.host, port: $0.port, username: $0.user, knownHostsPath: $0.knownHosts)
            }, prompts: prompts, nextID: { request += 1; return request }, completion: { value in DispatchQueue.main.async { result = value } })
            owner.async { flow.start() }
            guard wait({ result != nil }) else { owner.async { flow.cancel() }; return XCTFail("Duplicate-reply route timed out") }
            guard case .success(let value) = result else { return XCTFail("Duplicate reply was consumed twice") }
            owner.async { value.connection.close() }
            XCTAssertEqual(seen, Array(repeating: 1, count: targets.count))
            XCTAssertEqual(accepted, Array(repeating: 1, count: targets.count))
            print("SSH JUMP IO PASS"); return
        }
        if mode == "cancel" {
            model.connect(to: .host("destination"))
            XCTAssertTrue(wait({ model.tabs.activeTab?.status.isFailed == true }))
            XCTAssertEqual(interaction.hosts.count, 1); XCTAssertTrue(keys.values.isEmpty)
            print("SSH JUMP IO PASS"); return
        }
        if mode == "open-cancel" {
            model.connect(to: .host("destination"))
            guard wait({ keys.values.contains("/fixture/identity-1") }) else { return XCTFail("Second-hop credential request missing") }
            model.reload(); releaseKey.signal()
            guard wait({ model.tabs.activeTab?.status.isFailed == true }) else { return XCTFail("Reload left authentication active") }
            XCTAssertFalse(model.tabs.activeTab?.activePane?.isRunning ?? true)
            XCTAssertEqual(keys.values, ["/fixture/identity-0", "/fixture/identity-1"])
            print("SSH JUMP IO PASS"); return
        }
        if mode == "sftp" || mode == "pty-cancel" || mode == "wait-error" {
            model.openSFTP(for: "destination")
            guard wait({ model.remoteFiles?.panel != nil || model.remoteFiles?.error != nil }) else { return XCTFail("SFTP admission timeout: \(model.status)") }
            guard let panel = model.remoteFiles?.panel else { return XCTFail("SFTP admission failed: \(model.status)") }
            guard wait({ !panel.isLoading }) else { return XCTFail("SFTP listing timeout") }
            XCTAssertNil(panel.errorText)
            XCTAssertTrue(panel.entries.contains { $0.name == "payload" }, "SFTP must browse the owned fixture directory")
            if mode == "wait-error" {
              do {
                let backend = try XCTUnwrap(panel.backend as? SFTPFileBackend)
                let remoteRoot = try backend.realpath(".")
                guard remoteRoot == (known as NSString).deletingLastPathComponent else { return XCTFail("Remote operations escaped fixture directory") }
                let folder = remoteRoot + "/operations-" + UUID().uuidString
                try backend.mkdir(folder, mode: 0o700)
                let source = directory.appendingPathComponent("upload").path
                let destination = directory.appendingPathComponent("download").path
                let bytes = Data((0..<262144).map { UInt8(truncatingIfNeeded: $0 * 17 + 3) })
                try bytes.write(to: URL(fileURLWithPath: source))
                let manager = TransferManager()
                var uploaded: TransferManager.Status?
                manager.enqueueUpload(backend, local: source, remote: folder + "/original", name: "upload", isDirectory: false, onFinish: { uploaded = $0 })
                guard wait({ uploaded != nil }, seconds: 8), uploaded == .done else { return XCTFail("Jump upload failed: \(String(describing: uploaded))") }
                try backend.rename(folder + "/original", folder + "/renamed")
                try backend.chmod(folder + "/renamed", mode: 0o600)
                XCTAssertEqual(try backend.list(folder).first.map { $0.mode & 0o777 }, 0o600)
                var downloaded: TransferManager.Status?
                manager.enqueueDownload(backend, remote: folder + "/renamed", local: destination, name: "download", isDirectory: false, onFinish: { downloaded = $0 })
                guard wait({ downloaded != nil }, seconds: 8), downloaded == .done else { return XCTFail("Jump download failed: \(String(describing: downloaded))") }
                XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: destination)), bytes)
                XCTAssertEqual(try backend.list(folder).map(\.name), ["renamed"])
                try backend.remove(folder + "/renamed", recursive: false)
                XCTAssertTrue(try backend.list(folder).isEmpty)
                try backend.remove(folder, recursive: true)
                XCTAssertFalse(try backend.list(remoteRoot).contains { $0.path == folder })
                manager.clearFinished()
              } catch { return XCTFail("Jump file operations failed: \(error)") }
            }
        } else {
            if mode == "pty" {
                // main's unified chooser probes tmux first. Unsupported management
                // must remain explicit while its ordinary terminal action works.
                model.openHost(.host("destination"), forceChooser: true)
                guard let chooser = model.tmuxChooser else { return XCTFail("Missing connection chooser") }
                guard wait({ !chooser.busy }) else { return XCTFail("Chooser probe did not finish") }
                XCTAssertTrue(chooser.message.contains("tmux 管理暂不支持 ProxyJump"), chooser.message)
                XCTAssertTrue(interaction.hosts.isEmpty)
                chooser.openTerminal()
                XCTAssertNil(model.tmuxChooser)
            } else {
                model.connect(to: .host("destination"))
            }
            guard let tab = model.tabs.activeTab, let pane = tab.activePane else { return XCTFail("Missing terminal pane") }
            guard wait({ pane.isRunning || pane.status.isFailed }) else { return XCTFail("Terminal admission timeout") }
            guard let surface = pane.surface else { return XCTFail("Terminal admission failed: \(tab.status)") }
            surface.setSize(width: 640, height: 400)
            surface.sendCommandLine("printf 'ROUTE_%s\\n' PRODUCT")
            XCTAssertTrue(wait({ surface.readText()?.contains("ROUTE_PRODUCT") == true }), "Terminal stream did not reach the final hop")
            if mode == "pty-echo" {
                model.openFilesPanel(for: tab)
                guard wait({ tab.filesPanel != nil || tab.filesPanelError != nil }), let panel = tab.filesPanel else { return XCTFail("Independent panel failed: \(tab.filesPanelError ?? "timeout")") }
                guard wait({ !panel.isLoading }) else { return XCTFail("Panel listing timeout") }
                XCTAssertNil(panel.errorText); XCTAssertTrue(panel.entries.contains { $0.name == "payload" })
            }
        }
        let expected = (mode == "pty-cancel" ? [ports.count - 1] : Array(0..<ports.count)).map { "/fixture/identity-\($0)" }
        XCTAssertEqual(Array(keys.values.prefix(expected.count)), expected)
        if mode == "pty-echo" { XCTAssertEqual(keys.values.count, ports.count * 2) }
        if mode == "pty-idle" { XCTAssertEqual(interaction.hosts.map(\.port), ports) }
        else { XCTAssertTrue(interaction.hosts.isEmpty) }
        XCTAssertEqual(try String(contentsOfFile: config, encoding: .utf8), text)
        print("SSH JUMP IO PASS")
    }
}
