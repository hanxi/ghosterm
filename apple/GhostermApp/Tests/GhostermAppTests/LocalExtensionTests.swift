import CryptoKit
import Darwin
@testable import GhostermApp
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

final class LocalExtensionTests: XCTestCase {
    private let digest = String(repeating: "a", count: 64)
    private func request(arguments: [String] = [], attachment: ExtensionRelayBootstrap.Attachment? = nil) -> ExtensionRelayBootstrap.Request {
        .init(executable: "/selected/gterm", executorDirectory: attachment == nil ? "/package" : "",
            executorDigest: digest, clientDigest: digest, methods: [], attachment: attachment,
            command: "fixture", arguments: arguments)
    }
    func testLocalArgumentsKeepRawBytesWithoutSSHLimit() throws {
        let payload = ["", "目录😀", "';$(touch NOPE)", "line\nnext", String(repeating: "x", count: 1500)]
        let argv = try ExtensionRelayBootstrap.buildLocal(request(arguments: payload))
        XCTAssertEqual(Array(argv.suffix(payload.count)), payload)
        XCTAssertEqual(Array(argv.prefix(3)), ["/selected/gterm", "relay", "--stdio"])
        XCTAssertFalse(argv.contains("exec")); XCTAssertFalse(argv.contains("/bin/sh"))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(arguments: payload)))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.buildLocal(request(arguments: ["bad\0argument"])))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.buildLocal(request(arguments: Array(repeating: String(repeating: "x", count: 4096), count: 5))))
    }
    func testLocalAttachmentKeepsExactInstanceAndInvocation() throws {
        let attachment = ExtensionRelayBootstrap.Attachment(directory: "/private/instance", instance: String(repeating: "b", count: 32), authorized: true)
        XCTAssertEqual(try ExtensionRelayBootstrap.buildLocal(request(arguments: ["", "'😀"], attachment: attachment)),
            ["/selected/gterm", "relay", "--attach", "/private/instance", "--instance", String(repeating: "b", count: 32), "--", "fixture", "", "'😀"])
        XCTAssertThrowsError(try ExtensionRelayBootstrap.buildLocal(request(attachment: .init(directory: "/private/instance", instance: attachment.instance, authorized: false))))
    }
    private func fixture() throws -> (URL, String, String) {
        var root = URL(fileURLWithPath: #filePath); for _ in 0..<5 { root.deleteLastPathComponent() }
        let helper = ProcessInfo.processInfo.environment["GHOSTERM_PROCESS_CHANNEL_CHILD"].map { URL(fileURLWithPath: $0) } ?? root.appendingPathComponent("core/build/test_process_channel_child")
        guard FileManager.default.isExecutableFile(atPath: helper.path) else { throw GhostermError.failed("compiled Native child fixture missing") }
        let directory = root.appendingPathComponent("tmp/local-controller-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let digest = SHA256.hash(data: try Data(contentsOf: helper)).map { String(format: "%02x", $0) }.joined()
        return (directory, helper.path, digest)
    }
    private func controller() throws -> ExtensionController {
        try .init(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: ["portable-js"], capabilities: ["session.rpc"]))
    }
    func testLocalWorkerRejectsMissingApprovalBeforeExecution() async throws {
        let (directory, helper, digest) = try fixture(); defer { try? FileManager.default.removeItem(at: directory) }
        let controller = try controller()
        do {
            _ = try await controller.openLocalChannel(executable: helper, arguments: [helper, "hold-silent"],
                launch: .init(executableSHA256: digest, workingDirectory: directory.path, temporaryDirectory: directory.path, authorized: false))
            XCTFail("unapproved Native execution started")
        } catch { XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent("ready").path)) }
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testDigestAndEnvironmentRejectBeforeExecution() async throws {
        let (directory, helper, digest) = try fixture(); defer { try? FileManager.default.removeItem(at: directory) }
        for (sha, names) in [(String(repeating: "0", count: 64), [String]()), (digest, ["LD_PRELOAD"]), (digest, ["NODE_OPTIONS"])] {
            let controller = try controller()
            do {
                _ = try await controller.openLocalChannel(executable: helper, arguments: [helper, "hold-silent"],
                    launch: .init(executableSHA256: sha, workingDirectory: directory.path, temporaryDirectory: directory.path, authorized: true), environmentNames: names)
                XCTFail("untrusted executable or injected environment started")
            } catch { XCTAssertFalse(FileManager.default.fileExists(atPath: directory.appendingPathComponent("ready").path)) }
            let closed = await controller.close(); XCTAssertTrue(closed)
            try? FileManager.default.removeItem(at: directory.appendingPathComponent("ready"))
        }
    }
    func testLocalCloseAwaitsReapAndKeepsFinalObservation() async throws {
        let (directory, helper, digest) = try fixture(); defer { try? FileManager.default.removeItem(at: directory) }
        let working = directory.appendingPathComponent("work")
        try FileManager.default.createDirectory(at: working, withIntermediateDirectories: false)
        let controller = try controller()
        let opened = try await controller.openLocalChannel(executable: helper, arguments: [helper, "hold-silent"],
            launch: .init(executableSHA256: digest, workingDirectory: working.path, temporaryDirectory: directory.path, authorized: true))
        XCTAssertEqual(opened.phase, ExtensionChannel.Phase.ready.rawValue); XCTAssertNotNil(opened.process)
        let end = ProcessInfo.processInfo.systemUptime + 2
        while (try? Data(contentsOf: working.appendingPathComponent("ready"))) != Data([82]), ProcessInfo.processInfo.systemUptime < end {
            try await Task.sleep(nanoseconds: 1_000_000)
        }
        XCTAssertEqual(try? Data(contentsOf: working.appendingPathComponent("ready")), Data([82]))
        try await controller.stopSSHChannel()
        try await controller.releaseFinished()
        let final = try await controller.localProcessState()
        XCTAssertTrue(final?.childExited == true && final?.reaped == true && final?.exitCode == 37)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testLocalArgumentCountIsNotSilentlyTruncated() {
        let roots = (0..<8).map { ExtensionRelayBootstrap.ScopedRoot(id: "root\($0)", directory: "/r") }
        let methods = (0..<16).map { "fixture.echo\($0)" }
        XCTAssertThrowsError(try ExtensionRelayBootstrap.buildLocal(.init(executable: "/gterm", executorDirectory: "/package",
            executorDigest: digest, clientDigest: digest, methods: methods, scopedRoots: roots, command: "fixture")))
    }
    func testLoaderEnvironmentIsRejectedWhenValueExists() async throws {
        let previous = getenv("NODE_OPTIONS").map { String(cString: $0) }
        setenv("NODE_OPTIONS", "--trace-deprecation", 1)
        defer { if let previous { setenv("NODE_OPTIONS", previous, 1) } else { unsetenv("NODE_OPTIONS") } }
        let (directory, helper, digest) = try fixture(); defer { try? FileManager.default.removeItem(at: directory) }
        let controller = try controller()
        do {
            _ = try await controller.openLocalChannel(executable: helper, arguments: [helper, "hold-silent"],
                launch: .init(executableSHA256: digest, workingDirectory: directory.path, temporaryDirectory: directory.path, authorized: true), environmentNames: ["NODE_OPTIONS"])
            XCTFail("loader environment reached the child")
        } catch { }
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    @MainActor func testLocalFormEditsRevokeCapturedApproval() {
        let review = ExtensionController.PackageReview(id: "org.ghosterm.fixture", version: "0.1.0", publisher: "fixture",
            license: "MIT", digest: digest, runtime: "portable-js", requiredCapabilities: [], optionalMissingCapabilities: [],
            authorizationGranted: false, publisherAuthenticated: false, commands: [], sourceApproved: true)
        var entry = ExtensionsPane.Entry(review: review); entry.localConnection = true
        let edits: [(inout ExtensionsPane.Entry) -> Void] = [
            { $0.localDigest = "changed" }, { $0.localWorkingDirectory = "/changed" }, { $0.localTemporaryDirectory = "/changed" },
            { $0.executable = "/changed" }, { $0.argumentsJSON = "[]" }, { $0.command = "changed" },
            { $0.methodsJSON = "[]" }, { $0.ipcDirectory = "/changed" }, { $0.ipcInstance = "changed" },
            { $0.remoteProcesses.toggle() }, { $0.attachExisting.toggle() }, { $0.executionDraft.choice = .node }, { $0.localConnection = false }, { $0.localConnection = true }]
        for edit in edits {
            entry.localAllowed = true; edit(&entry); XCTAssertFalse(entry.localAllowed)
        }
        entry.localConnection = true; entry.localAllowed = true; let snapshot = entry.localLaunch
        entry.localDigest = "another"; XCTAssertTrue(snapshot?.authorized == true)
    }
    func testTermIgnoreUsesFiniteCloseRetries() async throws {
        guard let path = ProcessInfo.processInfo.environment["GHOSTERM_LOCAL_IDLE_CHILD"] else { throw XCTSkip("explicit owned TERM-ignore fixture required") }
        let (directory, _, _) = try fixture(); defer { try? FileManager.default.removeItem(at: directory) }
        let digest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: path))).map { String(format: "%02x", $0) }.joined()
        let controller = try controller()
        _ = try await controller.openLocalChannel(executable: path, arguments: [path],
            launch: .init(executableSHA256: digest, workingDirectory: directory.path, temporaryDirectory: directory.path, authorized: true))
        let ready = directory.appendingPathComponent("ready"), end = ProcessInfo.processInfo.systemUptime + 2
        while (try? Data(contentsOf: ready)) != Data([82]), ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(nanoseconds: 1_000_000) }
        XCTAssertEqual(try? Data(contentsOf: ready), Data([82]))
        try await controller.stopSSHChannel()
        var failed = false
        do { try await controller.releaseFinished() } catch { failed = true }
        XCTAssertFalse(failed, "DRAINING must retain and advance the owned process within two seconds")
        let closed = await controller.close(); XCTAssertTrue(closed)
        let state = try await controller.localProcessState(); XCTAssertTrue(state?.reaped == true && state?.termSignal == SIGKILL)
    }

}
