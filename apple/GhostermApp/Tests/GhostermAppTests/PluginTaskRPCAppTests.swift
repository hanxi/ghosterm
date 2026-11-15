import AppKit
import CryptoKit
import Darwin
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Same official Executor over local process transport or owned direct/jump SSH.
@MainActor final class PluginTaskRPCAppTests: XCTestCase {
    private func fixture() throws -> [String: Any] {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_TASK_PORT"].flatMap(UInt16.init), let token = env["GHOSTERM_TASK_TOKEN"],
              ExtensionRuntime.supportStatus == 0 else { throw XCTSkip("requires explicit owned Native task runner") }
        let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw GhostermError.failed("owned task fixture socket") }; defer { Darwin.close(fd) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian; address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let connected = withUnsafePointer(to: &address) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard connected == 0 else { throw GhostermError.failed("owned task fixture connection") }
        var timeout = timeval(tv_sec: 5, tv_usec: 0)
        guard setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)) == 0 else { throw GhostermError.failed("owned task fixture timeout") }
        let request = Data((token + "\n").utf8)
        guard request.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == request.count else { throw GhostermError.failed("owned task fixture request") }
        func read(_ size: Int) throws -> Data {
            var data = Data(count: size), at = 0
            while at < size {
                let n = data.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress!.advanced(by: at), size - at) }
                guard n > 0 else { throw GhostermError.failed("owned task fixture reply") }; at += n
            }
            return data
        }
        let size = try read(4).reduce(0) { ($0 << 8) | Int($1) }
        guard (1...1048576).contains(size), let value = try JSONSerialization.jsonObject(with: read(size)) as? [String: Any] else { throw GhostermError.failed("owned task fixture framing") }
        return value
    }
    private func until(_ label: String, _ check: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 20
        while !check() && ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(for: .milliseconds(20)) }
        guard check() else { XCTFail(label); throw GhostermError.failed(label) }
    }
    func testTaskThroughNativeAppModel() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let f = try fixture()
        func string(_ name: String) throws -> String { try XCTUnwrap(f[name] as? String) }
        let route = try string("route"), scenario = try string("scenario"), limit = try XCTUnwrap(f["limit"] as? Int)
        let root = URL(fileURLWithPath: try string("scratch")), package = URL(fileURLWithPath: try string("package"))
        let config = root.appendingPathComponent("config"), known = root.appendingPathComponent("known")
        let ports = try XCTUnwrap(f["ports"] as? [Int]), user = try string("user")
        var source = "Host task-target\n HostName 127.0.0.1\n Port \(ports[1])\n User \(user)\n IdentityFile /owned-in-memory\n"
        if route == "jump" { source += " ProxyJump task-jump\nHost task-jump\n HostName 127.0.0.1\n Port \(ports[0])\n User \(user)\n IdentityFile /owned-in-memory\n" }
        try Data(source.utf8).write(to: config)
        let keys = try XCTUnwrap(f["hostKeys"] as? [String])
        try Data(zip(ports, keys).map { "[127.0.0.1]:\($0) \($1)" }.joined().utf8).write(to: known)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: known.path,
            pluginResourceDirectory: root.appendingPathComponent("resources"))
        let controller = try XCTUnwrap(model.extensionController)
        addTeardownBlock { await model.stopExtensionRun(); let closed = await controller.close(); XCTAssertTrue(closed) }
        let pem = try string("pem")
        model.sshCredentialStorage = .init(key: { path in
            XCTAssertNotEqual(route, "local"); XCTAssertEqual(path, "/owned-in-memory"); return pem
        }, secret: { _, _ in nil }, save: { _, _, _ in XCTFail("must not persist fixture secrets") }, remove: { _, _ in })
        let interaction = CodexRouteInteraction(); model.interaction = interaction
        let review = try await controller.review(directory: package)
        XCTAssertEqual(review.commands.first?.views, ["task"])
        let approved = try await controller.approve(id: review.id, digest: review.digest); XCTAssertEqual(approved, 0)
        let gterm = try string("gterm")
        let digest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: gterm))).map { String(format: "%02x", $0) }.joined()
        let local: ExtensionLocalLaunch? = route == "local" ? .init(executableSHA256: digest,
            workingDirectory: root.path, temporaryDirectory: root.appendingPathComponent("stage").path, authorized: true) : nil
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "task primes",
            arguments: ["--limit", String(limit)], sshAlias: "task-target", executable: gterm,
            executorDirectory: package.path, executorDigest: review.digest, methods: [], hostMethods: [], localLaunch: local)
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertEqual(model.extensionRun?.requiredCapabilities, ["session.rpc", "ui.task"])
        XCTAssertEqual(model.extensionRun?.methods, PluginTaskModel.methods)
        guard model.extensionRun?.methods == PluginTaskModel.methods else { return }
        await model.decideExtensionConsent(allow: true)
        try await until("Task Client Runtime did not activate: \(model.extensionRunIssue ?? "no issue")") {
            model.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
        }
        let task = try XCTUnwrap(model.extensionTask)
        await task.load()
        XCTAssertNil(task.issue); XCTAssertEqual(task.snapshot?.total, String(max(0, limit - 1)))
        // An invalid read is rejected before the business handler. Its declared
        // failure must survive the same production Native RPC path as success.
        var declaredFailure = false
        do {
            let outcome = try await model.callPluginViewOutcome("view.task.read", inputBytes: Data("{}".utf8), command: "task primes")
            _ = try PluginTaskCodec.success(outcome)
        } catch let failure as PluginTaskCodec.RPCFailure {
            if case .failed(let code, _, let effect) = failure { declaredFailure = code == "INVALID_INPUT" && effect == "none" }
        } catch { }
        XCTAssertTrue(declaredFailure)
        guard declaredFailure else { return }
        let pane = NSHostingView(rootView: ExtensionsPane(model: model).background(Color(nsColor: .windowBackgroundColor)))
        pane.frame = NSRect(x: 0, y: 0, width: 1440, height: 2200)
        let window = PluginTableWindow(contentRect: pane.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.close() }
        try await Task.sleep(for: .milliseconds(100))
        let nodes = PluginTableRendering.elements(pane)
        let rendered = nodes.contains { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-task-count" }
        XCTAssertTrue(rendered)
        guard rendered else { return }
        if scenario == "cancel" {
            XCTAssertTrue(task.canCancel && task.snapshot?.phase == .running)
            let button = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-task-cancel" })
            XCTAssertTrue(PluginTableRendering.press(button))
            try await until("Native cancel did not receive acknowledgement") { task.cancellation == .requested }
            // The production view refreshes read-only state every 500 ms until confirmation.
            try await until("Native task did not confirm cancellation: \(task.issue ?? "no issue")") { task.snapshot?.phase == .cancelled }
            XCTAssertEqual(task.snapshot?.result, .cancelled("none")); XCTAssertNotNil(task.cancelRequestID)
            XCTAssertTrue(UInt64(task.snapshot?.completed ?? "0")! < UInt64(limit - 1))
        } else {
            try await until("Native task did not finish: \(task.issue ?? "no issue")") { task.snapshot?.phase == .succeeded }
            XCTAssertEqual(task.snapshot?.completed, String(max(0, limit - 1)))
            let expected = limit == 10000 ? "1229 primes up to 10000" : "0 primes up to 0"
            XCTAssertEqual(task.snapshot?.result, .success(expected)); XCTAssertNil(task.cancelRequestID)
        }
        XCTAssertFalse(task.canCancel); XCTAssertNil(task.issue)
        XCTAssertEqual(interaction.hosts.count, 0, "Only explicitly pinned owned SSH hosts may be used")
        let last = task.snapshot
        await model.stopExtensionRun()
        XCTAssertNil(model.extensionRun); XCTAssertNil(model.extensionRunIssue)
        XCTAssertFalse(task.valid); XCTAssertEqual(task.snapshot, last)
        if route == "local" {
            XCTAssertTrue(model.extensionLastLocalProcess?.reaped == true)
            XCTAssertEqual(model.extensionLastLocalTemporary?.phase, .removed)
        }
    }
}
