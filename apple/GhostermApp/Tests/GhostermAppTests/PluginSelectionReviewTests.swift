import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

/// Signed declarations with an injected metadata consumer, separate from the
/// enabled suite's actual Core capture and native controls.
@MainActor
final class PluginSelectionFixture {
    let files: PluginPolicyFixture
    var contents: [UInt64: ExtensionController.VersionDeclarations] = [:]
    init() throws { files = try PluginPolicyFixture() }
    func add(version: String, capabilities: [String] = ["resource.receive"], purpose: String = "Fixture purpose", programs: [String] = []) throws -> String {
        let id = try files.add(version: version), path = files.installed.appendingPathComponent(id + "/payload")
        var info = stat(); guard stat(path.path, &info) == 0 else { throw PluginReleaseCatalog.Failure.io }
        let identity = files.identity
        let review = ExtensionController.PackageReview(id: identity.id, version: version, publisher: identity.publisher, license: "MIT", digest: identity.packageSHA256,
            runtime: "lua", requiredCapabilities: capabilities, optionalMissingCapabilities: [], authorizationGranted: false,
            publisherAuthenticated: false, commands: [.init(path: "preview", title: "Fixture", argsSchema: "schema.json", views: ["resource"])], sourceApproved: false)
        contents[UInt64(info.st_ino)] = .init(review: review, sdkVersion: "1.0.0", protocols: [1],
            permissions: [.init(side: "client", capability: "resource.receive", purpose: purpose, scope: "instance", programs: programs)], executorDeclarations: nil)
        return id
    }
    func choose(_ id: String?, enabled: Bool = true) throws {
        if let id { files.choices[files.identity.id] = .init(releaseID: id, enabled: enabled, generation: UUID().uuidString) }
        else { files.choices = [:] }
        try files.saveChoices()
    }
    func inspect(_ fd: Int32) throws -> ExtensionController.VersionDeclarations {
        var info = stat(); guard fstat(fd, &info) == 0, let value = contents[UInt64(info.st_ino)] else { throw PluginReleaseCatalog.Failure.invalid }; return value
    }
    func preview(_ id: String, revision: String? = nil, inspect: ((Int32) throws -> ExtensionController.VersionDeclarations)? = nil) throws -> PluginSelectionReview {
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: files.installed, exclusive: true))
        return try PluginSelectionReview(owner: owner, releaseID: id, expectedRevision: revision ?? owner.selection().revision,
            policyURL: files.policy, inspect: inspect ?? self.inspect)
    }
    func validate(_ ticket: PluginSelectionReview) throws {
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: files.installed, exclusive: true)); try ticket.validate(owner: owner)
    }
}

