import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

@MainActor
final class PluginSelectionReviewSignedTests: XCTestCase {
    private func controller(_ root: URL) throws -> ExtensionController {
        try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: ["portable-js"], capabilities: ["resource.receive"]),
            resourceDirectory: root, installationDirectory: root.appendingPathComponent("installed"))
    }
    private func install(_ controller: ExtensionController, input: URL) async throws -> ExtensionController.InstalledRelease {
        let prepared = try await controller.prepareSignedRelease(artifact: input.appendingPathComponent("r/plugin.zip"), descriptor: input.appendingPathComponent("r/release.json"),
            signature: input.appendingPathComponent("r/release.sig"), trustedPolicy: input.appendingPathComponent("policy.json"))
        return try await controller.installPreparedRelease(id: prepared.id)
    }
    private func setup() async throws -> (URL, URL, ExtensionController, ExtensionController.InstalledRelease, ExtensionController.InstalledRelease) {
        guard ExtensionRuntime.supportStatus == 0, let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else { throw XCTSkip("requires explicit JS-enabled signed selection fixtures") }
        let input = URL(fileURLWithPath: path), root = sessionTestDirectory().appendingPathComponent("selection-signed-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        let owner = try controller(root); addTeardownBlock { let done = await owner.close(); XCTAssertTrue(done) }
        let old = try await install(owner, input: input), next = try await install(owner, input: input.appendingPathComponent("upgrade"))
        _ = try await owner.selectInstalledVersion(releaseID: old.signature.descriptorSHA256, expectedRevision: nil, trustedPolicy: input.appendingPathComponent("policy.json"))
        return (input, root, owner, old, next)
    }
    private func preview(_ controller: ExtensionController, next: ExtensionController.InstalledRelease, input: URL) async throws -> ExtensionController.SelectionReview {
        let catalog = try await controller.installedVersions()
        return try await controller.previewInstalledSelection(releaseID: next.signature.descriptorSHA256, expectedRevision: catalog.revision, trustedPolicy: input.appendingPathComponent("policy.json"))
    }
    func testActualCorePreviewBindsFullManifestAndLeavesSelectionAndApprovalAlone() async throws {
        let (input, _, controller, old, next) = try await setup()
        let before = try await controller.installedVersions(), view = try await preview(controller, next: next, input: input)
        XCTAssertEqual(view.previousReleaseID, old.signature.descriptorSHA256); XCTAssertEqual(view.previousState, .verified)
        XCTAssertEqual(view.addedPermissions?.first?.purpose, "Review changed declaration before a version switch.")
        XCTAssertEqual(view.addedPermissions?.count, 1); XCTAssertEqual(view.removedPermissions?.count, 1)
        XCTAssertTrue(view.candidate.executorDeclarations?.contains("portable-reviewed") == true)
        XCTAssertTrue(view.previous?.executorDeclarations?.contains("portable-reviewed") == false)
        XCTAssertFalse(view.candidate.review.sourceApproved); XCTAssertFalse(view.candidate.review.authorizationGranted)
        let after = try await controller.installedVersions(); XCTAssertEqual(after.revision, before.revision)
        XCTAssertEqual(after.selections.first?.releaseID, old.signature.descriptorSHA256)
        let status = try await controller.resolve(command: "preview", selectedID: next.review.id, selectedDigest: next.review.digest).status; XCTAssertNotEqual(status, 0)
    }
    func testAcknowledgementRequiredAndConfirmedSwitchStillNeedsNewSourceApproval() async throws {
        let (input, _, controller, old, next) = try await setup()
        let approved = try await controller.approve(id: old.review.id, digest: old.review.digest); XCTAssertEqual(approved, 0)
        let view = try await preview(controller, next: next, input: input)
        do { _ = try await controller.confirmInstalledSelection(id: view.id); XCTFail("selection changed without reviewed confirmation"); return }
        catch PluginSelectionReview.Failure.confirmation {} catch { XCTFail("unexpected confirmation refusal: \(error)") }
        let before = try await controller.installedVersions(); XCTAssertEqual(before.selections.first?.releaseID, old.signature.descriptorSHA256)
        let result = try await controller.confirmInstalledSelection(id: view.id, acknowledged: true)
        XCTAssertTrue(result.durabilityConfirmed); XCTAssertEqual(result.catalog.selections.first?.releaseID, next.signature.descriptorSHA256)
        let denied = try await controller.resolve(command: "preview", selectedID: next.review.id, selectedDigest: next.review.digest).status; XCTAssertNotEqual(denied, 0)
        let fresh = try await controller.approve(id: next.review.id, digest: next.review.digest); XCTAssertEqual(fresh, 0)
        let rollback = try await preview(controller, next: old, input: input)
        XCTAssertEqual(rollback.previous?.review.version, "0.3.0"); XCTAssertEqual(rollback.candidate.review.version, "0.1.0")
        _ = try await controller.confirmInstalledSelection(id: rollback.id, acknowledged: true)
        let deniedOld = try await controller.resolve(command: "preview", selectedID: old.review.id, selectedDigest: old.review.digest).status; XCTAssertNotEqual(deniedOld, 0)
    }
    func testChangedPolicyAfterPreviewCannotUseItsOldAttestation() async throws {
        let (input, root, controller, old, next) = try await setup(), policy = root.appendingPathComponent("current.json")
        try Data(contentsOf: input.appendingPathComponent("policy.json")).write(to: policy)
        let before = try await controller.installedVersions()
        let view = try await controller.previewInstalledSelection(releaseID: next.signature.descriptorSHA256, expectedRevision: before.revision, trustedPolicy: policy)
        try (Data(contentsOf: policy) + Data([10])).write(to: policy)
        do { _ = try await controller.confirmInstalledSelection(id: view.id, acknowledged: true); XCTFail("changed policy retained old review") } catch {}
        let after = try await controller.installedVersions(); XCTAssertEqual(after.revision, before.revision)
        XCTAssertEqual(after.selections.first?.releaseID, old.signature.descriptorSHA256)
    }
    func testChangedCandidatePayloadMustBeRecapturedAtConfirmation() async throws {
        let (input, _, controller, old, next) = try await setup(), view = try await preview(controller, next: next, input: input)
        let file = next.directory.appendingPathComponent("client/index.js")
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
        try Data("changed".utf8).write(to: file); try FileManager.default.setAttributes([.posixPermissions: 0o400], ofItemAtPath: file.path)
        do { _ = try await controller.confirmInstalledSelection(id: view.id, acknowledged: true); XCTFail("modified candidate was enabled") } catch {}
        let after = try await controller.installedVersions(); XCTAssertEqual(after.selections.first?.releaseID, old.signature.descriptorSHA256)
    }
    func testCancelReleasesRealVersionReferenceWithoutChangingSelection() async throws {
        let (input, _, controller, old, next) = try await setup(), view = try await preview(controller, next: next, input: input)
        do {
            let unexpected = try await controller.previewVersionRemoval(releaseID: next.signature.descriptorSHA256, expectedRevision: view.revision)
            await controller.cancelVersionRemoval(id: unexpected.id)
            XCTFail("review reference did not pin actual candidate"); return
        }
        catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .occupied) }
        await controller.cancelInstalledSelection(id: view.id)
        let removal = try await controller.previewVersionRemoval(releaseID: next.signature.descriptorSHA256, expectedRevision: view.revision)
        await controller.cancelVersionRemoval(id: removal.id)
        let after = try await controller.installedVersions(); XCTAssertEqual(after.selections.first?.releaseID, old.signature.descriptorSHA256)
    }
    func testUnknownOldSignatureIsShownWithoutAnEmptyPermissionDifference() async throws {
        let (input, _, controller, old, next) = try await setup(), file = old.directory.deletingLastPathComponent().appendingPathComponent("release.sig")
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
        try Data(repeating: 0, count: 64).write(to: file); try FileManager.default.setAttributes([.posixPermissions: 0o400], ofItemAtPath: file.path)
        let view = try await preview(controller, next: next, input: input)
        XCTAssertEqual(view.previousState, .unknown); XCTAssertNil(view.addedPermissions); XCTAssertNil(view.removedPermissions)
        XCTAssertNotNil(view.previousIssue)
        let result = try await controller.confirmInstalledSelection(id: view.id, acknowledged: true)
        XCTAssertEqual(result.catalog.selections.first?.releaseID, next.signature.descriptorSHA256)
    }
    func testOtherControllerChoiceChangeRejectsTheEntirePreview() async throws {
        let (input, root, controller, old, next) = try await setup(), view = try await preview(controller, next: next, input: input)
        let other = try self.controller(root); addTeardownBlock { let closed = await other.close(); XCTAssertTrue(closed) }
        _ = try await other.disableInstalledPlugin(pluginID: old.review.id, expectedRevision: view.revision)
        do { _ = try await controller.confirmInstalledSelection(id: view.id, acknowledged: true); XCTFail("stale version transition enabled candidate") } catch {}
        let after = try await controller.installedVersions(); XCTAssertEqual(after.selections.first?.releaseID, old.signature.descriptorSHA256)
        XCTAssertEqual(after.selections.first?.enabled, false)
    }
}
