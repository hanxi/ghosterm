// ConfigStoreTests — 「文件即真相」写侧的硬断言。
//
// 覆盖（每条对应铁律 #4 的一个执行面）：
//   1. 原子写：落盘内容逐字节等于提交内容；
//   2. 备份：save 前旧文件被复制到 .bak；
//   3. 冲突检测：load 后外部修改 → save 拒绝（不静默覆盖别人）；
//   4. 二次 save：快照刷新后可继续写；
//   5. 新文件（原不存在）：不备份、直接原子创建。
import Darwin
@testable import GhostermAppModel
import XCTest

final class ConfigStoreTests: XCTestCase {
    private func makeFile(_ initial: String?) throws -> String {
        let dir = NSTemporaryDirectory() + "cfgstore-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/config"
        if let initial { try initial.write(toFile: path, atomically: true, encoding: .utf8) }
        return path
    }

    func testAtomicWriteRoundTrip() throws {
        let path = try makeFile("Host web\n  User deploy\n")
        let store = ConfigStore(path: path)
        try store.save("Host web\n  User ops\n")
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8),
                       "Host web\n  User ops\n", "落盘内容必须逐字节等于提交内容")
        // 半份文件不存在（原子 rename 的临时文件已清理）。
        let dir = (path as NSString).deletingLastPathComponent
        let leftovers = try FileManager.default.contentsOfDirectory(atPath: dir)
            .filter { $0.hasPrefix(".config.tmp") }
        XCTAssertTrue(leftovers.isEmpty, "不得残留临时文件（得到 \(leftovers)）")
    }

    func testBackupBeforeOverwrite() throws {
        let original = "Host web\n  User deploy\n"
        let path = try makeFile(original)
        let store = ConfigStore(path: path)
        try store.save("Host web\n  User ops\n")
        let backup = (path as NSString).deletingPathExtension + ".bak"
        XCTAssertEqual(try? String(contentsOfFile: backup, encoding: .utf8), original,
                       "save 前必须把原文件备份到 .bak（内容逐字节）")
    }

    func testExternalModificationRefusesSave() throws {
        let path = try makeFile("Host web\n  User deploy\n")
        let store = ConfigStore(path: path)
        // 外部（比如用户用 vim 改了）：
        Thread.sleep(forTimeInterval: 0.05)
        try "Host web\n  User someone-else\n".write(toFile: path, atomically: true, encoding: .utf8)
        XCTAssertThrowsError(try store.save("Host web\n  User ops\n")) { error in
            guard case ConfigStoreError.conflict = error else {
                return XCTFail("外部修改后 save 必须抛 conflict，得到 \(error)")
            }
        }
        // 且外部内容未被覆盖：
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8),
                       "Host web\n  User someone-else\n")
    }

    func testSnapshotRefreshesAfterSave() throws {
        let path = try makeFile("Host web\n  User deploy\n")
        let store = ConfigStore(path: path)
        try store.save("v1\n")
        XCTAssertFalse(store.isExternallyModified(), "save 后快照应刷新")
        try store.save("v2\n") // 不应抛 conflict
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8), "v2\n")
    }

    func testCreateNewFileDoesNotBackup() throws {
        let path = try makeFile(nil) // 不存在
        let store = ConfigStore(path: path)
        try store.save("Host web\n  User deploy\n")
        let backup = (path as NSString).deletingPathExtension + ".bak"
        XCTAssertFalse(FileManager.default.fileExists(atPath: backup),
                       "原文件不存在时不得凭空造备份")
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8),
                       "Host web\n  User deploy\n")
    }

    /// 文件在 load 之后才出现（原本没有 config）也是冲突。
    func testAppearedAfterLoadIsConflict() throws {
        let path = try makeFile(nil)
        let store = ConfigStore(path: path)
        try "external\n".write(toFile: path, atomically: true, encoding: .utf8)
        XCTAssertTrue(store.isExternallyModified())
        XCTAssertThrowsError(try store.save("mine\n"))
    }

    func testBackupFailurePreservesOriginalAndExistingBackupDirectory() throws {
        let original = "Host web\n  User deploy\n"
        let path = try makeFile(original)
        let backup = (path as NSString).deletingPathExtension + ".bak"
        try FileManager.default.createDirectory(atPath: backup, withIntermediateDirectories: false)
        let marker = backup + "/keep"
        try "existing backup".write(toFile: marker, atomically: false, encoding: .utf8)
        let store = ConfigStore(path: path)
        var rejected = false
        do { try store.save("replacement\n") } catch { rejected = true }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == original
                      && (try? String(contentsOfFile: marker, encoding: .utf8)) == "existing backup",
                      "backup failure must preserve the original and unrelated backup directory contents")
    }

    func testSameLengthChangeWithRestoredMtimeRefusesSave() throws {
        let path = try makeFile("original\n")
        let date = Date(timeIntervalSince1970: 1_600_000_000)
        try FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: path)
        let store = ConfigStore(path: path)
        let handle = try FileHandle(forWritingTo: URL(fileURLWithPath: path))
        try handle.write(contentsOf: Data("external\n".utf8))
        try handle.close()
        try FileManager.default.setAttributes([.modificationDate: date], ofItemAtPath: path)
        var rejected = false
        do { try store.save("mine\n") } catch { rejected = true }
        XCTAssertTrue(rejected && store.isExternallyModified()
                      && (try? String(contentsOfFile: path, encoding: .utf8)) == "external\n",
                      "same-length changes with restored mtime must not be overwritten")
    }

    func testNonregularInitialTargetIsNotAnEmptyConfiguration() throws {
        let path = try makeFile(nil)
        try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: false)
        let marker = path + "/keep"
        try "retain".write(toFile: marker, atomically: false, encoding: .utf8)
        let store = ConfigStore(path: path)
        var rejected = false
        do { try store.save("mine\n") } catch { rejected = true }
        XCTAssertTrue(rejected && store.isExternallyModified()
                      && (try? String(contentsOfFile: marker, encoding: .utf8)) == "retain",
                      "a nonregular target must stay a read error, never an absent configuration")
    }

    func testSymlinkTargetIsRejectedWithoutChangingReferent() throws {
        let path = try makeFile(nil), target = path + "-target"
        try "original\n".write(toFile: target, atomically: false, encoding: .utf8)
        try FileManager.default.createSymbolicLink(atPath: path, withDestinationPath: target)
        let store = ConfigStore(path: path)
        var rejected = false
        do { try store.save("mine\n") } catch { rejected = true }
        XCTAssertTrue(rejected && (try? FileManager.default.destinationOfSymbolicLink(atPath: path)) == target
                      && (try? String(contentsOfFile: target, encoding: .utf8)) == "original\n",
                      "a symlink must neither be followed nor silently replaced")
    }

    func testExistingPermissionsSurviveReplacement() throws {
        let path = try makeFile("original\n")
        try FileManager.default.setAttributes([.posixPermissions: 0o640], ofItemAtPath: path)
        let store = ConfigStore(path: path)
        try store.save("updated\n")
        let attrs = try FileManager.default.attributesOfItem(atPath: path)
        XCTAssertEqual((attrs[.posixPermissions] as? NSNumber)?.intValue, 0o640,
                       "atomic replacement must preserve existing POSIX permissions")
    }

    func testSecondBackupContainsImmediatelyPreviousBytes() throws {
        let path = try makeFile("original\n"), backup = (path as NSString).deletingPathExtension + ".bak"
        let store = ConfigStore(path: path)
        try store.save("first\n")
        try store.save("second\n")
        XCTAssertEqual(try String(contentsOfFile: backup, encoding: .utf8), "first\n")
    }

    func testEmptyConfigurationAndRawBackupBytes() throws {
        let path = try makeFile(nil)
        let original = Data([0xff, 0x0d, 0x0a, 0x00])
        try original.write(to: URL(fileURLWithPath: path))
        let store = ConfigStore(path: path)
        try store.save("")
        let backup = (path as NSString).deletingPathExtension + ".bak"
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: backup)), original)
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: path)), Data())
    }

    private func noTemporaryFiles(_ path: String) -> Bool {
        let directory = (path as NSString).deletingLastPathComponent
        guard let names = try? FileManager.default.contentsOfDirectory(atPath: directory) else { return false }
        return !names.contains { $0.hasPrefix(".config.tmp-") }
    }

    func testOldPathStillExistsAtAtomicReplacement() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), observedOriginal = false
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            if target == "config" {
                observedOriginal = (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
            }
            return rename(directory, source, target, replace)
        }
        try ConfigStore(path: path, operations: operations).save("updated\n")
        XCTAssertTrue(observedOriginal, "the old target must remain present until the single atomic replacement")
    }

    func testFinalRenameFailurePreservesOriginalAndCleansStaging() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations()
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            if target == "config" { errno = EIO; return -1 }
            return rename(directory, source, target, replace)
        }
        var rejected = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
                      && noTemporaryFiles(path), "failed final rename must preserve the original and remove its staging files")
    }

    func testBackupRenameFailureCannotBeIgnored() throws {
        let path = try makeFile("original\n"), backup = (path as NSString).deletingPathExtension + ".bak"
        try "prior backup".write(toFile: backup, atomically: false, encoding: .utf8)
        var operations = ConfigStore.FileOperations()
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            if target == "config.bak" { errno = EACCES; return -1 }
            return rename(directory, source, target, replace)
        }
        var rejected = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
                      && (try? String(contentsOfFile: backup, encoding: .utf8)) == "prior backup"
                      && noTemporaryFiles(path), "backup replacement failure must stop before any target write")
    }

    func testWriteFailureCannotPublishPartialConfiguration() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), writes = 0
        let write = operations.write
        operations.write = { fd, pointer, count in
            writes += 1
            if writes == 2 { errno = EIO; return -1 }
            return write(fd, pointer, count)
        }
        var rejected = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
                      && noTemporaryFiles(path), "staging write failure must not publish or retain a partial configuration")
    }

    func testShortWritesAndInterruptionsPreserveExactBytes() throws {
        let path = try makeFile("original\n"), backup = (path as NSString).deletingPathExtension + ".bak"
        let proposed = String(repeating: "主机\r\n", count: 16)
        var operations = ConfigStore.FileOperations(), writes = 0
        let write = operations.write
        operations.write = { fd, pointer, count in
            writes += 1
            if writes == 1 { errno = EINTR; return -1 }
            return write(fd, pointer, min(count, 3))
        }
        var accepted = false
        do { try ConfigStore(path: path, operations: operations).save(proposed); accepted = true } catch { }
        XCTAssertTrue(accepted && (try? Data(contentsOf: URL(fileURLWithPath: path))) == Data(proposed.utf8)
                      && (try? String(contentsOfFile: backup, encoding: .utf8)) == "original\n",
                      "short writes and EINTR must preserve the complete new and backup bytes")
    }

    func testStagingSyncFailureStopsBeforeCommit() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), synchronizations = 0
        let synchronize = operations.sync
        operations.sync = { fd in
            synchronizations += 1
            if synchronizations == 3 { errno = EIO; return -1 }
            return synchronize(fd)
        }
        var rejected = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
                      && noTemporaryFiles(path), "failed file synchronization must prevent target commit")
    }

    func testBackupDirectorySyncFailureStopsBeforeCommit() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), synchronizations = 0
        let synchronize = operations.sync
        operations.sync = { fd in
            synchronizations += 1
            if synchronizations == 2 { errno = EIO; return -1 }
            return synchronize(fd)
        }
        var rejected = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
                      && noTemporaryFiles(path), "an unsynchronized backup must stop target replacement")
    }

    func testStagingCloseFailureStopsBeforeCommit() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), closes = 0
        operations.close = { fd in
            let result = Darwin.close(fd)
            closes += 1
            if closes == 2 { errno = EIO; return -1 }
            return result
        }
        var rejected = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
                      && noTemporaryFiles(path), "a failed staging close must prevent commit without leaking the temporary name")
    }

    func testPostCommitSyncFailureReportsEffectAndRequiresReload() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), synchronizations = 0
        let synchronize = operations.sync
        operations.sync = { fd in
            synchronizations += 1
            if synchronizations == 4 { errno = EIO; return -1 }
            return synchronize(fd)
        }
        let store = ConfigStore(path: path, operations: operations)
        var committed = false, retryRejected = false
        do { try store.save("updated\n") } catch { if case ConfigStoreError.committed = error { committed = true } }
        do { try store.save("retry\n") } catch { if case ConfigStoreError.io = error { retryRejected = true } }
        XCTAssertTrue(committed && retryRejected && store.isExternallyModified()
                      && (try? String(contentsOfFile: path, encoding: .utf8)) == "updated\n"
                      && noTemporaryFiles(path), "post-rename sync failure must report the committed effect and block blind retry")
    }

    func testPostCommitReadbackErrorCannotBecomeUncommittedFailure() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), synchronizations = 0, observedCommit = false
        let synchronize = operations.sync
        operations.sync = { fd in
            synchronizations += 1
            let result = synchronize(fd)
            if synchronizations == 4 {
                observedCommit = (try? String(contentsOfFile: path, encoding: .utf8)) == "updated\n"
                if unlinkat(fd, "config", 0) != 0 || mkdirat(fd, "config", 0o700) != 0 { return -1 }
            }
            return result
        }
        let store = ConfigStore(path: path, operations: operations)
        var committed = false, retryRejected = false
        do { try store.save("updated\n") } catch { if case ConfigStoreError.committed = error { committed = true } }
        do { try store.save("retry\n") } catch { if case ConfigStoreError.io = error { retryRejected = true } }
        XCTAssertTrue(observedCommit && committed && retryRejected && noTemporaryFiles(path),
                      "readback failure after a real commit must retain that effect and require reload")
    }

    func testExternalChangeAfterBackupIsRecheckedBeforeCommit() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), changed = false
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            let result = rename(directory, source, target, replace)
            if target == "config.bak", result == 0 {
                do { try "external\n".write(toFile: path, atomically: true, encoding: .utf8); changed = true }
                catch { return -1 }
            }
            return result
        }
        var conflict = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.conflict = error { conflict = true } }
        XCTAssertTrue(changed && conflict && (try? String(contentsOfFile: path, encoding: .utf8)) == "external\n"
                      && noTemporaryFiles(path), "target changes after backup must be rechecked before atomic commit")
    }

    func testLateCreationCannotBeOverwrittenByFirstSave() throws {
        let path = try makeFile(nil)
        var operations = ConfigStore.FileOperations(), created = false
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            if target == "config" {
                do { try "external\n".write(toFile: path, atomically: false, encoding: .utf8); created = true }
                catch { return -1 }
            }
            return rename(directory, source, target, replace)
        }
        var conflict = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.conflict = error { conflict = true } }
        XCTAssertTrue(created && conflict && (try? String(contentsOfFile: path, encoding: .utf8)) == "external\n",
                      "first save must use exclusive final rename even if the path appears after the last check")
    }

    func testReadErrorCannotBecomeMissingWithoutReload() throws {
        let path = try makeFile(nil)
        try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: false)
        let store = ConfigStore(path: path)
        try FileManager.default.removeItem(atPath: path)
        var rejected = false
        do { try store.save("updated\n") } catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && !FileManager.default.fileExists(atPath: path),
                      "an initial read error must not become an absent configuration without reload")
    }

    func testSuccessfulCommitRefreshesSnapshotForNextEdit() throws {
        let path = try makeFile("original\n"), store = ConfigStore(path: path)
        var accepted = false
        do { try store.save("first\n"); try store.save("second\n"); accepted = true } catch { }
        XCTAssertTrue(accepted && !store.isExternallyModified()
                      && (try? String(contentsOfFile: path, encoding: .utf8)) == "second\n",
                      "successful commit must refresh its identity and bytes for the next independent edit")
    }

    func testChangedParentDirectoryCannotReceiveOrHideCommit() throws {
        let path = try makeFile("original\n"), parent = (path as NSString).deletingLastPathComponent
        let moved = parent + "-moved"
        addTeardownBlock { try? FileManager.default.removeItem(atPath: moved) }
        var operations = ConfigStore.FileOperations(), changed = false
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            let result = rename(directory, source, target, replace)
            if target == "config.bak", result == 0 {
                do {
                    try FileManager.default.moveItem(atPath: parent, toPath: moved)
                    try FileManager.default.createDirectory(atPath: parent, withIntermediateDirectories: false)
                    try "external\n".write(toFile: path, atomically: false, encoding: .utf8)
                    changed = true
                } catch { return -1 }
            }
            return result
        }
        var conflict = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.conflict = error { conflict = true } }
        XCTAssertTrue(changed && conflict && (try? String(contentsOfFile: path, encoding: .utf8)) == "external\n"
                      && (try? String(contentsOfFile: moved + "/config", encoding: .utf8)) == "original\n",
                      "a replaced parent must not redirect the write or hide a commit in a detached directory")
    }

    func testPostCommitChangedBytesAreReportedAsCommittedError() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), synchronizations = 0, observedCommit = false
        let synchronize = operations.sync
        operations.sync = { fd in
            synchronizations += 1
            let result = synchronize(fd)
            if synchronizations == 4 {
                observedCommit = (try? String(contentsOfFile: path, encoding: .utf8)) == "updated\n"
                do { try "external\n".write(toFile: path, atomically: false, encoding: .utf8) }
                catch { return -1 }
            }
            return result
        }
        var committed = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.committed = error { committed = true } }
        XCTAssertTrue(observedCommit && committed && (try? String(contentsOfFile: path, encoding: .utf8)) == "external\n",
                      "readback with different bytes must report the prior commit rather than claiming saved content")
    }

    func testCooperatingWriterCannotEnterBetweenCheckAndRename() throws {
        let path = try makeFile("original\n")
        var operations = ConfigStore.FileOperations(), contenderRejected = false
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            if target == "config" {
                do { try ConfigStore(path: path).save("contender\n") }
                catch { if case ConfigStoreError.io = error { contenderRejected = true } }
            }
            return rename(directory, source, target, replace)
        }
        try ConfigStore(path: path, operations: operations).save("updated\n")
        let backup = (path as NSString).deletingPathExtension + ".bak"
        XCTAssertTrue(contenderRejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "updated\n"
                      && (try? String(contentsOfFile: backup, encoding: .utf8)) == "original\n",
                      "another store must not enter the last-check-to-rename interval under the same stable lock")
    }

    func testHardlinkedCoordinationFileCannotBeAdopted() throws {
        let path = try makeFile("original\n"), unrelated = path + "-unrelated"
        try "retain".write(toFile: unrelated, atomically: false, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: unrelated)
        let lock = (path as NSString).deletingLastPathComponent + "/.config.lock"
        try FileManager.default.linkItem(atPath: unrelated, toPath: lock)
        var rejected = false
        do { try ConfigStore(path: path).save("updated\n") }
        catch { if case ConfigStoreError.io = error { rejected = true } }
        XCTAssertTrue(rejected && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n"
                      && (try? String(contentsOfFile: unrelated, encoding: .utf8)) == "retain",
                      "an unrelated hardlinked file cannot be adopted as this configuration's coordination file")
    }

    func testReplacedCoordinationFileStopsCommit() throws {
        let path = try makeFile("original\n")
        let lock = (path as NSString).deletingLastPathComponent + "/.config.lock"
        var operations = ConfigStore.FileOperations(), replaced = false
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            let result = rename(directory, source, target, replace)
            if target == "config.bak", result == 0 {
                do {
                    try FileManager.default.removeItem(atPath: lock)
                    try "replacement lock".write(toFile: lock, atomically: false, encoding: .utf8)
                    replaced = true
                } catch { return -1 }
            }
            return result
        }
        var conflict = false
        do { try ConfigStore(path: path, operations: operations).save("updated\n") }
        catch { if case ConfigStoreError.conflict = error { conflict = true } }
        XCTAssertTrue(replaced && conflict && (try? String(contentsOfFile: path, encoding: .utf8)) == "original\n",
                      "a changed lock inode must stop commit instead of pretending writers remain coordinated")
    }
}
