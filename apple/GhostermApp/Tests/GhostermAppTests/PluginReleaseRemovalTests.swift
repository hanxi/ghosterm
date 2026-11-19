import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

@MainActor
final class PluginReleaseRemovalTests: XCTestCase {
    private let releaseID = String(repeating: "a", count: 64)
    private let pluginID = "org.ghosterm.preview"
    private enum Interrupted: Error { case stop }
    private func tree() throws -> URL {
        let root = sessionTestDirectory().appendingPathComponent("removal-" + UUID().uuidString)
        let version = root.appendingPathComponent(releaseID)
        let nested = version.appendingPathComponent("payload/nested")
        try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try Data("first".utf8).write(to: nested.appendingPathComponent("data.txt"))
        try Data("second".utf8).write(to: version.appendingPathComponent("payload/keep.txt"))
        for path in ["payload/nested/data.txt", "payload/keep.txt"] { XCTAssertEqual(chmod(version.appendingPathComponent(path).path, 0o400), 0) }
        for path in ["payload/nested", "payload", ""] { XCTAssertEqual(chmod(version.appendingPathComponent(path).path, 0o500), 0) }
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        return root
    }
    private func owner(_ root: URL) throws -> PluginReleaseCatalog { try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true)) }
    private func preview(_ root: URL, revision: String? = nil, sync: @escaping (Int32) -> Int32 = { fsync($0) },
                         checkpoint: @escaping (String) throws -> Void = { _ in }) throws -> PluginReleaseRemoval {
        try PluginReleaseRemoval(owner: owner(root), releaseID: releaseID, expectedRevision: revision, synchronize: sync, checkpoint: checkpoint)
    }
    private func choice(_ root: URL, enabled: Bool) throws -> String? {
        let owner = try owner(root), current = try owner.selection()
        var next = current
        next.choices[pluginID] = .init(releaseID: releaseID, enabled: enabled, generation: UUID().uuidString)
        return try owner.writeSelection(next, expected: current.revision).0.revision
    }
    private func journal(_ root: URL) -> URL { root.appendingPathComponent(PluginReleaseCatalog.removalName) }
    private func detached(_ root: URL) throws -> URL {
        let raw = try Data(contentsOf: journal(root)), line = raw.split(separator: 10).first!
        let header = try JSONSerialization.jsonObject(with: Data(line)) as! [String: Any]
        return root.appendingPathComponent(header["tombstone"] as! String)
    }
    private func interrupted(_ root: URL, at point: String) throws {
        var preview: PluginReleaseRemoval? = try self.preview(root, checkpoint: { if $0 == point { throw Interrupted.stop } })
        XCTAssertThrowsError(try preview!.confirm(owner: owner(root))) { XCTAssertTrue($0 is Interrupted) }
        preview = nil
    }

    func testPreviewCancelOnlyReleasesExclusiveReference() throws {
        let root = try tree()
        var preview: PluginReleaseRemoval? = try self.preview(root)
        XCTAssertEqual(preview?.members.map(\.path), ["payload", "payload/keep.txt", "payload/nested", "payload/nested/data.txt"])
        XCTAssertEqual(preview?.bytes, 11)
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(root).path))
        let other = try owner(root).openVersion(releaseID)
        XCTAssertThrowsError(try other.pin())
        preview = nil
        XCTAssertNoThrow(try other.pin())
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appendingPathComponent(releaseID).path))
    }
    func testEnabledChoiceRequiresExplicitDisable() throws {
        let root = try tree(), revision = try choice(root, enabled: true)
        XCTAssertThrowsError(try preview(root, revision: revision)) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .enabled) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(root).path))
    }
    func testSharedRunLeaseBlocksRemovalAcrossOwners() throws {
        let root = try tree(), lease = try owner(root).openVersion(releaseID)
        try lease.pin()
        XCTAssertThrowsError(try preview(root)) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .occupied) }
        let observer = try owner(root).openVersion(releaseID)
        XCTAssertEqual(observer.useObservation(), true)
        XCTAssertThrowsError(try preview(root))
    }
    func testConfirmedRemovalPreservesDisabledSelection() throws {
        let root = try tree(), revision = try choice(root, enabled: false)
        let preview = try self.preview(root, revision: revision)
        let result = try preview.confirm(owner: owner(root))
        XCTAssertEqual(result.removed, 5); XCTAssertEqual(result.observedAbsent, 0)
        XCTAssertTrue(result.complete && result.durabilityConfirmed)
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent(releaseID).path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(root).path))
        let current = try owner(root).selection()
        XCTAssertEqual(current.revision, revision); XCTAssertEqual(current.choices[pluginID]?.enabled, false)
    }
    func testChangedChoiceRejectsStaleConfirmation() throws {
        let root = try tree(), preview = try self.preview(root)
        _ = try choice(root, enabled: false)
        XCTAssertThrowsError(try preview.confirm(owner: owner(root))) { XCTAssertEqual($0 as? PluginReleaseCatalog.Failure, .conflict) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(root).path))
    }
    func testChangedFileRejectsConfirmation() throws {
        let root = try tree(), preview = try self.preview(root)
        let path = root.appendingPathComponent(releaseID + "/payload/keep.txt")
        XCTAssertEqual(chmod(path.path, 0o600), 0); try Data("other!".utf8).write(to: path)
        XCTAssertEqual(chmod(path.path, 0o400), 0)
        XCTAssertThrowsError(try preview.confirm(owner: owner(root))) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .changed) }
        XCTAssertEqual(try Data(contentsOf: path), Data("other!".utf8))
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(root).path), "changed preview must not become a durable deletion intent")
    }
    func testAddedForeignFileIsNotDeleted() throws {
        let root = try tree(), preview = try self.preview(root), parent = root.appendingPathComponent(releaseID + "/payload")
        XCTAssertEqual(chmod(parent.path, 0o700), 0)
        let foreign = parent.appendingPathComponent("foreign"); try Data("keep me".utf8).write(to: foreign)
        XCTAssertEqual(chmod(parent.path, 0o500), 0)
        XCTAssertThrowsError(try preview.confirm(owner: owner(root)))
        XCTAssertEqual(try Data(contentsOf: foreign), Data("keep me".utf8))
    }
    func testLinksAndSpecialFilesRejectWholeTree() throws {
        for kind in ["symlink", "hardlink", "fifo"] {
            let root = try tree(), parent = root.appendingPathComponent(releaseID + "/payload"), target = parent.appendingPathComponent("foreign")
            XCTAssertEqual(chmod(parent.path, 0o700), 0)
            if kind == "symlink" { XCTAssertEqual(symlink("keep.txt", target.path), 0) }
            else if kind == "hardlink" { XCTAssertEqual(link(parent.appendingPathComponent("keep.txt").path, target.path), 0) }
            else { XCTAssertEqual(mkfifo(target.path, 0o600), 0) }
            XCTAssertEqual(chmod(parent.path, 0o500), 0)
            XCTAssertThrowsError(try preview(root), kind)
        }
    }
    func testRootReplacementRejectsConfirmation() throws {
        let root = try tree(), preview = try self.preview(root), moved = root.appendingPathExtension("old")
        try FileManager.default.moveItem(at: root, to: moved)
        addTeardownBlock { try PluginReleaseInstallTests.thaw(moved) }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        XCTAssertThrowsError(try preview.confirm(owner: owner(root)))
        XCTAssertTrue(FileManager.default.fileExists(atPath: moved.appendingPathComponent(releaseID).path))
    }
    func testDurableIntentBeforeRenameCanResumeExplicitly() throws {
        let root = try tree(); try interrupted(root, at: "after-intent")
        guard FileManager.default.fileExists(atPath: journal(root).path) else { XCTFail("intent must exist before detaching"); return }
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appendingPathComponent(releaseID).path))
        XCTAssertThrowsError(try PluginReleaseStage(directory: root))
        XCTAssertThrowsError(try preview(root)) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .pending) }
        let result = try PluginReleaseRemoval.resume(owner: owner(root))
        XCTAssertTrue(result.complete); XCTAssertEqual(result.removed, 5)
    }
    func testDetachedRecoveryPreservesNewOriginalSlot() throws {
        let root = try tree(); try interrupted(root, at: "after-detach")
        let new = root.appendingPathComponent(releaseID)
        try FileManager.default.createDirectory(at: new, withIntermediateDirectories: false)
        let file = new.appendingPathComponent("replacement"); try Data("new".utf8).write(to: file)
        let result = try PluginReleaseRemoval.resume(owner: owner(root))
        XCTAssertTrue(result.complete); XCTAssertEqual(try Data(contentsOf: file), Data("new".utf8))
    }
    func testCancelPendingOnlyBeforeDetachAndForExactOperation() throws {
        let root = try tree(); try interrupted(root, at: "after-intent")
        let pending = try XCTUnwrap(PluginReleaseRemoval.pendingPreview(owner: owner(root)))
        XCTAssertThrowsError(try PluginReleaseRemoval.cancelPending(owner: owner(root), id: UUID().uuidString))
        _ = try choice(root, enabled: false) // An unrelated CAS change cannot trap an unstarted intent forever.
        XCTAssertTrue(try PluginReleaseRemoval.cancelPending(owner: owner(root), id: pending.0))
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appendingPathComponent(releaseID).path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(root).path))
        let current = try owner(root).selection()
        var removal: PluginReleaseRemoval? = try preview(root, revision: current.revision, checkpoint: { if $0 == "after-detach" { throw Interrupted.stop } })
        XCTAssertThrowsError(try removal!.confirm(owner: owner(root))); removal = nil
        let detached = try XCTUnwrap(PluginReleaseRemoval.pendingPreview(owner: owner(root)))
        XCTAssertThrowsError(try PluginReleaseRemoval.cancelPending(owner: owner(root), id: detached.0)) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .started) }
        let result = try PluginReleaseRemoval.resume(owner: owner(root)); XCTAssertTrue(result.complete)
    }
    func testUnrelatedTombstoneDoesNotProveDeletionStarted() throws {
        let root = try tree(); try interrupted(root, at: "after-intent")
        let value = try XCTUnwrap(PluginReleaseRemoval.pendingPreview(owner: owner(root)))
        let line = try XCTUnwrap(Data(contentsOf: journal(root)).split(separator: 10).first)
        let header = try JSONSerialization.jsonObject(with: Data(line)) as! [String: Any]
        let foreign = root.appendingPathComponent(header["tombstone"] as! String)
        try FileManager.default.createDirectory(at: foreign, withIntermediateDirectories: false)
        XCTAssertThrowsError(try PluginReleaseRemoval.cancelPending(owner: owner(root), id: value.0)) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .changed) }
        XCTAssertTrue(FileManager.default.fileExists(atPath: foreign.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appendingPathComponent(releaseID).path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: journal(root).path))
        try FileManager.default.removeItem(at: foreign) // This test's own empty collision fixture only.
        XCTAssertTrue(try PluginReleaseRemoval.cancelPending(owner: owner(root), id: value.0))
    }
    func testPartialUnlinkRecoverySeparatesObservedAbsence() throws {
        let root = try tree(); try interrupted(root, at: "after-unlink")
        let result = try PluginReleaseRemoval.resume(owner: owner(root))
        XCTAssertTrue(result.complete); XCTAssertEqual(result.observedAbsent, 1); XCTAssertEqual(result.removed, 4)
    }
    func testRecoveryRejectsReplacedRemainingFile() throws {
        let root = try tree(); try interrupted(root, at: "after-detach")
        let directory = try detached(root), file = directory.appendingPathComponent("payload/keep.txt")
        XCTAssertEqual(chmod(file.path, 0o600), 0); try Data("foreign".utf8).write(to: file)
        XCTAssertEqual(chmod(file.path, 0o400), 0)
        XCTAssertThrowsError(try PluginReleaseRemoval.resume(owner: owner(root))) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .changed) }
        XCTAssertEqual(try? Data(contentsOf: file), Data("foreign".utf8))
        XCTAssertTrue(FileManager.default.fileExists(atPath: directory.appendingPathComponent("payload/nested/data.txt").path), "validate the complete remaining tree before the first unlink")
        XCTAssertTrue(FileManager.default.fileExists(atPath: journal(root).path))
    }
    func testRecoveryRejectsUnknownAddedMemberBeforeDeletingAnything() throws {
        let root = try tree(); try interrupted(root, at: "after-detach")
        let parent = try detached(root).appendingPathComponent("payload")
        XCTAssertEqual(chmod(parent.path, 0o700), 0)
        let foreign = parent.appendingPathComponent("foreign"); try Data("keep".utf8).write(to: foreign)
        XCTAssertThrowsError(try PluginReleaseRemoval.resume(owner: owner(root)))
        XCTAssertEqual(try Data(contentsOf: foreign), Data("keep".utf8))
        XCTAssertTrue(FileManager.default.fileExists(atPath: parent.appendingPathComponent("nested/data.txt").path))
    }
    func testAlreadyAbsentIsAnObservationAfterInterruption() throws {
        let root = try tree(); try interrupted(root, at: "before-clear")
        let result = try PluginReleaseRemoval.resume(owner: owner(root))
        XCTAssertTrue(result.complete); XCTAssertEqual(result.removed, 0); XCTAssertEqual(result.observedAbsent, 5)
    }
    func testUnsyncedIntentRetainsVersionAndRecord() throws {
        let root = try tree(), preview = try self.preview(root, sync: { _ in -1 })
        let result = try preview.confirm(owner: owner(root))
        XCTAssertFalse(result.complete); XCTAssertFalse(result.durabilityConfirmed); XCTAssertEqual(result.removed, 0)
        XCTAssertTrue(FileManager.default.fileExists(atPath: root.appendingPathComponent(releaseID).path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: journal(root).path))
    }
    func testUnsyncedDetachNeverUnlinksBeforeAcknowledgment() throws {
        let root = try tree()
        var calls = 0
        var preview: PluginReleaseRemoval? = try self.preview(root, sync: { fd in calls += 1; return calls == 2 ? -1 : fsync(fd) })
        let result = try preview!.confirm(owner: owner(root)); preview = nil
        XCTAssertFalse(result.complete); XCTAssertFalse(result.durabilityConfirmed); XCTAssertEqual(result.removed, 0)
        let file = try detached(root).appendingPathComponent("payload/nested/data.txt")
        XCTAssertEqual(try Data(contentsOf: file), Data("first".utf8))
        let resumed = try PluginReleaseRemoval.resume(owner: owner(root)); XCTAssertTrue(resumed.complete); XCTAssertEqual(resumed.removed, 5)
    }
    func testFinalSyncFailureReportsCompletedEffectAndUnknownDurability() throws {
        let root = try tree()
        var calls = 0
        let preview = try self.preview(root, sync: { fd in calls += 1; return calls == 4 ? -1 : fsync(fd) })
        let result = try preview.confirm(owner: owner(root))
        XCTAssertTrue(result.complete); XCTAssertFalse(result.durabilityConfirmed); XCTAssertEqual(result.removed, 5)
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(root).path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent(releaseID).path))
    }
    func testRemovalJournalRejectsNoncanonicalSchemaWithoutUnlinking() throws {
        let root = try tree()
        try interrupted(root, at: "after-detach")
        let record = journal(root)
        let original = try Data(contentsOf: record)
        let text = try XCTUnwrap(String(data: original, encoding: .utf8))
        XCTAssertTrue(text.contains("\"schema\":1"))
        let protectedFile = try detached(root).appendingPathComponent("payload/nested/data.txt")
        let protectedBytes = try Data(contentsOf: protectedFile)

        for token in ["1.0", "1e0", "null", "\"1\"", "4"] {
            let malformed = Data(text.replacingOccurrences(of: "\"schema\":1", with: "\"schema\":\(token)").utf8)
            try malformed.write(to: record)
            do {
                _ = try PluginReleaseRemoval.pendingPreview(owner: owner(root))
                return XCTFail("invalid schema must be rejected before preview: \(token)")
            } catch PluginReleaseRemoval.Failure.invalid {}
            XCTAssertThrowsError(try PluginReleaseRemoval.resume(owner: owner(root)))
            XCTAssertEqual(try Data(contentsOf: protectedFile), protectedBytes)
            XCTAssertEqual(try Data(contentsOf: record), malformed)
        }
        try original.write(to: record)
        XCTAssertNotNil(try PluginReleaseRemoval.pendingPreview(owner: owner(root)))
    }

    func testDuplicateAndSymlinkJournalFailClosed() throws {
        let root = try tree(); try interrupted(root, at: "after-detach")
        let original = try Data(contentsOf: journal(root))
        let duplicate = Data("{\"schema\":1,\"schema\":1}\n".utf8)
        try duplicate.write(to: journal(root))
        XCTAssertThrowsError(try PluginReleaseRemoval.resume(owner: owner(root)))
        try FileManager.default.removeItem(at: journal(root))
        let elsewhere = root.appendingPathComponent("public-journal"); try original.write(to: elsewhere)
        XCTAssertEqual(chmod(elsewhere.path, 0o600), 0)
        XCTAssertEqual(symlink(elsewhere.lastPathComponent, journal(root).path), 0)
        XCTAssertThrowsError(try PluginReleaseRemoval.resume(owner: owner(root)))
        XCTAssertEqual(try Data(contentsOf: elsewhere), original)
    }
    func testMemberAndFileByteLimitsRejectBeforeConfirmation() throws {
        let oversized = try tree(), payload = oversized.appendingPathComponent(releaseID + "/payload")
        XCTAssertEqual(chmod(payload.path, 0o700), 0)
        let fd = open(payload.appendingPathComponent("large").path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o400)
        guard fd >= 0 else { XCTFail("owned sparse fixture creation failed"); return }
        XCTAssertEqual(ftruncate(fd, 16777217), 0); XCTAssertEqual(Darwin.close(fd), 0)
        XCTAssertEqual(chmod(payload.path, 0o500), 0)
        XCTAssertThrowsError(try preview(oversized))
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(oversized).path))
        let crowded = try tree(), parent = crowded.appendingPathComponent(releaseID + "/payload")
        XCTAssertEqual(chmod(parent.path, 0o700), 0)
        for index in 0..<4093 { try Data().write(to: parent.appendingPathComponent("member-\(index)")) }
        XCTAssertEqual(chmod(parent.path, 0o500), 0)
        XCTAssertThrowsError(try preview(crowded), "4097 members must not become a deletion preview")
        XCTAssertFalse(FileManager.default.fileExists(atPath: journal(crowded).path))
    }
}
