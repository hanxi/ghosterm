import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

@MainActor
final class PluginInstallJournalTests: XCTestCase {
    private func root() throws -> URL {
        let root = sessionTestDirectory().appendingPathComponent("install-journal-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        return root
    }
    private func controller(_ root: URL) throws -> ExtensionController {
        try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: [], capabilities: []), installationDirectory: root)
    }
    private func journal<T>(_ root: URL, _ body: (PluginInstallJournal) throws -> T) throws -> T {
        let fd = open(root.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw PluginReleaseStage.Failure.root }
        defer { Darwin.close(fd) }
        return try body(PluginInstallJournal(parent: fd, create: true)!)
    }
    private func abandoned(_ root: URL) throws -> String {
        var stage: PluginReleaseStage? = try PluginReleaseStage(directory: root)
        try stage!.write(Data("retained".utf8), path: "payload/file.txt")
        let name = stage!.name
        stage = nil
        return name
    }
    private func entry(_ controller: ExtensionController) async throws -> ExtensionController.InstallationResidue {
        let result = try await controller.installationResidues()
        XCTAssertTrue(result.issues.isEmpty)
        XCTAssertEqual(result.entries.count, 1)
        return try XCTUnwrap(result.entries.first)
    }
    func testLiveOwnerIsObservedWithoutDowngradingItsRootLock() async throws {
        let root = try root(), stage = try PluginReleaseStage(directory: root)
        let fd = try stage.catalogLease(); defer { Darwin.close(fd) }
        let journal = try XCTUnwrap(PluginInstallJournal(parent: fd))
        XCTAssertEqual(try journal.listing().entries.first?.state, .active)
        XCTAssertThrowsError(try PluginReleaseCatalog(directory: root, exclusive: true))
        try stage.discard()
        XCTAssertTrue(try journal.listing().entries.isEmpty)
    }
    func testAbandonedStagePreviewCancelAndExplicitRemoval() async throws {
        let root = try root(), name = try abandoned(root), controller = try controller(root)
        let value = try await entry(controller); XCTAssertEqual(value.state, .abandoned)
        XCTAssertEqual(value.directory.lastPathComponent, name)
        let preview = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest)
        XCTAssertEqual(preview.files.map(\.path), ["payload", "payload/file.txt"])
        await controller.cancelVersionRemoval(id: preview.id)
        XCTAssertEqual(try Data(contentsOf: root.appendingPathComponent(name + "/payload/file.txt")), Data("retained".utf8))
        let again = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest)
        let result = try await controller.confirmVersionRemoval(id: again.id)
        XCTAssertTrue(result.complete && result.durabilityConfirmed); XCTAssertNil(result.cleanupIssue)
        XCTAssertEqual(result.removed, 3)
        let after = try await controller.installationResidues(); XCTAssertTrue(after.entries.isEmpty)
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent(name).path))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testReservationWithoutDirectoryCanOnlyRetireMetadata() async throws {
        let root = try root(), id = UUID().uuidString.lowercased()
        _ = try journal(root) { try $0.reserve(id: id) }
        let controller = try controller(root), value = try await entry(controller)
        XCTAssertEqual(value.state, .absent)
        try await controller.retireInstallationRecord(id: value.id, expectedDigest: value.digest)
        let after = try await controller.installationResidues(); XCTAssertTrue(after.entries.isEmpty)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testUnconfirmedSameNameDirectoryIsNotAdopted() async throws {
        let root = try root(), id = UUID().uuidString.lowercased()
        let record = try journal(root) { try $0.reserve(id: id) }
        let directory = root.appendingPathComponent(record.name)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let file = directory.appendingPathComponent("unconfirmed"); try Data("keep".utf8).write(to: file)
        let controller = try controller(root), value = try await entry(controller)
        XCTAssertEqual(value.state, .unconfirmed)
        do { _ = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest); XCTFail("reservation adopted a same-name object") } catch {}
        do { try await controller.retireInstallationRecord(id: value.id, expectedDigest: value.digest); XCTFail("unconfirmed reservation disappeared") } catch {}
        XCTAssertEqual(try? Data(contentsOf: file), Data("keep".utf8))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testReplacedDirectoryIsPreserved() async throws {
        let root = try root(), name = try abandoned(root), path = root.appendingPathComponent(name)
        let moved = root.appendingPathComponent("original"); try FileManager.default.moveItem(at: path, to: moved)
        try FileManager.default.createDirectory(at: path, withIntermediateDirectories: false)
        let file = path.appendingPathComponent("foreign"); try Data("foreign".utf8).write(to: file)
        let controller = try controller(root), value = try await entry(controller)
        XCTAssertEqual(value.state, .changed)
        do { _ = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest); XCTFail("replacement adopted") } catch {}
        XCTAssertEqual(try? Data(contentsOf: file), Data("foreign".utf8))
        XCTAssertTrue(FileManager.default.fileExists(atPath: moved.appendingPathComponent("payload/file.txt").path))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testRecordCASRejectsChangedPreview() async throws {
        let root = try root(), name = try abandoned(root), controller = try controller(root), value = try await entry(controller)
        let preview = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest)
        try journal(root) { journal in
            var (record, reference) = try journal.read(value.id)
            record.target = String(repeating: "f", count: 64)
            try journal.write(record, expected: reference.digest)
        }
        do { _ = try await controller.confirmVersionRemoval(id: preview.id); XCTFail("changed ownership record confirmed") } catch {}
        XCTAssertEqual(try? Data(contentsOf: root.appendingPathComponent(name + "/payload/file.txt")), Data("retained".utf8))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testTreeChangesAfterPreviewArePreserved() async throws {
        let root = try root(), name = try abandoned(root), controller = try controller(root), value = try await entry(controller)
        let preview = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest)
        let file = root.appendingPathComponent(name + "/payload/foreign"); try Data("new".utf8).write(to: file)
        do { _ = try await controller.confirmVersionRemoval(id: preview.id); XCTFail("changed tree deleted") } catch {}
        XCTAssertEqual(try? Data(contentsOf: file), Data("new".utf8))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testAlreadyPublishedRecordNeverDeletesCanonicalVersion() async throws {
        let root = try root(), target = String(repeating: "a", count: 64)
        var stage: PluginReleaseStage? = try PluginReleaseStage(directory: root, parentSync: { _ in -1 })
        try stage!.write(Data("published".utf8), path: "payload/file.txt")
        let result = try stage!.publish(id: target); XCTAssertFalse(result.durabilityConfirmed); XCTAssertNotNil(stage!.maintenanceIssue)
        stage = nil
        let controller = try controller(root), value = try await entry(controller)
        XCTAssertEqual(value.state, .published)
        do { _ = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest); XCTFail("published directory offered as staging") } catch {}
        try await controller.retireInstallationRecord(id: value.id, expectedDigest: value.digest)
        XCTAssertEqual(try? Data(contentsOf: result.directory.appendingPathComponent("payload/file.txt")), Data("published".utf8))
        let after = try await controller.installationResidues(); XCTAssertTrue(after.entries.isEmpty)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testConflictingPublicationOnlyRecoversOwnedSource() async throws {
        let root = try root(), target = String(repeating: "b", count: 64)
        var stage: PluginReleaseStage? = try PluginReleaseStage(directory: root)
        try stage!.write(Data([1]), path: "payload/file")
        let foreign = root.appendingPathComponent(target)
        try FileManager.default.createDirectory(at: foreign, withIntermediateDirectories: false)
        let file = foreign.appendingPathComponent("foreign"); try Data("keep".utf8).write(to: file)
        XCTAssertThrowsError(try stage!.publish(id: target)); stage = nil
        let controller = try controller(root), value = try await entry(controller)
        XCTAssertEqual(value.state, .abandoned)
        let preview = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest)
        let result = try await controller.confirmVersionRemoval(id: preview.id)
        XCTAssertTrue(result.complete); XCTAssertNil(result.cleanupIssue)
        XCTAssertEqual(try? Data(contentsOf: file), Data("keep".utf8))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testMissingSourceOnlyRetiresRecord() async throws {
        let root = try root(), name = try abandoned(root)
        let path = root.appendingPathComponent(name); try PluginReleaseInstallTests.thaw(path); try FileManager.default.removeItem(at: path)
        let foreign = root.appendingPathComponent("unrelated"); try Data("untouched".utf8).write(to: foreign)
        let controller = try controller(root), value = try await entry(controller); XCTAssertEqual(value.state, .absent)
        try await controller.retireInstallationRecord(id: value.id, expectedDigest: value.digest)
        XCTAssertEqual(try? Data(contentsOf: foreign), Data("untouched".utf8))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testLegacyPrefixWithoutRecordHasNoMaintenanceAuthority() async throws {
        let root = try root(), directory = root.appendingPathComponent(".prepare-" + UUID().uuidString.lowercased())
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
        let file = directory.appendingPathComponent("legacy"); try Data("keep".utf8).write(to: file)
        let controller = try controller(root), listing = try await controller.installationResidues()
        XCTAssertTrue(listing.entries.isEmpty)
        let catalog = try await controller.installedVersions(); XCTAssertEqual(catalog.maintenanceCount, 1)
        XCTAssertEqual(try? Data(contentsOf: file), Data("keep".utf8))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testMalformedRecordIsVisibleAndNotSilentlyRetired() async throws {
        let root = try root(), id = UUID().uuidString.lowercased()
        _ = try journal(root) { try $0.reserve(id: id) }
        let file = root.appendingPathComponent(PluginInstallJournal.directoryName + "/" + id + ".json")
        let raw = Data("{\"schema\":1,\"schema\":1}".utf8); try raw.write(to: file)
        let controller = try controller(root), listing = try await controller.installationResidues()
        XCTAssertTrue(listing.entries.isEmpty); XCTAssertEqual(listing.issues.count, 1)
        XCTAssertEqual(try Data(contentsOf: file), raw)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testReservationLimitCountsMetadataWithoutDirectories() throws {
        let root = try root()
        try journal(root) { journal in
            for _ in 0..<64 { _ = try journal.reserve(id: UUID().uuidString.lowercased()) }
            XCTAssertThrowsError(try journal.reserve(id: UUID().uuidString.lowercased())) { XCTAssertEqual($0 as? PluginReleaseStage.Failure, .limit) }
        }
    }
    func testRecordCannotRedirectToAnotherDirectoryName() async throws {
        let root = try root(), name = try abandoned(root), controller = try controller(root), value = try await entry(controller)
        let other = root.appendingPathComponent(".prepare-" + UUID().uuidString.lowercased())
        try FileManager.default.createDirectory(at: other, withIntermediateDirectories: false)
        let file = other.appendingPathComponent("foreign"); try Data("keep".utf8).write(to: file)
        let recordFile = root.appendingPathComponent(PluginInstallJournal.directoryName + "/" + value.id + ".json")
        var object = try JSONSerialization.jsonObject(with: Data(contentsOf: recordFile)) as! [String: Any]
        var info = stat(); XCTAssertEqual(lstat(other.path, &info), 0)
        object["name"] = other.lastPathComponent
        object["directory"] = try JSONSerialization.jsonObject(with: JSONEncoder().encode(PluginInstallJournal.Identity(info)))
        try JSONSerialization.data(withJSONObject: object).write(to: recordFile)
        let listing = try await controller.installationResidues(); XCTAssertTrue(listing.entries.isEmpty); XCTAssertEqual(listing.issues.count, 1)
        do { _ = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest); XCTFail("redirected name adopted") } catch {}
        XCTAssertEqual(try? Data(contentsOf: file), Data("keep".utf8))
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appendingPathComponent(name).path))
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testLedgerDoesNotMoveAuthorityToReplacedRoot() async throws {
        let root = try root(), name = try abandoned(root), before = try controller(root), value = try await entry(before)
        let done = await before.close(); XCTAssertTrue(done)
        let old = root.appendingPathExtension("old")
        try FileManager.default.moveItem(at: root, to: old)
        addTeardownBlock { try PluginReleaseInstallTests.thaw(old) }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        for component in [name, PluginInstallJournal.directoryName] {
            try FileManager.default.moveItem(at: old.appendingPathComponent(component), to: root.appendingPathComponent(component))
        }
        let next = try controller(root), listing = try await next.installationResidues()
        XCTAssertTrue(listing.entries.isEmpty); XCTAssertEqual(listing.issues.count, 1)
        do { _ = try await next.previewInstallationResidue(id: value.id, expectedDigest: value.digest); XCTFail("old root authority transferred") } catch {}
        XCTAssertEqual(try? Data(contentsOf: root.appendingPathComponent(name + "/payload/file.txt")), Data("retained".utf8))
        let closed = await next.close(); XCTAssertTrue(closed)
    }
    func testStagingRemovalIntentResumesAfterPartialDeletion() async throws {
        let root = try root(), name = try abandoned(root), controller = try controller(root), value = try await entry(controller)
        enum Stop: Error { case point }
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let reference = try journal(root) { try $0.read(value.id).1 }
            let removal = try PluginReleaseRemoval(owner: owner, releaseID: value.digest, expectedRevision: nil,
                checkpoint: { if $0 == "after-unlink" { throw Stop.point } }, stage: reference)
            XCTAssertThrowsError(try removal.confirm(owner: owner))
        }
        let pending = try await controller.pendingVersionRemoval(), ticket = try XCTUnwrap(pending)
        let result = try await controller.resumeVersionRemoval(id: ticket.id)
        XCTAssertTrue(result.complete && result.durabilityConfirmed); XCTAssertNil(result.cleanupIssue)
        XCTAssertEqual(result.removed, 2); XCTAssertEqual(result.observedAbsent, 1)
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent(name).path))
        let after = try await controller.installationResidues(); XCTAssertTrue(after.entries.isEmpty)
        let closed = await controller.close(); XCTAssertTrue(closed)
    }
    func testRootPermissionsChangedInvalidateLiveStage() throws {
        let root = try root(), stage = try PluginReleaseStage(directory: root)
        let fd = try stage.catalogLease(); defer { Darwin.close(fd) }
        let journal = try XCTUnwrap(PluginInstallJournal(parent: fd))
        let reference = try XCTUnwrap(journal.listing().entries.first).reference
        XCTAssertEqual(chmod(root.path, 0o755), 0)
        XCTAssertThrowsError(try stage.write(Data("must reject".utf8), path: "payload/foreign"))
        XCTAssertThrowsError(try journal.read(reference.id))
        XCTAssertEqual(chmod(root.path, 0o700), 0)
        try stage.discard()
    }
}
