import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

/// Explicit subprocess fixture only. The driver kills the paused owner, then
/// launches a separate XCTest process for observation and recovery assertions.
@MainActor
final class PluginRemovalProcessProbeTests: XCTestCase {
    private struct Request: Decodable {
        let nonce, scope, releaseID, action: String
        let device, inode: String
        let point: String?
        let revision: String
        let pending: Bool
        let versions: Int
        let removed, absent: Int?
        let expectedError: String?
        let regressionErrors: [String]?
        let journalSHA: String?
        let survivors: [String: String]
        let descriptor: String?
        let residueState: String?
        let removalID: String?
    }
    private enum ProbeError: Error { case request, io, continuation }
    private func emit(_ kind: String, _ value: [String: Any]) throws {
        var bytes = Data(("GHOSTERM_REMOVAL_" + kind + " ").utf8)
        bytes.append(try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])); bytes.append(10)
        try bytes.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let count = Darwin.write(STDOUT_FILENO, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw ProbeError.io }; offset += count
            }
        }
    }
    private func pause(_ point: String, request: Request) throws {
        try emit("READY", ["nonce": request.nonce, "pid": getpid(), "point": point, "action": request.action,
            "device": request.device, "inode": request.inode])
        var input = pollfd(fd: STDIN_FILENO, events: Int16(POLLIN), revents: 0)
        guard poll(&input, 1, 60000) > 0 else { throw ProbeError.continuation }
        var byte: UInt8 = 0
        guard Darwin.read(STDIN_FILENO, &byte, 1) == 1, byte == 99 else { throw ProbeError.continuation }
    }
    private func errorKey(_ error: Error) -> String? {
        if let error = error as? PluginReleaseRemoval.Failure { return "removal." + error.rawValue }
        if let error = error as? PluginReleaseCatalog.Failure { return "catalog." + error.rawValue }
        if let error = error as? PluginReleaseStage.Failure { return "stage." + error.rawValue }
        if let error = error as? PluginDirectorySignatureVerifier.Failure { return "json." + error.rawValue }
        return nil
    }
    private func sha(_ url: URL) -> String? {
        guard let bytes = try? Data(contentsOf: url) else { return nil }
        return SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
    }
    private func assertSurvivors(_ request: Request, root: URL) {
        for (path, expected) in request.survivors {
            XCTAssertEqual(sha(root.appendingPathComponent(path)), expected, "must retain confirmed fixture member: \(path)")
        }
    }
    private func observe(_ request: Request, controller: ExtensionController, root: URL) async throws {
        let catalog = try await controller.installedVersions()
        XCTAssertEqual(catalog.revision, request.revision)
        XCTAssertEqual(catalog.removalPending, request.pending)
        XCTAssertEqual(catalog.versions.count, request.versions)
        XCTAssertTrue(catalog.issues.isEmpty)
        XCTAssertEqual(catalog.selections.count, 1)
        XCTAssertEqual(catalog.selections.first?.releaseID, request.releaseID)
        XCTAssertEqual(catalog.selections.first?.enabled, false)
        let pending = try await controller.pendingVersionRemoval()
        XCTAssertEqual(pending != nil, request.pending)
        if request.pending { XCTAssertEqual(pending?.releaseID, request.removalID ?? request.releaseID) }
        if let expected = request.journalSHA {
            XCTAssertEqual(sha(root.appendingPathComponent(PluginReleaseCatalog.removalName)), expected, "inspection must not replay or rewrite deletion")
        }
        assertSurvivors(request, root: root)
        try emit("RESULT", ["nonce": request.nonce, "action": request.action, "pending": catalog.removalPending,
            "versions": catalog.versions.count, "pid": getpid()])
    }
    func testRunExplicitOwnedRequest() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let path = env["GHOSTERM_REMOVAL_PROBE_REQUEST"] else { throw XCTSkip("only the explicit owned subprocess driver runs this probe") }
        guard let nonce = env["GHOSTERM_REMOVAL_PROBE_NONCE"], UUID(uuidString: nonce) != nil,
              let temporary = env["TMPDIR"] else { throw ProbeError.request }
        let file = URL(fileURLWithPath: path), scope = file.deletingLastPathComponent()
        let permitted = URL(fileURLWithPath: temporary).standardizedFileURL.path + "/"
        guard scope.standardizedFileURL.path.hasPrefix(permitted), !scope.path.contains("/../") else { throw ProbeError.request }
        let parent = open(scope.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard parent >= 0 else { throw ProbeError.request }
        defer { Darwin.close(parent) }
        var info = stat()
        guard fstat(parent, &info) == 0, info.st_uid == geteuid(), info.st_mode & 0o077 == 0 else { throw ProbeError.request }
        let request = try JSONDecoder().decode(Request.self, from: PluginReleaseCatalog.read(parent, name: file.lastPathComponent, limit: 32768))
        guard request.nonce == nonce, request.scope == scope.path, PluginReleaseCatalog.digest(request.releaseID) else { throw ProbeError.request }
        let root = scope.appendingPathComponent("installed")
        let fd = openat(parent, "installed", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw ProbeError.request }
        defer { Darwin.close(fd) }
        guard fstat(fd, &info) == 0, info.st_uid == geteuid(), info.st_mode & 0o077 == 0,
              String(info.st_dev) == request.device, String(info.st_ino) == request.inode else { throw ProbeError.request }
        let controller = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: [], capabilities: []),
            installationDirectory: root)
        if request.action == "stage-gc-begin" {
            var stage: PluginReleaseStage? = try PluginReleaseStage(directory: root)
            try stage!.write(Data("retained".utf8), path: "payload/file")
            stage = nil
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let fd = try owner.journalRoot(); defer { Darwin.close(fd) }
            let journal = try XCTUnwrap(PluginInstallJournal(parent: fd))
            let reference = try XCTUnwrap(journal.listing().entries.first).reference
            let removal = try PluginReleaseRemoval(owner: owner, releaseID: reference.digest, expectedRevision: request.revision,
                checkpoint: { if $0 == "after-unlink" { try self.pause("after-unlink", request: request) } }, stage: reference)
            _ = try removal.confirm(owner: owner)
            XCTFail("the driver must kill this staging cleanup owner")
        } else if request.action == "stage-begin" {
            guard let point = request.point, let descriptor = request.descriptor,
                  ["after-reservation", "after-directory-creation", "after-directory-registration", "after-file-create", "after-file-write", "before-publication-rename", "after-publication-rename", "after-publication-sync"].contains(point) else { throw ProbeError.request }
            let stage = try PluginReleaseStage(directory: root, checkpoint: { if $0 == point { try self.pause(point, request: request) } })
            try stage.write(Data("retained".utf8), path: "payload/file.txt")
            try stage.write(Data(descriptor.utf8), path: "release.json")
            try stage.write(Data(repeating: 0, count: 64), path: "release.sig")
            try stage.write(Data("{}".utf8), path: "policy.json")
            _ = try stage.publish(id: request.releaseID)
            XCTFail("the driver must kill this staging owner")
        } else if request.action == "stage-inspect" {
            let inventory = try await controller.installationResidues()
            XCTAssertTrue(inventory.issues.isEmpty); XCTAssertEqual(inventory.entries.count, 1)
            XCTAssertEqual(inventory.entries.first?.state.rawValue, request.residueState)
            assertSurvivors(request, root: root)
            try emit("RESULT", ["nonce": nonce, "action": request.action, "pid": getpid(), "state": inventory.entries.first?.state.rawValue ?? "missing"])
        } else if request.action == "inspect" {
            try await observe(request, controller: controller, root: root)
        } else if request.action == "begin" {
            guard let point = request.point, ["before-intent", "after-intent", "after-detach", "after-unlink", "before-clear"].contains(point) else { throw ProbeError.request }
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let removal = try PluginReleaseRemoval(owner: owner, releaseID: request.releaseID, expectedRevision: request.revision,
                checkpoint: { if $0 == point { try self.pause(point, request: request) } })
            _ = try removal.confirm(owner: owner)
            XCTFail("the driver must kill this owner at the requested boundary")
        } else if request.action == "hold-shared" {
            let lease: PluginReleaseCatalog.VersionHandle
            do {
                let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
                lease = try owner.openVersion(request.releaseID); try lease.pin()
            } // Deliberately release the root flock; only the version lease remains.
            try pause("holding", request: request)
            withExtendedLifetime(lease) {}
        } else if request.action == "hold-preview" {
            let value = try await controller.previewVersionRemoval(releaseID: request.releaseID, expectedRevision: request.revision)
            try pause("holding", request: request)
            await controller.cancelVersionRemoval(id: value.id)
        } else {
            do {
                var result: [String: Any] = ["nonce": nonce, "action": request.action, "pid": getpid()]
                switch request.action {
                case "stage-clean", "stage-retire", "stage-preview-denied":
                    let inventory = try await controller.installationResidues()
                    XCTAssertTrue(inventory.issues.isEmpty); XCTAssertEqual(inventory.entries.count, 1)
                    guard let entry = inventory.entries.first else { throw ProbeError.request }
                    if request.action == "stage-retire" {
                        try await controller.retireInstallationRecord(id: entry.id, expectedDigest: entry.digest)
                        result["retired"] = true
                    } else {
                        let preview = try await controller.previewInstallationResidue(id: entry.id, expectedDigest: entry.digest)
                        let value = try await controller.confirmVersionRemoval(id: preview.id)
                        XCTAssertTrue(value.complete && value.durabilityConfirmed); XCTAssertNil(value.cleanupIssue)
                        XCTAssertEqual(value.removed, request.removed); XCTAssertEqual(value.observedAbsent, request.absent)
                        result["removed"] = value.removed
                    }
                case "preview-cancel":
                    let value = try await controller.previewVersionRemoval(releaseID: request.releaseID, expectedRevision: request.revision)
                    await controller.cancelVersionRemoval(id: value.id)
                    result["cancelled"] = true
                case "confirm", "resume":
                    let value: ExtensionController.RemovalResult
                    if request.action == "confirm" {
                        let preview = try await controller.previewVersionRemoval(releaseID: request.releaseID, expectedRevision: request.revision)
                        value = try await controller.confirmVersionRemoval(id: preview.id)
                    } else {
                        let pending = try await controller.pendingVersionRemoval()
                        guard let pending else { XCTFail("explicit recovery requires the durable intent"); return }
                        value = try await controller.resumeVersionRemoval(id: pending.id)
                    }
                    XCTAssertTrue(value.complete && value.durabilityConfirmed)
                    XCTAssertNil(value.cleanupIssue)
                    XCTAssertEqual(value.removed, request.removed)
                    XCTAssertEqual(value.observedAbsent, request.absent)
                    result["removed"] = value.removed; result["absent"] = value.observedAbsent
                case "resume-native":
                    let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
                    let value = try PluginReleaseRemoval.resume(owner: owner)
                    XCTAssertTrue(value.complete && value.durabilityConfirmed)
                    XCTAssertEqual(value.removed, request.removed); XCTAssertEqual(value.observedAbsent, request.absent)
                    result["removed"] = value.removed; result["absent"] = value.observedAbsent
                default: throw ProbeError.request
                }
                XCTAssertNil(request.expectedError, "operation should have rejected: \(request.expectedError ?? "")")
                assertSurvivors(request, root: root)
                try emit("RESULT", result)
            } catch {
                guard let key = errorKey(error) else { throw error }
                guard !["removal.io", "catalog.io", "catalog.root", "json.file"].contains(key) else { throw error }
                if let expected = request.expectedError {
                    XCTAssertEqual(key, expected)
                } else if request.regressionErrors?.contains(key) == true {
                    XCTFail("expected successful maintenance, got semantic rejection \(key)")
                } else { throw error }
                assertSurvivors(request, root: root)
                try emit("RESULT", ["nonce": nonce, "action": request.action, "error": key, "pid": getpid()])
            }
        }
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
}
