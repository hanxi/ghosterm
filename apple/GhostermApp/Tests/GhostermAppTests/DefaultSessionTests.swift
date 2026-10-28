import AppKit
import Darwin
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel

@MainActor final class DefaultSessionTests: XCTestCase {
    private func until(_ predicate: () -> Bool, file: StaticString = #filePath, line: UInt = #line) async throws {
        let deadline = Date().addingTimeInterval(8)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), file: file, line: line)
    }
    private func pid(_ pane: TerminalPane) -> Int32? {
        pane.surface?.readText()?.components(separatedBy: .newlines).compactMap { line in
            let value = line.trimmingCharacters(in: .whitespaces)
            return value.hasPrefix("LOCAL-PID:") ? Int32(value.dropFirst(10)) : nil
        }.first
    }
    func testOrdinaryLocalSessionsHaveIndependentPTYsAndCloseStopsProcesses() async throws {
        _ = NSApplication.shared
        let model = isolatedAppModel(); defer { model.disconnectForTesting() }
        let shell = model.preferences.defaultShell; model.preferences.defaultShell = "/bin/sh"
        defer { model.preferences.defaultShell = shell }
        for _ in 0..<6 { model.connect(to: .localShell) }
        let panes = model.tabs.tabs.flatMap(\.panes)
        XCTAssertEqual(panes.count, 6)
        XCTAssertTrue(panes.allSatisfy { $0.isRunning && $0.sessionRecord == nil })
        for pane in panes { pane.surface?.sendCommittedText("printf 'LOCAL-PID:%s\\n' \"$$\"\r") }
        try await until { panes.allSatisfy { self.pid($0) != nil } }
        let processes = panes.compactMap(pid)
        XCTAssertEqual(Set(processes).count, 6)
        for tab in model.tabs.tabs { model.tabs.close(tab) }
        try await until { processes.allSatisfy { Darwin.kill($0, 0) == -1 && errno == ESRCH } }
        XCTAssertTrue(panes.allSatisfy { !$0.isRunning })
    }
    func testStartupIsIdempotentAndDoesNotRestoreOldSessions() async throws {
        _ = NSApplication.shared
        let model = isolatedAppModel(); defer { model.disconnectForTesting() }
        let launch = model.preferences.shellOnLaunch, lock = model.preferences.lockTabsOnLaunch
        model.preferences.shellOnLaunch = true; model.preferences.lockTabsOnLaunch = true
        defer { model.preferences.shellOnLaunch = launch; model.preferences.lockTabsOnLaunch = lock }
        model.startInitialSessions(); model.startInitialSessions()
        XCTAssertEqual(model.tabs.tabs.count, 1)
        XCTAssertEqual(model.tabs.activeTab?.isLocked, true)
        XCTAssertEqual(model.activePane?.isRunning, true)
        XCTAssertNil(model.activePane?.sessionRecord)
        let disabled = isolatedAppModel(); disabled.preferences.shellOnLaunch = false
        disabled.startInitialSessions(); XCTAssertTrue(disabled.tabs.tabs.isEmpty)
    }
    func testDefaultSplitInheritsDirectoryAndKeepsOriginalTask() async throws {
        _ = NSApplication.shared
        let directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let model = isolatedAppModel(); defer { model.disconnectForTesting() }
        model.connect(to: .localShell, cwd: directory.path)
        guard let original = model.activePane else { return XCTFail("missing ordinary pane") }
        original.surface?.sendCommittedText("printf 'LOCAL-PID:%s\\n' \"$$\"\r")
        try await until { self.pid(original) != nil }
        let process = pid(original)
        model.splitActiveTab(axis: .horizontal)
        XCTAssertEqual(model.tabs.activeTab?.panes.count, 2)
        guard let next = model.activePane else { return XCTFail("missing split") }
        XCTAssertTrue(next !== original)
        XCTAssertTrue(next.isRunning); XCTAssertNil(next.sessionRecord)
        next.surface?.sendCommittedText("printf 'LOCAL-PID:%s\\n' \"$$\"\r")
        try await until { self.pid(next) != nil }
        XCTAssertEqual(next.currentDirectory, directory.path)
        XCTAssertNotEqual(pid(next), process); XCTAssertEqual(pid(original), process)
        original.surface?.sendCommittedText("sleep 20\r")
        try await until { original.childState == .job }
        XCTAssertFalse(model.shouldTerminate(), "普通终端前台任务必须保留退出确认")
    }
}
