import Darwin
import Foundation
@testable import GhostermAppModel
import XCTest

final class SettingsStoreTests: XCTestCase {
    private func directory() throws -> URL {
        let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("settings-store-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: directory) }
        return directory
    }
    func testFirstSaveCreatesMissingParentsAndPrivateFileThenPreservesExactBackup() throws {
        let path = try directory().appendingPathComponent("nested/config/ghostty.conf")
        let store = SettingsStore(terminalConfigPath: path.path)
        guard try store.rawConfig() == "" else { return XCTFail("missing snapshot") }
        try store.saveTerminalConfig("# first\r\nfont-size = 12\r\n")
        let attributes = try FileManager.default.attributesOfItem(atPath: path.path)
        let privateMode = (attributes[.posixPermissions] as? NSNumber)?.uint32Value == 0o600
        let firstBackupMissing = !FileManager.default.fileExists(atPath: path.path + ".bak")
        try store.saveTerminalConfig("font-size = 18\n")
        XCTAssertTrue(privateMode && firstBackupMissing && (try? Data(contentsOf: URL(fileURLWithPath: path.path + ".bak"))) == Data("# first\r\nfont-size = 12\r\n".utf8) && (try? store.rawConfig()) == "font-size = 18\n" && !store.isExternallyModified())
    }
    func testFailedReadsNeverBecomeMissingOrWritableUntilExplicitReload() throws {
        let path = try directory().appendingPathComponent("ghostty.conf")
        try FileManager.default.createDirectory(at: path, withIntermediateDirectories: false)
        let store = SettingsStore(terminalConfigPath: path.path)
        var refused = 0
        do { _ = try store.rawConfig() } catch { refused += 1 }
        try FileManager.default.removeItem(at: path)
        try Data("font-size = 12\n".utf8).write(to: path)
        do { try store.saveTerminalConfig("font-size = 20\n") } catch { refused += 1 }
        let stillError = store.loadedText == nil && store.loadError != nil
        let reloaded = try? store.reload()
        var saved = false
        do { try store.saveTerminalConfig("font-size = 18\n"); saved = true } catch {}
        XCTAssertTrue(refused == 2 && stillError && saved && reloaded == "font-size = 12\n" && store.loadError == nil && (try? store.rawConfig()) == "font-size = 18\n")
    }
    func testInvalidUtf8AndNulCannotBeDisplayedAsSuccessfulEmptyFilesOrOverwritten() throws {
        let root = try directory(); var refused = 0
        for (i, data) in [Data([255]), Data("font-size = 12\0\n".utf8)].enumerated() {
            let path = root.appendingPathComponent("\(i).conf"); try data.write(to: path)
            let store = SettingsStore(terminalConfigPath: path.path)
            do { try store.saveTerminalConfig("font-size = 20\n") } catch { refused += 1 }
            if store.loadError != nil && store.loadedText == nil && (try? Data(contentsOf: path)) == data { refused += 1 }
        }
        XCTAssertEqual(refused, 4)
    }
    func testExternallyCreatedAfterMissingSnapshotIsAConflict() throws {
        let path = try directory().appendingPathComponent("ghostty.conf")
        let store = SettingsStore(terminalConfigPath: path.path)
        try Data("# external\n".utf8).write(to: path)
        var conflict = false
        do { try store.saveTerminalConfig("font-size = 18\n") }
        catch { conflict = String(describing: error).contains("外部修改") }
        XCTAssertTrue(conflict && store.isExternallyModified() && (try? Data(contentsOf: path)) == Data("# external\n".utf8))
    }
    func testSnapshotDoesNotAdoptExternalReplacementWithSameLengthAndMtime() throws {
        let path = try directory().appendingPathComponent("ghostty.conf")
        try Data("font-size = 12\n".utf8).write(to: path)
        let original = try FileManager.default.attributesOfItem(atPath: path.path)
        let store = SettingsStore(terminalConfigPath: path.path)
        try Data("font-size = 24\n".utf8).write(to: path, options: .atomic)
        try FileManager.default.setAttributes([.modificationDate: original[.modificationDate]!], ofItemAtPath: path.path)
        var refused = false
        do { try store.saveTerminalConfig("font-size = 30\n") } catch { refused = true }
        XCTAssertTrue(refused && (try? store.rawConfig()) == "font-size = 12\n" && (try? String(contentsOf: path, encoding: .utf8)) == "font-size = 24\n")
    }
    func testBackupFailurePreservesPriorBackupDirectoryAndOriginal() throws {
        let path = try directory().appendingPathComponent("ghostty.conf")
        try Data("font-size = 12\n".utf8).write(to: path)
        let backup = URL(fileURLWithPath: path.path + ".bak")
        try FileManager.default.createDirectory(at: backup, withIntermediateDirectories: false)
        try Data("keep".utf8).write(to: backup.appendingPathComponent("marker"))
        let store = SettingsStore(terminalConfigPath: path.path)
        var refused = false
        do { try store.saveTerminalConfig("font-size = 20\n") } catch { refused = true }
        XCTAssertTrue(refused && (try? Data(contentsOf: path)) == Data("font-size = 12\n".utf8) && (try? Data(contentsOf: backup.appendingPathComponent("marker"))) == Data("keep".utf8))
    }
    func testPostCommitFailureInvalidatesSettingsAndRequiresReload() throws {
        let path = try directory().appendingPathComponent("ghostty.conf")
        try Data("font-size = 12\n".utf8).write(to: path)
        var operations = ConfigStore.FileOperations(), committed = false
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            let result = rename(directory, source, target, replace)
            if target == "ghostty.conf", result == 0 { committed = true }
            return result
        }
        operations.sync = { fd in
            var identity = stat()
            if committed, fstat(fd, &identity) == 0, identity.st_mode & S_IFMT == S_IFDIR { errno = EIO; return -1 }
            return fsync(fd)
        }
        let store = SettingsStore(terminalConfigPath: path.path, operations: operations)
        var commitReported = false, secondRefused = false, readRefused = false
        do { try store.saveTerminalConfig("font-size = 18\n") }
        catch { if case ConfigStoreError.committed = error { commitReported = true } }
        do { try store.saveTerminalConfig("font-size = 30\n") } catch { secondRefused = true }
        do { _ = try store.rawConfig() } catch { readRefused = true }
        let errorVisible = store.loadError != nil && store.loadedText == nil
        let actual = try store.reload()
        XCTAssertTrue(commitReported && secondRefused && readRefused && errorVisible && actual == "font-size = 18\n" && (try? Data(contentsOf: path)) == Data(actual.utf8))
    }
    func testSymlinkCannotBeReadAsASettingsTarget() throws {
        let root = try directory(), target = root.appendingPathComponent("external.conf"), link = root.appendingPathComponent("ghostty.conf")
        try Data("# original target\n".utf8).write(to: target)
        try FileManager.default.createSymbolicLink(at: link, withDestinationURL: target)
        let store = SettingsStore(terminalConfigPath: link.path)
        var rejected = false
        do { try store.saveTerminalConfig("font-size = 20\n") } catch { rejected = true }
        XCTAssertTrue(rejected && store.loadError != nil && (try? Data(contentsOf: target)) == Data("# original target\n".utf8))
    }
    func testScalarUpdatesLastDeclarationAndDeletingRemovesEveryLocalOverride() {
        let original = "# keep\r\nfont-size = 12\r\nunknown = preserve\r\n\tfont-size = 18\r\n"
        let replaced = SettingsStore.merging(original, edits: [("font-size", "24")])
        let removed = SettingsStore.merging(original, edits: [("font-size", nil)])
        XCTAssertTrue(replaced == "# keep\r\nfont-size = 12\r\nunknown = preserve\r\nfont-size = 24\r\n" && SettingsStore.value(of: "font-size", in: replaced) == "24" && removed == "# keep\r\nunknown = preserve\r\n")
    }
    func testEmptyRepeatableDeclarationIsRecognizedAndFontReplacementRemovesLocalFallbacks() {
        let original = "font-family = \nfont-family = Menlo\nfont-family = Monaco\nkeybind = a=b\nkeybind = c=d\n"
        let result = TerminalFontSelection.replacing(in: original, family: "Menlo")
        XCTAssertTrue(SettingsStore.value(of: "font-family", in: "font-family = \n") == "" && result == "keybind = a=b\nkeybind = c=d\nfont-family = \"Menlo\"\n")
    }
}
