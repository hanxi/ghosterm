import CryptoKit
import Darwin
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel
@testable import GhostermApp

@MainActor
final class PluginReleaseInstallTests: XCTestCase {
    private func fixture() throws -> URL {
        guard ExtensionRuntime.supportStatus == 0,
              let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else {
            throw XCTSkip("requires explicit JS-enabled Core and signed release fixtures")
        }
        return URL(fileURLWithPath: path)
    }
    private func directory() throws -> URL {
        let root = sessionTestDirectory().appendingPathComponent("install-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try Self.thaw(root) }
        return root
    }
    nonisolated static func thaw(_ root: URL) throws {
        var value = stat()
        guard lstat(root.path, &value) == 0, value.st_mode & S_IFMT == S_IFDIR else { return }
        guard chmod(root.path, 0o700) == 0 else { throw PluginReleaseStage.Failure.io }
        for child in try FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: nil) { try thaw(child) }
    }
    private func controller(_ root: URL) throws -> ExtensionController {
        try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1],
            runtimes: ["portable-js"], capabilities: ["resource.receive"]), resourceDirectory: root,
            installationDirectory: root.appendingPathComponent("installed"))
    }
    private func prepare(_ controller: ExtensionController, _ root: URL) async throws -> ExtensionController.PreparedRelease {
        try await controller.prepareSignedRelease(artifact: root.appendingPathComponent("r/plugin.zip"),
            descriptor: root.appendingPathComponent("r/release.json"), signature: root.appendingPathComponent("r/release.sig"),
            trustedPolicy: root.appendingPathComponent("policy.json"))
    }
    private func names(_ root: URL) throws -> [String] {
        guard FileManager.default.fileExists(atPath: root.path) else { return [] }
        return try FileManager.default.contentsOfDirectory(atPath: root.path).sorted()
    }
    private func hash(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

    func testPreparedReleaseIsPrivateAndCancelCleansIt() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let prepared = try await prepare(controller, input)
        let store = root.appendingPathComponent("installed")
        XCTAssertTrue(prepared.permissions.contains { $0.side == "client" && $0.capability == "resource.receive" && !$0.purpose.isEmpty })
        XCTAssertNotNil(prepared.executorDeclarations)
        XCTAssertEqual(try names(store).filter { $0.hasPrefix(".prepare-") }.count, 1)
        XCTAssertFalse(try names(store).contains(prepared.signature.descriptorSHA256))
        let resolution = try await controller.resolve(command: "preview", selectedID: prepared.review.id, selectedDigest: prepared.review.digest)
        XCTAssertNotEqual(resolution.status, 0, "preparation must not expose the package to resolution")
        try await controller.discardPreparedRelease(id: prepared.id)
        XCTAssertEqual(try names(store), [], "cancel must remove the owned stage")
        do { _ = try await controller.installPreparedRelease(id: prepared.id); XCTFail("cancelled preparation installed") } catch {}
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testInstallPublishesReadOnlyVersionWithoutSourceApproval() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let prepared = try await prepare(controller, input)
        let installed = try await controller.installPreparedRelease(id: prepared.id)
        XCTAssertTrue(installed.durabilityConfirmed)
        XCTAssertEqual(installed.signature, prepared.signature)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")), [prepared.signature.descriptorSHA256])
        XCTAssertEqual(hash(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))), prepared.review.digest)
        var value = stat()
        XCTAssertEqual(lstat(installed.directory.path, &value), 0)
        XCTAssertEqual(value.st_mode & 0o777, 0o500, "published package directory must be read-only")
        XCTAssertEqual(lstat(installed.directory.appendingPathComponent("plugin.json").path, &value), 0)
        XCTAssertEqual(value.st_mode & 0o222, 0, "published members must be read-only")
        do {
            let resolution = try await controller.resolve(command: "preview", selectedID: prepared.review.id, selectedDigest: prepared.review.digest)
            XCTAssertNotEqual(resolution.status, 0, "installation must not approve source")
        } catch { XCTFail("published read-only version must remain inspectable: \(error)") }
        XCTAssertFalse(installed.review.authorizationGranted)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testDuplicatePublicationCannotOverwriteExistingVersion() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let first = try await prepare(controller, input)
        let installed = try await controller.installPreparedRelease(id: first.id)
        let bytes = try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))
        let second = try await prepare(controller, input)
        do { _ = try await controller.installPreparedRelease(id: second.id); XCTFail("existing release was overwritten") }
        catch { XCTAssertEqual(error as? PluginReleaseStage.Failure, .conflict) }
        try await controller.discardPreparedRelease(id: second.id)
        XCTAssertEqual(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json")), bytes)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")), [first.signature.descriptorSHA256])
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testSecondVersionKeepsOldCheckedSnapshotAndDirectory() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let first = try await prepare(controller, input)
        let old = try await controller.installPreparedRelease(id: first.id)
        _ = try await controller.selectInstalledVersion(releaseID: first.signature.descriptorSHA256, expectedRevision: nil, trustedPolicy: input.appendingPathComponent("policy.json"))
        _ = try await controller.approve(id: first.review.id, digest: first.review.digest)
        let second = try await prepare(controller, input.appendingPathComponent("v2"))
        let next = try await controller.installPreparedRelease(id: second.id)
        XCTAssertNotEqual(old.review.digest, next.review.digest)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")).filter(PluginReleaseCatalog.digest).count, 2)
        XCTAssertEqual(hash(try Data(contentsOf: old.directory.appendingPathComponent("integrity.json"))), old.review.digest)
        let selected = try await controller.resolve(command: "preview", selectedID: old.review.id, selectedDigest: old.review.digest)
        XCTAssertEqual(selected.status, 0, "new publication must retain the exact old checked package")
        let unapproved = try await controller.resolve(command: "preview", selectedID: next.review.id, selectedDigest: next.review.digest)
        XCTAssertNotEqual(unapproved.status, 0, "new digest needs its own source approval")
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testDeflatedReleaseUsesTheSameCheckedIdentity() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let stored = try await prepare(controller, input)
        try await controller.discardPreparedRelease(id: stored.id)
        let compressed = try await prepare(controller, input.appendingPathComponent("compressed"))
        XCTAssertEqual(stored.review.digest, compressed.review.digest)
        let installed = try await controller.installPreparedRelease(id: compressed.id)
        XCTAssertEqual(hash(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))), stored.review.digest)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testBadSignedArchivesNeverPublishOrLeaveOwnedStage() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        for name in ["signature", "bytehash", "crc", "hidden", "path", "case", "file-parent", "link", "size", "extra", "compressed-tail", "identity"] {
            do { _ = try await prepare(controller, input.appendingPathComponent("bad/" + name)); XCTFail("bad signed release accepted: \(name)") }
            catch {
                XCTAssertEqual(try names(root.appendingPathComponent("installed")), [], "failed release must not publish or retain a stage: \(name)")
            }
        }
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testCloseDiscardsPendingAndFurtherPrepareFails() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        _ = try await prepare(controller, input)
        let closed = await controller.close(); XCTAssertTrue(closed)
        XCTAssertEqual(try names(root.appendingPathComponent("installed")), [])
        do { _ = try await prepare(controller, input); XCTFail("closed owner accepted release") } catch {}
    }

    func testFailedDiscardRetainsOwnerForExplicitRetry() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let value = try await prepare(controller, input)
        let store = root.appendingPathComponent("installed")
        let name = try XCTUnwrap(names(store).first)
        let original = store.appendingPathComponent(name), held = store.appendingPathComponent("held")
        try FileManager.default.moveItem(at: original, to: held)
        do { try await controller.discardPreparedRelease(id: value.id); XCTFail("changed stage location was removed") }
        catch { XCTAssertTrue(error.localizedDescription.contains("安装暂存清理失败")) }
        XCTAssertTrue(FileManager.default.fileExists(atPath: held.path))
        let unfinished = await controller.close(); XCTAssertFalse(unfinished)
        try FileManager.default.moveItem(at: held, to: original)
        do { try await controller.retryFailedInstallCleanup() }
        catch { XCTFail("same owned stage must remain retryable: \(error)") }
        XCTAssertEqual(try names(store), [], "retry must clean the original owned namespace")
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testFailedReplacementPreservesOldDirectoryAndResolution() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let first = try await prepare(controller, input)
        let installed = try await controller.installPreparedRelease(id: first.id)
        _ = try await controller.selectInstalledVersion(releaseID: first.signature.descriptorSHA256, expectedRevision: nil, trustedPolicy: input.appendingPathComponent("policy.json"))
        _ = try await controller.approve(id: first.review.id, digest: first.review.digest)
        for bad in ["crc", "identity"] {
            do { _ = try await prepare(controller, input.appendingPathComponent("bad/" + bad)); XCTFail("bad replacement accepted") } catch {}
            XCTAssertEqual(try names(root.appendingPathComponent("installed")), [first.signature.descriptorSHA256, PluginReleaseCatalog.selectionName].sorted())
            XCTAssertEqual(hash(try Data(contentsOf: installed.directory.appendingPathComponent("integrity.json"))), first.review.digest)
            let selected = try await controller.resolve(command: "preview", selectedID: first.review.id, selectedDigest: first.review.digest)
            XCTAssertEqual(selected.status, 0, "failed replacement must retain old source decision and checked bytes")
        }
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testChangedPreparedFilesMustNotPublish() async throws {
        let input = try fixture(), root = try directory(), controller = try controller(root)
        let value = try await prepare(controller, input)
        let store = root.appendingPathComponent("installed")
        let name = try XCTUnwrap(names(store).first)
        let file = store.appendingPathComponent(name + "/payload/client/index.js")
        XCTAssertEqual(chmod(file.path, 0o600), 0)
        try Data("changed after inspection".utf8).write(to: file)
        XCTAssertEqual(chmod(file.path, 0o400), 0)
        do { _ = try await controller.installPreparedRelease(id: value.id); XCTFail("changed prepared bytes were published") }
        catch { XCTAssertEqual(error as? PluginReleaseStage.Failure, .changed) }
        XCTAssertFalse(try names(store).contains(value.signature.descriptorSHA256))
        try await controller.discardPreparedRelease(id: value.id)
        XCTAssertEqual(try names(store), [])
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testAddedOrMissingPreparedMembersMustNotPublish() throws {
        for extra in [false, true] {
            let root = try directory().appendingPathComponent("store"), stage = try PluginReleaseStage(directory: root)
            try stage.write(Data([1]), path: "payload/a")
            let payload = stage.payloadURL
            if extra { try Data([2]).write(to: payload.appendingPathComponent("extra")); XCTAssertEqual(chmod(payload.appendingPathComponent("extra").path, 0o400), 0) }
            else { try FileManager.default.removeItem(at: payload.appendingPathComponent("a")) }
            XCTAssertThrowsError(try stage.publish(id: String(repeating: "d", count: 64))) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .changed) }
            try stage.discard()
        }
    }

    func testStageLockFreezeAndFailedPublishCleanup() throws {
        let root = try directory().appendingPathComponent("store"), id = String(repeating: "a", count: 64)
        let stage = try PluginReleaseStage(directory: root)
        try stage.write(Data("snapshot".utf8), path: "payload/a")
        XCTAssertThrowsError(try PluginReleaseStage(directory: root)) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .busy) }
        enum Injected: Error { case beforeRename }
        XCTAssertThrowsError(try stage.publish(id: id, beforeRename: { throw Injected.beforeRename }))
        XCTAssertFalse(try names(root).contains(id), "failure before rename must not publish")
        try stage.discard()
        XCTAssertEqual(try names(root), [], "failed frozen stage must still clean")
        let next = try PluginReleaseStage(directory: root)
        try next.discard()
    }

    func testPublishedButUnsyncedParentIsExplicitlyUnknown() throws {
        let root = try directory().appendingPathComponent("store")
        let stage = try PluginReleaseStage(directory: root, parentSync: { _ in -1 })
        try stage.write(Data([1]), path: "payload/a")
        let result = try stage.publish(id: String(repeating: "b", count: 64))
        XCTAssertFalse(result.durabilityConfirmed, "post-rename fsync failure must not claim durable success")
        XCTAssertEqual(try Data(contentsOf: result.directory.appendingPathComponent("payload/a")), Data([1]))
        try stage.discard()
        XCTAssertTrue(FileManager.default.fileExists(atPath: result.directory.path), "discard must never delete an already-published release")
    }

    func testExistingEmptySlotIsNeverAdopted() throws {
        let root = try directory().appendingPathComponent("store"), id = String(repeating: "c", count: 64)
        let stage = try PluginReleaseStage(directory: root)
        try stage.write(Data([1]), path: "payload/a")
        let existing = root.appendingPathComponent(id)
        try FileManager.default.createDirectory(at: existing, withIntermediateDirectories: false)
        XCTAssertThrowsError(try stage.publish(id: id)) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .conflict) }
        XCTAssertEqual(try names(existing), [], "existing empty slot must not be overwritten")
        try stage.discard()
    }

    func testPrivateRootAndRetainedVersionLimit() throws {
        let parent = try directory(), root = parent.appendingPathComponent("store")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o755])
        XCTAssertThrowsError(try PluginReleaseStage(directory: root))
        XCTAssertEqual(chmod(root.path, 0o700), 0)
        for index in 0..<64 { try FileManager.default.createDirectory(at: root.appendingPathComponent(String(index)), withIntermediateDirectories: false) }
        XCTAssertThrowsError(try PluginReleaseStage(directory: root)) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .limit) }
    }

    func testPortableArchivePathsHaveExplicitBounds() {
        for path in ["../x", "/x", "a//b", "a/./b", "a/../b", "a\\b", "a:b", "a.", "a ", "e\u{301}"] {
            XCTAssertFalse(PluginReleaseArchive.validPath(path), path)
        }
        XCTAssertTrue(PluginReleaseArchive.validPath(Array(repeating: "x", count: 32).joined(separator: "/")))
        XCTAssertFalse(PluginReleaseArchive.validPath(Array(repeating: "x", count: 33).joined(separator: "/")))
    }
}