@MainActor
final class PluginSelectionReviewTests: XCTestCase {
    private func fixture() throws -> PluginSelectionFixture {
        let f = try PluginSelectionFixture(); addTeardownBlock { try PluginReleaseInstallTests.thaw(f.files.directory) }; return f
    }
    func testPreviewIsReadOnlyAndSameVersionDoesNotInferAnUpgrade() throws {
        let f = try fixture(), id = try f.add(version: "arbitrary-version")
        let before = try f.files.selection(), ticket = try f.preview(id)
        XCTAssertEqual(ticket.value.previousState, .verified)
        XCTAssertEqual(ticket.value.previousReleaseID, id); XCTAssertEqual(ticket.value.candidate.review.version, "arbitrary-version")
        XCTAssertEqual(ticket.value.addedPermissions, []); XCTAssertEqual(ticket.value.removedPermissions, [])
        XCTAssertEqual(try f.files.selection().revision, before.revision)
        XCTAssertFalse(ticket.value.candidate.review.sourceApproved); XCTAssertFalse(ticket.value.candidate.review.authorizationGranted)
    }
    func testFirstSelectionHasExplicitNoneAndAllCandidateDeclarations() throws {
        let f = try fixture(), id = try f.add(version: "0.1.0"); try f.choose(nil)
        let value = try f.preview(id).value
        XCTAssertEqual(value.previousState, .none); XCTAssertNil(value.previousReleaseID)
        XCTAssertEqual(value.addedPermissions, value.candidate.permissions); XCTAssertEqual(value.removedPermissions, [])
        XCTAssertEqual(value.addedCapabilities, ["resource.receive"])
    }
    func testPermissionPurposeProgramAndCapabilityChangesRemainVisible() throws {
        let f = try fixture(), old = try f.add(version: "0.1.0", capabilities: ["old.cap"], purpose: "Old", programs: ["old"])
        let next = try f.add(version: "0.2.0", capabilities: ["new.cap"], purpose: "New", programs: ["new"]); try f.choose(old)
        let value = try f.preview(next).value
        XCTAssertEqual(value.previousState, .verified)
        XCTAssertEqual(value.addedPermissions?.first?.purpose, "New"); XCTAssertEqual(value.addedPermissions?.first?.programs, ["new"])
        XCTAssertEqual(value.removedPermissions?.first?.purpose, "Old"); XCTAssertEqual(value.removedPermissions?.first?.programs, ["old"])
        XCTAssertEqual(value.addedCapabilities, ["new.cap"]); XCTAssertEqual(value.removedCapabilities, ["old.cap"])
    }
    func testMissingPreviousIsUnknownRatherThanAnEmptyDifference() throws {
        let f = try fixture(), next = try f.add(version: "0.2.0"); try f.choose(String(repeating: "f", count: 64))
        let value = try f.preview(next).value
        XCTAssertEqual(value.previousState, .unknown); XCTAssertNotNil(value.previousIssue)
        XCTAssertNil(value.previous); XCTAssertNil(value.addedPermissions); XCTAssertNil(value.removedPermissions)
        XCTAssertNil(value.addedCapabilities); XCTAssertNil(value.removedCapabilities)
    }
    func testInvalidPreviousSignatureDoesNotPreventReviewingValidCandidate() throws {
        let f = try fixture(), old = try f.add(version: "0.1.0"), next = try f.add(version: "0.2.0"); try f.choose(old)
        try Data(repeating: 0, count: 64).write(to: f.files.installed.appendingPathComponent(old + "/release.sig"))
        let value = try f.preview(next).value
        XCTAssertEqual(value.previousState, .unknown); XCTAssertEqual(value.candidate.review.version, "0.2.0")
    }
    func testDisabledPreviousChoiceKeepsItsExplicitState() throws {
        let f = try fixture(), old = try f.add(version: "0.1.0"), next = try f.add(version: "0.2.0"); try f.choose(old, enabled: false)
        let value = try f.preview(next).value
        XCTAssertEqual(value.previousEnabled, false); XCTAssertEqual(value.previousState, .verified)
        XCTAssertFalse(try f.files.selection().choices[f.files.identity.id]!.enabled)
    }
    func testRollbackUsesTheSameReviewedTransitionWithoutVersionOrdering() throws {
        let f = try fixture(), old = try f.add(version: "9.0.0"), next = try f.add(version: "1.0.0"); try f.choose(old)
        let value = try f.preview(next).value
        XCTAssertEqual(value.previous?.review.version, "9.0.0"); XCTAssertEqual(value.candidate.review.version, "1.0.0")
        XCTAssertEqual(value.previousState, .verified)
    }
    func testCandidateSignatureAndScopeAreRequired() throws {
        let f = try fixture(), next = try f.add(version: "0.2.0"); try f.files.writePolicy(revoked: true)
        XCTAssertThrowsError(try f.preview(next))
        XCTAssertTrue(try f.files.selection().choices[f.files.identity.id]!.enabled)
    }
    func testCandidateMetadataCannotBorrowSignedVersion() throws {
        let f = try fixture(), old = try f.add(version: "0.1.0"), next = try f.add(version: "0.2.0"); try f.choose(old)
        let other = try f.preview(old).value.candidate
        XCTAssertThrowsError(try f.preview(next, inspect: { _ in other }))
    }
    func testCandidateCaptureFailureCannotFallBackToDeclaration() throws {
        let f = try fixture(), next = try f.add(version: "0.2.0")
        XCTAssertThrowsError(try f.preview(next, inspect: { _ in throw PluginReleaseCatalog.Failure.io }))
    }
    func testStaleSelectionCannotStartOrConfirmReview() throws {
        let f = try fixture(), old = try f.add(version: "0.1.0"), next = try f.add(version: "0.2.0"); try f.choose(old)
        let ticket = try f.preview(next), revision = try XCTUnwrap(ticket.value.revision)
        try f.choose(old)
        XCTAssertThrowsError(try f.preview(next, revision: revision))
        XCTAssertThrowsError(try f.validate(ticket))
        XCTAssertEqual(try f.files.selection().choices[f.files.identity.id]!.releaseID, old)
    }
    func testRootReplacementCannotReuseIdenticalSelectionBytes() throws {
        let f = try fixture(), next = try f.add(version: "0.2.0"), ticket = try f.preview(next)
        let bytes = try Data(contentsOf: f.files.installed.appendingPathComponent(PluginReleaseCatalog.selectionName))
        let moved = f.files.directory.appendingPathComponent("old"); try FileManager.default.moveItem(at: f.files.installed, to: moved)
        try FileManager.default.createDirectory(at: f.files.installed, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        try bytes.write(to: f.files.installed.appendingPathComponent(PluginReleaseCatalog.selectionName))
        // Preserve the candidate inode as well, so only the root binding can
        // reject this relocation; a missing/different candidate must not mask it.
        let originalCandidate = moved.appendingPathComponent(next), newCandidate = f.files.installed.appendingPathComponent(next)
        // Darwin requires owner write permission while reparenting a directory.
        // Only this owned fixture is thawed; the reviewed object is readonly again.
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: originalCandidate.path)
        try FileManager.default.moveItem(at: originalCandidate, to: newCandidate)
        try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: newCandidate.path)
        XCTAssertThrowsError(try f.validate(ticket))
    }
    func testReplacedCandidateCannotReuseIdenticalSelectionAndDescriptor() throws {
        let f = try fixture(), next = try f.add(version: "0.2.0"), ticket = try f.preview(next)
        let selection = f.files.installed.appendingPathComponent(PluginReleaseCatalog.selectionName), bytes = try Data(contentsOf: selection)
        let original = f.files.installed.appendingPathComponent(next), moved = f.files.installed.appendingPathComponent("old")
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: original.path)
        try FileManager.default.moveItem(at: original, to: moved)
        try FileManager.default.setAttributes([.posixPermissions: 0o500], ofItemAtPath: moved.path)
        _ = try f.add(version: "0.2.0"); try bytes.write(to: selection)
        XCTAssertThrowsError(try f.validate(ticket))
    }
    func testCandidateReferenceBlocksRemovalUntilReviewIsReleased() throws {
        let f = try fixture(), next = try f.add(version: "0.2.0"); try f.choose(next, enabled: false)
        var ticket: PluginSelectionReview? = try f.preview(next)
        let value = try XCTUnwrap(ticket?.value)
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: f.files.installed, exclusive: true))
            XCTAssertThrowsError(try PluginReleaseRemoval(owner: owner, releaseID: next, expectedRevision: value.revision)) {
                XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .occupied)
            }
        }
        withExtendedLifetime(ticket) {}
        ticket = nil
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: f.files.installed, exclusive: true))
        let removal = try PluginReleaseRemoval(owner: owner, releaseID: next, expectedRevision: value.revision)
        XCTAssertFalse(removal.members.isEmpty)
    }
}

