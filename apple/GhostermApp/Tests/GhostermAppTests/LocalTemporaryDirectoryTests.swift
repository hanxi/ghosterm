import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

final class LocalTemporaryDirectoryTests: XCTestCase {
    private func journal() throws -> ExtensionLocalJournal {
        var project = URL(fileURLWithPath: #filePath); for _ in 0..<5 { project.deleteLastPathComponent() }
        let path = project.appendingPathComponent("tmp/native-journal-\(UUID().uuidString)")
        let journal = try ExtensionLocalJournal(directory: path)
        addTeardownBlock { try FileManager.default.removeItem(at: path) }
        return journal
    }
    private func fixture() throws -> (URL, ExtensionLocalTemporaryDirectory) {
        var project = URL(fileURLWithPath: #filePath); for _ in 0..<5 { project.deleteLastPathComponent() }
        let base = project.appendingPathComponent("tmp/native-temporary-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: base, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        addTeardownBlock { try FileManager.default.removeItem(at: base) }
        let fd = open(base.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw GhostermLocalLaunchError.invalid("fixture directory unavailable") }
        defer { Darwin.close(fd) }
        return (base, try .init(parentFD: fd, base: base.path, journal: journal()))
    }
    private func finish(_ owner: ExtensionLocalTemporaryDirectory) throws {
        let end = ProcessInfo.processInfo.systemUptime + 2
        while ProcessInfo.processInfo.systemUptime < end {
            if try owner.closeStep() { return }
        }
        throw GhostermLocalLaunchError.invalid("Native temporary cleanup exceeded two seconds")
    }
    private func names(_ path: String) throws -> Set<String> { Set(try FileManager.default.contentsOfDirectory(atPath: path)) }
    private func fill(_ root: String, count: Int) throws {
        let fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw GhostermLocalLaunchError.invalid("fixture root unavailable") }; defer { Darwin.close(fd) }
        for index in 0..<count {
            let child = "file-\(index)".withCString { openat(fd, $0, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600) }
            guard child >= 0 else { throw GhostermLocalLaunchError.invalid("fixture file creation failed") }
            Darwin.close(child)
        }
    }
    func testPrivateNamespaceRemovesSealedFilesWithoutFollowingLinksOrAdoptingSiblings() throws {
        let (base, owner) = try fixture(), fm = FileManager.default
        let outside = base.appendingPathComponent("keep"), stale = base.appendingPathComponent("ghosterm-local-unowned")
        try Data("KEEP".utf8).write(to: outside); try fm.setAttributes([.posixPermissions: 0o640], ofItemAtPath: outside.path)
        try fm.createDirectory(at: stale, withIntermediateDirectories: false)
        let sealed = URL(fileURLWithPath: owner.directory).appendingPathComponent("sealed")
        try fm.createDirectory(at: sealed, withIntermediateDirectories: false)
        try Data([1, 0, 255]).write(to: sealed.appendingPathComponent("file"))
        try fm.createSymbolicLink(atPath: sealed.appendingPathComponent("link").path, withDestinationPath: base.path)
        guard link(outside.path, sealed.appendingPathComponent("hard-link").path) == 0 else { throw GhostermLocalLaunchError.invalid("fixture hard link failed") }
        try fm.setAttributes([.posixPermissions: 0o500], ofItemAtPath: sealed.path)
        XCTAssertEqual(owner.observation.phase, .active); XCTAssertNotEqual(owner.directory, base.path)
        try finish(owner)
        XCTAssertEqual(try names(base.path), ["keep", "ghosterm-local-unowned"])
        XCTAssertEqual(try Data(contentsOf: outside), Data("KEEP".utf8))
        XCTAssertEqual((try fm.attributesOfItem(atPath: outside.path)[.posixPermissions] as? NSNumber)?.intValue, 0o640)
        XCTAssertEqual(owner.observation.phase, .removed); XCTAssertEqual(owner.observation.error, 0)
        XCTAssertTrue(try owner.closeStep())
    }
    func testOneStepHasFiniteWorkAndCanContinue() throws {
        let (_, owner) = try fixture(); try fill(owner.directory, count: 150)
        let done = try owner.closeStep(); XCTAssertFalse(done)
        try finish(owner)
    }
    func testReplacedRootRetainsBothNamespacesAndCanRetryAfterRestoration() throws {
        let (base, owner) = try fixture(); try fill(owner.directory, count: 150)
        _ = try owner.closeStep()
        let expected = try names(owner.directory), moved = base.appendingPathComponent("moved")
        try FileManager.default.moveItem(atPath: owner.directory, toPath: moved.path)
        try FileManager.default.createDirectory(atPath: owner.directory, withIntermediateDirectories: false)
        try Data("foreign".utf8).write(to: URL(fileURLWithPath: owner.directory).appendingPathComponent("victim"))
        var code: Int32?
        do { _ = try owner.closeStep() } catch { code = (error as? ExtensionLocalTemporaryDirectory.Failure)?.code }
        let movedNames = try names(moved.path), replacementNames = try names(owner.directory)
        XCTAssertTrue(code == ESTALE && movedNames == expected && replacementNames == ["victim"] && owner.observation.phase == .retained)
        try FileManager.default.removeItem(atPath: owner.directory)
        try FileManager.default.moveItem(atPath: moved.path, toPath: owner.directory)
        try finish(owner)
    }
    func testReplacedNestedDirectoryIsNotTraversedThroughRetainedFD() throws {
        let (_, owner) = try fixture(), fm = FileManager.default
        let child = owner.directory + "/child", moved = owner.directory + "/moved"
        try fm.createDirectory(atPath: child, withIntermediateDirectories: false); try fill(child, count: 150)
        _ = try owner.closeStep(); let expected = try names(child)
        try fm.moveItem(atPath: child, toPath: moved); try fm.createDirectory(atPath: child, withIntermediateDirectories: false)
        try Data("foreign".utf8).write(to: URL(fileURLWithPath: child).appendingPathComponent("victim"))
        var code: Int32?
        do { _ = try owner.closeStep() } catch { code = (error as? ExtensionLocalTemporaryDirectory.Failure)?.code }
        let movedNames = try names(moved), replacementNames = try names(child)
        XCTAssertTrue(code == ESTALE && movedNames == expected && replacementNames == ["victim"] && owner.observation.phase == .retained)
        try fm.removeItem(atPath: child); try fm.moveItem(atPath: moved, toPath: child); try finish(owner)
    }
    func testDepthLimitRetainsOwner() throws {
        let (_, owner) = try fixture(); var path = owner.directory
        for _ in 0..<65 { path += "/d"; try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: false) }
        var code: Int32?
        do { try finish(owner) } catch { code = (error as? ExtensionLocalTemporaryDirectory.Failure)?.code }
        XCTAssertTrue(code == ELOOP && owner.observation.phase == .retained)
    }
    func testEntryLimitRetainsOwner() throws {
        let (_, owner) = try fixture(); try fill(owner.directory, count: 16385)
        var code: Int32?
        do { try finish(owner) } catch { code = (error as? ExtensionLocalTemporaryDirectory.Failure)?.code }
        XCTAssertTrue(code == EFBIG && owner.observation.phase == .retained)
    }
    func testSealedDirectoryIsMadeWritableBeforeCleanup() throws {
        let (_, owner) = try fixture(), fm = FileManager.default
        let child = owner.directory + "/sealed"
        try fm.createDirectory(atPath: child, withIntermediateDirectories: false)
        try fill(child, count: 1); try fm.setAttributes([.posixPermissions: 0o500], ofItemAtPath: child)
        defer { try? fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: child) }
        var success = false
        do { try finish(owner); success = true } catch { }
        XCTAssertTrue(success && owner.observation.phase == .removed)
    }
    func testInvalidUTF8NameIsRetainedAndExplicitRetryReopensDirectoryCursor() throws {
        let (_, owner) = try fixture()
        let fd = open(owner.directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw GhostermLocalLaunchError.invalid("fixture unavailable") }; defer { Darwin.close(fd) }
        let bytes: [CChar] = [-1, 0]
        let child = bytes.withUnsafeBufferPointer { openat(fd, $0.baseAddress!, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0o600) }
        if child < 0 { XCTAssertEqual(errno, EILSEQ); try finish(owner); return } // APFS rejects the fixture itself.
        Darwin.close(child)
        XCTAssertThrowsError(try owner.closeStep()) { XCTAssertEqual(($0 as? ExtensionLocalTemporaryDirectory.Failure)?.code, EILSEQ) }
        guard bytes.withUnsafeBufferPointer({ unlinkat(fd, $0.baseAddress!, 0) }) == 0 else { throw GhostermLocalLaunchError.invalid("fixture unlink failed") }
        try fill(owner.directory, count: 1); try finish(owner)
    }
    func testMismatchingParentPathDoesNotAuthorizeOtherDirectory() throws {
        let (base, _) = try fixture(), fm = FileManager.default
        let other = base.appendingPathComponent("other")
        try fm.createDirectory(at: other, withIntermediateDirectories: false)
        try Data("KEEP".utf8).write(to: other.appendingPathComponent("victim"))
        let fd = open(base.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard fd >= 0 else { throw GhostermLocalLaunchError.invalid("fixture unavailable") }; defer { Darwin.close(fd) }
        let owner = try ExtensionLocalTemporaryDirectory(parentFD: fd, base: other.path, journal: journal())
        XCTAssertThrowsError(try owner.executionPath()); XCTAssertThrowsError(try owner.closeStep())
        XCTAssertEqual(try names(other.path), ["victim"])
    }
}
