import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

/// Public signed declarations, with a separate injected metadata consumer for
/// data-layer tests. Actual Core capture is covered by the enabled fixture suite.
@MainActor
final class PluginPolicyFixture {
    let directory: URL, installed: URL, policy: URL
    let key = Curve25519.Signing.PrivateKey()
    var choices: [String: PluginReleaseCatalog.Choice] = [:]
    let identity = PluginDirectorySignatureVerifier.Identity(id: "org.ghosterm.one", version: "0.1.0", publisher: "Policy Fixture", packageSHA256: String(repeating: "a", count: 64))
    var keyID: String { PluginReleaseCatalog.sha(key.publicKey.rawRepresentation) }
    init() throws {
        directory = sessionTestDirectory().appendingPathComponent("policy-" + UUID().uuidString)
        installed = directory.appendingPathComponent("installed"); policy = directory.appendingPathComponent("policy.json")
        try FileManager.default.createDirectory(at: installed, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try writePolicy()
    }
    func json(_ value: Any) throws -> Data { try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]) }
    func writePolicy(revoked: Bool = false, channel: String = "test") throws {
        let value: [String: Any] = ["schemaVersion": 1, "channel": channel, "keys": [["id": keyID, "algorithm": "ed25519",
            "publicKey": key.publicKey.rawRepresentation.base64EncodedString(), "publisher": identity.publisher,
            "pluginIds": ["org.ghosterm.one", "org.ghosterm.two", "org.ghosterm.three"]]], "revokedKeyIds": revoked ? [keyID] : []]
        try json(value).write(to: policy)
    }
    @discardableResult func add(_ id: String = "org.ghosterm.one", enabled: Bool = true, channel: String = "test", version: String = "0.1.0") throws -> String {
        let descriptor = try json(["schemaVersion": 1, "channel": channel, "keyId": keyID, "pluginId": id,
            "pluginVersion": version, "publisher": identity.publisher, "packageSHA256": identity.packageSHA256,
            "artifact": ["format": "zip-v1", "bytes": 22, "sha256": String(repeating: "b", count: 64)]])
        let release = PluginReleaseCatalog.sha(descriptor), path = installed.appendingPathComponent(release)
        try FileManager.default.createDirectory(at: path.appendingPathComponent("payload"), withIntermediateDirectories: true)
        try descriptor.write(to: path.appendingPathComponent("release.json"))
        try key.signature(for: Data("GHOSTERM-PLUGIN-RELEASE-V1\0".utf8) + descriptor).write(to: path.appendingPathComponent("release.sig"))
        try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: path.appendingPathComponent("payload").path)
        try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: path.path)
        choices[id] = .init(releaseID: release, enabled: enabled, generation: UUID().uuidString)
        try saveChoices(); return release
    }
    func saveChoices() throws {
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: installed, exclusive: true))
        _ = try owner.writeSelection(.init(revision: nil, choices: choices), expected: owner.selection().revision)
    }
    func selection() throws -> PluginReleaseCatalog.Selection {
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: installed, exclusive: false)); return try owner.selection()
    }
    func review(inspect: ((Int32) throws -> PluginDirectorySignatureVerifier.Identity)? = nil) throws -> PluginPolicyReview {
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: installed, exclusive: true))
        return try PluginPolicyReview(owner: owner, policyURL: policy, inspect: inspect ?? { _ in self.identity })
    }
    func apply(_ review: PluginPolicyReview, ids: [String] = ["org.ghosterm.one"], sync: @escaping (Int32) -> Int32 = { fsync($0) }) throws -> (PluginReleaseCatalog.Selection, Bool) {
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: installed, exclusive: true, parentSync: sync))
        return try review.apply(owner: owner, pluginIDs: ids)
    }
    func controller() throws -> ExtensionController {
        try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: [], capabilities: []), installationDirectory: installed)
    }
}