final class PluginReleaseDeclarationsTests: XCTestCase {
    private func bytes(_ program: String = "fixture-worker") throws -> Data {
        try JSONSerialization.data(withJSONObject: ["schemaVersion": 2,
            "executor": ["entry": "executor.lua", "requires": ["process.spawn"], "programs": [program]]],
            options: [.sortedKeys])
    }
    func testDisplayFieldsUseExactlyTheManifestCoreAttested() throws {
        let raw = try bytes(), (permissions, executor) = try PluginReleaseDeclarations.decode(raw, manifestDigest: PluginReleaseCatalog.sha(raw))
        XCTAssertEqual(permissions.first?.capability, "process.spawn")
        XCTAssertEqual(permissions.first?.programs, ["fixture-worker"])
        XCTAssertTrue(executor?.contains("executor.lua") == true)
    }
    func testLuaManifestShowsCapabilitiesForBothRolesAndSpawnPrograms() throws {
        let raw = Data("""
        {"schemaVersion":2,"client":{"requires":["session.rpc"]},
         "executor":{"requires":["process.spawn","process.pipes"],"programs":["codex"]}}
        """.utf8)
        let (permissions, executor) = try PluginReleaseDeclarations.decode(raw, manifestDigest: PluginReleaseCatalog.sha(raw))
        XCTAssertEqual(permissions.map(\.side), ["client", "executor", "executor"])
        XCTAssertEqual(permissions.map(\.capability), ["session.rpc", "process.spawn", "process.pipes"])
        XCTAssertEqual(permissions.map(\.programs), [[], ["codex"], []])
        XCTAssertTrue(executor?.contains("codex") == true)
    }
    func testChangedManifestCannotBorrowAnOldCoreDigest() throws {
        let raw = try bytes(), changed = try bytes("Different")
        XCTAssertThrowsError(try PluginReleaseDeclarations.decode(changed, manifestDigest: PluginReleaseCatalog.sha(raw)))
    }
    func testInvalidDuplicateAndMissingDeclarationsAreNotSilentlyEmpty() throws {
        for raw in [Data("{}".utf8), Data(#"{"schemaVersion":1,"permissions":[]}"#.utf8),
                    Data(#"{"schemaVersion":2,"executor":{}}"#.utf8),
                    Data(#"{"schemaVersion":2,"client":{"requires":[],"requires":[]}}"#.utf8)] {
            XCTAssertThrowsError(try PluginReleaseDeclarations.decode(raw, manifestDigest: PluginReleaseCatalog.sha(raw)))
        }
    }
    func testDisplayLimitRejectsWholeManifestRatherThanTruncating() throws {
        let raw = Data(repeating: 32, count: 65537)
        XCTAssertThrowsError(try PluginReleaseDeclarations.decode(raw, manifestDigest: PluginReleaseCatalog.sha(raw))) {
            XCTAssertEqual($0 as? PluginDirectorySignatureVerifier.Failure, .limit)
        }
    }
}
