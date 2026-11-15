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
@MainActor final class PluginTableRPCAppTests: XCTestCase {
    private func fixture() throws -> [String: Any] {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_TABLE_PORT"].flatMap(UInt16.init), let token = env["GHOSTERM_TABLE_TOKEN"],
              ExtensionRuntime.supportStatus == 0 else { throw XCTSkip("requires explicit owned Native table runner") }
        let fd = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw GhostermError.failed("owned table fixture socket") }; defer { Darwin.close(fd) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian; address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let connected = withUnsafePointer(to: &address) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard connected == 0 else { throw GhostermError.failed("owned table fixture connection") }
        var timeout = timeval(tv_sec: 5, tv_usec: 0)
        guard setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)) == 0 else { throw GhostermError.failed("owned table fixture timeout") }
        let request = Data((token + "\n").utf8)
        guard request.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == request.count else { throw GhostermError.failed("owned table fixture request") }
        func read(_ size: Int) throws -> Data {
            var data = Data(count: size), at = 0
            while at < size {
                let n = data.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress!.advanced(by: at), size - at) }
                guard n > 0 else { throw GhostermError.failed("owned table fixture reply") }; at += n
            }
            return data
        }
        let size = try read(4).reduce(0) { ($0 << 8) | Int($1) }
        guard (1...1048576).contains(size), let value = try JSONSerialization.jsonObject(with: read(size)) as? [String: Any] else { throw GhostermError.failed("owned table fixture framing") }
        return value
    }
    private func until(_ label: String, _ check: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 20
        while !check() && ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(for: .milliseconds(20)) }
        guard check() else { XCTFail(label); throw GhostermError.failed(label) }
    }
    func testTableThroughNativeAppModel() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let f = try fixture()
        func string(_ name: String) throws -> String { try XCTUnwrap(f[name] as? String) }
        let route = try string("route"), count = try XCTUnwrap(f["count"] as? Int)
        let root = URL(fileURLWithPath: try string("scratch")), package = URL(fileURLWithPath: try string("package"))
        let config = root.appendingPathComponent("config"), known = root.appendingPathComponent("known")
        let ports = try XCTUnwrap(f["ports"] as? [Int]), user = try string("user")
        var source = "Host table-target\n HostName 127.0.0.1\n Port \(ports[1])\n User \(user)\n IdentityFile /owned-in-memory\n"
        if route == "jump" { source += " ProxyJump table-jump\nHost table-jump\n HostName 127.0.0.1\n Port \(ports[0])\n User \(user)\n IdentityFile /owned-in-memory\n" }
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
        XCTAssertEqual(review.commands.first?.views, ["table"])
        let approved = try await controller.approve(id: review.id, digest: review.digest); XCTAssertEqual(approved, 0)
        let gterm = try string("gterm")
        let digest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: gterm))).map { String(format: "%02x", $0) }.joined()
        let local: ExtensionLocalLaunch? = route == "local" ? .init(executableSHA256: digest,
            workingDirectory: root.path, temporaryDirectory: root.appendingPathComponent("stage").path, authorized: true) : nil
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "table",
            arguments: ["--count", String(count)], sshAlias: "table-target", executable: gterm,
            executorDirectory: package.path, executorDigest: review.digest, methods: [], hostMethods: [], localLaunch: local)
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertEqual(model.extensionRun?.requiredCapabilities, ["session.rpc", "ui.table"])
        XCTAssertEqual(model.extensionRun?.methods, PluginTableModel.methods)
        guard model.extensionRun?.methods == PluginTableModel.methods else { return }
        await model.decideExtensionConsent(allow: true)
        try await until("Table Client Runtime did not activate: \(model.extensionRunIssue ?? "no issue")") {
            model.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
        }
        let table = try XCTUnwrap(model.extensionTable)
        await table.load()
        XCTAssertNil(table.issue); XCTAssertEqual(table.descriptor?.total, count)
        XCTAssertEqual(table.page?.rows.count, min(16, count))
        if count > 0 {
            await table.showPage(offset: 16)
            XCTAssertEqual(table.page?.rows.first?.id, "17")
            await table.showPage(offset: table.lastOffset)
            XCTAssertEqual(table.page?.rows.last?.cells, ["1000000", "1000000000000", "1000000000000000000"])
        }
        let pane = NSHostingView(rootView: ExtensionsPane(model: model).background(Color(nsColor: .windowBackgroundColor)))
        pane.frame = NSRect(x: 0, y: 0, width: 1440, height: 2800)
        let window = PluginTableWindow(contentRect: pane.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.close() }
        try await Task.sleep(for: .milliseconds(100))
        let words = try PluginTableRendering.words(pane).joined(separator: " ")
        if count > 0 { XCTAssertTrue(words.contains("Square") && words.contains("Cube"), words) }
        else {
            XCTAssertTrue(PluginTableRendering.elements(pane).contains {
                PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-table-empty"
            })
        }
        XCTAssertEqual(interaction.hosts.count, 0, "Only explicitly pinned owned SSH hosts may be used")
        await model.stopExtensionRun()
        XCTAssertNil(model.extensionRun); XCTAssertNil(model.extensionRunIssue)
        XCTAssertFalse(table.valid); XCTAssertNil(table.page)
        if route == "local" {
            XCTAssertTrue(model.extensionLastLocalProcess?.reaped == true)
            XCTAssertEqual(model.extensionLastLocalTemporary?.phase, .removed)
        }
    }
}
