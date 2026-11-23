import AppKit
import CryptoKit
import Darwin
import Foundation
import GhostermCore
import SwiftUI
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

/// A real App-owned group; the fixture supplies only owned endpoints and bytes.
@MainActor final class ExtensionMixedConnectionTests: XCTestCase {
    private func fixtureExchange(_ operation: String? = nil) throws -> Data {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_MIXED_PORT"].flatMap(UInt16.init), let token = env["GHOSTERM_MIXED_TOKEN"] else {
            throw XCTSkip("explicit owned JS/signature/SSH-enabled mixed fixture required")
        }
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw GhostermError.failed("fixture socket") }; defer { Darwin.close(fd) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian; address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let result = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard result == 0 else { throw GhostermError.failed("fixture connect") }
        var timeout = timeval(tv_sec: 5, tv_usec: 0)
        guard setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)) == 0 else { throw GhostermError.failed("fixture timeout") }
        let auth = Array((token + (operation.map { ":" + $0 } ?? "") + "\n").utf8)
        guard auth.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == auth.count else { throw GhostermError.failed("fixture auth") }
        var bytes = Data(), buffer = [UInt8](repeating: 0, count: 4096)
        while true {
            let count = Darwin.read(fd, &buffer, buffer.count); if count == 0 { break }
            guard count > 0, bytes.count + count <= 131072 else { throw GhostermError.failed("fixture read") }
            bytes.append(contentsOf: buffer.prefix(count))
        }
        return bytes
    }
    private func fixture() throws -> [String: Any] {
        let bytes = try fixtureExchange()
        guard bytes.count >= 4, bytes.prefix(4).reduce(0, { $0 * 256 + Int($1) }) == bytes.count - 4,
              let value = try JSONSerialization.jsonObject(with: bytes.dropFirst(4)) as? [String: Any] else { throw GhostermError.failed("fixture JSON") }
        return value
    }
    private func until(_ label: String, _ predicate: () async throws -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 20
        repeat {
            if try await predicate() { return }
            try await Task.sleep(for: .milliseconds(10))
        } while ProcessInfo.processInfo.systemUptime < end
        XCTFail(label); throw GhostermError.failed(label)
    }
    @discardableResult private func echo(_ model: ExtensionInstanceModel, text: String, count: Int,
                                         stage: String? = nil, timeoutMilliseconds: UInt32 = 10000) async throws -> Bool {
        let operation = "fixture.echo \(text)/\(count)"
        let label = stage.map { "\($0): \(operation)" } ?? operation
        let started = ProcessInfo.processInfo.systemUptime
        let owner = try XCTUnwrap(model.extensionController)
        let admission = try await owner.requestExecutorRPC(method: "fixture.echo",
            input: JSONSerialization.data(withJSONObject: ["text": text, "count": count]), timeoutMilliseconds: timeoutMilliseconds)
        let handle = try XCTUnwrap(admission.handle)
        var terminal: ExtensionSession.RPCResult?
        try await until("\(label) did not reach a terminal state") {
            let (status, result) = try await owner.executorRPCResult(handle: handle)
            guard status == .ok else { throw GhostermError.failed("\(label): result status \(status)") }
            guard let result, result.state != .queued, result.state != .pending else { return false }
            terminal = result
            return true
        }
        let result = try XCTUnwrap(terminal)
        let detail = String(decoding: result.outcome, as: UTF8.self)
        guard result.state == .succeeded, result.confirmed else {
            let run = model.extensionRun
            let elapsed = ProcessInfo.processInfo.systemUptime - started
            XCTFail("\(label): \(result.state), confirmed=\(result.confirmed), elapsed=\(elapsed)s, " +
                    "runtime=\(run?.runtime?.phase.description ?? "unobserved"), " +
                    "session=\(run?.session?.phase.description ?? "unobserved"), " +
                    "resources=\(run?.receivedResources.count ?? 0), \(detail)")
            return false
        }
        let outcome = try XCTUnwrap(JSONSerialization.jsonObject(with: result.outcome) as? [String: Any])
        let value = try XCTUnwrap(outcome["value"] as? [String: Any])
        let returnedText = value["text"] as? String
        let returnedCount = value["count"] as? Int
        XCTAssertTrue(returnedText == text && returnedCount == count,
                      "\(label): unexpected response text=\(returnedText ?? "missing"), count=\(String(describing: returnedCount))")
        let forgotten = try await owner.forgetExecutorRPC(handle: handle); XCTAssertEqual(forgotten, .ok)
        return returnedText == text && returnedCount == count
    }
    private func resource(_ model: ExtensionInstanceModel, slot: Int, stage: String) async throws -> Bool {
        // Keep the publication deadline at ten seconds and allow time for its result to travel back.
        guard try await echo(model, text: "resource", count: slot, stage: stage, timeoutMilliseconds: 15000) else { return false }
        try await until("\(stage): resource \(slot) was not received") { model.extensionRun?.receivedResources.count == 1 }
        let resource = try XCTUnwrap(model.extensionRun?.receivedResources.first)
        let expected = Data((0..<262144).map { UInt8(($0 + slot * 17) % 251) })
        XCTAssertEqual(resource.bytes, expected); XCTAssertEqual(resource.size, 262144)
        XCTAssertEqual(resource.sha256, SHA256.hash(data: expected).map { String(format: "%02x", $0) }.joined())
        return true
    }
    func testEightMixedExecutorsKeepResourcesAndAdmissionFailuresIndependent() async throws {
        TestWindows.enterAutomationMode()
        let payload = try fixture()
        func string(_ name: String) throws -> String { try XCTUnwrap(payload[name] as? String) }
        let root = URL(fileURLWithPath: try string("scratch")), route = try string("route"), binary = try string("gterm")
        let channel = payload["revocation"] as? [String: Any]
        let channelDirectory = root.appendingPathComponent("channel")
        func decode(_ source: [String: Any], _ field: String) throws -> Data {
            try XCTUnwrap((source[field] as? String).flatMap { Data(base64Encoded: $0) })
        }
        let trust = try channel.map { value in
            ExtensionController.ChannelTrust(channel: "fixture", root: try decode(value, "root"), stateDirectory: channelDirectory)
        }
        func refresh(_ name: String) throws -> PluginDirectorySignatureVerifier.VerifiedChannelPolicy {
            let configuration = try XCTUnwrap(channel)
            let materials = try XCTUnwrap(configuration[name] as? [String: Any])
            return try PluginDirectorySignatureVerifier.refreshChannelPolicy(statement: decode(materials, "statement"),
                signature: decode(materials, "signature"), policy: decode(materials, "policy"), channel: "fixture",
                channelRoot: decode(configuration, "root"), now: UInt64(Date().timeIntervalSince1970), stateDirectory: channelDirectory)
        }
        let stage = root.appendingPathComponent("stage"), config = root.appendingPathComponent("config"), known = root.appendingPathComponent("known")
        let ports = try XCTUnwrap(payload["ports"] as? [Int]), keys = try XCTUnwrap(payload["hostKeys"] as? [String]), user = try string("user")
        var source = "Host mixed-target\n HostName 127.0.0.1\n Port \(ports[1])\n User \(user)\n IdentityFile /owned-in-memory\n"
        if route == "jump" { source += " ProxyJump mixed-jump\nHost mixed-jump\n HostName 127.0.0.1\n Port \(ports[0])\n User \(user)\n IdentityFile /owned-in-memory\n" }
        try Data(source.utf8).write(to: config)
        try Data(zip(ports, keys).map { "[127.0.0.1]:\($0) \($1.trimmingCharacters(in: .newlines))\n" }.joined().utf8).write(to: known)
        let app = isolatedAppModel(configPath: config.path, knownHostsPath: known.path,
            pluginResourceDirectory: root.appendingPathComponent("resources"), pluginChannelTrust: trust)
        let catalog = try XCTUnwrap(app.extensionController), pem = try string("pem")
        app.sshCredentialStorage = .init(key: { path in XCTAssertNotEqual(route, "local"); XCTAssertEqual(path, "/owned-in-memory"); return pem },
            secret: { _, _ in nil }, save: { _, _, _ in XCTFail("fixture must not save credentials") }, remove: { _, _ in })
        addTeardownBlock { let done = await app.closeExtensionOwners(); XCTAssertTrue(done) }
        let review: ExtensionController.PackageReview
        let managedReleaseID: String?
        if channel != nil {
            try FileManager.default.createDirectory(at: channelDirectory, withIntermediateDirectories: false,
                                                    attributes: [.posixPermissions: 0o700])
            XCTAssertEqual(try refresh("initial").generation, 1)
            let prepared = try await catalog.prepareReleaseUsingConfiguredChannel(
                artifact: URL(fileURLWithPath: string("artifact")), descriptor: URL(fileURLWithPath: string("descriptor")),
                signature: URL(fileURLWithPath: string("signature")))
            let installed = try await catalog.installPreparedRelease(id: prepared.id)
            let selection = try await catalog.previewInstalledSelection(releaseID: installed.signature.descriptorSHA256, expectedRevision: nil)
            _ = try await catalog.confirmInstalledSelection(id: selection.id, acknowledged: true)
            review = installed.review
            managedReleaseID = installed.signature.descriptorSHA256
        } else {
            review = try await catalog.review(directory: URL(fileURLWithPath: try string("package")))
            managedReleaseID = nil
        }
        let approved = try await catalog.approve(id: review.id, digest: review.digest); XCTAssertEqual(approved, 0)
        let local: ExtensionLocalLaunch? = route == "local" ? .init(executableSHA256: try string("gtermSHA256"),
            workingDirectory: root.path, temporaryDirectory: stage.path, authorized: true) : nil
        func invocation(_ slot: Int, signed: Bool, stale: Bool = false) throws -> ExtensionGroupInvocation {
            let evidence: ExtensionRelayBootstrap.ReleaseEvidence? = signed ? .init(directory: try string("release"),
                policyPath: try string("policy"), policySHA256: stale ? String(repeating: "0", count: 64) : try string("policySHA256"), authorized: true) : nil
            return .init(packageID: review.id, request: .init(executable: binary,
                executorDirectory: try string(signed ? "installed" : "package"), executorDigest: review.digest,
                clientDigest: review.digest, methods: ["fixture.echo"],
                scopedRoots: [.init(id: "workspace", directory: root.appendingPathComponent("workspace-\(slot)").path)],
                releaseEvidence: evidence, variantID: "lua", command: "fixture"),
                sshAlias: "mixed-target", localLaunch: local, clientWorkspace: nil, temporaryDirectory: stage.path)
        }
        let disconnect = payload["disconnectOnStop"] as? Bool == true
        let single = disconnect || channel != nil
        let instanceCount = single ? 1 : 8
        let invocations = try (0..<instanceCount).map { try invocation($0, signed: single || $0 % 2 == 1) }
        await app.startExtensionGroup(invocations)
        let group = try XCTUnwrap(app.extensionConnections.last)
        XCTAssertNil(group.issue); XCTAssertEqual(group.instances.count, instanceCount)
        guard group.instances.count == instanceCount else { return }
        for model in group.instances {
            XCTAssertEqual(model.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
            XCTAssertNil(model.extensionRun?.runtime)
            await model.decideExtensionConsent(allow: true)
        }
        try await until("mixed activation: \(group.instances.compactMap(\.extensionRunIssue))") {
            group.instances.allSatisfy { $0.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        }
        if let managedReleaseID {
            let model = try XCTUnwrap(group.instances.first)
            guard try await resource(model, slot: 0, stage: "before revocation") else { return }
            for tab in app.tabs.tabs { tab.workspacePanes.removeAll { $0.extensionInstance === model } }
            XCTAssertTrue(model.canInteractWithRuntime)
            let before = try await catalog.installedVersions()
            XCTAssertTrue(before.versions.contains { $0.releaseID == managedReleaseID && $0.inUse == true })
            let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
            NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
            defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
            let settings = ConversationUIProbe(root: NSHostingView(rootView: ScrollView { ExtensionsPane(model: app) }))
            defer { settings.close() }
            func node(_ identifier: String) -> NSObject? {
                settings.root.layoutSubtreeIfNeeded()
                return PluginTableRendering.elements(settings.root).first {
                    PluginTableRendering.attribute($0, "accessibilityIdentifier") == identifier
                }
            }
            XCTAssertNil(node("plugin-installed-channel-affected"))
            let revoked = try refresh("revoked")
            XCTAssertEqual(revoked.generation, 2)
            let affected = try await catalog.runningInstancesWithRevokedPublisher(in: revoked)
            XCTAssertEqual(affected, 1)
            try await until("Settings did not observe revocation of a group-only instance") {
                node("plugin-installed-channel-stop") != nil
            }
            XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(node("plugin-installed-channel-stop"))))
            try await until("Settings did not stop and release the detached group instance") {
                guard let notice = node("plugin-installed-channel-notice") else { return false }
                return model.extensionRun == nil && !model.extensionRunBusy &&
                    ["accessibilityValue", "accessibilityLabel"].contains {
                        PluginTableRendering.attribute(notice, $0)?.contains("已确认停止 1 个实例。") == true
                    }
            }
            let after = try await catalog.installedVersions()
            XCTAssertTrue(after.versions.contains { $0.releaseID == managedReleaseID && $0.inUse == false })
            let closed = await group.close()
            XCTAssertTrue(closed)
            return
        }
        if disconnect {
            let model = try XCTUnwrap(group.instances.first)
            guard try await resource(model, slot: 0, stage: "before disconnect") else { return }
            app.showExtensionWorkspace()
            XCTAssertEqual(app.channelStopTargets.filter { $0 === app.extensionInstance }.count, 1,
                           "the primary instance and its pane must be visited only once")
            XCTAssertEqual(app.channelStopTargets.filter { $0 === model }.count, 1)
            for tab in app.tabs.tabs {
                tab.workspacePanes.removeAll { $0.extensionInstance === model }
            }
            XCTAssertTrue(model.canInteractWithRuntime)
            XCTAssertEqual(app.channelStopTargets.filter { $0 === model }.count, 1,
                           "a live group owner remains a channel stop target without its workspace pane")
            XCTAssertEqual(try fixtureExchange("blackhole"), Data("OK\n".utf8))
            let owner = try XCTUnwrap(model.extensionController)
            try await owner.stopRuntime()
            XCTAssertEqual(try fixtureExchange("reset"), Data("OK\n".utf8))
            let resources = root.appendingPathComponent("resources")
            let children = try FileManager.default.contentsOfDirectory(at: resources, includingPropertiesForKeys: nil)
                .filter { $0.lastPathComponent.hasPrefix("resource-") }
            XCTAssertEqual(children.count, 1)
            let directory = try XCTUnwrap(children.first)
            let remaining = directory.appendingPathComponent("owned-cleanup-probe")
            try Data("owned".utf8).write(to: remaining)
            defer { try? FileManager.default.removeItem(at: remaining) }
            let firstClose = await group.close()
            XCTAssertFalse(firstClose, "local cleanup failure must not report closure")
            guard !firstClose else { return }
            XCTAssertTrue(group.cleanupPending)
            XCTAssertFalse(group.closed)
            XCTAssertFalse(group.canAdd)
            XCTAssertEqual(try Data(contentsOf: remaining), Data("owned".utf8))
            try FileManager.default.removeItem(at: remaining)
            let done = await group.close()
            XCTAssertTrue(done)
            XCTAssertFalse(group.cleanupPending)
            XCTAssertFalse(FileManager.default.fileExists(atPath: directory.path))
            XCTAssertTrue(group.state?.released == true)
            XCTAssertFalse(try XCTUnwrap(group.state).peerEOF)
            XCTAssertTrue(group.issue?.contains("执行端退出未确认") == true,
                          "released connection must preserve unconfirmed remote exit")
            return
        }
        await withTaskGroup(of: Void.self) { tasks in
            for (slot, model) in group.instances.enumerated() { tasks.addTask { @MainActor in
                do { try await self.echo(model, text: "slot-\(slot)", count: slot) } catch { XCTFail("mixed RPC: \(error)") }
            } }
        }
        for (slot, model) in group.instances.enumerated() {
            guard try await resource(model, slot: slot, stage: "initial instances") else { return }
        }
        XCTAssertEqual(Set(group.instances.compactMap { $0.extensionRun?.receivedResources.first?.sha256 }).count, 8)
        let uncertain = group.instances[3]
        let invalidInput = Data("[]".utf8)
        let invalid = try await uncertain.callPluginViewOutcome("fixture.echo", inputBytes: invalidInput, command: "fixture")
        let rejectedOutcome = try XCTUnwrap(JSONSerialization.jsonObject(with: invalid) as? [String: Any])
        XCTAssertEqual((rejectedOutcome["error"] as? [String: String])?["code"], "INVALID_INPUT")
        var receivedAdmission = false
        do {
            _ = try await uncertain.callPluginViewOutcome("fixture.echo", inputBytes: invalidInput, command: "fixture") {
                receivedAdmission = true
                uncertain.runtimeObservationFailed("owned mixed observation failure")
                XCTAssertEqual(uncertain.extensionRun?.runtime?.phase, ExtensionRuntime.Phase.active.rawValue, "Retain the actual last observation")
                XCTAssertFalse(uncertain.canInteractWithRuntime)
                XCTAssertTrue(uncertain.runtimeStateIsUnknown)
                XCTAssertTrue(group.instances.enumerated().allSatisfy { $0.offset == 3 || $0.element.canInteractWithRuntime })
                do {
                    _ = try await uncertain.callPluginViewOutcome("fixture.echo", inputBytes: Data(#"{"text":"blocked","count":3}"#.utf8), command: "fixture")
                    XCTFail("An unobserved Runtime must not admit another business request")
                } catch { XCTAssertTrue(String(describing: error).contains("运行状态未知")) }
                let stopped = await uncertain.requestClose(); XCTAssertTrue(stopped)
            }
            XCTFail("Late rejected admission was returned after closing its original run")
        } catch is CancellationError {
            XCTAssertTrue(receivedAdmission)
        }
        let rejected = await group.add(try invocation(3, signed: true, stale: true), app: app)
        XCTAssertFalse(rejected); XCTAssertEqual(group.instances.filter { !$0.canDiscard }.count, 7)
        for (slot, model) in group.instances.enumerated() where !model.canDiscard { try await echo(model, text: "after-rejection-\(slot)", count: slot) }
        let added = await group.add(try invocation(3, signed: true), app: app)
        XCTAssertTrue(added, group.issue ?? "")
        let replacement = try XCTUnwrap(group.instances.last)
        XCTAssertEqual(replacement.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
        await replacement.decideExtensionConsent(allow: true)
        try await until("replacement activation") { replacement.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        guard try await resource(replacement, slot: 3, stage: "online Node replacement") else { return }
        for model in group.instances { let done = await model.requestClose(); XCTAssertTrue(done) }
        let empty = try await group.controller.stepMultiplexConnection(); XCTAssertEqual(empty.instances, 0); XCTAssertTrue(group.canAdd)
        let reopened = await group.add(try invocation(0, signed: true), app: app)
        XCTAssertTrue(reopened, group.issue ?? "")
        let final = try XCTUnwrap(group.instances.last)
        await final.decideExtensionConsent(allow: true)
        try await until("empty connection Exec activation") { final.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        guard try await resource(final, slot: 0, stage: "Exec reopened after empty group") else { return }
        let done = await group.close(); XCTAssertTrue(done)
        XCTAssertTrue(group.state?.released == true); XCTAssertTrue(group.state?.peerEOF == true)
        if route == "local" { XCTAssertTrue(group.process?.reaped == true) }
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: stage.path).isEmpty)
    }
}
