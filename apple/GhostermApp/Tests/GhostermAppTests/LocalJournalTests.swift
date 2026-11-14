import Darwin
import Foundation
import XCTest
import CryptoKit
import GhostermCore
@testable import GhostermAppModel

final class LocalJournalTests: XCTestCase {
    private func fixture() throws -> (URL, URL, ExtensionLocalJournal) {
        var project = URL(fileURLWithPath: #filePath); for _ in 0..<5 { project.deleteLastPathComponent() }
        let root = project.appendingPathComponent("tmp/local-journal-\(UUID().uuidString)")
        let base = root.appendingPathComponent("base"), ledger = root.appendingPathComponent("ledger")
        try FileManager.default.createDirectory(at: base, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try FileManager.default.removeItem(at: root) }
        return (base, ledger, try ExtensionLocalJournal(directory: ledger))
    }
    private func owner(_ base: URL, _ journal: ExtensionLocalJournal) throws -> ExtensionLocalTemporaryDirectory {
        let fd = open(base.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw ExtensionLocalTemporaryDirectory.Failure(errno, "fixture base unavailable") }
        defer { Darwin.close(fd) }
        return try .init(parentFD: fd, base: base.path, journal: journal)
    }
    private func reserve(_ base: URL, _ journal: ExtensionLocalJournal) throws -> ExtensionLocalJournal.Lease {
        let fd = open(base.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw ExtensionLocalTemporaryDirectory.Failure(errno, "fixture base unavailable") }
        defer { Darwin.close(fd) }
        return try journal.reserve(parent: fd, base: base.path, id: UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased())
    }
    private func entry(_ journal: ExtensionLocalJournal) throws -> ExtensionLocalJournalEntry {
        let entries = try journal.inspect()
        guard entries.count == 1, let entry = entries.first else { throw ExtensionLocalTemporaryDirectory.Failure(EIO, "expected one fixture reservation") }
        return entry
    }
    private func fails(_ body: () throws -> Void) -> Bool { do { try body(); return false } catch { return true } }
    private func finish(_ owner: ExtensionLocalTemporaryDirectory) throws {
        let end = ProcessInfo.processInfo.systemUptime + 2
        while ProcessInfo.processInfo.systemUptime < end { if try owner.closeStep() { return } }
        throw ExtensionLocalTemporaryDirectory.Failure(ETIMEDOUT, "fixture cleanup exceeded two seconds")
    }

    func testHeldLeaseCannotBeReleasedThroughAnotherManager() throws {
        let (base, path, journal) = try fixture(), lease = try reserve(base, journal)
        let second = try ExtensionLocalJournal(directory: path), value = try entry(second)
        let rejected = fails { try second.releaseMissing(id: value.id, snapshot: value.snapshot, confirmed: true) }
        withExtendedLifetime(lease) {
            XCTAssertTrue(value.held && value.location == .missing && rejected, "a live Native lease must keep its reservation")
        }
    }
    func testUnboundReservationPersistsAndNeedsExplicitConfirmation() throws {
        let (base, path, journal) = try fixture()
        do { let lease = try reserve(base, journal); withExtendedLifetime(lease) {} }
        let reopened = try ExtensionLocalJournal(directory: path), value = try entry(reopened)
        let rejected = fails { try reopened.releaseMissing(id: value.id, snapshot: value.snapshot, confirmed: false) }
        let before = try reopened.inspect().count
        if rejected { try reopened.releaseMissing(id: value.id, snapshot: value.snapshot, confirmed: true) }
        let empty = try reopened.inspect().isEmpty
        XCTAssertTrue(!value.held && rejected && before == 1 && empty,
                      "missing intent still occupies quota until an explicit exact-snapshot decision")
    }
    func testBudgetSurvivesOwnerDropAndRejectsBeforeMkdir() throws {
        let (base, path, journal) = try fixture()
        for _ in 0..<4 { let lease = try reserve(base, journal); withExtendedLifetime(lease) {} }
        let reopened = try ExtensionLocalJournal(directory: path)
        let rejected = fails { _ = try owner(base, reopened) }
        let entries = try? reopened.inspect(), names = try FileManager.default.contentsOfDirectory(atPath: base.path)
        XCTAssertTrue(rejected && entries?.count == 4 && entries?.reduce(UInt64(0), { $0 + $1.reservedBytes }) == ExtensionLocalJournal.budget && names.isEmpty,
                      "retained reservations must block the fifth namespace before mkdir")
    }
    func testRecordedNamespaceIsOnlyInspectedAfterOwnerDrop() throws {
        let (base, path, journal) = try fixture()
        var owned: ExtensionLocalTemporaryDirectory? = try owner(base, journal)
        let directory = try XCTUnwrap(owned?.directory), file = URL(fileURLWithPath: directory).appendingPathComponent("keep")
        try Data("KEEP".utf8).write(to: file); owned = nil
        let reopened = try ExtensionLocalJournal(directory: path), value = try entry(reopened)
        let rejected = fails { try reopened.releaseMissing(id: value.id, snapshot: value.snapshot, confirmed: true) }
        let bytes = try Data(contentsOf: file)
        XCTAssertTrue(!value.held && value.location == .matching && rejected && bytes == Data("KEEP".utf8),
                      "reopening metadata must not delete or adopt a remaining namespace")
    }
    func testSuccessfulCleanupRetiresItsReservation() throws {
        let (base, _, journal) = try fixture(), owned = try owner(base, journal)
        let before = try journal.inspect()
        try finish(owned)
        let empty = try journal.inspect().isEmpty
        XCTAssertTrue(before.count == 1 && before[0].held && empty && !FileManager.default.fileExists(atPath: owned.directory),
                      "only completed namespace cleanup retires the persistent reservation")
    }
    func testJournalFailureAfterRmdirKeepsRetryableOwner() throws {
        let (base, path, journal) = try fixture(), owned = try owner(base, journal), value = try entry(journal)
        let record = path.appendingPathComponent(value.id + ".json"), saved = base.appendingPathComponent("saved-record")
        try FileManager.default.moveItem(at: record, to: saved)
        try Data("foreign".utf8).write(to: record, options: .atomic)
        let rejected = fails { try finish(owned) }, retained = owned.observation.phase == .retained
        let absent = !FileManager.default.fileExists(atPath: owned.directory)
        let foreign = try Data(contentsOf: record)
        try FileManager.default.removeItem(at: record); try FileManager.default.moveItem(at: saved, to: record)
        try finish(owned)
        let empty = try journal.inspect().isEmpty
        XCTAssertTrue(rejected && retained && absent && foreign == Data("foreign".utf8) && owned.observation.phase == .removed && empty,
                      "rmdir followed by journal failure must retain a finite retry path")
    }
    func testChangedBaseCannotReleaseAnApparentlyMissingDirectory() throws {
        let (base, _, journal) = try fixture()
        do { let lease = try reserve(base, journal); withExtendedLifetime(lease) {} }
        let old = try entry(journal), moved = base.deletingLastPathComponent().appendingPathComponent("old-base")
        try FileManager.default.moveItem(at: base, to: moved)
        try FileManager.default.createDirectory(at: base, withIntermediateDirectories: false)
        let value = try entry(journal), rejected = fails { try journal.releaseMissing(id: old.id, snapshot: old.snapshot, confirmed: true) }
        let count = try journal.inspect().count
        XCTAssertTrue(value.location == .changed && rejected && count == 1,
                      "ENOENT under a replacement parent must not release quota")
    }
    func testInPlaceRecordChangeRetainsOwnerUntilRestored() throws {
        let (base, path, journal) = try fixture(), owned = try owner(base, journal), value = try entry(journal)
        let record = path.appendingPathComponent(value.id + ".json"), original = try Data(contentsOf: record)
        let text = try XCTUnwrap(String(data: original, encoding: .utf8))
        let changed = Data(text.replacingOccurrences(of: "268435456", with: "268435457").utf8)
        guard changed != original, changed.count == original.count else { throw ExtensionLocalTemporaryDirectory.Failure(EIO, "fixture needs an equal-size record change") }
        try changed.write(to: record)
        let rejected = fails { try finish(owned) }, retained = owned.observation.phase == .retained
        if rejected, FileManager.default.fileExists(atPath: record.path) {
            try original.write(to: record); try finish(owned)
        }
        XCTAssertTrue(rejected && retained && owned.observation.phase == .removed,
                      "same-inode equal-size metadata changes must retain the cleanup owner")
    }
    func testNamespaceReplacementCannotBeForgottenOrDeleted() throws {
        let (base, _, journal) = try fixture()
        var owned: ExtensionLocalTemporaryDirectory? = try owner(base, journal)
        let directory = try XCTUnwrap(owned?.directory); owned = nil
        try FileManager.default.moveItem(atPath: directory, toPath: directory + "-old")
        try FileManager.default.createDirectory(atPath: directory, withIntermediateDirectories: false)
        let file = URL(fileURLWithPath: directory).appendingPathComponent("keep"); try Data("KEEP".utf8).write(to: file)
        let value = try entry(journal), rejected = fails { try journal.releaseMissing(id: value.id, snapshot: value.snapshot, confirmed: true) }
        let bytes = try Data(contentsOf: file)
        XCTAssertTrue(value.location == .changed && rejected && bytes == Data("KEEP".utf8),
                      "inspection never adopts a replacement namespace")
    }
    func testChangedSnapshotRequiresAnotherDecision() throws {
        let (base, path, journal) = try fixture()
        do { let lease = try reserve(base, journal); withExtendedLifetime(lease) {} }
        let value = try entry(journal), record = path.appendingPathComponent(value.id + ".json")
        let object = try JSONSerialization.jsonObject(with: Data(contentsOf: record))
        try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys]).write(to: record)
        let rejected = fails { try journal.releaseMissing(id: value.id, snapshot: value.snapshot, confirmed: true) }
        let count = try journal.inspect().count
        XCTAssertTrue(rejected && count == 1, "stale metadata confirmation must never retire a changed record")
    }
    func testUnknownAndIncompleteMetadataBlockAdmission() throws {
        let (base, path, journal) = try fixture(), file = path.appendingPathComponent("pending")
        try Data("interrupted write".utf8).write(to: file)
        let inspection = fails { _ = try journal.inspect() }, admission = fails { _ = try owner(base, journal) }
        let empty = try FileManager.default.contentsOfDirectory(atPath: base.path).isEmpty, bytes = try Data(contentsOf: file)
        XCTAssertTrue(inspection && admission && empty && bytes == Data("interrupted write".utf8),
                      "unknown transaction data must be preserved and must not free budget")
    }
    func testFormatChangesAreNotSilentlyAdopted() throws {
        let (base, path, journal) = try fixture()
        try Data("different format".utf8).write(to: path.appendingPathComponent("format"))
        let existing = fails { _ = try journal.inspect() }, reopened = fails { _ = try ExtensionLocalJournal(directory: path) }
        let admission = fails { _ = try owner(base, journal) }
        XCTAssertTrue(existing && reopened && admission, "a changed format must reject both existing and reopened owners")
    }
    func testJournalSymlinksAndForeignPermissionsAreRejected() throws {
        let (base, path, _) = try fixture(), link = base.appendingPathComponent("link")
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: path)
        let symlink = fails { _ = try ExtensionLocalJournal(directory: link) }
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: path.path)
        let permissions = fails { _ = try ExtensionLocalJournal(directory: path) }
        XCTAssertTrue(symlink && permissions, "metadata storage cannot follow a link or adopt public permissions")
    }

    @MainActor func testAppModelReopensAndRequiresExplicitRelease() async throws {
        guard ExtensionRuntime.supportStatus == 0 else { throw XCTSkip("explicit JS-enabled Native AppModel fixture required") }
        let (base, path, _) = try fixture(), resources = path.deletingLastPathComponent().appendingPathComponent("resources")
        try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let journal = try ExtensionLocalJournal(directory: resources.appendingPathComponent("NativeLocalRuns"))
        do { let lease = try reserve(base, journal); withExtendedLifetime(lease) {} }
        let model = isolatedAppModel(pluginResourceDirectory: resources)
        await model.inspectLocalReservations()
        let value = try XCTUnwrap(model.localReservationEntries.first)
        await model.releaseMissingLocalReservation(value, confirmed: false)
        let denied = model.localReservationIssue != nil && model.localReservationEntries.count == 1
        await model.releaseMissingLocalReservation(value, confirmed: true)
        XCTAssertTrue(denied && model.localReservationIssue == nil && model.localReservationEntries.isEmpty && !model.localReservationBusy,
                      "Native AppModel must not treat inspection or an unconfirmed action as quota release")
    }

    @MainActor func testAppModelStaleDecisionDoesNotReleaseChangedMetadata() async throws {
        guard ExtensionRuntime.supportStatus == 0 else { throw XCTSkip("explicit JS-enabled Native AppModel fixture required") }
        let (base, path, _) = try fixture(), resources = path.deletingLastPathComponent().appendingPathComponent("resources")
        try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let journalPath = resources.appendingPathComponent("NativeLocalRuns"), journal = try ExtensionLocalJournal(directory: journalPath)
        do { let lease = try reserve(base, journal); withExtendedLifetime(lease) {} }
        let model = isolatedAppModel(pluginResourceDirectory: resources)
        await model.inspectLocalReservations()
        let value = try XCTUnwrap(model.localReservationEntries.first), record = journalPath.appendingPathComponent(value.id + ".json")
        let object = try JSONSerialization.jsonObject(with: Data(contentsOf: record))
        try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys]).write(to: record)
        await model.releaseMissingLocalReservation(value, confirmed: true)
        let current = try journal.inspect()
        XCTAssertTrue(model.localReservationIssue != nil && current.count == 1 && current[0].snapshot != value.snapshot,
                      "the confirmation must retain the selected snapshot across the Native worker boundary")
    }

    @MainActor func testControllerCannotStartWhenRecoveredBudgetIsFull() async throws {
        guard ExtensionRuntime.supportStatus == 0 else { throw XCTSkip("explicit JS-enabled Native AppModel fixture required") }
        let (base, path, _) = try fixture(), resources = path.deletingLastPathComponent().appendingPathComponent("resources")
        try FileManager.default.createDirectory(at: resources, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        let journal = try ExtensionLocalJournal(directory: resources.appendingPathComponent("NativeLocalRuns"))
        for _ in 0..<4 { let lease = try reserve(base, journal); withExtendedLifetime(lease) {} }
        let model = isolatedAppModel(pluginResourceDirectory: resources), controller = try XCTUnwrap(model.extensionController)
        var project = URL(fileURLWithPath: #filePath); for _ in 0..<5 { project.deleteLastPathComponent() }
        let helper = ProcessInfo.processInfo.environment["GHOSTERM_PROCESS_CHANNEL_CHILD"] ?? project.appendingPathComponent("core/build/test_process_channel_child").path
        let digest = SHA256.hash(data: try Data(contentsOf: URL(fileURLWithPath: helper))).map { String(format: "%02x", $0) }.joined()
        var rejected = false
        do {
            _ = try await controller.openLocalChannel(executable: helper, arguments: [helper, "hold-silent"],
                launch: .init(executableSHA256: digest, workingDirectory: base.path, temporaryDirectory: base.path, authorized: true))
        } catch { rejected = true }
        try? await controller.stopSSHChannel(); _ = await controller.close()
        let count = try journal.inspect().count, names = try FileManager.default.contentsOfDirectory(atPath: base.path)
        XCTAssertTrue(rejected && count == 4 && names.isEmpty, "Native launch must charge recovered reservations before mkdir or spawn")
    }
}
