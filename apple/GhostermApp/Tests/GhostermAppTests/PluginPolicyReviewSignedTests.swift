import Darwin
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

@MainActor
final class PluginPolicyReviewSignedTests: XCTestCase {
    private func setup() async throws -> (URL, URL, ExtensionController, ExtensionController.InstalledRelease) {
        guard ExtensionRuntime.supportStatus == 0, let path = ProcessInfo.processInfo.environment["GHOSTERM_TEST_SIGNED_RELEASE"] else { throw XCTSkip("requires JS-enabled public signed fixtures") }
        let input = URL(fileURLWithPath: path), root = sessionTestDirectory().appendingPathComponent("policy-signed-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        let controller = try other(root); addTeardownBlock { let closed = await controller.close(); XCTAssertTrue(closed) }
        let release = try await controller.prepareSignedRelease(artifact: input.appendingPathComponent("r/plugin.zip"), descriptor: input.appendingPathComponent("r/release.json"),
            signature: input.appendingPathComponent("r/release.sig"), trustedPolicy: input.appendingPathComponent("policy.json"))
        let installed = try await controller.installPreparedRelease(id: release.id)
        _ = try await controller.selectInstalledVersion(releaseID: installed.signature.descriptorSHA256, expectedRevision: nil, trustedPolicy: input.appendingPathComponent("policy.json"))
        return (input, root, controller, installed)
    }
    private func other(_ root: URL) throws -> ExtensionController {
        try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: ["lua"], capabilities: ["session.rpc", "resource.receive"]), resourceDirectory: root, installationDirectory: root.appendingPathComponent("installed"))
    }
    private func revoked(_ input: URL, root: URL, keyID: String) throws -> URL {
        let path = root.appendingPathComponent("revoked.json")
        var policy = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: input.appendingPathComponent("policy.json"))) as? [String: Any])
        policy["revokedKeyIds"] = [keyID]
        try JSONSerialization.data(withJSONObject: policy, options: [.sortedKeys]).write(to: path); return path
    }
    private func status(_ controller: ExtensionController, release: ExtensionController.InstalledRelease) async throws -> UInt32 {
        try await controller.resolve(command: "preview", selectedID: release.review.id, selectedDigest: release.review.digest).status
    }
    func testRealCoreAcceptedObservationDoesNotGrantSourceOrMutateChoices() async throws {
        let (input, root, controller, release) = try await setup()
        let before = try await controller.installedVersions()
        let review = try await controller.previewInstalledPolicy(trustedPolicy: input.appendingPathComponent("policy.json"))
        XCTAssertEqual(review.entries.first?.state, .accepted)
        let after = try await controller.installedVersions(); XCTAssertEqual(after.revision, before.revision)
        let initial = try await status(controller, release: release); XCTAssertNotEqual(initial, 0)
        do { _ = try await controller.disableReviewedPlugins(id: review.id, pluginIDs: [release.review.id]); XCTFail("accepted row submitted to rejection action") } catch {}
        let next = try other(root); addTeardownBlock { let closed = await next.close(); XCTAssertTrue(closed) }
        let nextReview = try await next.previewInstalledPolicy(trustedPolicy: input.appendingPathComponent("policy.json"))
        XCTAssertEqual(nextReview.entries.first?.state, .accepted)
        let nextStatus = try await status(next, release: release); XCTAssertNotEqual(nextStatus, 0, "audit must not load or approve a package")
        do {
            _ = try await next.approve(id: release.review.id, digest: release.review.digest)
            XCTFail("audit unexpectedly loaded a source approval candidate")
        } catch ExtensionController.ControllerError.invalidPackageURL {}
        catch { XCTFail("unexpected source-approval error: \(error)") }
    }
    func testExplicitRevocationBlocksOtherControllerAndNeedsFreshApproval() async throws {
        let (input, root, controller, release) = try await setup()
        let granted = try await controller.approve(id: release.review.id, digest: release.review.digest); XCTAssertEqual(granted, 0)
        let before = try await status(controller, release: release); XCTAssertEqual(before, 0)
        let path = try revoked(input, root: root, keyID: release.signature.keyID), second = try other(root)
        addTeardownBlock { let closed = await second.close(); XCTAssertTrue(closed) }
        let review = try await second.previewInstalledPolicy(trustedPolicy: path)
        XCTAssertEqual(review.entries.first?.state, .rejected)
        let observed = try await status(controller, release: release); XCTAssertEqual(observed, 0, "read-only observation cannot revoke implicitly")
        let result = try await second.disableReviewedPlugins(id: review.id, pluginIDs: [release.review.id]); XCTAssertTrue(result.selection.durabilityConfirmed)
        let denied = try await status(controller, release: release); XCTAssertNotEqual(denied, 0)
        let disabled = try await controller.installedVersions(); XCTAssertFalse(disabled.selections[0].enabled)
        do { _ = try await controller.selectInstalledVersion(releaseID: release.signature.descriptorSHA256, expectedRevision: disabled.revision, trustedPolicy: path); XCTFail("revoked policy re-enabled package") } catch {}
        _ = try await controller.selectInstalledVersion(releaseID: release.signature.descriptorSHA256, expectedRevision: disabled.revision, trustedPolicy: input.appendingPathComponent("policy.json"))
        let stillDenied = try await status(controller, release: release); XCTAssertNotEqual(stillDenied, 0, "explicitly selecting another trusted policy cannot restore old approval")
    }
    func testChangedPayloadCannotBeReportedAcceptedFromStoredSignature() async throws {
        let (input, _, controller, release) = try await setup()
        let file = release.directory.appendingPathComponent("main.lua")
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: file.path)
        try Data("changed content".utf8).write(to: file)
        try FileManager.default.setAttributes([.posixPermissions: 0o400], ofItemAtPath: file.path)
        let review = try await controller.previewInstalledPolicy(trustedPolicy: input.appendingPathComponent("policy.json"))
        XCTAssertEqual(review.entries.first?.state, .unknown)
        let catalog = try await controller.installedVersions(); XCTAssertTrue(catalog.selections.first!.enabled)
    }
    func testReplacingReviewTicketDoesNotAuthorizeAnOlderSnapshot() async throws {
        let (input, root, controller, release) = try await setup(), path = try revoked(input, root: root, keyID: release.signature.keyID)
        let old = try await controller.previewInstalledPolicy(trustedPolicy: path)
        let current = try await controller.previewInstalledPolicy(trustedPolicy: path)
        XCTAssertNotEqual(old.id, current.id)
        do { _ = try await controller.disableReviewedPlugins(id: old.id, pluginIDs: [release.review.id]); XCTFail("old review ticket used") } catch {}
        let result = try await controller.disableReviewedPlugins(id: current.id, pluginIDs: [release.review.id])
        XCTAssertFalse(result.selection.catalog.selections.first!.enabled)
        let fresh = try other(root); addTeardownBlock { let closed = await fresh.close(); XCTAssertTrue(closed) }
        let after = try await fresh.installedVersions(); XCTAssertFalse(after.selections.first!.enabled)
    }
}
