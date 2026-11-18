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
        let localTransport = payload["localTransport"] as? Bool == true
        let clientCase = mode.hasPrefix("client-")
        let resourceCase = mode.hasPrefix("resource") || clientCase
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
            XCTAssertFalse(localTransport, "Local launch must not ask for SSH credentials"); XCTAssertEqual(path, "/owned-in-memory"); return pem
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
        let attaching = payload["ipcDirectory"] is String
        let execution: ExtensionRelayBootstrap.Execution? = attaching ? nil : ExtensionRelayBootstrap.Execution(runtime: selected, temporaryDirectory: try string("temporary"), authorized: mode != "no-approval")
        let attachment: ExtensionRelayBootstrap.Attachment? = attaching ? .init(directory: try string("ipcDirectory"), instance: try string("ipcInstance"), authorized: mode != "no-approval", remoteProcesses: processCase) : nil
        let programs: [ExtensionRelayBootstrap.ProcessProgram] = processCase
            ? [.init(name: "cat", executable: try string("processExecutable"), version: "fixture", sha256: try string("processDigest"))] : []
        var environment = ["GT_EXECUTION_APP_FIXTURE"]
        if mode == "process-environment" { environment.append("GT_SELECTED") }
        let local = scratch.appendingPathComponent("local")
        var clientWorkspace: ExtensionController.ClientWorkspace?
        if clientCase {
            try FileManager.default.createDirectory(at: local, withIntermediateDirectories: false)
            try Data((0..<262144).map { UInt8(($0 * 7 + 3) % 251) }).write(to: local.appendingPathComponent("upload.bin"))
            let outside = scratch.appendingPathComponent("outside.bin")
            try Data("UNAUTHORIZED owned fixture".utf8).write(to: outside)
            try FileManager.default.createSymbolicLink(atPath: local.appendingPathComponent("link.bin").path, withDestinationPath: outside.path)
            let selectedPath: String
            if mode == "client-directory-link" {
                let link = scratch.appendingPathComponent("directory-link")
                try FileManager.default.createSymbolicLink(atPath: link.path, withDestinationPath: local.path)
                selectedPath = link.path
            } else { selectedPath = mode == "client-directory-file" ? local.appendingPathComponent("upload.bin").path : local.path }
            clientWorkspace = .init(directory: selectedPath, authorized: mode != "client-no-approval")
        }
        let processStatePath = localTransport ? URL(fileURLWithPath: try string("processStatePath")) : nil
        addTeardownBlock {
            await model.stopExtensionRun()
            let closed = await controller.close(); XCTAssertTrue(closed)
            if let processStatePath, let state = try await controller.localProcessState() {
                var object: [String: Any] = ["exited": state.childExited, "reaped": state.reaped,
                    "code": state.exitCode as Any? ?? NSNull(), "signal": state.termSignal as Any? ?? NSNull(),
                    "groupFenced": state.groupFenced, "groupError": state.groupSignalError, "error": state.error]
                if let temporary = try await controller.localTemporaryState() {
                    object["temporary"] = ["directory": temporary.directory, "phase": temporary.phase.rawValue, "error": temporary.error]
                    XCTAssertEqual(temporary.phase, .removed); XCTAssertEqual(temporary.error, 0)
                } else { XCTFail("Native temporary ownership observation missing") }
                try JSONSerialization.data(withJSONObject: object).write(to: processStatePath)
            }
        }
        let localLaunch: ExtensionLocalLaunch? = localTransport ? .init(
            executableSHA256: mode == "local-digest" ? String(repeating: "0", count: 64) : try string("localDigest"),
            workingDirectory: try string("localWorkingDirectory") + (mode == "local-cwd" ? "/missing" : ""),
            temporaryDirectory: try string("temporary"), authorized: mode != "local-no-approval") : nil
        await model.startExtensionClient(packageID: review.id, packageDigest: review.digest, command: "fixture",
            arguments: ["中文😀", "", "literal'$(owned)"], sshAlias: "execution-target",
            executable: try string("wrapper"), executorDirectory: attaching ? "" : package.path, executorDigest: review.digest,
            methods: ["fixture.echo"], hostMethods: [],
            scopedRoots: !attaching && (resourceCase || processCase) ? [.init(id: "workspace", directory: try string("workspace"))] : [],
            processPrograms: attaching ? [] : programs, processEnvironmentNames: attaching ? [] : environment, executorExecution: execution, executorAttachment: attachment, clientWorkspace: clientWorkspace, localLaunch: localLaunch)
        if ["no-approval", "client-no-approval", "client-undeclared", "local-no-approval", "local-digest", "local-cwd"].contains(mode) {
            XCTAssertNotNil(model.extensionRunIssue)
            guard model.extensionRunIssue != nil else { return }
            XCTAssertNil(model.extensionRun)
            XCTAssertTrue(interaction.hosts.isEmpty)
            XCTAssertEqual(try service("audit")["wrapperStarted"] as? Bool, attaching)
            if attaching {
                XCTAssertEqual(try service("audit")["attachStarted"] as? Bool, false)
                XCTAssertEqual(try service("audit")["ipcListening"] as? Bool, true)
                _ = try service("stop-front")
            }
            return
        }
        if mode.hasPrefix("attach-") {
            XCTAssertNotNil(model.extensionRunIssue)
            XCTAssertNil(model.extensionRun)
            XCTAssertEqual(try service("audit")["activated"] as? Bool, false)
            XCTAssertEqual(try service("audit")["entered"] as? Bool, false)
            if localTransport {
                XCTAssertNotNil(model.extensionLastLocalProcess, "Native must retain the rejected attacher's process observation")
            } else {
                XCTAssertEqual(try service("audit")["attachStarted"] as? Bool, true)
            }
            XCTAssertEqual(try service("audit")["ipcListening"] as? Bool, true)
            _ = try service("stop-front")
            return
        }
        XCTAssertNil(model.extensionRunIssue)
        let run = try XCTUnwrap(model.extensionRun, model.extensionRunIssue ?? "missing run")
        XCTAssertEqual(run.session?.phase, ExtensionSession.Phase.consent.rawValue)
        XCTAssertEqual(run.executorExecution, execution)
        XCTAssertEqual(run.executorAttachment, attachment)
        if attaching {
            if localTransport {
                let argv = try JSONDecoder().decode([String].self, from: Data(run.remoteCommand.utf8))
                XCTAssertTrue(argv.contains("--attach")); XCTAssertFalse(argv.contains("--stdio"))
            } else { XCTAssertTrue(run.remoteCommand.contains("'--attach'")); XCTAssertFalse(run.remoteCommand.contains("'--stdio'")) }
        }
        XCTAssertEqual(run.clientWorkspace, clientWorkspace)
        XCTAssertFalse(run.remoteCommand.contains(local.path), "Local grant must not become a remote path")
        var required = ["session.rpc"]
        if resourceCase { required += ["resource.receive", "file.readScoped"] }
        if processCase { required += ["process.spawn", "process.pipes"] }
        XCTAssertEqual(Set(run.requiredCapabilities), Set(required))
        XCTAssertEqual(interaction.hosts.count, localTransport ? 0 : route == "jump" ? 2 : 1)
        XCTAssertEqual(try service("audit")["activated"] as? Bool, false, "No module execution before consent")
        XCTAssertEqual(try service("audit")["entered"] as? Bool, false, "No Executor entry before consent")
        if mode == "deny" {
            await model.decideExtensionConsent(allow: false)
            XCTAssertEqual(try service("audit")["activated"] as? Bool, false)
            await model.stopExtensionRun(); XCTAssertNil(model.extensionRunIssue)
            return
        }
        await model.decideExtensionConsent(allow: true)
        if mode == "client-directory-link" || mode == "client-directory-file" {
            XCTAssertNotNil(model.extensionRunIssue)
            guard model.extensionRunIssue != nil else { return }
            XCTAssertNil(model.extensionRun?.runtime)
            await model.stopExtensionRun(); XCTAssertNil(model.extensionRunIssue)
            return
        }
        try await until("Client and remote Executor did not activate: \(model.extensionRunIssue ?? "")") {
            let audit = try self.service("audit")
            return model.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue && (audit["activated"] as? Bool) == true
        }
        let actualTemporary: String
        if localTransport && !attaching {
            actualTemporary = try XCTUnwrap(run.session?.temporary?.directory)
            XCTAssertEqual(URL(fileURLWithPath: actualTemporary).deletingLastPathComponent().path, try string("temporary"))
            XCTAssertTrue(URL(fileURLWithPath: actualTemporary).lastPathComponent.hasPrefix("ghosterm-local-"))
        } else { actualTemporary = try string("temporary") }
        XCTAssertEqual(try service("audit")["environment"] as? String, actualTemporary)
        let ttyState = try service("audit")["tty"] as? [Bool]
        let foregroundTmux = payload["foregroundTmux"] as? Bool == true
        let expectedTTY = Array(repeating: foregroundTmux, count: 3)
        if !localTransport || attaching { XCTAssertEqual(ttyState, expectedTTY, "Original foreground descriptors must match the selected fixture") }
        guard localTransport && !attaching || ttyState == expectedTTY else { return }
        let runtimeTTY = try service("audit")["runtimeTTY"] as? [Bool]
        XCTAssertEqual(runtimeTTY, [false, false, false], "Plugin protocol must remain independent of terminal PTYs")
        guard runtimeTTY == [false, false, false] else { return }
        let checkOrdinaryTerminal = mode == "allow" && !localTransport && !attaching
        if checkOrdinaryTerminal {
            model.connect(to: .host("execution-target"))
            addTeardownBlock { await model.disconnectForTesting() }
            try await until("Ordinary SSH terminal did not start alongside the plugin") {
                model.tabs.activeTab?.activePane?.isRunning == true
            }
            try await verifyOrdinaryTerminal(model, stage: "ACTIVE")
            XCTAssertEqual(model.extensionRun?.runtime?.phase, ExtensionRuntime.Phase.active.rawValue)
        }
        if payload["tmuxTopology"] is String {
            func probe(_ label: String) async throws {
                let admission = try await controller.requestExecutorRPC(method: "fixture.echo",
                    input: JSONSerialization.data(withJSONObject: ["text": label, "count": 23]))
                let handle = try XCTUnwrap(admission.handle)
                try await until("Plugin did not confirm after terminal routing changed") {
                    let (_, result) = try await controller.executorRPCResult(handle: handle)
                    return result?.confirmed == true
                }
                let (_, result) = try await controller.executorRPCResult(handle: handle)
                XCTAssertEqual(result?.state, .succeeded)
                let outcome = try XCTUnwrap(JSONSerialization.jsonObject(with: XCTUnwrap(result).outcome) as? [String: Any])
                let value = try XCTUnwrap(outcome["value"] as? [String: Any])
                XCTAssertEqual(value["text"] as? String, label); XCTAssertEqual(value["count"] as? Int, 23)
                let forgotten = try await controller.forgetExecutorRPC(handle: handle); XCTAssertEqual(forgotten, .ok)
            }
            _ = try service("tmux-pane"); try await probe("TMUX_PANE")
            _ = try service("tmux-window"); try await probe("TMUX_WINDOW")
            if foregroundTmux { _ = try service("tmux-front"); try await probe("TMUX_FRONT") }
            if payload["stopTerminal"] as? Bool == true {
                _ = try service("tmux-stop-terminal"); try await probe("TMUX_CLOSED")
            }
        }
        if clientCase {
            var rejected = false
            do { _ = try await controller.publishClientResource(instanceID: "obsolete-runtime", relativePath: "upload.bin") }
            catch ExtensionController.ControllerError.resourceUnavailable { rejected = true }
            XCTAssertTrue(rejected, "Stale Native action must not publish into another Runtime")
            guard rejected else { return }
            XCTAssertNil(model.extensionRun?.runtime?.publication)
        }
        if clientCase {
            let path = mode == "client-parent" ? "../outside.bin" : mode == "client-link" ? "link.bin" : "upload.bin"
            await model.publishClientResource(relativePath: path, root: mode == "client-root" ? "outside" : "client")
            if ["client-stop", "client-loss", "client-cancel"].contains(mode) {
                try await until("Native publication did not enter a partial send") {
                    guard let publication = model.extensionRun?.runtime?.publication else { return false }
                    return publication.phase == ExtensionFileSource.Phase.sending.rawValue && publication.accepted > 0 && publication.accepted < 262144
                }
                XCTAssertFalse(try XCTUnwrap(model.extensionRun?.runtime?.publication).confirmed)
                if mode == "client-cancel" {
                    let id = try XCTUnwrap(model.extensionRun?.runtime?.publication?.id)
                    await model.cancelClientPublication(id: id)
                    try await until("Cancelled Source did not release") { model.extensionRun?.runtime?.publication?.released == true }
                    let publication = try XCTUnwrap(model.extensionRun?.runtime?.publication)
                    XCTAssertEqual(publication.phase, ExtensionFileSource.Phase.stopped.rawValue)
                    XCTAssertEqual(publication.error, ExtensionFileSource.Status.cancelled.rawValue)
                    XCTAssertFalse(publication.expired)
                } else {
                    if mode == "client-loss" { _ = try service("kill") } else { try await controller.stopRuntime() }
                    try await until("Interrupted Runtime did not stop") {
                        guard let runtime = model.extensionRun?.runtime else { return false }
                        return ![ExtensionRuntime.Phase.created.rawValue, ExtensionRuntime.Phase.starting.rawValue, ExtensionRuntime.Phase.active.rawValue].contains(runtime.phase)
                    }
                    // A pending Source can pin a stopped Runtime until the explicit
                    // release drives native Session close. Never call it published.
                    let publication = try XCTUnwrap(model.extensionRun?.runtime?.publication)
                    XCTAssertFalse(publication.confirmed)
                    XCTAssertNotEqual(publication.phase, ExtensionFileSource.Phase.succeeded.rawValue)
                }
                await model.stopExtensionRun(); XCTAssertNil(model.extensionRunIssue)
                return
            }
            try await until("Native publication did not release its Source") { model.extensionRun?.runtime?.publication?.released == true }
            let publication = try XCTUnwrap(model.extensionRun?.runtime?.publication)
            XCTAssertFalse(publication.expired); XCTAssertEqual(publication.cleanupError, 0)
            if mode == "client-resource" {
                XCTAssertEqual(publication.phase, ExtensionFileSource.Phase.succeeded.rawValue)
                XCTAssertTrue(publication.confirmed); XCTAssertEqual(publication.bytes, 262144)
                let bytes = Data((0..<262144).map { UInt8(($0 * 7 + 3) % 251) })
                XCTAssertEqual(publication.sha256, SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined())
            } else {
                XCTAssertEqual(publication.phase, ExtensionFileSource.Phase.failed.rawValue)
                XCTAssertFalse(publication.confirmed)
                XCTAssertEqual(publication.error, (mode == "client-link" ? ExtensionFileSource.Status.unsupported : .denied).rawValue)
            }
        }
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
                    let expectedSize: Int, expectedDigest: String
                    if localTransport && !attaching && mode == "process-environment" {
                        var environment = try XCTUnwrap(payload["processEnvironment"] as? [String: String])
                        environment["TMPDIR"] = actualTemporary
                        let bytes = Data((environment.keys.sorted().map { "\($0)=\(environment[$0]!)" }.joined(separator: "\n") + "\n").utf8)
                        expectedSize = bytes.count; expectedDigest = SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
                    } else { expectedSize = try XCTUnwrap(payload["processOutputSize"] as? Int); expectedDigest = try string("processOutputDigest") }
                    XCTAssertEqual(count, expectedSize)
                    guard count == expectedSize else { return }
                    XCTAssertEqual(digest, expectedDigest)
                }
            }
        } else {
            XCTAssertEqual(echoed["text"] as? String, input["text"] as? String)
            XCTAssertEqual(echoed["count"] as? Int, 17)
        }
        let forgotten = try await controller.forgetExecutorRPC(handle: handle); XCTAssertEqual(forgotten, .ok)
        if mode == "resource" || clientCase {
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
        if clientCase {
            let audit = try service("audit")
            if mode == "client-resource" {
                let publication = try XCTUnwrap(model.extensionRun?.runtime?.publication)
                XCTAssertEqual(audit["clientReceived"] as? String, publication.sha256 + " 262144")
            } else { XCTAssertNil(audit["clientReceived"]) }
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
        if mode == "temporary-retry" {
            let directory = try XCTUnwrap(run.session?.temporary?.directory)
            let moved = scratch.appendingPathComponent("moved-private")
            try FileManager.default.moveItem(atPath: directory, toPath: moved.path)
            await model.stopExtensionRun()
            XCTAssertNotNil(model.extensionRunIssue); XCTAssertNotNil(model.extensionRun)
            XCTAssertEqual(model.extensionLastLocalTemporary?.phase, .retained)
            try FileManager.default.moveItem(atPath: moved.path, toPath: directory)
        }
        await model.stopExtensionRun()
        if checkOrdinaryTerminal {
            try await verifyOrdinaryTerminal(model, stage: "STOPPED")
        }
        if mode == "temporary-retry" {
            XCTAssertTrue(model.extensionRunIssue == nil && model.extensionRun == nil, "Stop retry must finish retained temporary cleanup after Core handles were released")
        } else { XCTAssertNil(model.extensionRunIssue); XCTAssertNil(model.extensionRun) }
    }

    private func verifyOrdinaryTerminal(_ model: AppModel, stage: String) async throws {
        let surface = try XCTUnwrap(model.tabs.activeTab?.activePane?.surface)
        surface.setSize(width: 640, height: 400)
        // Build the expected line on the remote side, so the typed command echo
        // cannot satisfy the grid assertion.
        surface.sendCommandLine(
            #"printf '%s_%s_' GHOSTERM "# + stage
                + #"; printf '\000\377\033\177\303\251' | /usr/bin/od -An -tx1 | /usr/bin/tr -d ' '"#
        )
        try await until("Ordinary SSH terminal grid missing the remote byte receipt (\(stage))") {
            surface.readText()?.contains("GHOSTERM_\(stage)_00ff1b7fc3a9") == true
        }
    }
}
