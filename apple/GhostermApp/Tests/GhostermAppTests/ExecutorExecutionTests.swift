import Foundation
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

final class ExecutorExecutionTests: XCTestCase {
    private let digest = String(repeating: "a", count: 64)
    private func node(executable: String = "/n", version: String = "26.4.0", sha256: String? = nil,
                      sdk: String = "/s", catalog: String? = nil) -> ExtensionRelayBootstrap.Execution.Runtime {
        .node(.init(executable: executable, version: version, sha256: sha256 ?? digest,
            sdkDirectory: sdk, sdkCatalogSHA256: catalog ?? String(repeating: "b", count: 64)))
    }
    private func request(_ runtime: ExtensionRelayBootstrap.Execution.Runtime? = nil,
                         temporary: String = "/t", authorized: Bool = true,
                         environment: [String] = []) -> ExtensionRelayBootstrap.Request {
        .init(executable: "gterm", executorDirectory: "/k", executorDigest: digest, clientDigest: digest,
              methods: [], processEnvironmentNames: environment,
              execution: runtime.map { .init(runtime: $0, temporaryDirectory: temporary, authorized: authorized) },
              command: "fixture")
    }
    func testPortableDefaultHasNoExecutionOrDependencyFlags() throws {
        let r = request()
        XCTAssertNil(r.execution)
        XCTAssertEqual(try ExtensionRelayBootstrap.build(r),
            "exec 'gterm' 'relay' '--stdio' '--trust-development-digest' '\(digest)' '--client-digest' '\(digest)' '--' '/k' 'fixture'")
    }
    func testExecRequiresSeparateExecutionApproval() {
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(.exec, authorized: false)))
    }
    func testNodeRequiresSeparateExecutionApproval() {
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(node(), authorized: false)))
    }
    func testSelectedRuntimeEmitsPinnedDependenciesAndEnvironmentWithoutBusinessGrants() throws {
        let exec = try ExtensionRelayBootstrap.build(request(.exec, environment: ["FIXTURE_ROOT"]))
        XCTAssertTrue(exec.hasPrefix("TMPDIR='/t' exec "))
        XCTAssertTrue(exec.contains("'--runtime' 'exec' '--allow-process-execution'"))
        XCTAssertTrue(exec.contains("'--process-environment' 'FIXTURE_ROOT'"))
        XCTAssertFalse(exec.contains("--node-runtime")); XCTAssertFalse(exec.contains("--process-program"))
        let node = try ExtensionRelayBootstrap.build(request(node()))
        XCTAssertTrue(node.contains("'--runtime' 'node' '--allow-process-execution' '--node-runtime' '/n' '26.4.0' '\(digest)' '--node-sdk' '/s' '\(String(repeating: "b", count: 64))'"))
        XCTAssertFalse(node.contains("--process-program")); XCTAssertFalse(node.contains("--root"))
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(environment: ["FIXTURE_ROOT"])))
    }
    func testInvalidRuntimeDependenciesFailBeforeSSH() {
        for runtime in [node(executable: "node"), node(executable: "/n\nentry"), node(version: "v26.4.0"),
            node(version: ""), node(version: "26 4"), node(version: String(repeating: "1", count: 129)),
            node(sha256: "a"), node(sha256: String(repeating: "A", count: 64)), node(sdk: "sdk"),
            node(sdk: "/s\0bad"), node(catalog: ""), node(catalog: String(repeating: "g", count: 64))] {
            XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(runtime)))
        }
        for temp in ["", "relative", "/t\nentry", "/t\rentry", "/t\0entry", "/" + String(repeating: "x", count: 4096)] {
            XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(.exec, temporary: temp)))
        }
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(node(executable: "/" + String(repeating: "x", count: 1000)))))
    }
    func testActualPOSIXCommandPreservesQuotedDependencyPathsAndTemporaryDirectory() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tmp/executor-shell-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let probe = root.appendingPathComponent("gterm'quoted")
        try Data("#!/bin/sh\nprintf '%s\\0' \"$TMPDIR\" \"$@\"\n".utf8).write(to: probe)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: probe.path)
        let malicious = "/中'$(touch SHOULD_NOT_EXIST);😀"
        let execution = ExtensionRelayBootstrap.Execution(runtime: node(executable: malicious, sdk: malicious),
            temporaryDirectory: malicious, authorized: true)
        let command = try ExtensionRelayBootstrap.build(.init(executable: probe.path, executorDirectory: "/k",
            executorDigest: digest, clientDigest: digest, methods: [], execution: execution, command: "fixture",
            arguments: ["", "'$(touch SHOULD_NOT_EXIST)"]))
        let process = Process(), output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/bin/sh"); process.arguments = ["-c", command]
        process.currentDirectoryURL = root; process.environment = ["PATH": "/usr/bin:/bin"]
        process.standardOutput = output; try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
        XCTAssertEqual(process.terminationStatus, 0)
        let values = String(decoding: data, as: UTF8.self).components(separatedBy: "\0")
        XCTAssertEqual(values, [malicious, "relay", "--stdio", "--trust-development-digest", digest,
            "--client-digest", digest, "--runtime", "node", "--allow-process-execution", "--node-runtime",
            malicious, "26.4.0", digest, "--node-sdk", malicious, String(repeating: "b", count: 64), "--", "/k", "fixture", "", "'$(touch SHOULD_NOT_EXIST)", ""])
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent("SHOULD_NOT_EXIST").path))
    }
    func testExecutionDraftRevokesApprovalOnEveryDependencyEditAndRuntimeChange() {
        var draft = ExecutorExecutionDraft()
        XCTAssertNil(draft.selection)
        draft.choice = .node; draft.authorized = true
        let edits: [(inout ExecutorExecutionDraft) -> Void] = [
            { $0.temporaryDirectory = "/t" }, { $0.executable = "/n" }, { $0.version = "26.4.0" },
            { $0.sha256 = "a" }, { $0.sdkDirectory = "/s" }, { $0.sdkCatalogSHA256 = "b" },
            { $0.choice = .exec }, { $0.choice = .node }]
        for edit in edits { draft.authorized = true; edit(&draft); XCTAssertEqual(draft.selection?.authorized, false) }
        draft.authorized = true; let snapshot = draft.selection
        draft.temporaryDirectory = "/changed"
        XCTAssertEqual(snapshot?.authorized, true); XCTAssertEqual(snapshot?.temporaryDirectory, "/t")
        XCTAssertTrue(snapshot?.reviewDescription.contains("Node 26.4.0") == true)
        draft.choice = .portable; XCTAssertNil(draft.selection)
    }
}
