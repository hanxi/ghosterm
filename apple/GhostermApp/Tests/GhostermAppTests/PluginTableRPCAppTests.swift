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
        let project = f["project"] as? String
        let temporaryRetry = f["temporaryRetry"] as? Bool == true
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
        addTeardownBlock {
            let closed = await model.closeExtensionOwners()
            if !temporaryRetry { XCTAssertTrue(closed) }
        }
        let pem = try string("pem")
        model.sshCredentialStorage = .init(key: { path in
            XCTAssertNotEqual(route, "local"); XCTAssertEqual(path, "/owned-in-memory"); return pem
        }, secret: { _, _ in nil }, save: { _, _, _ in XCTFail("must not persist fixture secrets") }, remove: { _, _ in })
        let interaction = CodexRouteInteraction(); model.interaction = interaction
        let review = try await controller.review(directory: package)
        XCTAssertEqual(review.commands.first?.views, ["table"])
        XCTAssertEqual(review.commands.first?.togglePanel, true)
        let approved = try await controller.approve(id: review.id, digest: review.digest); XCTAssertEqual(approved, 0)
        let gterm = try string("gterm")
        let digest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: gterm))).map { String(format: "%02x", $0) }.joined()
        let local: ExtensionLocalLaunch? = route == "local" ? .init(executableSHA256: digest,
            workingDirectory: root.path, temporaryDirectory: root.appendingPathComponent("stage").path, authorized: true) : nil
        let origin = TerminalTab(kind: .workspace)
        let other = TerminalTab(kind: .workspace)
        model.tabs.add(origin)
        model.tabs.add(other)
        XCTAssertEqual(model.tabs.activeTab?.id, other.id)
        let request = ExtensionRelayBootstrap.Request(executable: gterm, executorDirectory: package.path,
            executorDigest: review.digest, clientDigest: review.digest, methods: [], command: "table",
            arguments: project.map { ["--project", $0] } ?? ["--count", String(count)])
        let launch = ExtensionGroupInvocation(packageID: review.id, request: request, sshAlias: "table-target",
            localLaunch: local, clientWorkspace: nil, temporaryDirectory: local?.temporaryDirectory ?? "")
        model.prepareExtensionCommand(launch, title: "Table", workspaceID: origin.id,
            togglePanel: review.commands.first?.togglePanel == true)
        let commandID = try XCTUnwrap(model.preparedExtensionCommands.first?.id)
        let wrongWorkspace = await model.invokePreparedExtensionCommand(id: commandID, workspaceID: other.id)
        XCTAssertFalse(wrongWorkspace)
        XCTAssertTrue(origin.workspacePanes.isEmpty)
        let invoked: Bool
        if project == "sample-b" {
            model.togglePluginCommandShortcut(id: commandID)
            let unmatchedShortcut = await model.invokeExtensionShortcut(workspaceID: other.id)
            XCTAssertFalse(unmatchedShortcut)
            invoked = await model.invokeExtensionShortcut(workspaceID: origin.id)
        } else {
            invoked = await model.invokePreparedExtensionCommand(id: commandID, workspaceID: origin.id)
        }
        XCTAssertTrue(invoked)
        XCTAssertNil(model.preparedExtensionCommandIssues[commandID])
        let instance = try XCTUnwrap(origin.activeWorkspacePane?.extensionInstance)
        XCTAssertFalse(instance === model.extensionInstance)
        XCTAssertTrue(other.workspacePanes.isEmpty)
        XCTAssertEqual(instance.extensionRun?.requiredCapabilities, ["session.rpc", "ui.table"])
        XCTAssertEqual(instance.extensionRun?.methods, PluginTableModel.methods)
        guard instance.extensionRun?.methods == PluginTableModel.methods else { return }
        await instance.decideExtensionConsent(allow: true)
        try await until("Table Client Runtime did not activate: \(instance.extensionRunIssue ?? "no issue")") {
            instance.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
        }
        let table = try XCTUnwrap(instance.extensionTable)
        await table.load()
        XCTAssertNil(table.issue); XCTAssertEqual(table.descriptor?.total, count)
        if temporaryRetry {
            XCTAssertEqual(route, "local")
            let temporary = try XCTUnwrap(instance.extensionRun?.session?.temporary?.directory)
            let moved = root.appendingPathComponent("moved-private").path
            try FileManager.default.moveItem(atPath: temporary, toPath: moved)
            await instance.stopExtensionRun()
            XCTAssertNotNil(instance.extensionRunIssue)
            XCTAssertNotNil(instance.extensionRun)
            XCTAssertTrue(instance.extensionLastLocalProcess?.reaped == true)
            XCTAssertEqual(instance.extensionLastLocalTemporary?.phase, .retained)
            try FileManager.default.moveItem(atPath: moved, toPath: temporary)
            await instance.stopExtensionRun()
            XCTAssertTrue(instance.extensionRunIssue == nil && instance.extensionRun == nil &&
                instance.extensionLastLocalTemporary?.phase == .removed,
                "Lua stop retry must finish retained temporary cleanup")
            return
        }
        XCTAssertEqual(table.page?.rows.count, min(16, count))
        if let project {
            XCTAssertEqual(table.descriptor?.title, "Project tasks · \(project)")
        } else if count > 0 {
            await table.showPage(offset: 16)
            XCTAssertEqual(table.page?.rows.first?.id, "17")
            await table.showPage(offset: table.lastOffset)
            XCTAssertEqual(table.page?.rows.last?.cells, ["1000000", "1000000000000", "1000000000000000000"])
        }
        let pane = NSHostingView(rootView: WorkspaceView(model: model, tab: origin, isActive: true,
            opts: .init()).background(Color(nsColor: .windowBackgroundColor)))
        pane.frame = NSRect(x: 0, y: 0, width: 1440, height: 2800)
        let window = PluginTableWindow(contentRect: pane.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = pane; TestWindows.presentOffscreen(window)
        defer { window.close() }
        try await Task.sleep(for: .milliseconds(100))
        if let project {
            let row = project == "sample-a" ? "sample-a-review" : "sample-b-inventory"
            let title = project == "sample-a" ? "Review release notes" : "Update inventory"
            XCTAssertEqual(table.page?.rows.first?.id, row)
            XCTAssertEqual(table.page?.rows.first?.cells.first, title)
            try await until("Native project task cell missing: \(row)") {
                PluginTableRendering.elements(pane).contains {
                    PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-table-cell-\(row)-task"
                }
            }
        } else if count > 0 {
            let words = try PluginTableRendering.words(pane).joined(separator: " ")
            XCTAssertTrue(words.contains("Square") && words.contains("Cube"), "root \(pane.bounds); \(words)")
        }
        else {
            XCTAssertTrue(PluginTableRendering.elements(pane).contains {
                PluginTableRendering.attribute($0, "accessibilityIdentifier") == "plugin-table-empty"
            })
        }
        XCTAssertEqual(interaction.hosts.count, 0, "Only explicitly pinned owned SSH hosts may be used")
        var siblingTable: PluginTableModel?
        if let project {
            let otherProject = project == "sample-a" ? "sample-b" : "sample-a"
            let otherStage = root.appendingPathComponent("stage-other")
            try FileManager.default.createDirectory(at: otherStage, withIntermediateDirectories: false)
            let otherLocal = local.map { ExtensionLocalLaunch(executableSHA256: $0.executableSHA256,
                workingDirectory: $0.workingDirectory, temporaryDirectory: otherStage.path, authorized: true) }
            let otherRequest = ExtensionRelayBootstrap.Request(executable: gterm, executorDirectory: package.path,
                executorDigest: review.digest, clientDigest: review.digest, methods: [], command: "table",
                arguments: ["--project", otherProject])
            let otherLaunch = ExtensionGroupInvocation(packageID: review.id, request: otherRequest, sshAlias: "table-target",
                localLaunch: otherLocal, clientWorkspace: nil, temporaryDirectory: otherStage.path)
            model.prepareExtensionCommand(otherLaunch, title: "Table", workspaceID: other.id, togglePanel: true)
            let otherID = try XCTUnwrap(model.preparedExtensionCommands.first { $0.workspaceID == other.id }?.id)
            let siblingInvoked = await model.invokePreparedExtensionCommand(id: otherID, workspaceID: other.id)
            XCTAssertTrue(siblingInvoked)
            let sibling = try XCTUnwrap(other.activeWorkspacePane?.extensionInstance)
            XCTAssertNotEqual(sibling.id, instance.id)
            await sibling.decideExtensionConsent(allow: true)
            try await until("Sibling Runtime did not activate: \(sibling.extensionRunIssue ?? "no issue")") {
                sibling.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
            }
            siblingTable = try XCTUnwrap(sibling.extensionTable)
            await siblingTable?.load()
            XCTAssertEqual(siblingTable?.descriptor?.title, "Project tasks · \(otherProject)")
            XCTAssertEqual(table.descriptor?.title, "Project tasks · \(project)")
        }
        // Buffer a confirmed page from the real SSH Executor at the UI boundary.
        // Releasing it after the panel closes makes the order independent of SSH timing.
        var delayedReply: CheckedContinuation<Void, Never>?
        var delayedPage: Task<Void, Never>?
        var delayedTable: PluginTableModel?
        defer { delayedReply?.resume(); delayedPage?.cancel() }
        if project != nil && route != "local" {
            let controller = try XCTUnwrap(instance.extensionController)
            let modelUnderTest = PluginTableModel(rpc: { method, input in
                let admission = try await controller.requestExecutorRPC(method: method, input: input)
                let handle = try XCTUnwrap(admission.handle)
                let deadline = ProcessInfo.processInfo.systemUptime + 15
                while ProcessInfo.processInfo.systemUptime < deadline {
                    let (status, result) = try await controller.executorRPCResult(handle: handle)
                    if status == .ok, let result, result.state == .succeeded, result.confirmed {
                        let released = try await controller.forgetExecutorRPC(handle: handle)
                        XCTAssertEqual(released, .ok)
                        if method == PluginTableModel.methods[1] {
                            await withCheckedContinuation { delayedReply = $0 }
                        }
                        return result.outcome
                    }
                    try await Task.sleep(for: .milliseconds(20))
                }
                throw GhostermError.failed("No confirmed SSH Table response")
            }, isCurrent: { [weak table] in table?.valid == true })
            delayedTable = modelUnderTest
            delayedPage = Task { await modelUnderTest.load() }
            try await until("Executor did not confirm the deferred page over SSH") { delayedReply != nil }
        }
        let toggled: Bool
        if project == "sample-b" {
            toggled = await model.invokeExtensionShortcut(workspaceID: origin.id)
        } else {
            toggled = await model.invokePreparedExtensionCommand(id: commandID, workspaceID: origin.id)
        }
        XCTAssertTrue(toggled)
        try await until("Repeated command did not remove its pane and release the Runtime") {
            origin.workspacePanes.isEmpty && instance.canDiscard
        }
        XCTAssertNil(instance.extensionRun); XCTAssertNil(instance.extensionRunIssue)
        XCTAssertFalse(table.valid); XCTAssertNil(table.page)
        if let delayedTable {
            delayedReply?.resume()
            delayedReply = nil
            await delayedPage?.value
            XCTAssertFalse(delayedTable.available)
            XCTAssertNil(delayedTable.page)
        }
        if let siblingTable {
            XCTAssertTrue(siblingTable.valid)
            await siblingTable.showPage(offset: 0)
            XCTAssertNotNil(siblingTable.page)
        }
        if route == "local" {
            XCTAssertTrue(instance.extensionLastLocalProcess?.reaped == true)
            XCTAssertEqual(instance.extensionLastLocalTemporary?.phase, .removed)
        }
    }
}
