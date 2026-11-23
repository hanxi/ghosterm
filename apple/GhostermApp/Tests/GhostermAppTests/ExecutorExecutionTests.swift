import Foundation
import XCTest
@testable import GhostermAppModel

final class ExecutorExecutionTests: XCTestCase {
    private let digest = String(repeating: "a", count: 64)
    private func request(environment: [String] = []) -> ExtensionRelayBootstrap.Request {
        .init(executable: "gterm", executorDirectory: "/k", executorDigest: digest, clientDigest: digest,
              methods: [], processEnvironmentNames: environment, command: "fixture")
    }
    func testLuaDefaultHasNoExecutionOrDependencyFlags() throws {
        let r = request()
        XCTAssertEqual(try ExtensionRelayBootstrap.build(r),
            "exec 'gterm' 'relay' '--stdio' '--trust-development-digest' '\(digest)' '--client-digest' '\(digest)' '--' '/k' 'fixture'")
    }
    func testEnvironmentSelectionRequiresProcessMapping() {
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(environment: ["FIXTURE_ROOT"])))
    }
    func testActualPOSIXCommandPreservesQuotedLuaArguments() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tmp/executor-shell-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let probe = root.appendingPathComponent("gterm'quoted")
        try Data("#!/bin/sh\nprintf '%s\\0' \"$TMPDIR\" \"$@\"\n".utf8).write(to: probe)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: probe.path)
        let command = try ExtensionRelayBootstrap.build(.init(executable: probe.path, executorDirectory: "/中'😀",
            executorDigest: digest, clientDigest: digest, methods: [], command: "fixture",
            arguments: ["", "'$(touch SHOULD_NOT_EXIST)"]))
        let process = Process(), output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/bin/sh"); process.arguments = ["-c", command]
        process.currentDirectoryURL = root; process.environment = ["PATH": "/usr/bin:/bin"]
        process.standardOutput = output; try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
        XCTAssertEqual(process.terminationStatus, 0)
        let values = String(decoding: data, as: UTF8.self).components(separatedBy: "\0")
        XCTAssertEqual(values, ["", "relay", "--stdio", "--trust-development-digest", digest,
            "--client-digest", digest, "--", "/中'😀", "fixture", "", "'$(touch SHOULD_NOT_EXIST)", ""])
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent("SHOULD_NOT_EXIST").path))
    }
}
