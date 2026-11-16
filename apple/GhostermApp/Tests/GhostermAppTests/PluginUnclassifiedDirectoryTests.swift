import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

@MainActor
final class PluginUnclassifiedDirectoryTests: XCTestCase {
    private func root() throws -> URL {
        let root = sessionTestDirectory().appendingPathComponent("unclassified-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try PluginReleaseInstallTests.thaw(root) }
        return root
    }
    private func directory(_ root: URL, name: String = "unrecognized directory") throws -> URL {
        let directory = root.appendingPathComponent(name)
        try FileManager.default.createDirectory(at: directory.appendingPathComponent("nested"), withIntermediateDirectories: true)
        try Data("retained".utf8).write(to: directory.appendingPathComponent("nested/file.txt"))
        return directory
    }
    private func controller(_ root: URL) throws -> ExtensionController {
        let value = try ExtensionController(profile: .init(side: .client, sdkVersion: "1.0.0", protocols: [1], runtimes: [], capabilities: []), installationDirectory: root)
        addTeardownBlock { let closed = await value.close(); XCTAssertTrue(closed) }
        return value
    }
    private func entry(_ controller: ExtensionController, name: String) async throws -> ExtensionController.UnclassifiedEntry {
        let entries = try await controller.unclassifiedEntries()
        return try XCTUnwrap(entries.first { $0.name == name })
    }
    private func preview(_ controller: ExtensionController, name: String) async throws -> ExtensionController.RemovalPreview {
        let value = try await entry(controller, name: name)
        return try await controller.previewUnclassifiedDirectory(name: value.name, expectedIdentity: value.identity)
    }
    func testNamesDoNotEstablishInstallationOrigin() async throws {
        let root = try root(), legacy = ".prepare-" + UUID().uuidString.lowercased()
        _ = try directory(root, name: legacy); _ = try directory(root, name: "ordinary folder")
        let controller = try controller(root), entries = try await controller.unclassifiedEntries()
        XCTAssertEqual(Set(entries.map(\.name)), [legacy, "ordinary folder"])
        XCTAssertTrue(entries.allSatisfy(\.canPreview))
        let preview = try await self.preview(controller, name: legacy)
        XCTAssertTrue(preview.unknownOrigin); XCTAssertEqual(preview.files.map(\.path), ["nested", "nested/file.txt"])
        let records = try await controller.installationResidues(); XCTAssertTrue(records.entries.isEmpty)
        await controller.cancelVersionRemoval(id: preview.id)
    }
    func testSeparateAcknowledgementIsRequiredBeforeAnyDeletionIntent() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let preview = try await preview(controller, name: path.lastPathComponent)
        do { _ = try await controller.confirmVersionRemoval(id: preview.id); XCTFail("unknown origin was implicitly accepted") }
        catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .confirmation) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent(PluginReleaseCatalog.removalName).path))
        XCTAssertEqual(try? Data(contentsOf: path.appendingPathComponent("nested/file.txt")), Data("retained".utf8))
        let result = try await controller.confirmVersionRemoval(id: preview.id, acknowledgedUnknown: true)
        XCTAssertTrue(result.complete && result.durabilityConfirmed); XCTAssertNil(result.cleanupIssue); XCTAssertEqual(result.removed, 3)
        XCTAssertFalse(FileManager.default.fileExists(atPath: path.path))
    }
    func testNativeRemovalAlsoRequiresSeparateAcknowledgement() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let value = try await entry(controller, name: path.lastPathComponent)
        let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
        let removal = try PluginReleaseRemoval(owner: owner, releaseID: PluginReleaseCatalog.sha(Data(value.name.utf8)), expectedRevision: nil,
            unknownName: value.name, expectedIdentity: value.identity)
        XCTAssertThrowsError(try removal.confirm(owner: owner)) { XCTAssertEqual($0 as? PluginReleaseRemoval.Failure, .confirmation) }
        XCTAssertFalse(FileManager.default.fileExists(atPath: root.appendingPathComponent(PluginReleaseCatalog.removalName).path))
        XCTAssertEqual(try? Data(contentsOf: path.appendingPathComponent("nested/file.txt")), Data("retained".utf8))
    }
    func testManualJournalCannotRedirectOriginalName() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root), value = try await entry(controller, name: path.lastPathComponent)
        enum Stop: Error { case point }
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let removal = try PluginReleaseRemoval(owner: owner, releaseID: PluginReleaseCatalog.sha(Data(value.name.utf8)), expectedRevision: nil,
                checkpoint: { if $0 == "after-intent" { throw Stop.point } }, unknownName: value.name, expectedIdentity: value.identity)
            XCTAssertThrowsError(try removal.confirm(owner: owner, acknowledgedUnknown: true))
        }
        let journal = root.appendingPathComponent(PluginReleaseCatalog.removalName)
        var lines = try String(contentsOf: journal, encoding: .utf8).split(separator: "\n").map(String.init)
        var header = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(lines[0].utf8)) as? [String: Any])
        let other = try directory(root, name: "another object")
        header["unknownName"] = other.lastPathComponent
        lines[0] = String(decoding: try JSONSerialization.data(withJSONObject: header, options: [.sortedKeys]), as: UTF8.self)
        try Data((lines.joined(separator: "\n") + "\n").utf8).write(to: journal)
        do { _ = try await controller.pendingVersionRemoval(); XCTFail("manual journal redirected the reviewed name") }
        catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .invalid) }
        XCTAssertTrue(FileManager.default.fileExists(atPath: path.path)); XCTAssertTrue(FileManager.default.fileExists(atPath: other.path))
    }
    func testCancelPreservesTreeAndReleasesReference() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let preview = try await preview(controller, name: path.lastPathComponent)
        await controller.cancelVersionRemoval(id: preview.id)
        let next = try await self.preview(controller, name: path.lastPathComponent)
        await controller.cancelVersionRemoval(id: next.id)
        XCTAssertEqual(try? Data(contentsOf: path.appendingPathComponent("nested/file.txt")), Data("retained".utf8))
    }
    func testReservedNamesAndPathTraversalCannotBeReviewed() async throws {
        let root = try root(), controller = try controller(root)
        for name in ["", ".", "..", "../outside", "a/b", "a\\b", "a\0b", PluginReleaseCatalog.selectionName, PluginReleaseCatalog.removalName, PluginInstallJournal.directoryName] {
            XCTAssertFalse(PluginUnclassifiedDirectories.allowedName(name))
            do { _ = try await controller.previewUnclassifiedDirectory(name: name, expectedIdentity: String(repeating: "a", count: 64)); XCTFail("reserved/path name accepted: \(name)") } catch {}
        }
    }
    func testReplacedListingCannotAuthorizeAnotherObject() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let before = try await entry(controller, name: path.lastPathComponent)
        let moved = root.appendingPathComponent("original"); try FileManager.default.moveItem(at: path, to: moved)
        _ = try directory(root)
        do { _ = try await controller.previewUnclassifiedDirectory(name: before.name, expectedIdentity: before.identity); XCTFail("stale listing adopted replacement") }
        catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .changed) }
        XCTAssertTrue(FileManager.default.fileExists(atPath: moved.appendingPathComponent("nested/file.txt").path))
    }
    func testAddedMemberAfterPreviewRejectsConfirmation() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let preview = try await preview(controller, name: path.lastPathComponent)
        let file = path.appendingPathComponent("new"); try Data("foreign".utf8).write(to: file)
        do { _ = try await controller.confirmVersionRemoval(id: preview.id, acknowledgedUnknown: true); XCTFail("new member deleted") } catch {}
        XCTAssertEqual(try? Data(contentsOf: file), Data("foreign".utf8))
    }
    func testChangedNestedBytesRejectConfirmation() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let preview = try await preview(controller, name: path.lastPathComponent)
        let file = path.appendingPathComponent("nested/file.txt"); try Data("different".utf8).write(to: file)
        do { _ = try await controller.confirmVersionRemoval(id: preview.id, acknowledgedUnknown: true); XCTFail("changed nested content deleted") } catch {}
        XCTAssertEqual(try? Data(contentsOf: file), Data("different".utf8))
    }
    func testRootReplacementDoesNotRedirectConfirmation() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let preview = try await preview(controller, name: path.lastPathComponent), old = root.appendingPathExtension("old")
        try FileManager.default.moveItem(at: root, to: old)
        addTeardownBlock { try PluginReleaseInstallTests.thaw(old) }
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        _ = try directory(root)
        do { _ = try await controller.confirmVersionRemoval(id: preview.id, acknowledgedUnknown: true); XCTFail("replaced installation root adopted") } catch {}
        XCTAssertTrue(FileManager.default.fileExists(atPath: old.appendingPathComponent(path.lastPathComponent + "/nested/file.txt").path))
    }
    func testEnabledUnreadableVersionCannotBypassDisableViaUnknownEntry() async throws {
        let root = try root(), name = String(repeating: "a", count: 64), path = try directory(root, name: name)
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let selection = PluginReleaseCatalog.Selection(revision: nil, choices: ["org.ghosterm.probe": .init(releaseID: name, enabled: true, generation: UUID().uuidString)])
            _ = try owner.writeSelection(selection, expected: nil)
        }
        let controller = try controller(root), value = try await entry(controller, name: name)
        do {
            let unexpected = try await controller.previewUnclassifiedDirectory(name: name, expectedIdentity: value.identity)
            await controller.cancelVersionRemoval(id: unexpected.id)
            XCTFail("enabled unknown version offered for deletion")
        }
        catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .enabled) }
        XCTAssertTrue(FileManager.default.fileExists(atPath: path.path))
        let catalog = try await controller.installedVersions()
        _ = try await controller.disableInstalledPlugin(pluginID: "org.ghosterm.probe", expectedRevision: catalog.revision)
        let preview = try await self.preview(controller, name: name)
        let result = try await controller.confirmVersionRemoval(id: preview.id, acknowledgedUnknown: true)
        XCTAssertTrue(result.complete)
        let after = try await controller.installedVersions(); XCTAssertEqual(after.selections.first?.enabled, false)
    }
    func testSharedDirectoryReferenceBlocksManualCleanup() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let fd = open(path.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard fd >= 0 else { XCTFail("fixture fd"); return }
        var lease: PluginReleaseCatalog.VersionHandle? = .init(fd, id: String(repeating: "a", count: 64)); try lease!.pin()
        let value = try await entry(controller, name: path.lastPathComponent)
        do { _ = try await controller.previewUnclassifiedDirectory(name: value.name, expectedIdentity: value.identity); XCTFail("manual cleanup bypassed a live reference") }
        catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .occupied) }
        lease = nil
        let preview = try await self.preview(controller, name: path.lastPathComponent)
        await controller.cancelVersionRemoval(id: preview.id)
    }
    func testLinksAndNonDirectoriesAreListedWithoutDeleteAction() async throws {
        let root = try root(), outside = try self.root(), controller = try controller(root)
        let file = root.appendingPathComponent("file"); try Data("keep".utf8).write(to: file)
        let link = root.appendingPathComponent("link"); XCTAssertEqual(symlink(outside.path, link.path), 0)
        let entries = try await controller.unclassifiedEntries()
        XCTAssertEqual(Set(entries.map(\.name)), ["file", "link"]); XCTAssertTrue(entries.allSatisfy { !$0.canPreview && $0.issue != nil })
        for entry in entries {
            do { _ = try await controller.previewUnclassifiedDirectory(name: entry.name, expectedIdentity: entry.identity); XCTFail("non-directory or link accepted") } catch {}
        }
        XCTAssertTrue(FileManager.default.fileExists(atPath: outside.path))
    }
    func testNestedLinkRejectsWholeDirectoryWithoutReadingTarget() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root)
        let outside = root.appendingPathComponent("outside"); try Data("outside".utf8).write(to: outside)
        XCTAssertEqual(symlink(outside.path, path.appendingPathComponent("nested/link").path), 0)
        do { _ = try await preview(controller, name: path.lastPathComponent); XCTFail("nested link accepted") } catch {}
        XCTAssertEqual(try? Data(contentsOf: outside), Data("outside".utf8))
    }
    func testPendingUnknownIntentCanBeCancelledWithoutDeletingDirectory() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root), value = try await entry(controller, name: path.lastPathComponent)
        enum Stop: Error { case point }
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let removal = try PluginReleaseRemoval(owner: owner, releaseID: PluginReleaseCatalog.sha(Data(value.name.utf8)), expectedRevision: nil,
                checkpoint: { if $0 == "after-intent" { throw Stop.point } }, unknownName: value.name, expectedIdentity: value.identity)
            XCTAssertThrowsError(try removal.confirm(owner: owner, acknowledgedUnknown: true))
        }
        let pending = try await controller.pendingVersionRemoval(), ticket = try XCTUnwrap(pending)
        XCTAssertTrue(ticket.unknownOrigin)
        let durable = try await controller.cancelPendingVersionRemoval(id: ticket.id); XCTAssertTrue(durable)
        XCTAssertEqual(try? Data(contentsOf: path.appendingPathComponent("nested/file.txt")), Data("retained".utf8))
    }
    func testFormatThreePartialDeletionCanResumeExplicitly() async throws {
        let root = try root(), path = try directory(root), controller = try controller(root), value = try await entry(controller, name: path.lastPathComponent)
        enum Stop: Error { case point }
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let removal = try PluginReleaseRemoval(owner: owner, releaseID: PluginReleaseCatalog.sha(Data(value.name.utf8)), expectedRevision: nil,
                checkpoint: { if $0 == "after-unlink" { throw Stop.point } }, unknownName: value.name, expectedIdentity: value.identity)
            XCTAssertThrowsError(try removal.confirm(owner: owner, acknowledgedUnknown: true))
        }
        let pending = try await controller.pendingVersionRemoval(), ticket = try XCTUnwrap(pending)
        XCTAssertTrue(ticket.unknownOrigin)
        let result = try await controller.resumeVersionRemoval(id: ticket.id)
        XCTAssertTrue(result.complete && result.durabilityConfirmed); XCTAssertEqual(result.removed, 2); XCTAssertEqual(result.observedAbsent, 1)
    }
    func testManualRecoveryRetainsDirectoryIfSelectionWasEnabled() async throws {
        let root = try root(), name = String(repeating: "b", count: 64), path = try directory(root, name: name)
        let controller = try controller(root), value = try await entry(controller, name: name)
        enum Stop: Error { case point }
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            let removal = try PluginReleaseRemoval(owner: owner, releaseID: PluginReleaseCatalog.sha(Data(name.utf8)), expectedRevision: nil,
                checkpoint: { if $0 == "after-detach" { throw Stop.point } }, unknownName: name, expectedIdentity: value.identity)
            XCTAssertThrowsError(try removal.confirm(owner: owner, acknowledgedUnknown: true))
        }
        do {
            let owner = try XCTUnwrap(PluginReleaseCatalog(directory: root, exclusive: true))
            _ = try owner.writeSelection(.init(revision: nil, choices: ["org.ghosterm.probe": .init(releaseID: name, enabled: true, generation: UUID().uuidString)]), expected: nil)
        }
        let pending = try await controller.pendingVersionRemoval(), ticket = try XCTUnwrap(pending)
        do { _ = try await controller.resumeVersionRemoval(id: ticket.id); XCTFail("recovery ignored enabled selection") }
        catch { XCTAssertEqual(error as? PluginReleaseRemoval.Failure, .enabled) }
        XCTAssertEqual(try? Data(contentsOf: ticket.directory.appendingPathComponent("nested/file.txt")), Data("retained".utf8))
        XCTAssertFalse(FileManager.default.fileExists(atPath: path.path))
    }
    func testFreshManualAuthorityDoesNotRetireUnconfirmedOldRecord() async throws {
        let root = try root(), id = UUID().uuidString.lowercased(), fd = open(root.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
        guard fd >= 0 else { XCTFail("fixture root"); return }; defer { Darwin.close(fd) }
        let journal = try XCTUnwrap(PluginInstallJournal(parent: fd, create: true)), record = try journal.reserve(id: id)
        let path = try directory(root, name: record.name), controller = try controller(root)
        let before = try await controller.installationResidues(); XCTAssertEqual(before.entries.first?.state, .unconfirmed)
        let preview = try await self.preview(controller, name: path.lastPathComponent)
        XCTAssertTrue(preview.unknownOrigin)
        let result = try await controller.confirmVersionRemoval(id: preview.id, acknowledgedUnknown: true); XCTAssertTrue(result.complete)
        let after = try await controller.installationResidues(); XCTAssertEqual(after.entries.first?.state, .absent)
        XCTAssertEqual(after.entries.first?.digest, before.entries.first?.digest, "fresh manual confirmation must not rewrite old ownership")
    }
}
