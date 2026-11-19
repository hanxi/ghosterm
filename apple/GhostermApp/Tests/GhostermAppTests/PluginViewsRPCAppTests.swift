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
@MainActor final class PluginViewsRPCAppTests: XCTestCase {
    private func fixture(_ operation: String? = nil) throws -> [String: Any] {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_VIEWS_PORT"].flatMap(UInt16.init), let token = env["GHOSTERM_VIEWS_TOKEN"],
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
        let request = Data((token + (operation.map { " " + $0 } ?? "") + "\n").utf8)
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
    func testRegisteredViewsThroughNativeAppModel() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let f = try fixture()
        func string(_ name: String) throws -> String { try XCTUnwrap(f[name] as? String) }
        let route = try string("route"), root = URL(fileURLWithPath: try string("scratch"))
        let package = URL(fileURLWithPath: try string("package")), config = root.appendingPathComponent("config"), known = root.appendingPathComponent("known")
        let ports = try XCTUnwrap(f["ports"] as? [Int]), user = try string("user"), keys = try XCTUnwrap(f["hostKeys"] as? [String])
        var source = "Host view-target\n HostName 127.0.0.1\n Port \(ports[1])\n User \(user)\n IdentityFile /owned-in-memory\n"
        if route == "jump" { source += " ProxyJump view-jump\nHost view-jump\n HostName 127.0.0.1\n Port \(ports[0])\n User \(user)\n IdentityFile /owned-in-memory\n" }
        source += "Host view-invalid\n HostName 127.0.0.1\n ProxyJump view-invalid\n"
        try Data(source.utf8).write(to: config)
        try Data(zip(ports, keys).map { "[127.0.0.1]:\($0) \($1)" }.joined().utf8).write(to: known)
        let app = isolatedAppModel(configPath: config.path, knownHostsPath: known.path, pluginResourceDirectory: root.appendingPathComponent("resources"))
        let controller = try XCTUnwrap(app.extensionController), pem = try string("pem")
        addTeardownBlock { await app.stopExtensionRun(); let closed = await controller.close(); XCTAssertTrue(closed) }
        app.sshCredentialStorage = .init(key: { path in XCTAssertNotEqual(route, "local"); XCTAssertEqual(path, "/owned-in-memory"); return pem },
            secret: { _, _ in nil }, save: { _, _, _ in XCTFail("no credential persistence") }, remove: { _, _ in })
        let interaction = CodexRouteInteraction(); app.interaction = interaction
        let review = try await controller.review(directory: package)
        XCTAssertEqual(review.commands.first?.views, ["table", "diff", "task"])
        let approved = try await controller.approve(id: review.id, digest: review.digest); XCTAssertEqual(approved, 0)
        let gterm = try string("gterm")
        let digest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: gterm))).map { String(format: "%02x", $0) }.joined()
        let launch: ExtensionLocalLaunch? = route == "local" ? .init(executableSHA256: digest, workingDirectory: root.path,
            temporaryDirectory: root.appendingPathComponent("stage").path, authorized: true) : nil
        let methods = ["alpha.describe", "alpha.page", "beta.describe", "beta.page", "changes.describe", "changes.page", "work.describe", "work.read", "work.cancel", "views.reopen"]
        var attachment: ExtensionRelayBootstrap.Attachment?
        if let directory = f["ipcDirectory"] as? String {
            attachment = .init(directory: directory, instance: try string("ipcInstance"), authorized: true)
        }
        if route != "local" {
            var validated = false
            await app.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "views demo",
                arguments: [], sshAlias: "view-invalid", executable: gterm,
                executorDirectory: attachment == nil ? package.path : "", executorDigest: review.digest,
                methods: methods, hostMethods: ["views.client.open"], executorAttachment: attachment) {
                validated = true
            }
            XCTAssertFalse(validated)
            XCTAssertNotNil(app.extensionRunIssue)
            XCTAssertNil(app.extensionRun)
            XCTAssertFalse(app.tabs.tabs.flatMap(\.workspacePanes).contains { $0.extensionInstance != nil })
        }
        await app.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "views demo", arguments: [], sshAlias: "view-target",
            executable: gterm, executorDirectory: attachment == nil ? package.path : "", executorDigest: review.digest, methods: methods, hostMethods: ["views.client.open"], executorAttachment: attachment, localLaunch: launch)
        XCTAssertNil(app.extensionRunIssue); guard app.extensionRunIssue == nil else { return }
        XCTAssertTrue(app.extensionRun?.requiredCapabilities.contains("ui.views") == true)
        XCTAssertEqual(Set(app.extensionRun?.methods ?? []), Set(methods))
        await app.decideExtensionConsent(allow: true)
        try await until("View Runtime activation: \(app.extensionRunIssue ?? "no issue")") { app.extensionRegisteredViews?.items.count == 4 }
        if let attachment {
            XCTAssertEqual(app.extensionRun?.executorAttachment, attachment)
            let observation = try XCTUnwrap(try fixture()["ipcObservation"] as? [String: Any])
            let frontend = try XCTUnwrap(observation["frontendPID"] as? Int)
            let attachers = try XCTUnwrap(observation["attacherPIDs"] as? [Int])
            XCTAssertTrue(observation["frontendAlive"] as? Bool == true && observation["endpointRevoked"] as? Bool == true &&
                !attachers.isEmpty && !attachers.contains(frontend))
        }
        let views = try XCTUnwrap(app.extensionRegisteredViews)
        XCTAssertNil(app.extensionTable); XCTAssertNil(app.extensionDiff); XCTAssertNil(app.extensionTask)
        guard case .table(let alpha) = views.items[0].content, case .table(let beta) = views.items[1].content,
              case .diff(let diff) = views.items[2].content, case .task(let task) = views.items[3].content else { return XCTFail("standard component types") }
        await alpha.load(); await beta.load(); await diff.load(); await task.load()
        XCTAssertNil(alpha.issue); XCTAssertNil(beta.issue); XCTAssertNil(diff.issue); XCTAssertNil(task.issue)
        XCTAssertEqual(alpha.descriptor?.viewID, "alpha"); XCTAssertEqual(beta.descriptor?.viewID, "beta")
        XCTAssertEqual(diff.page?.rows.map(\.text), ["before", "after"]); XCTAssertEqual(task.snapshot?.phase, .running)
        let pane = NSHostingView(rootView: PluginWorkspaceTestView(model: app).background(Color(nsColor: .windowBackgroundColor)))
        pane.frame = NSRect(x: 0, y: 0, width: 1440, height: 4400)
        let window = PluginTableWindow(contentRect: pane.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.close() }
        try await Task.sleep(for: .milliseconds(100)); pane.layoutSubtreeIfNeeded()
        XCTAssertEqual(window.alphaValue, 0); XCTAssertTrue(NSScreen.screens.allSatisfy { !$0.frame.intersects(window.frame) })
        var nodes = PluginTableRendering.elements(pane)
        XCTAssertEqual(nodes.filter { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-view-close" }.count, 4)
        let next = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-table-next" })
        XCTAssertTrue(PluginTableRendering.press(next))
        try await until("independent Alpha page") { alpha.page?.offset == 16 }
        XCTAssertEqual(beta.page?.offset, 0)
        let oldHandle = views.items[0].id
        let close = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-view-close" })
        XCTAssertTrue(PluginTableRendering.press(close))
        try await until("Native registration close") { views.items.count == 3 }
        XCTAssertFalse(alpha.valid); XCTAssertTrue(views.items.contains { if case .table(let m) = $0.content { return m === beta }; return false })
        let reopened = try await app.callPluginViewOutcome("views.reopen", inputBytes: Data(#"{"viewId":"alpha"}"#.utf8), command: "views demo")
        XCTAssertTrue(String(decoding: reopened, as: UTF8.self).contains("\"opened\":true"))
        try await until("Client must reopen with a fresh handle") { views.items.count == 4 && views.items.contains { $0.registration.viewID == "alpha" && $0.id != oldHandle } }
        // Closing a task view is not a cancel operation.
        let taskHandle = try XCTUnwrap(views.items.first { $0.registration.kind == .task }?.id)
        await views.closeView(handle: taskHandle)
        try await until("task view closes independently") { views.items.count == 3 }
        let read = try await app.callPluginViewOutcome("work.read", inputBytes: Data(#"{"viewId":"progress","revision":"1"}"#.utf8), command: "views demo")
        XCTAssertTrue(String(decoding: read, as: UTF8.self).contains("\"phase\":\"running\""))
        _ = try await app.callPluginViewOutcome("views.reopen", inputBytes: Data(#"{"viewId":"progress"}"#.utf8), command: "views demo")
        try await until("task reopens") { views.items.count == 4 }
        guard case .task(let reopenedTask) = views.items.first(where: { $0.registration.kind == .task })?.content else { return XCTFail("reopened task") }
        await reopenedTask.load()
        try await until("reopened task ready for explicit cancellation") { reopenedTask.canCancel }
        if f["foregroundLoss"] as? Bool == true {
            XCTAssertNotNil(attachment)
            XCTAssertEqual(reopenedTask.snapshot?.phase, .running)
            let observed = try XCTUnwrap(try fixture("lose-foreground")["ipcObservation"] as? [String: Any])
            XCTAssertEqual(observed["frontendAlive"] as? Bool, false)
            XCTAssertEqual(observed["fault"] as? String, "injected-ipc-loss")
            try await until("foreground loss invalidates Native views") { !views.valid && views.items.isEmpty }
            XCTAssertFalse(reopenedTask.available)
            XCTAssertNotEqual(reopenedTask.snapshot?.phase, .cancelled)
            try await until("foreground loss removes Native close controls") {
                pane.layoutSubtreeIfNeeded()
                return !PluginTableRendering.elements(pane).contains {
                    PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-view-close"
                }
            }
        } else {
            try await Task.sleep(for: .milliseconds(100)); nodes = PluginTableRendering.elements(pane)
            let cancel = try XCTUnwrap(nodes.first { PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-task-cancel" })
            XCTAssertTrue(PluginTableRendering.press(cancel))
            try await until("actual task cancellation") { reopenedTask.snapshot?.phase == .cancelled }
            XCTAssertEqual(reopenedTask.cancellation, .requested); XCTAssertEqual(reopenedTask.snapshot?.result, .cancelled("none"))
        }
        XCTAssertEqual(interaction.hosts.count, 0)
        await app.stopExtensionRun()
        XCTAssertNil(app.extensionRun); XCTAssertNil(app.extensionRunIssue); XCTAssertFalse(views.valid); XCTAssertTrue(views.items.isEmpty)
        if route == "local" { XCTAssertTrue(app.extensionLastLocalProcess?.reaped == true); XCTAssertEqual(app.extensionLastLocalTemporary?.phase, .removed) }
    }
}
