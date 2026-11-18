import AppKit
import CryptoKit
import Darwin
import Foundation
import GhostermCore
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

/// A real App-owned group; the fixture supplies only owned endpoints and bytes.
@MainActor final class ExtensionMixedConnectionTests: XCTestCase {
    private func fixture() throws -> [String: Any] {
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
        let auth = Array((token + "\n").utf8)
        guard auth.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == auth.count else { throw GhostermError.failed("fixture auth") }
        var bytes = Data(), buffer = [UInt8](repeating: 0, count: 4096)
        while true {
            let count = Darwin.read(fd, &buffer, buffer.count); if count == 0 { break }
            guard count > 0, bytes.count + count <= 131072 else { throw GhostermError.failed("fixture read") }
            bytes.append(contentsOf: buffer.prefix(count))
        }
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
    @discardableResult private func echo(_ model: ExtensionInstanceModel, text: String, count: Int) async throws -> Bool {
        let owner = try XCTUnwrap(model.extensionController)
        let admission = try await owner.requestExecutorRPC(method: "fixture.echo",
            input: JSONSerialization.data(withJSONObject: ["text": text, "count": count]), timeoutMilliseconds: 10000)
        let handle = try XCTUnwrap(admission.handle)
        try await until("RPC \(count): \(model.extensionRunIssue ?? "")") {
            let (_, value) = try await owner.executorRPCResult(handle: handle); return value?.confirmed == true
        }
        let (_, result) = try await owner.executorRPCResult(handle: handle)
        XCTAssertEqual(result?.state, .succeeded, "fixture.echo \(text)/\(count): " + String(decoding: result?.outcome ?? Data(), as: UTF8.self))
        guard result?.state == .succeeded else { return false }
        let outcome = try XCTUnwrap(JSONSerialization.jsonObject(with: XCTUnwrap(result).outcome) as? [String: Any])
        let value = try XCTUnwrap(outcome["value"] as? [String: Any])
        XCTAssertEqual(value["text"] as? String, text); XCTAssertEqual(value["count"] as? Int, count)
        let forgotten = try await owner.forgetExecutorRPC(handle: handle); XCTAssertEqual(forgotten, .ok)
        return true
    }
    private func resource(_ model: ExtensionInstanceModel, slot: Int) async throws -> Bool {
        guard try await echo(model, text: "resource", count: slot) else { return false }
        try await until("resource \(slot): \(model.extensionRunIssue ?? "")") { model.extensionRun?.receivedResources.count == 1 }
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
        let stage = root.appendingPathComponent("stage"), config = root.appendingPathComponent("config"), known = root.appendingPathComponent("known")
        let ports = try XCTUnwrap(payload["ports"] as? [Int]), keys = try XCTUnwrap(payload["hostKeys"] as? [String]), user = try string("user")
        var source = "Host mixed-target\n HostName 127.0.0.1\n Port \(ports[1])\n User \(user)\n IdentityFile /owned-in-memory\n"
        if route == "jump" { source += " ProxyJump mixed-jump\nHost mixed-jump\n HostName 127.0.0.1\n Port \(ports[0])\n User \(user)\n IdentityFile /owned-in-memory\n" }
        try Data(source.utf8).write(to: config)
        try Data(zip(ports, keys).map { "[127.0.0.1]:\($0) \($1.trimmingCharacters(in: .newlines))\n" }.joined().utf8).write(to: known)
        let app = isolatedAppModel(configPath: config.path, knownHostsPath: known.path, pluginResourceDirectory: root.appendingPathComponent("resources"))
        let catalog = try XCTUnwrap(app.extensionController), pem = try string("pem")
        app.sshCredentialStorage = .init(key: { path in XCTAssertNotEqual(route, "local"); XCTAssertEqual(path, "/owned-in-memory"); return pem },
            secret: { _, _ in nil }, save: { _, _, _ in XCTFail("fixture must not save credentials") }, remove: { _, _ in })
        addTeardownBlock { let done = await app.closeExtensionOwners(); XCTAssertTrue(done) }
        let review = try await catalog.review(directory: URL(fileURLWithPath: try string("package")))
        let approved = try await catalog.approve(id: review.id, digest: review.digest); XCTAssertEqual(approved, 0)
        let local: ExtensionLocalLaunch? = route == "local" ? .init(executableSHA256: try string("gtermSHA256"),
            workingDirectory: root.path, temporaryDirectory: stage.path, authorized: true) : nil
        let node = ExtensionRelayBootstrap.NodeDependency(executable: try string("node"), version: try string("nodeVersion"),
            sha256: try string("nodeSHA256"), sdkDirectory: try string("sdk"), sdkCatalogSHA256: try string("sdkSHA256"))
        func invocation(_ slot: Int, runtime: String, signed: Bool, stale: Bool = false) throws -> ExtensionGroupInvocation {
            let execution: ExtensionRelayBootstrap.Execution? = runtime == "portable" ? nil :
                .init(runtime: runtime == "node" ? .node(node) : .exec, temporaryDirectory: stage.path, authorized: true)
            let evidence: ExtensionRelayBootstrap.ReleaseEvidence? = signed ? .init(directory: try string("release"),
                policyPath: try string("policy"), policySHA256: stale ? String(repeating: "0", count: 64) : try string("policySHA256"), authorized: true) : nil
            return .init(packageID: review.id, request: .init(executable: binary,
                executorDirectory: try string(signed ? "installed" : "package"), executorDigest: review.digest,
                clientDigest: review.digest, methods: ["fixture.echo"],
                scopedRoots: [.init(id: "workspace", directory: root.appendingPathComponent("workspace-\(slot)").path)],
                execution: execution, releaseEvidence: evidence, variantID: runtime, command: "fixture"),
                sshAlias: "mixed-target", localLaunch: local, clientWorkspace: nil, temporaryDirectory: stage.path)
        }
        let runtimes = ["portable", "node", "exec", "portable", "node", "exec", "portable", "node"]
        let invocations = try runtimes.enumerated().map { try invocation($0.offset, runtime: $0.element, signed: $0.offset % 2 == 1) }
        await app.startExtensionGroup(invocations)
        let group = try XCTUnwrap(app.extensionConnections.last)
        XCTAssertNil(group.issue); XCTAssertEqual(group.instances.count, 8)
        guard group.instances.count == 8 else { return }
        for model in group.instances {
            XCTAssertEqual(model.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
            XCTAssertNil(model.extensionRun?.runtime)
            await model.decideExtensionConsent(allow: true)
        }
        try await until("mixed activation: \(group.instances.compactMap(\.extensionRunIssue))") {
            group.instances.allSatisfy { $0.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        }
        await withTaskGroup(of: Void.self) { tasks in
            for (slot, model) in group.instances.enumerated() { tasks.addTask { @MainActor in
                do { try await self.echo(model, text: "slot-\(slot)", count: slot) } catch { XCTFail("mixed RPC: \(error)") }
            } }
        }
        for (slot, model) in group.instances.enumerated() { guard try await resource(model, slot: slot) else { return } }
        XCTAssertEqual(Set(group.instances.compactMap { $0.extensionRun?.receivedResources.first?.sha256 }).count, 8)
        let uncertain = group.instances[3]
        uncertain.runtimeObservationFailed("owned mixed observation failure")
        XCTAssertEqual(uncertain.extensionRun?.runtime?.phase, ExtensionRuntime.Phase.active.rawValue, "Retain the actual last observation")
        XCTAssertFalse(uncertain.canInteractWithRuntime)
        XCTAssertTrue(uncertain.runtimeStateIsUnknown)
        XCTAssertTrue(group.instances.enumerated().allSatisfy { $0.offset == 3 || $0.element.canInteractWithRuntime })
        do {
            _ = try await uncertain.callPluginViewOutcome("fixture.echo", inputBytes: Data(#"{"text":"blocked","count":3}"#.utf8), command: "fixture")
            XCTFail("An unobserved Runtime must not admit another business request")
        } catch { XCTAssertTrue(String(describing: error).contains("运行状态未知")) }
        let stopped = await group.instances[3].requestClose(); XCTAssertTrue(stopped)
        let rejected = await group.add(try invocation(3, runtime: "node", signed: true, stale: true), app: app)
        XCTAssertFalse(rejected); XCTAssertEqual(group.instances.filter { !$0.canDiscard }.count, 7)
        for (slot, model) in group.instances.enumerated() where !model.canDiscard { try await echo(model, text: "after-rejection-\(slot)", count: slot) }
        let added = await group.add(try invocation(3, runtime: "node", signed: true), app: app)
        XCTAssertTrue(added, group.issue ?? "")
        let replacement = try XCTUnwrap(group.instances.last)
        XCTAssertEqual(replacement.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
        await replacement.decideExtensionConsent(allow: true)
        try await until("replacement activation") { replacement.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        guard try await resource(replacement, slot: 3) else { return }
        for model in group.instances { let done = await model.requestClose(); XCTAssertTrue(done) }
        let empty = try await group.controller.stepMultiplexConnection(); XCTAssertEqual(empty.instances, 0); XCTAssertTrue(group.canAdd)
        let reopened = await group.add(try invocation(0, runtime: "exec", signed: true), app: app)
        XCTAssertTrue(reopened, group.issue ?? "")
        let final = try XCTUnwrap(group.instances.last)
        await final.decideExtensionConsent(allow: true)
        try await until("empty connection Exec activation") { final.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        guard try await resource(final, slot: 0) else { return }
        let done = await group.close(); XCTAssertTrue(done)
        XCTAssertTrue(group.state?.released == true); XCTAssertTrue(group.state?.peerEOF == true)
        if route == "local" { XCTAssertTrue(group.process?.reaped == true) }
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: stage.path).isEmpty)
    }
}
