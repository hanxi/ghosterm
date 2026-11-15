import Darwin
import CryptoKit
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

@MainActor
final class PluginReleaseCatalogTests: XCTestCase {
    private func root() throws -> URL {
        let value = sessionTestDirectory().appendingPathComponent("catalog-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: value, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(value) }
        return value
    }
    private func fixture() throws -> URL {
        guard ExtensionRuntime.supportStatus == 0, let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else { throw XCTSkip("requires explicit JS-enabled signed fixtures") }
        return URL(fileURLWithPath: path)
    }
    private func controller(_ root: URL) throws -> ExtensionController {
        try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: ["portable-js"],
            capabilities: ["resource.receive"]), resourceDirectory: root, installationDirectory: root.appendingPathComponent("installed"))
    }
    private func install(_ controller: ExtensionController, _ input: URL) async throws -> ExtensionController.InstalledRelease {
        let prepared = try await controller.prepareSignedRelease(artifact: input.appendingPathComponent("r/plugin.zip"), descriptor: input.appendingPathComponent("r/release.json"),
            signature: input.appendingPathComponent("r/release.sig"), trustedPolicy: input.appendingPathComponent("policy.json"))
        return try await controller.installPreparedRelease(id: prepared.id)
    }
    private func select(_ controller: ExtensionController, _ version: ExtensionController.InstalledRelease, _ input: URL) async throws -> ExtensionController.VersionSelectionResult {
        let catalog = try await controller.installedVersions()
        return try await controller.selectInstalledVersion(releaseID: version.signature.descriptorSHA256, expectedRevision: catalog.revision, trustedPolicy: input.appendingPathComponent("policy.json"))
    }
    private func source(_ controller: ExtensionController, _ version: ExtensionController.InstalledRelease) async throws -> UInt32 {
        try await controller.approve(id: version.review.id, digest: version.review.digest)
    }
    private func resolve(_ controller: ExtensionController, _ version: ExtensionController.InstalledRelease) async throws -> UInt32 {
        try await controller.resolve(command: "preview", selectedID: version.review.id, selectedDigest: version.review.digest).status
    }

    func testRestartListsWithoutRestoringSourceAuthority() async throws {
        let input = try fixture(), root = try root(), first = try controller(root)
        let version = try await install(first, input)
        let beforeSelect = try await source(first, version); XCTAssertNotEqual(beforeSelect, 0)
        let selected = try await select(first, version, input)
        XCTAssertTrue(selected.durabilityConfirmed)
        let unknown = try await resolve(first, version); XCTAssertNotEqual(unknown, 0)
        let approval = try await source(first, version); XCTAssertEqual(approval, 0)
        let ready = try await resolve(first, version); XCTAssertEqual(ready, 0)
        let closed = await first.close(); XCTAssertTrue(closed)
        let next = try controller(root)
        let list = try await next.installedVersions()
        XCTAssertEqual(list.versions.count, 1)
        XCTAssertTrue(list.versions[0].selected && list.versions[0].enabled)
        let notLoaded = try await resolve(next, version); XCTAssertNotEqual(notLoaded, 0)
        _ = try await select(next, version, input)
        let notApproved = try await resolve(next, version); XCTAssertNotEqual(notApproved, 0, "restart must not restore source approval")
        let nextClosed = await next.close(); XCTAssertTrue(nextClosed)
    }

    func testUpgradeRollbackAndDisableNeedFreshSourceApproval() async throws {
        let input = try fixture(), root = try root(), controller = try controller(root)
        let old = try await install(controller, input)
        _ = try await select(controller, old, input)
        let firstApproval = try await source(controller, old); XCTAssertEqual(firstApproval, 0)
        let new = try await install(controller, input.appendingPathComponent("v2"))
        _ = try await select(controller, new, input)
        let oldStatus = try await resolve(controller, old); XCTAssertNotEqual(oldStatus, 0, "inactive version must not resolve")
        let oldApproval = try await source(controller, old); XCTAssertNotEqual(oldApproval, 0, "inactive version must not be approved")
        let newStatus = try await resolve(controller, new); XCTAssertNotEqual(newStatus, 0)
        let newApproval = try await source(controller, new); XCTAssertEqual(newApproval, 0, "explicit approval must replace the old registry association")
        let newReady = try await resolve(controller, new); XCTAssertEqual(newReady, 0)
        _ = try await select(controller, old, input)
        let rollback = try await resolve(controller, old); XCTAssertNotEqual(rollback, 0, "rollback must not restore an old approval")
        let rollbackApproval = try await source(controller, old); XCTAssertEqual(rollbackApproval, 0)
        let snapshot = try await controller.installedVersions()
        _ = try await controller.disableInstalledPlugin(pluginID: old.review.id, expectedRevision: snapshot.revision)
        let disabled = try await resolve(controller, old); XCTAssertNotEqual(disabled, 0)
        let deniedApproval = try await source(controller, old); XCTAssertNotEqual(deniedApproval, 0)
        _ = try await select(controller, old, input)
        let reenabled = try await resolve(controller, old); XCTAssertNotEqual(reenabled, 0, "reenabling needs fresh source approval")
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testOtherControllerDisableAndStaleRevisionCannotBeBypassed() async throws {
        let input = try fixture(), root = try root(), a = try controller(root), b = try controller(root)
        let version = try await install(a, input)
        _ = try await select(a, version, input)
        let approval = try await source(a, version); XCTAssertEqual(approval, 0)
        let before = try await a.installedVersions()
        _ = try await b.disableInstalledPlugin(pluginID: version.review.id, expectedRevision: before.revision)
        let stale = try await resolve(a, version); XCTAssertNotEqual(stale, 0, "another owner's disable must block new admissions")
        do {
            _ = try await a.selectInstalledVersion(releaseID: version.signature.descriptorSHA256, expectedRevision: before.revision, trustedPolicy: input.appendingPathComponent("policy.json"))
            XCTFail("stale selection overwrote another owner")
        } catch { XCTAssertEqual(error as? PluginReleaseCatalog.Failure, .conflict) }
        _ = try await select(b, version, input)
        let staleGeneration = try await resolve(a, version); XCTAssertNotEqual(staleGeneration, 0, "same digest after another choice must not resurrect old approval")
        let aClosed = await a.close(), bClosed = await b.close(); XCTAssertTrue(aClosed && bClosed)
    }

    func testCurrentPolicyRevocationCannotUseStoredPolicyAsTrust() async throws {
        let input = try fixture(), root = try root(), controller = try controller(root)
        let old = try await install(controller, input), new = try await install(controller, input.appendingPathComponent("v2"))
        _ = try await select(controller, old, input)
        let before = try await controller.installedVersions()
        var policy = try JSONSerialization.jsonObject(with: Data(contentsOf: input.appendingPathComponent("policy.json"))) as! [String: Any]
        let key = (policy["keys"] as! [[String: Any]])[0]["id"] as! String
        policy["revokedKeyIds"] = [key]
        let revoked = root.appendingPathComponent("revoked.json")
        try JSONSerialization.data(withJSONObject: policy).write(to: revoked)
        do { _ = try await controller.selectInstalledVersion(releaseID: new.signature.descriptorSHA256, expectedRevision: before.revision, trustedPolicy: revoked); XCTFail("revoked key selected") }
        catch { XCTAssertEqual(error as? PluginDirectorySignatureVerifier.Failure, .revokedKey) }
        let after = try await controller.installedVersions(); XCTAssertEqual(after, before, "failed revalidation must preserve old choice")
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testCorruptOrDeletedSelectionFailsClosed() async throws {
        let input = try fixture(), root = try root(), controller = try controller(root)
        let version = try await install(controller, input)
        _ = try await select(controller, version, input)
        let approved = try await source(controller, version); XCTAssertEqual(approved, 0)
        let file = root.appendingPathComponent("installed/" + PluginReleaseCatalog.selectionName)
        let raw = try Data(contentsOf: file)
        try Data("{\"schemaVersion\":1,\"schemaVersion\":1}".utf8).write(to: file)
        do { _ = try await resolve(controller, version); XCTFail("corrupt state admitted a new run") } catch {}
        try raw.write(to: file)
        try FileManager.default.removeItem(at: file)
        let absent = try await resolve(controller, version); XCTAssertNotEqual(absent, 0, "missing selection must not fall back to a development source")
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testAlteredPayloadCannotBorrowStoredSignature() async throws {
        let input = try fixture(), root = try root(), controller = try controller(root)
        let version = try await install(controller, input)
        let manifest = version.directory.appendingPathComponent("plugin.json")
        var object = try JSONSerialization.jsonObject(with: Data(contentsOf: manifest)) as! [String: Any]
        object["pluginVersion"] = "9.9.9"
        XCTAssertEqual(chmod(manifest.path, 0o600), 0)
        try JSONSerialization.data(withJSONObject: object).write(to: manifest)
        XCTAssertEqual(chmod(manifest.path, 0o400), 0)
        let integrity = version.directory.appendingPathComponent("integrity.json")
        var index = try JSONSerialization.jsonObject(with: Data(contentsOf: integrity)) as! [String: Any]
        var members = index["members"] as! [[String: Any]]
        for i in members.indices {
            let data = try Data(contentsOf: version.directory.appendingPathComponent(members[i]["path"] as! String))
            members[i]["bytes"] = data.count
            members[i]["sha256"] = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        }
        index["members"] = members
        XCTAssertEqual(chmod(integrity.path, 0o600), 0)
        try JSONSerialization.data(withJSONObject: index).write(to: integrity)
        XCTAssertEqual(chmod(integrity.path, 0o400), 0)
        do { _ = try await select(controller, version, input); XCTFail("changed payload borrowed original release signature") }
        catch { XCTAssertEqual(error as? PluginDirectorySignatureVerifier.Failure, .identity) }
        let after = try await controller.installedVersions(); XCTAssertNil(after.revision)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testMissingSelectedVersionIsVisibleAndCanBeDisabled() async throws {
        let input = try fixture(), root = try root(), controller = try controller(root)
        let version = try await install(controller, input)
        _ = try await select(controller, version, input)
        let directory = version.directory.deletingLastPathComponent()
        try FileManager.default.moveItem(at: directory, to: directory.deletingLastPathComponent().appendingPathComponent("held-version"))
        let catalog = try await controller.installedVersions()
        XCTAssertTrue(catalog.versions.isEmpty)
        XCTAssertEqual(catalog.selections.first?.releaseID, version.signature.descriptorSHA256)
        do { _ = try await resolve(controller, version); XCTFail("missing selected version admitted a new run") }
        catch { XCTAssertEqual(error as? PluginReleaseCatalog.Failure, .invalid) }
        let disabled = try await controller.disableInstalledPlugin(pluginID: version.review.id, expectedRevision: catalog.revision)
        XCTAssertEqual(disabled.catalog.selections.first?.enabled, false)
        let denied = try await resolve(controller, version); XCTAssertNotEqual(denied, 0)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }

    func testAtomicSelectionCASAndFailureBeforeRename() throws {
        let root = try root(), id = String(repeating: "a", count: 64)
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
        let choice = PluginReleaseCatalog.Choice(releaseID: id, enabled: true, generation: UUID().uuidString)
        let proposed = PluginReleaseCatalog.Selection(revision: nil, choices: ["org.ghosterm.preview": choice])
        let (first, durable) = try owner.writeSelection(proposed, expected: nil)
        XCTAssertTrue(durable)
        XCTAssertThrowsError(try owner.writeSelection(proposed, expected: nil)) { XCTAssertEqual($0 as? PluginReleaseCatalog.Failure, .conflict) }
        enum Injected: Error { case beforeRename }
        XCTAssertThrowsError(try owner.writeSelection(proposed, expected: first.revision, beforeRename: { throw Injected.beforeRename }))
        XCTAssertEqual(try owner.selection(), first, "pre-rename failure must preserve old state")
        XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: root.path).contains { $0.hasPrefix(".selection-") })
        let (again, _) = try owner.writeSelection(proposed, expected: first.revision)
        XCTAssertNotEqual(again.revision, first.revision, "same choices still need an ABA-resistant revision")
        let file = root.appendingPathComponent(PluginReleaseCatalog.selectionName)
        let other = try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "nonce": UUID().uuidString, "choices": [:] as [String: String]])
        XCTAssertThrowsError(try owner.writeSelection(proposed, expected: again.revision, beforeRename: { try other.write(to: file) })) {
            XCTAssertEqual($0 as? PluginReleaseCatalog.Failure, .conflict)
        }
        XCTAssertEqual(try Data(contentsOf: file), other, "commit must recheck the exact observed state")
    }

    func testSelectionParentSyncFailureIsPublishedButUnknown() throws {
        let root = try root(), owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true, parentSync: { _ in -1 }))
        let (value, durable) = try owner.writeSelection(.init(revision: nil, choices: [:]), expected: nil)
        XCTAssertFalse(durable)
        XCTAssertEqual(try owner.selection(), value, "post-rename fsync failure is not an absent write")
    }

    func testStrictSelectionRejectsDuplicateBooleanAndSymlink() throws {
        let root = try root(), owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
        let file = root.appendingPathComponent(PluginReleaseCatalog.selectionName)
        for raw in [#"{"schemaVersion":true,"nonce":"x","choices":{}}"#, #"{"schemaVersion":1,"schemaVersion":1,"nonce":"x","choices":{}}"#,
                    #"{"schemaVersion":1,"nonce":"x","choices":{"__proto__":{}}}"#] {
            try Data(raw.utf8).write(to: file)
            XCTAssertThrowsError(try owner.selection())
        }
        try FileManager.default.removeItem(at: file)
        let outside = root.appendingPathComponent("outside")
        try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "nonce": UUID().uuidString, "choices": [:] as [String: String]]).write(to: outside)
        try FileManager.default.createSymbolicLink(at: file, withDestinationURL: outside)
        XCTAssertThrowsError(try owner.selection())
    }
}
