import AppKit
import CryptoKit
import Darwin
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Actual AppModel/Client Runtime -> owned SSH -> gterm -> Node or native Exec.
/// The authenticated fixture service lends ephemeral credentials in memory only.
@MainActor final class ExecutorSSHAppTests: XCTestCase {
    private func service(_ action: String = "fixture") throws -> [String: Any] {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_EXECUTION_PORT"], let token = env["GHOSTERM_EXECUTION_TOKEN"] else {
            throw XCTSkip("requires the owned execution SSH fixture")
        }
        let socket = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        XCTAssertGreaterThanOrEqual(socket, 0); defer { Darwin.close(socket) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_port = UInt16(port)!.bigEndian
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let connected = withUnsafePointer(to: &address) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(socket, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        XCTAssertEqual(connected, 0)
        var timeout = timeval(tv_sec: 5, tv_usec: 0)
        XCTAssertEqual(setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size)), 0)
        let request = Data("\(token)\n\(action)\n".utf8)
        XCTAssertEqual(request.withUnsafeBytes { Darwin.write(socket, $0.baseAddress, $0.count) }, request.count)
        func read(_ n: Int) throws -> Data {
            var data = Data(count: n), at = 0
            while at < n {
                let got = data.withUnsafeMutableBytes { Darwin.read(socket, $0.baseAddress!.advanced(by: at), n - at) }
                guard got > 0 else { throw GhostermError.failed("owned fixture read failed") }
                at += got
            }
            return data
        }
        let header = try read(4)
        let size = header.reduce(0) { ($0 << 8) | Int($1) }
        guard (1...1048576).contains(size) else { throw GhostermError.failed("owned fixture frame limit") }
        return try XCTUnwrap(JSONSerialization.jsonObject(with: read(size)) as? [String: Any])
    }
    private func until(_ label: String, _ check: @escaping () async throws -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 20
        while ProcessInfo.processInfo.systemUptime < deadline {
            if try await check() { return }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        XCTFail(label); throw GhostermError.failed(label)
    }
    func testRemoteExecutionThroughAppModel() async throws {
        TestWindows.enterAutomationMode()
        let payload = try service()
        func string(_ name: String) throws -> String { try XCTUnwrap(payload[name] as? String) }
        let runtime = try string("runtime"), mode = try string("mode"), route = try string("route")
        let resourceCase = mode.hasPrefix("resource")
        let processCase = mode.hasPrefix("process")
        let processPending = mode == "process-stop" || mode == "process-loss"
        let scratch = URL(fileURLWithPath: try string("scratch")), package = URL(fileURLWithPath: try string("packageDirectory"))
        let config = scratch.appendingPathComponent("ssh_config"), known = scratch.appendingPathComponent("known")
        let ports = try XCTUnwrap(payload["ports"] as? [Int]), user = try string("user")
        var configuration = "Host execution-target\n HostName 127.0.0.1\n Port \(ports[1])\n User \(user)\n IdentityFile /owned-in-memory\n"
        if route == "jump" { configuration += " ProxyJump execution-jump\nHost execution-jump\n HostName 127.0.0.1\n Port \(ports[0])\n User \(user)\n IdentityFile /owned-in-memory\n" }
        try Data(configuration.utf8).write(to: config); try Data().write(to: known)
        let model = isolatedAppModel(configPath: config.path, knownHostsPath: known.path)
        let controller = try XCTUnwrap(model.extensionController)
        let pem = try string("pem")
        model.sshCredentialStorage = .init(key: { path in
            XCTAssertEqual(path, "/owned-in-memory"); return pem
        }, secret: { _, _ in nil }, save: { _, _, _ in XCTFail("fixture must not save secrets") }, remove: { _, _ in })
        let interaction = CodexRouteInteraction(); model.interaction = interaction
        let review = try await controller.review(directory: package)
        let approval = try await controller.approve(id: review.id, digest: review.digest)
        XCTAssertEqual(approval, 0)
        let selected: ExtensionRelayBootstrap.Execution.Runtime
        if runtime == "node" {
            selected = .node(.init(executable: try string("node"), version: try string("nodeVersion"),
                sha256: try string("nodeDigest"), sdkDirectory: try string("sdk"), sdkCatalogSHA256: try string("sdkDigest")))
        } else { selected = .exec }
        let execution = ExtensionRelayBootstrap.Execution(runtime: selected, temporaryDirectory: try string("temporary"), authorized: mode != "no-approval")
        let programs: [ExtensionRelayBootstrap.ProcessProgram] = processCase
            ? [.init(name: "cat", executable: try string("processExecutable"), version: "fixture", sha256: try string("processDigest"))] : []
        var environment = ["GT_EXECUTION_APP_FIXTURE"]
        if mode == "process-environment" { environment.append("GT_SELECTED") }
        addTeardownBlock { await model.stopExtensionRun(); _ = await controller.close() }
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "fixture",
            arguments: ["中文😀", "", "literal'$(owned)"], sshAlias: "execution-target",
            executable: try string("wrapper"), executorDirectory: package.path, executorDigest: review.digest,
            methods: ["fixture.echo"], hostMethods: [],
            scopedRoots: resourceCase || processCase ? [.init(id: "workspace", directory: try string("workspace"))] : [],
            processPrograms: programs, processEnvironmentNames: environment, executorExecution: execution)
        if mode == "no-approval" {
            XCTAssertNotNil(model.extensionRunIssue); XCTAssertNil(model.extensionRun)
            XCTAssertTrue(interaction.hosts.isEmpty)
            XCTAssertEqual(try service("audit")["wrapperStarted"] as? Bool, false)
            return
        }
        XCTAssertNil(model.extensionRunIssue)
        let run = try XCTUnwrap(model.extensionRun, model.extensionRunIssue ?? "missing run")
        XCTAssertEqual(run.session?.phase, ExtensionSession.Phase.consent.rawValue)
        XCTAssertEqual(run.executorExecution, execution)
        var required = ["session.rpc"]
        if resourceCase { required += ["resource.receive", "file.readScoped"] }
        if processCase { required += ["process.spawn", "process.pipes"] }
        XCTAssertEqual(Set(run.requiredCapabilities), Set(required))
        XCTAssertEqual(interaction.hosts.count, route == "jump" ? 2 : 1)
        XCTAssertEqual(try service("audit")["activated"] as? Bool, false, "No module execution before consent")
        XCTAssertEqual(try service("audit")["entered"] as? Bool, false, "No Executor entry before consent")
        if mode == "deny" {
            await model.decideExtensionConsent(allow: false)
            XCTAssertEqual(try service("audit")["activated"] as? Bool, false)
            await model.stopExtensionRun(); XCTAssertNil(model.extensionRunIssue)
            return
        }
        await model.decideExtensionConsent(allow: true)
        try await until("Client and remote Executor did not activate: \(model.extensionRunIssue ?? "")") {
            let audit = try self.service("audit")
            return model.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue && (audit["activated"] as? Bool) == true
        }
        XCTAssertEqual(try service("audit")["environment"] as? String, try string("temporary"))
        let specialized = resourceCase || (processCase && !processPending)
        let input: [String: Any] = ["text": specialized ? mode.replacingOccurrences(of: "-", with: "_").uppercased() : "echo 中文😀 '\" }", "count": 17]
        let admission = try await controller.requestExecutorRPC(method: "fixture.echo", input: JSONSerialization.data(withJSONObject: input))
        let handle = try XCTUnwrap(admission.handle); XCTAssertEqual(admission.status, .ok)
        var completed: ExtensionSession.RPCResult?
        try await until("RPC echo did not confirm") {
            let (status, result) = try await controller.executorRPCResult(handle: handle)
            XCTAssertEqual(status, .ok)
            completed = result
            return result?.confirmed == true
        }
        XCTAssertNil(model.extensionRunIssue)
        XCTAssertEqual(completed?.state, .succeeded, String(data: completed?.outcome ?? Data(), encoding: .utf8) ?? "")
        guard completed?.state == .succeeded else { return }
        let outcome = try XCTUnwrap(JSONSerialization.jsonObject(with: XCTUnwrap(completed).outcome) as? [String: Any])
        XCTAssertEqual(outcome["kind"] as? String, "success")
        let echoed = try XCTUnwrap(outcome["value"] as? [String: Any])
        if processCase && !processPending {
            let audit = try service("audit")
            let proof = try XCTUnwrap(audit["processProof"] as? [String: Any])
            if mode == "process-root" || mode == "process-program" {
                XCTAssertEqual(echoed["text"] as? String, "DENIED"); XCTAssertEqual(echoed["count"] as? Int, 0)
                XCTAssertEqual(proof["rejected"] as? Bool, true)
            } else if mode == "process-cancel" {
                XCTAssertEqual(echoed["text"] as? String, "cancelled")
                XCTAssertEqual(echoed["count"] as? Int, payload["processSignal"] as? Int)
                XCTAssertEqual(proof["signal"] as? Int, payload["processSignal"] as? Int)
                XCTAssertEqual(proof["reaped"] as? Bool, true)
            } else {
                let count = try XCTUnwrap(echoed["count"] as? Int), digest = try XCTUnwrap(echoed["text"] as? String)
                XCTAssertEqual(proof["reaped"] as? Bool, true); XCTAssertEqual(proof["signal"] as? Int, 0)
                XCTAssertEqual(proof["sha256"] as? String, digest); XCTAssertEqual(proof["bytes"] as? Int, count)
                if mode == "process-stderr" {
                    XCTAssertGreaterThan(count, 0); XCTAssertLessThanOrEqual(count, 8192)
                    XCTAssertEqual(digest.count, 64); XCTAssertNotEqual(proof["code"] as? Int, 0)
                } else {
                    XCTAssertEqual(proof["code"] as? Int, 0)
                    XCTAssertEqual(count, payload["processOutputSize"] as? Int)
                    guard count == (payload["processOutputSize"] as? Int) else { return }
                    XCTAssertEqual(digest, try string("processOutputDigest"))
                }
            }
        } else {
            XCTAssertEqual(echoed["text"] as? String, input["text"] as? String)
            XCTAssertEqual(echoed["count"] as? Int, 17)
        }
        let forgotten = try await controller.forgetExecutorRPC(handle: handle); XCTAssertEqual(forgotten, .ok)
        if mode == "resource" {
            try await until("Resource did not reach AppModel") { model.extensionRun?.receivedResources.count == 1 }
            let resource = try XCTUnwrap(model.extensionRun?.receivedResources.first)
            let expected = Data((0..<262144).map { UInt8($0 % 251) })
            XCTAssertEqual(resource.bytes, expected); XCTAssertEqual(resource.size, UInt64(expected.count))
            XCTAssertEqual(resource.displayName, "file.bin"); XCTAssertEqual(resource.mime, "application/octet-stream")
            let digest = SHA256.hash(data: expected).map { String(format: "%02x", $0) }.joined()
            XCTAssertEqual(resource.sha256, digest); XCTAssertEqual(digest, try string("resourceDigest"))
            XCTAssertEqual(try service("audit")["resourceReceipt"] as? String, digest + " 262144")
        } else if resourceCase {
            XCTAssertTrue(try XCTUnwrap(model.extensionRun).receivedResources.isEmpty)
            XCTAssertEqual(try service("audit")["resourceError"] as? String, mode == "resource-link" ? "UNSUPPORTED" : "DENIED")
            XCTAssertNil(try service("audit")["resourceReceipt"])
        }
        if mode == "pending-cancel" || mode == "pending-loss" || processPending {
            let text = processPending ? mode.replacingOccurrences(of: "-", with: "_").uppercased() : "WAIT"
            let pending = try await controller.requestExecutorRPC(method: "fixture.echo", input: JSONSerialization.data(withJSONObject: ["text": text, "count": 99]))
            let pendingHandle = try XCTUnwrap(pending.handle)
            try await until("Remote handler did not start") { try self.service("audit")["handler"] as? Bool == true }
            if processPending {
                let audit = try service("audit")
                XCTAssertFalse(try XCTUnwrap(audit["businessLivePids"] as? [Int]).isEmpty, "Business child must be observed live before interruption: \(audit.filter { $0.key.hasPrefix("business") })")
            }
            if mode == "pending-loss" || mode == "process-loss" { _ = try service("kill") }
            else if mode == "process-stop" { try await controller.stopRuntime() }
            else {
                let cancelled = try await controller.cancelExecutorRPC(handle: pendingHandle); XCTAssertEqual(cancelled, .ok)
                let (_, observed) = try await controller.executorRPCResult(handle: pendingHandle)
                XCTAssertTrue(try XCTUnwrap(observed).cancelRequested)
                XCTAssertFalse(try XCTUnwrap(observed).confirmed)
                try await controller.stopRuntime()
            }
            try await until("Pending interruption did not become unknown") {
                let (status, result) = try await controller.executorRPCResult(handle: pendingHandle)
                XCTAssertEqual(status, .ok)
                completed = result
                return result?.state == .unknown
            }
            XCTAssertFalse(try XCTUnwrap(completed).confirmed, "Interruption is not a remote business receipt")
        }
        await model.stopExtensionRun()
        XCTAssertNil(model.extensionRunIssue); XCTAssertNil(model.extensionRun)
    }
}
