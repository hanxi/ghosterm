import Foundation
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

final class ExecutorReleaseLaunchTests: XCTestCase {
    private let digest = String(repeating: "a", count: 64)
    private func request(_ evidence: ExtensionRelayBootstrap.ReleaseEvidence?, executable: String = "/selected/gterm",
                         attachment: ExtensionRelayBootstrap.Attachment? = nil) -> ExtensionRelayBootstrap.Request {
        .init(executable: executable, executorDirectory: attachment == nil ? "/selected/package" : "",
            executorDigest: digest, clientDigest: digest, methods: [], attachment: attachment,
            releaseEvidence: evidence, command: "fixture", arguments: ["", "中'😀"])
    }
    private func decode(_ hex: String) throws -> [String] {
        let chars = Array(hex.utf8)
        let data = try stride(from: 0, to: chars.count, by: 2).map {
            try XCTUnwrap(UInt8(String(decoding: chars[$0..<$0 + 2], as: UTF8.self), radix: 16))
        }
        XCTAssertEqual(data[0], 1)
        var at = 3, result: [String] = []
        for _ in 0..<(Int(data[1]) * 256 + Int(data[2])) {
            let length = Int(data[at]) * 256 + Int(data[at + 1]); at += 2
            result.append(String(decoding: data[at..<at + length], as: UTF8.self)); at += length
        }
        XCTAssertEqual(at, data.count); return result
    }
    func testSignedEvidenceSurvivesShellAndBothMultiplexProfiles() throws {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tmp/release-launch-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let probe = root.appendingPathComponent("gterm")
        try Data("#!/bin/sh\nprintf '%s\\0' \"$@\"\n".utf8).write(to: probe)
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: probe.path)
        let path = "/selected/中'$(touch SHOULD_NOT_EXIST);😀"
        let evidence = ExtensionRelayBootstrap.ReleaseEvidence(directory: path, policyPath: path + "/policy.json",
            policySHA256: digest, authorized: true)
        let selected = request(evidence, executable: probe.path)
        let argv = try ExtensionRelayBootstrap.buildLocal(selected)
        let process = Process(), pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", try ExtensionRelayBootstrap.build(selected)]
        process.currentDirectoryURL = root; process.environment = ["PATH": "/usr/bin:/bin"]
        process.standardOutput = pipe; try process.run()
        let data = pipe.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
        XCTAssertEqual(process.terminationStatus, 0)
        XCTAssertEqual(String(decoding: data, as: UTF8.self).components(separatedBy: "\0"), Array(argv.dropFirst()) + [""])
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent("SHOULD_NOT_EXIST").path))
        let scope = ExtensionController.MultiplexScope(instance: String(repeating: "b", count: 32), epoch: 1)
        let group = try ExtensionRelayBootstrap.buildMultiplex(connection: String(repeating: "c", count: 32),
            scopes: [scope], requests: [selected], local: true, temporaryDirectory: root.path, live: true)
        let online = try ExtensionRelayBootstrap.multiplexProfile(selected, local: true)
        XCTAssertEqual(group.argv.last, online)
        XCTAssertEqual(try decode(online), Array(argv.dropFirst(3)))
        XCTAssertEqual(Array(argv[7..<11]), ["--release-evidence", path, path + "/policy.json", digest])
    }
    func testInvalidOrUnapprovedEvidenceAndAttachmentNeverFallBack() {
        let valid = ExtensionRelayBootstrap.ReleaseEvidence(directory: "/release", policyPath: "/policy.json", policySHA256: digest, authorized: true)
        var values = [ExtensionRelayBootstrap.ReleaseEvidence(directory: valid.directory, policyPath: valid.policyPath, policySHA256: digest, authorized: false)]
        for path in ["", "relative", "/", "/r/", "/r//x", "/r/../x", "/r/./x", "/r\n", "/r\0"] {
            values.append(.init(directory: path, policyPath: valid.policyPath, policySHA256: digest, authorized: true))
            values.append(.init(directory: valid.directory, policyPath: path, policySHA256: digest, authorized: true))
        }
        for digest in ["", "a", String(repeating: "A", count: 64)] {
            values.append(.init(directory: valid.directory, policyPath: valid.policyPath, policySHA256: digest, authorized: true))
        }
        for evidence in values {
            XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(evidence)))
            XCTAssertThrowsError(try ExtensionRelayBootstrap.multiplexProfile(request(evidence), local: true))
        }
        XCTAssertThrowsError(try ExtensionRelayBootstrap.build(request(valid,
            attachment: .init(directory: "/front", instance: String(repeating: "b", count: 32), authorized: true))))
        XCTAssertFalse((try? ExtensionRelayBootstrap.build(request(nil)))?.contains("--release-evidence") ?? true)
    }
    @MainActor func testProductionDraftBindsPolicyApprovalToTargetAndKeepsSnapshotImmutable() {
        let review = ExtensionController.PackageReview(id: "org.ghosterm.fixture", version: "0.1.0", publisher: "Fixture", license: "MIT",
            digest: digest, runtime: "portable-js", requiredCapabilities: [], optionalMissingCapabilities: [],
            authorizationGranted: false, publisherAuthenticated: false, commands: [], sourceApproved: false)
        var entry = ExtensionsPane.Entry(review: review)
        entry.useReleaseEvidence = true; entry.releaseDirectory = "/release"; entry.releasePolicyPath = "/policy.json"
        entry.releasePolicySHA256 = digest; entry.releaseEvidenceAllowed = true
        let snapshot = entry.releaseEvidence
        let edits: [(inout ExtensionsPane.Entry) -> Void] = [
            { $0.sshAlias = "changed" }, { $0.localConnection = true }, { $0.executable = "/other/gterm" },
            { $0.executorDirectory = "/other/package" }, { $0.releaseDirectory = "/other/release" },
            { $0.releasePolicyPath = "/other/policy" }, { $0.releasePolicySHA256 = "b" }, { $0.argumentsJSON = "[\"new\"]" }]
        for edit in edits { entry.releaseEvidenceAllowed = true; edit(&entry); XCTAssertFalse(entry.releaseEvidenceAllowed) }
        XCTAssertEqual(snapshot?.directory, "/release"); XCTAssertEqual(snapshot?.authorized, true)
        entry.attachExisting = true; XCTAssertNil(entry.releaseEvidence)
    }
}
