import AppKit
import CryptoKit
import Darwin
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel
@testable import GhostermApp

@MainActor final class ExtensionConnectionTests: XCTestCase {
    func testNativeManagementBooleansDoNotFoldNumbersOrUnknown() throws {
        for source in ["0", "1", "null", "\"true\""] {
            let value = try JSONSerialization.jsonObject(with: Data(source.utf8), options: .fragmentsAllowed)
            XCTAssertNil(ExtensionConnectionModel.nativeBoolean(value), source)
        }
        XCTAssertEqual(ExtensionConnectionModel.nativeBoolean(true), true)
        XCTAssertEqual(ExtensionConnectionModel.nativeBoolean(false), false)
    }
    private func request(_ slot: Int) -> ExtensionRelayBootstrap.Request {
        .init(executable: "/selected/gterm", executorDirectory: "/selected/package", executorDigest: String(repeating: "a", count: 64),
            clientDigest: String(repeating: "b", count: 64), methods: ["fixture.echo"], command: "fixture", arguments: [String(slot), "中文", "", "'--"])
    }
    func testNativeGroupEncodesEightExactArgumentProfilesWithinProcessBounds() throws {
        let scopes = (0..<8).map { ExtensionController.MultiplexScope(instance: String(format: "%032x", $0 + 1), epoch: UInt64($0 + 1)) }
        let group = try ExtensionRelayBootstrap.buildMultiplex(connection: String(repeating: "c", count: 32), scopes: scopes,
            requests: (0..<8).map(request), local: true, temporaryDirectory: "/owned/tmp")
        XCTAssertEqual(group.argv.count, 37)
        XCTAssertGreaterThan(group.command.utf8.count, 1024)
        XCTAssertLessThanOrEqual(group.command.utf8.count, 32768)
        for i in 0..<8 {
            let hex = Array(group.argv[8 + i * 4].utf8)
            var data: [UInt8] = []
            for j in stride(from: 0, to: hex.count, by: 2) {
                data.append(try XCTUnwrap(UInt8(String(decoding: hex[j..<j+2], as: UTF8.self), radix: 16)))
            }
            XCTAssertEqual(data[0], 1)
            let count = Int(data[1]) * 256 + Int(data[2]); var at = 3, arguments: [String] = []
            for _ in 0..<count {
                let size = Int(data[at]) * 256 + Int(data[at + 1]); at += 2
                arguments.append(String(decoding: data[at..<at+size], as: UTF8.self)); at += size
            }
            XCTAssertEqual(at, data.count)
            XCTAssertEqual(Array(arguments.suffix(4)), request(i).arguments)
        }
    }
    func testGroupRejectsDuplicateScopeAndMissingTemporaryDirectoryBeforeOpening() {
        let scope = ExtensionController.MultiplexScope(instance: String(repeating: "a", count: 32), epoch: 1)
        for (scopes, requests, temporary) in [([scope, scope], [request(0), request(1)], "/tmp-in-fixture"),
                                             ([scope], [request(0)], "relative"),
                                             ([scope], [request(0)], "/bad\0path")] {
            XCTAssertThrowsError(try ExtensionRelayBootstrap.buildMultiplex(connection: String(repeating: "c", count: 32),
                scopes: scopes, requests: requests, local: true, temporaryDirectory: temporary))
        }
    }
    func testInvocationFacadesCloseWithoutClosingSharedCatalogOwner() async throws {
        let root = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: ["portable-js"], capabilities: []))
        let first = try await root.makeInvocationController(), second = try await root.makeInvocationController()
        let closed = await first.close(); XCTAssertTrue(closed)
        do {
            let resolution = try await second.resolve(command: "missing"); XCTAssertNotEqual(resolution.status, 0)
        } catch let error as ExtensionRegistry.RegistryError {
            XCTAssertNotEqual(ExtensionRuntime.supportStatus, 0); XCTAssertEqual(error.status, .unsupported)
        }
        let done = await root.close(); XCTAssertTrue(done)
        let again = await second.close(); XCTAssertTrue(again)
    }
    private func until(_ message: String, _ test: () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 12
        while !test(), ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(for: .milliseconds(20)) }
        XCTAssertTrue(test(), message)
    }

    func testRealEightTableInstancesShareOneConnectionAndCloseIndependently() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let port = env["GHOSTERM_APP_MUX_PORT"].flatMap(UInt16.init), let token = env["GHOSTERM_APP_MUX_TOKEN"] else {
            throw XCTSkip("explicit owned JS-enabled group fixture required")
        }
        let payload = try fixture(port: port, token: token)
        let binary = try XCTUnwrap(payload["gterm"] as? String), directory = try XCTUnwrap(payload["package"] as? String),
            scratch = try XCTUnwrap(payload["scratch"] as? String), route = try XCTUnwrap(payload["route"] as? String)
        TestWindows.enterAutomationMode()
        let root = URL(fileURLWithPath: scratch), config = root.appendingPathComponent("config")
        let ports = try XCTUnwrap(payload["ports"] as? [Int]), keys = try XCTUnwrap(payload["hostKeys"] as? [String])
        let user = try XCTUnwrap(payload["user"] as? String)
        var source = "Host mux-target\n HostName 127.0.0.1\n Port \(ports[1])\n User \(user)\n IdentityFile /owned-in-memory\n"
        if route == "jump" { source += " ProxyJump mux-jump\nHost mux-jump\n HostName 127.0.0.1\n Port \(ports[0])\n User \(user)\n IdentityFile /owned-in-memory\n" }
        try Data(source.utf8).write(to: config)
        try Data(zip(ports, keys).map { "[127.0.0.1]:\($0) \($1)" }.joined().utf8).write(to: root.appendingPathComponent("known"))
        let stage = root.appendingPathComponent("stage")
        try FileManager.default.createDirectory(at: stage, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let app = isolatedAppModel(configPath: config.path, knownHostsPath: root.appendingPathComponent("known").path,
            pluginResourceDirectory: root.appendingPathComponent("resources"))
        let catalog = try XCTUnwrap(app.extensionController)
        let pem = try XCTUnwrap(payload["pem"] as? String)
        app.sshCredentialStorage = .init(key: { path in XCTAssertEqual(path, "/owned-in-memory"); return pem },
            secret: { _, _ in nil }, save: { _, _, _ in XCTFail("must not persist fixture secrets") }, remove: { _, _ in })
        addTeardownBlock { let done = await app.closeExtensionOwners(); XCTAssertTrue(done) }
        let review = try await catalog.review(directory: URL(fileURLWithPath: directory))
        let approved = try await catalog.approve(id: review.id, digest: review.digest); XCTAssertEqual(approved, 0)
        let sha = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: binary))).map { String(format: "%02x", $0) }.joined()
        let launch: ExtensionLocalLaunch? = route == "local" ? .init(executableSHA256: sha, workingDirectory: root.path, temporaryDirectory: stage.path, authorized: true) : nil
        let invocations = (0..<8).map { i in
            ExtensionGroupInvocation(packageID: review.id, request: .init(executable: binary, executorDirectory: directory,
                executorDigest: review.digest, clientDigest: review.digest, methods: [], command: "table", arguments: ["--count", String(i)]),
                sshAlias: "mux-target", localLaunch: launch, clientWorkspace: nil, temporaryDirectory: stage.path)
        }
        let invalidArguments = ExtensionGroupInvocation(packageID: review.id, request: .init(
            executable: binary, executorDirectory: directory, executorDigest: review.digest,
            clientDigest: review.digest, methods: [], command: "table", arguments: ["--count", "invalid"]),
            sshAlias: "mux-target", localLaunch: launch, clientWorkspace: nil, temporaryDirectory: stage.path)
        do {
            try await app.validateQueuedExtensionGroup([invocations[0], invalidArguments])
            XCTFail("启动组预检接受了不符合命令声明的参数")
        } catch {
            XCTAssertTrue(app.extensionConnections.isEmpty)
        }
        try await app.validateQueuedExtensionGroup(invocations)
        XCTAssertTrue(app.extensionConnections.isEmpty)

        let mismatched = ExtensionGroupInvocation(packageID: review.id, request: invocations[0].request,
            sshAlias: "different-target", localLaunch: launch, clientWorkspace: nil, temporaryDirectory: stage.path)
        let rejected = await app.startExtensionGroup([invocations[0], mismatched])
        XCTAssertFalse(rejected)
        XCTAssertTrue(try XCTUnwrap(app.extensionConnections.last).canDiscard)
        XCTAssertNotNil(app.extensionConnectionIssue)

        let started = await app.startExtensionGroup(invocations)
        XCTAssertTrue(started, app.extensionConnectionIssue ?? "")
        let connection = try XCTUnwrap(app.extensionConnections.last)
        XCTAssertNil(connection.issue)
        XCTAssertEqual(connection.instances.count, 8)
        guard connection.instances.count == 8 else { return }
        XCTAssertEqual(Set(connection.instances.map(\.id)).count, 8)
        XCTAssertEqual(app.tabs.tabs.flatMap(\.workspacePanes).compactMap(\.extensionInstance).count, 8)
        for instance in connection.instances {
            XCTAssertNil(instance.extensionRunIssue)
            XCTAssertEqual(instance.extensionRun?.session?.phase, ExtensionSession.Phase.consent.rawValue)
            await instance.decideExtensionConsent(allow: true)
        }
        for (i, instance) in connection.instances.enumerated() {
            try await until("runtime \(i): \(instance.extensionRunIssue ?? "no issue")") { instance.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
            let table = try XCTUnwrap(instance.extensionTable)
            await table.load(); XCTAssertNil(table.issue); XCTAssertEqual(table.descriptor?.total, i)
        }
        let removed = connection.instances[3]
        let stopped = await removed.requestClose(); XCTAssertTrue(stopped)
        for (i, instance) in connection.instances.enumerated() where i != 3 {
            let table = try XCTUnwrap(instance.extensionTable)
            await table.load(); XCTAssertNil(table.issue); XCTAssertEqual(table.descriptor?.total, i)
            XCTAssertEqual(instance.extensionRun?.runtime?.phase, ExtensionRuntime.Phase.active.rawValue)
        }
        let state = try await connection.controller.stepMultiplexConnection()
        XCTAssertEqual(state.instances, 7); XCTAssertFalse(state.stopping)
        XCTAssertTrue(app.extensionInstance.canDiscard)
        XCTAssertTrue(connection.canAdd)
        if route == "local" {
            let original = invocations[3].request
            let widened = ExtensionGroupInvocation(packageID: review.id,
                request: .init(executable: binary, executorDirectory: directory, executorDigest: review.digest, clientDigest: review.digest,
                    methods: [], processEnvironmentNames: ["GHOSTERM_UNGRANTED"],
                    execution: .init(runtime: .exec, temporaryDirectory: stage.path, authorized: true), command: original.command, arguments: original.arguments),
                sshAlias: "mux-target", localLaunch: launch, clientWorkspace: nil, temporaryDirectory: stage.path)
            let rejected = await connection.add(widened, app: app); XCTAssertFalse(rejected)
            let unchanged = try await connection.controller.stepMultiplexConnection(); XCTAssertEqual(unchanged.instances, 7)
        }
        let added = await connection.add(invocations[3], app: app); XCTAssertTrue(added, connection.issue ?? "")
        let replacement = try XCTUnwrap(connection.instances.last)
        XCTAssertNotEqual(replacement.id, removed.id)
        await replacement.decideExtensionConsent(allow: true)
        try await until("replacement activation") { replacement.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        let replacementTable = try XCTUnwrap(replacement.extensionTable); await replacementTable.load()
        XCTAssertNil(replacementTable.issue); XCTAssertEqual(replacementTable.descriptor?.total, 3)
        for instance in connection.instances where instance.id != replacement.id {
            let table = try XCTUnwrap(instance.extensionTable); await table.load(); XCTAssertNil(table.issue)
        }
        for instance in connection.instances { let done = await instance.requestClose(); XCTAssertTrue(done) }
        let empty = try await connection.controller.stepMultiplexConnection(); XCTAssertEqual(empty.instances, 0)
        XCTAssertFalse(connection.closed); XCTAssertTrue(connection.canAdd)
        let hello = try await connection.controller.manageMultiplex("hello")
        XCTAssertTrue(String(decoding: hello, as: UTF8.self).contains("nativeControl"))
        let reopened = await connection.add(invocations[0], app: app); XCTAssertTrue(reopened, connection.issue ?? "")
        let finalInstance = try XCTUnwrap(connection.instances.last)
        await finalInstance.decideExtensionConsent(allow: true)
        try await until("empty connection reuse activation") { finalInstance.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue }
        let finalTable = try XCTUnwrap(finalInstance.extensionTable); await finalTable.load()
        XCTAssertNil(finalTable.issue); XCTAssertEqual(finalTable.descriptor?.total, 0)
        let closed = await connection.close(); XCTAssertTrue(closed)
        XCTAssertTrue(connection.state?.released == true)
        XCTAssertTrue(connection.state?.peerEOF == true, "Graceful group close must observe EOF before stopping physical IO")
        if route == "local" { XCTAssertTrue(connection.process?.reaped == true) }
        let end = ProcessInfo.processInfo.systemUptime + 5
        while !(try FileManager.default.contentsOfDirectory(atPath: stage.path)).isEmpty, ProcessInfo.processInfo.systemUptime < end {
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: stage.path), [])
    }

    private func fixture(port: UInt16, token: String) throws -> [String: Any] {
        let fd = socket(AF_INET, SOCK_STREAM, 0); guard fd >= 0 else { throw GhostermError.failed("fixture socket") }
        defer { Darwin.close(fd) }
        var address = sockaddr_in(); address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); address.sin_family = sa_family_t(AF_INET)
        address.sin_port = port.bigEndian; address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let connected = withUnsafePointer(to: &address) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard connected == 0 else { throw GhostermError.failed("fixture connect") }
        var timeout = timeval(tv_sec: 5, tv_usec: 0); _ = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, socklen_t(MemoryLayout<timeval>.size))
        let auth = Array((token + "\n").utf8)
        guard auth.withUnsafeBytes({ Darwin.write(fd, $0.baseAddress, $0.count) }) == auth.count else { throw GhostermError.failed("fixture auth") }
        var data = Data(), bytes = [UInt8](repeating: 0, count: 4096)
        while true {
            let n = Darwin.read(fd, &bytes, bytes.count); if n == 0 { break }
            guard n > 0, data.count + n <= 131072 else { throw GhostermError.failed("fixture read") }; data.append(contentsOf: bytes.prefix(n))
        }
        guard data.count >= 4, data.prefix(4).reduce(0, { ($0 << 8) | Int($1) }) == data.count - 4,
              let object = try JSONSerialization.jsonObject(with: data.dropFirst(4)) as? [String: Any] else { throw GhostermError.failed("fixture JSON") }
        return object
    }
}