@MainActor
final class PluginPolicyReviewTests: XCTestCase {
    private func fixture() throws -> PluginPolicyFixture {
        let f = try PluginPolicyFixture(); addTeardownBlock { try PluginReleaseInstallTests.thaw(f.directory) }; return f
    }
    func testValidPolicyAndContentObservationDoesNotWriteSelection() throws {
        let f = try fixture(); try f.add(); let before = try f.selection()
        var captures = 0
        let review = try f.review { _ in captures += 1; return f.identity }
        XCTAssertEqual(captures, 1); XCTAssertEqual(review.value.entries.first?.state, .accepted)
        XCTAssertEqual(review.value.policySHA256, PluginReleaseCatalog.sha(try Data(contentsOf: f.policy)))
        XCTAssertEqual(try f.selection().revision, before.revision)
        XCTAssertEqual(try f.selection().choices, before.choices)
    }
    func testRevokedSignatureIsRejectedWithoutCapturingOrExecuting() throws {
        let f = try fixture(); try f.add(); try f.writePolicy(revoked: true)
        let review = try f.review { _ in XCTFail("revoked content reached package capture"); return f.identity }
        XCTAssertEqual(review.value.entries.first?.state, .rejected)
        XCTAssertTrue(try f.selection().choices["org.ghosterm.one"]!.enabled, "observation cannot revoke implicitly")
    }
    func testCaptureFailureIsUnknownRatherThanRevokedOrAccepted() throws {
        let f = try fixture(); try f.add()
        let review = try f.review { _ in throw PluginReleaseCatalog.Failure.io }
        XCTAssertEqual(review.value.entries.first?.state, .unknown)
    }
    func testMissingVersionIsUnknownAndCanOnlyBeDisabledExplicitly() throws {
        let f = try fixture(); f.choices["org.ghosterm.one"] = .init(releaseID: String(repeating: "c", count: 64), enabled: true, generation: UUID().uuidString); try f.saveChoices()
        let review = try f.review(); XCTAssertEqual(review.value.entries.first?.state, .unknown)
        XCTAssertTrue(try f.selection().choices["org.ghosterm.one"]!.enabled)
        let (after, durable) = try f.apply(review); XCTAssertTrue(durable); XCTAssertFalse(after.choices["org.ghosterm.one"]!.enabled)
        XCTAssertEqual(review.value.entries.first?.state, .unknown)
    }
    func testDifferentChannelIsRejectedWithScopeReason() throws {
        let f = try fixture(); try f.add(channel: "another")
        let value = try f.review().value
        XCTAssertEqual(value.entries.first?.state, .rejected)
        XCTAssertEqual(value.entries.first?.reason, PluginDirectorySignatureVerifier.Failure.scope.errorDescription)
    }
    func testMetadataIdentityCannotBorrowAnotherPluginSignature() throws {
        let f = try fixture(); try f.add()
        let value = try f.review { _ in .init(id: "org.ghosterm.two", version: f.identity.version, publisher: f.identity.publisher, packageSHA256: f.identity.packageSHA256) }.value
        XCTAssertEqual(value.entries.first?.state, .rejected)
    }
    func testMetadataVersionDigestAndPublisherMustMatchSignedDescriptor() throws {
        let f = try fixture(); try f.add()
        for identity in [
            PluginDirectorySignatureVerifier.Identity(id: f.identity.id, version: "0.2.0", publisher: f.identity.publisher, packageSHA256: f.identity.packageSHA256),
            .init(id: f.identity.id, version: f.identity.version, publisher: "Other", packageSHA256: f.identity.packageSHA256),
            .init(id: f.identity.id, version: f.identity.version, publisher: f.identity.publisher, packageSHA256: String(repeating: "d", count: 64))
        ] { XCTAssertEqual(try f.review { _ in identity }.value.entries.first?.state, .rejected) }
    }
    func testValidDescriptorForAnotherSelectionKeyIsRejected() throws {
        let f = try fixture(); try f.add()
        f.choices["org.ghosterm.two"] = f.choices.removeValue(forKey: "org.ghosterm.one"); try f.saveChoices()
        XCTAssertEqual(try f.review().value.entries.first?.state, .rejected)
    }
    func testInvalidPolicyFailsEvenWithNoEnabledChoices() throws {
        let f = try fixture(); try Data("{}".utf8).write(to: f.policy)
        XCTAssertThrowsError(try f.review())
    }
    func testPolicyLinksAreNotFollowed() throws {
        let f = try fixture(), original = f.directory.appendingPathComponent("original.json")
        try FileManager.default.moveItem(at: f.policy, to: original)
        XCTAssertEqual(symlink(original.path, f.policy.path), 0)
        XCTAssertThrowsError(try f.review())
    }
    func testBatchOnlyDisablesExplicitSubsetInOneSelectionWrite() throws {
        let f = try fixture(); try f.add(); try f.add("org.ghosterm.two"); try f.add("org.ghosterm.three", enabled: false)
        try f.writePolicy(revoked: true); let before = try f.selection(), review = try f.review()
        XCTAssertEqual(review.value.entries.map(\.pluginID), ["org.ghosterm.one", "org.ghosterm.two"])
        let (after, durable) = try f.apply(review); XCTAssertTrue(durable); XCTAssertNotEqual(after.revision, before.revision)
        XCTAssertFalse(after.choices["org.ghosterm.one"]!.enabled)
        XCTAssertNotEqual(after.choices["org.ghosterm.one"]!.generation, before.choices["org.ghosterm.one"]!.generation)
        XCTAssertEqual(after.choices["org.ghosterm.two"], before.choices["org.ghosterm.two"])
        XCTAssertEqual(after.choices["org.ghosterm.three"], before.choices["org.ghosterm.three"])
    }
    func testAcceptedRowsCannotBeSubmittedForThisDisableAction() throws {
        let f = try fixture(); try f.add(); let review = try f.review(), before = try f.selection()
        XCTAssertEqual(review.value.entries.first?.state, .accepted)
        XCTAssertThrowsError(try f.apply(review))
        XCTAssertEqual(try f.selection().revision, before.revision)
    }
    func testEmptyDuplicateAndUnreviewedIDsCannotWrite() throws {
        let f = try fixture(); try f.add(); try f.writePolicy(revoked: true); let review = try f.review(), before = try f.selection()
        for ids in [[], ["org.ghosterm.one", "org.ghosterm.one"], ["org.ghosterm.two"]] {
            XCTAssertThrowsError(try f.apply(review, ids: ids))
            XCTAssertEqual(try f.selection().revision, before.revision)
        }
    }
    func testPolicyChangedAfterPreviewRejectsEvenWhitespaceOnlyChanges() throws {
        let f = try fixture(); try f.add(); try f.writePolicy(revoked: true); let review = try f.review(), before = try f.selection()
        try (Data(contentsOf: f.policy) + Data([10])).write(to: f.policy)
        XCTAssertThrowsError(try f.apply(review))
        XCTAssertEqual(try f.selection().revision, before.revision)
    }
    func testChangedSelectionRejectsWholeBatchWithoutPartialDisable() throws {
        let f = try fixture(); try f.add(); try f.add("org.ghosterm.two"); try f.writePolicy(revoked: true)
        let review = try f.review(); try f.saveChoices(); let before = try f.selection()
        XCTAssertThrowsError(try f.apply(review, ids: ["org.ghosterm.one", "org.ghosterm.two"]))
        XCTAssertEqual(try f.selection().choices, before.choices); XCTAssertEqual(try f.selection().revision, before.revision)
    }
    func testReplacedRootCannotReuseIdenticalSelectionReceipt() throws {
        let f = try fixture(); try f.add(); try f.writePolicy(revoked: true); let review = try f.review()
        let bytes = try Data(contentsOf: f.installed.appendingPathComponent(PluginReleaseCatalog.selectionName))
        let moved = f.directory.appendingPathComponent("old"); try FileManager.default.moveItem(at: f.installed, to: moved)
        try FileManager.default.createDirectory(at: f.installed, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        try bytes.write(to: f.installed.appendingPathComponent(PluginReleaseCatalog.selectionName))
        let before = try f.selection()
        XCTAssertThrowsError(try f.apply(review))
        XCTAssertEqual(try f.selection().revision, before.revision); XCTAssertTrue(try f.selection().choices["org.ghosterm.one"]!.enabled)
    }
    func testPublishedButUnsyncedDisableRemainsVisibleAsUnknownDurability() throws {
        let f = try fixture(); try f.add(); try f.writePolicy(revoked: true); let review = try f.review()
        let (selection, durable) = try f.apply(review, sync: { _ in -1 })
        XCTAssertFalse(durable); XCTAssertFalse(selection.choices["org.ghosterm.one"]!.enabled)
        XCTAssertEqual(try f.selection().revision, selection.revision)
    }
    func testControllerCancellationAndRestartCannotRecoverApprovalTicket() async throws {
        let f = try fixture(); try f.add(); try f.writePolicy(revoked: true)
        let controller = try f.controller(), review = try await controller.previewInstalledPolicy(trustedPolicy: f.policy)
        await controller.cancelInstalledPolicyReview(id: review.id)
        do { _ = try await controller.disableReviewedPlugins(id: review.id, pluginIDs: ["org.ghosterm.one"]); XCTFail("cancelled ticket used") } catch {}
        let next = try await controller.previewInstalledPolicy(trustedPolicy: f.policy)
        let closed = await controller.close(); XCTAssertTrue(closed)
        let other = try f.controller()
        do { _ = try await other.disableReviewedPlugins(id: next.id, pluginIDs: ["org.ghosterm.one"]); XCTFail("restart restored ticket") } catch {}
        let done = await other.close(); XCTAssertTrue(done)
        XCTAssertTrue(try f.selection().choices["org.ghosterm.one"]!.enabled)
    }
    func testControllerDisablePersistsWithoutDeletingOrLoadingPackage() async throws {
        let f = try fixture(), release = try f.add(); try f.writePolicy(revoked: true)
        let controller = try f.controller(), review = try await controller.previewInstalledPolicy(trustedPolicy: f.policy)
        XCTAssertEqual(review.entries.first?.state, .rejected)
        let result = try await controller.disableReviewedPlugins(id: review.id, pluginIDs: ["org.ghosterm.one"])
        XCTAssertEqual(result.disabled, ["org.ghosterm.one"]); XCTAssertTrue(result.selection.durabilityConfirmed)
        XCTAssertFalse(try f.selection().choices["org.ghosterm.one"]!.enabled)
        XCTAssertTrue(FileManager.default.fileExists(atPath: f.installed.appendingPathComponent(release + "/release.json").path))
        let resolution = try await controller.resolve(command: "preview", selectedID: "org.ghosterm.one", selectedDigest: f.identity.packageSHA256)
        XCTAssertNotEqual(resolution.status, 0)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
}
