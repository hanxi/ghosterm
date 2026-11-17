import AppKit
import Darwin
import Foundation
import GhostermTerminal
import XCTest
@testable import GhostermAppModel

@MainActor
final class TerminalConfigurationCommitTests: XCTestCase {
    private func fixture(_ text: String = "font-size = 8\n", operations: ConfigStore.FileOperations = .init()) throws -> (AppModel, URL, URL) {
        TestWindows.enterAutomationMode()
        let root = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("config-candidate-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: root) }
        let path = root.appendingPathComponent("ghostty.conf")
        try Data(text.utf8).write(to: path)
        let model = isolatedAppModel(configPath: root.appendingPathComponent("ssh_config").path,
            knownHostsPath: root.appendingPathComponent("known_hosts").path,
            settings: SettingsStore(terminalConfigPath: path.path, operations: operations))
        return (model, path, root)
    }
    func testPreparationDoesNotApplyAndRetainedCandidateUpdatesExistingSurfaceWithoutReread() throws {
        let (model, path, _) = try fixture()
        let app = try XCTUnwrap(model.terminalApp)
        let surface = try TerminalSurface(app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        surface.setSize(width: 800, height: 400)
        let before = surface.gridSize.cols
        let candidate = try app.prepareConfiguration(text: "font-size = 24\nbackground = #123456\n", origin: path.path)
        let unchanged = app.float("font-size") == 8 && surface.gridSize.cols == before
        try Data("bogus-candidate-key = 1\n".utf8).write(to: path)
        var applied = false
        do { try candidate.apply(); applied = true } catch {}
        surface.setSize(width: 800, height: 400)
        var consumed = false
        do { try candidate.apply() } catch { consumed = true }
        XCTAssertTrue(unchanged && applied && consumed && app.float("font-size") == 24 && surface.gridSize.cols < before && app.color("background")?.r == 0x12)
    }
    func testInvalidSparseAndFullCandidatesCannotWriteTargetOrBackupOrChangeSurface() throws {
        let (model, path, _) = try fixture()
        let backup = URL(fileURLWithPath: path.path + ".bak")
        try Data("previous backup".utf8).write(to: backup)
        let app = try XCTUnwrap(model.terminalApp)
        let surface = try TerminalSurface(app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        surface.setSize(width: 800, height: 400)
        let before = surface.gridSize.cols, revision = model.terminalConfigRevision
        var refused = 0
        do { try model.applyTerminalSettings([("font-size", "not-a-number")]) } catch { refused += 1 }
        do { try model.replaceTerminalConfig("font-size = 24\nbogus-candidate-key = 1\n") } catch { refused += 1 }
        XCTAssertTrue(refused == 2 && (try? Data(contentsOf: path)) == Data("font-size = 8\n".utf8) && (try? Data(contentsOf: backup)) == Data("previous backup".utf8) && app.float("font-size") == 8 && surface.gridSize.cols == before && model.terminalConfigRevision == revision)
    }
    func testSuccessfulSparseCommitChangesDiskBackupAndExistingSurfaceTogether() throws {
        let original = "# keep\r\nfont-size = 8\r\nbackground = #112233\r\n"
        let (model, path, _) = try fixture(original)
        let app = try XCTUnwrap(model.terminalApp)
        let surface = try TerminalSurface(app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        surface.setSize(width: 800, height: 400)
        let before = surface.gridSize.cols, revision = model.terminalConfigRevision
        var succeeded = false
        do { try model.applyTerminalSettings([("font-size", "24"), ("background", "#654321")]); succeeded = true } catch {}
        surface.setSize(width: 800, height: 400)
        let actual = try? String(contentsOf: path, encoding: .utf8)
        XCTAssertTrue(succeeded && model.terminalConfigRevision == revision + 1 && model.terminalConfigError == nil && actual == "# keep\r\nfont-size = 24\r\nbackground = #654321\r\n" && (try? Data(contentsOf: URL(fileURLWithPath: path.path + ".bak"))) == Data(original.utf8) && app.float("font-size") == 24 && app.color("background")?.r == 0x65 && surface.gridSize.cols < before)
    }
    func testBackupRefusalCannotApplyPreparedConfiguration() throws {
        let (model, path, _) = try fixture()
        let backup = URL(fileURLWithPath: path.path + ".bak")
        try FileManager.default.createDirectory(at: backup, withIntermediateDirectories: false)
        try Data("keep".utf8).write(to: backup.appendingPathComponent("marker"))
        let revision = model.terminalConfigRevision
        var refused = false
        do { try model.replaceTerminalConfig("font-size = 24\n") } catch { refused = true }
        XCTAssertTrue(refused && model.terminalApp?.float("font-size") == 8 && model.terminalConfigRevision == revision && (try? Data(contentsOf: path)) == Data("font-size = 8\n".utf8) && (try? Data(contentsOf: backup.appendingPathComponent("marker"))) == Data("keep".utf8))
    }
    func testPostCommitSyncFailureReportsSavedEffectWithoutApplyingCandidate() throws {
        var operations = ConfigStore.FileOperations(), committed = false, fail = true
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            let result = rename(directory, source, target, replace)
            if target == "ghostty.conf", result == 0 { committed = true }
            return result
        }
        operations.sync = { fd in
            var info = stat()
            if committed && fail, fstat(fd, &info) == 0, info.st_mode & S_IFMT == S_IFDIR { errno = EIO; return -1 }
            return fsync(fd)
        }
        let (model, path, _) = try fixture(operations: operations)
        var applications = 0, reported = false
        model.applyPreparedTerminalConfiguration = { try $0.apply(); applications += 1 }
        do { try model.replaceTerminalConfig("font-size = 24\n") }
        catch { if case ConfigStoreError.committed = error { reported = true } }
        let protected = reported && applications == 0 && model.settings.loadError != nil && model.terminalConfigError?.contains("未应用") == true && model.terminalApp?.float("font-size") == 8 && (try? Data(contentsOf: path)) == Data("font-size = 24\n".utf8)
        fail = false
        try model.readTerminalConfigFromDisk()
        try model.replaceTerminalConfig("font-size = 24\n")
        XCTAssertTrue(protected && applications == 1 && model.terminalApp?.float("font-size") == 24 && model.terminalConfigError == nil)
    }
    func testApplyFailureReportsCommittedFileAndExplicitRetryClearsError() throws {
        let (model, path, _) = try fixture()
        model.applyPreparedTerminalConfiguration = { _ in throw NSError(domain: "owned engine apply failure", code: 1) }
        var reported = false
        do { try model.replaceTerminalConfig("font-size = 24\n") }
        catch { if case SettingsStoreError.committed = error { reported = true } }
        let preserved = reported && model.terminalConfigError?.contains("磁盘已保存") == true && model.terminalApp?.float("font-size") == 8 && (try? Data(contentsOf: path)) == Data("font-size = 24\n".utf8) && model.terminalConfigText == "font-size = 24\n"
        model.applyPreparedTerminalConfiguration = { try $0.apply() }
        try model.replaceTerminalConfig("font-size = 24\n")
        XCTAssertTrue(preserved && model.terminalConfigError == nil && model.terminalApp?.float("font-size") == 24)
    }
    func testOldEditorSnapshotCannotOverwriteAnEarlierSuccessfulControlEdit() throws {
        let (model, path, _) = try fixture()
        let old = Data(model.terminalConfigText.utf8)
        try model.applyTerminalSettings([("font-size", "19")])
        var refused = false
        do { try model.replaceTerminalConfig("font-size = 30\n", expectedConfiguration: old) } catch { refused = true }
        XCTAssertTrue(refused && model.terminalApp?.float("font-size") == 19 && (try? Data(contentsOf: path)) == Data("font-size = 19\n".utf8))
    }
    func testRelativeRecursiveIncludeIsAppliedAndRetainedAfterDependencyChangesDuringCommit() throws {
        var operations = ConfigStore.FileOperations(); var child: URL?
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            let result = rename(directory, source, target, replace)
            if target == "ghostty.conf", result == 0, let child { try? Data("invalid-child-key = 1\n".utf8).write(to: child) }
            return result
        }
        let (model, path, root) = try fixture(operations: operations)
        let nested = root.appendingPathComponent("nested")
        try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: false)
        child = nested.appendingPathComponent("child.conf")
        try Data("font-size = 23\nbackground = #123456\n".utf8).write(to: child!)
        var succeeded = false
        do { try model.replaceTerminalConfig("font-size = 8\nconfig-file = nested/child.conf\n"); succeeded = true } catch {}
        XCTAssertTrue(succeeded && model.terminalApp?.float("font-size") == 23 && model.terminalApp?.color("background")?.r == 0x12 && model.terminalConfigError == nil && (try? String(contentsOf: child!, encoding: .utf8)) == "invalid-child-key = 1\n" && (try? Data(contentsOf: path)) == Data("font-size = 8\nconfig-file = nested/child.conf\n".utf8))
    }
    func testMissingRequiredIncludeFailsWhileOptionalAbsenceIsExplicitlyAllowed() throws {
        let (model, path, _) = try fixture()
        var rejected = false
        do { try model.replaceTerminalConfig("config-file = absent.conf\nfont-size = 24\n") } catch { rejected = true }
        let unchanged = rejected && (try? Data(contentsOf: path)) == Data("font-size = 8\n".utf8) && model.terminalApp?.float("font-size") == 8
        var optionalSucceeded = false
        do { try model.replaceTerminalConfig("config-file = ?absent.conf\nfont-size = 24\n"); optionalSucceeded = true } catch {}
        XCTAssertTrue(unchanged && optionalSucceeded && model.terminalApp?.float("font-size") == 24)
    }
    func testDirectSymlinkAndHardlinkRootCyclesCannotValidateOldRootBytes() throws {
        let (model, path, root) = try fixture()
        let symbolic = root.appendingPathComponent("symbolic.conf"), hard = root.appendingPathComponent("hard.conf")
        try FileManager.default.createSymbolicLink(at: symbolic, withDestinationURL: path)
        try FileManager.default.linkItem(at: path, to: hard)
        var refused = 0
        for reference in ["ghostty.conf", "symbolic.conf", "hard.conf"] {
            do { try model.replaceTerminalConfig("font-size = 24\nconfig-file = \(reference)\n") } catch { refused += 1 }
        }
        XCTAssertTrue(refused == 3 && model.terminalApp?.float("font-size") == 8 && (try? Data(contentsOf: path)) == Data("font-size = 8\n".utf8) && !FileManager.default.fileExists(atPath: path.path + ".bak"))
    }
    func testInvalidDarkThemeBranchIsRejectedBeforeAnyCommit() throws {
        let (model, path, root) = try fixture()
        let theme = root.appendingPathComponent("light.theme")
        try Data("background = #123456\n".utf8).write(to: theme)
        let text = "theme = light:\"\(theme.path)\",dark:\"\(root.path)/missing.theme\"\n"
        var refused = false
        do { try model.replaceTerminalConfig(text) } catch { refused = true }
        XCTAssertTrue(refused && (try? Data(contentsOf: path)) == Data("font-size = 8\n".utf8) && model.terminalApp?.float("font-size") == 8 && !FileManager.default.fileExists(atPath: path.path + ".bak"))
    }
    func testStartupAppliesRecursiveConfigurationFromTheLoadedSnapshot() throws {
        let (initial, path, root) = try fixture()
        let before = initial.terminalApp?.float("font-size") == 8
        try Data("font-size = 21\n".utf8).write(to: root.appendingPathComponent("child.conf"))
        let text = "font-size = 8\nconfig-file = child.conf\n"
        try Data(text.utf8).write(to: path)
        let reopened = isolatedAppModel(configPath: root.appendingPathComponent("other_config").path,
            knownHostsPath: root.appendingPathComponent("other_hosts").path,
            settings: SettingsStore(terminalConfigPath: path.path))
        XCTAssertTrue(before && reopened.terminalConfigError == nil && reopened.terminalApp?.float("font-size") == 21 && reopened.terminalConfigText == text)
    }
    func testCAbiOriginAndTextCannotBeTruncatedAtNul() throws {
        let (model, path, _) = try fixture()
        let app = try XCTUnwrap(model.terminalApp)
        var rejected = 0
        do { _ = try app.prepareConfiguration(text: "font-size = 24\0\n", origin: path.path) } catch { rejected += 1 }
        do { _ = try app.prepareConfiguration(text: "font-size = 24\n", origin: path.path + "\0ignored") } catch { rejected += 1 }
        XCTAssertTrue(rejected == 2 && app.float("font-size") == 8 && (try? Data(contentsOf: path)) == Data("font-size = 8\n".utf8))
    }
    func testReadbackUsesAppliedEngineUntilExplicitApplyAfterFileReload() throws {
        let (model, path, _) = try fixture()
        try Data("font-size = 19\n".utf8).write(to: path)
        try model.readTerminalConfigFromDisk()
        let notApplied = model.terminalConfigText == "font-size = 19\n" && model.terminalSetting("font-size").flatMap(Double.init) == 8
        try model.replaceTerminalConfig("font-size = 19\n")
        XCTAssertTrue(notApplied && model.terminalSetting("font-size").flatMap(Double.init) == 19)
    }
    func testSparseControlsCannotSilentlyEditAShadowedRootDeclaration() throws {
        let (model, path, root) = try fixture()
        try Data("font-size = 23\n".utf8).write(to: root.appendingPathComponent("child.conf"))
        let text = "font-size = 8\nconfig-file = child.conf\n"
        try model.replaceTerminalConfig(text)
        var refused = false
        do { try model.applyTerminalSettings([("font-size", "24")]) } catch { refused = true }
        XCTAssertTrue(refused && model.terminalSetting("font-size").flatMap(Double.init) == 23 && (try? Data(contentsOf: path)) == Data(text.utf8))
    }
    func testConditionalReadbackTracksActualSurfaceThemeState() throws {
        let (model, path, root) = try fixture()
        let light = root.appendingPathComponent("light.theme"), dark = root.appendingPathComponent("dark.theme")
        try Data("font-size = 8\nbackground = #112233\n".utf8).write(to: light)
        try Data("font-size = 24\nbackground = #654321\n".utf8).write(to: dark)
        let app = try XCTUnwrap(model.terminalApp)
        app.setColorScheme(dark: false)
        defer { app.setColorScheme(dark: false) }
        let candidate = try app.prepareConfiguration(text: "theme = light:\"\(light.path)\",dark:\"\(dark.path)\"\n", origin: path.path)
        try candidate.apply()
        let surface = try TerminalSurface(app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
        surface.setSize(width: 800, height: 400)
        let lightCols = surface.gridSize.cols
        let wasLight = app.float("font-size") == 8
        app.setColorScheme(dark: true)
        surface.setSize(width: 800, height: 400)
        XCTAssertTrue(wasLight && app.float("font-size") == 24 && model.terminalSetting("font-size").flatMap(Double.init) == 24 && surface.gridSize.cols < lightCols)
    }
    func testNativeApplyFailureBookkeepingIsChecked() throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let process = Process(), output = Pipe()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["python3", root.appendingPathComponent("scripts/test-macos-config-candidate.py").path]
        process.currentDirectoryURL = root
        process.standardOutput = output; process.standardError = output
        try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
        let log = String(decoding: data, as: UTF8.self)
        print(log)
        XCTAssertTrue(process.terminationReason == .exit && process.terminationStatus == 0 && log.contains("NATIVE_CONFIG_PROBE_COMPILED") && log.contains("PASS native config apply bookkeeping"))
    }
}
