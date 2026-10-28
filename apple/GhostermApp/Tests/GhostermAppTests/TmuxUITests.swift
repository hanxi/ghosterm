import AppKit
import Darwin
import Foundation
import GhostermCore
import XCTest
@testable import GhostermAppModel

@MainActor final class TmuxUITests: XCTestCase {
    private func until(_ predicate: () -> Bool, timeout: TimeInterval = 12, file: StaticString = #filePath, line: UInt = #line) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), file: file, line: line)
    }
    private func fixture(delayCreate: Bool = false) throws -> (URL, String, String) {
        _ = NSApplication.shared; NSApplication.shared.setActivationPolicy(.accessory)
        let root = sessionTestDirectory(); try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let env = ProcessInfo.processInfo.environment
        let binary = env["TMUX_BINARY"] ?? "/opt/homebrew/bin/tmux"
        guard FileManager.default.isExecutableFile(atPath: binary) else { throw GhostermError.failed("tmux >=3.6a is required for TmuxUITests; set TMUX_BINARY") }
        let config = root.appendingPathComponent("tmux.conf"), wrapper = root.appendingPathComponent("tmux")
        try "set -g default-shell /bin/sh\nset -g default-command \"exec /bin/sh\"\nset -g status off\nset -g assume-paste-time 0\nset -g destroy-unattached off\nset -s exit-unattached off\n".write(to: config, atomically: true, encoding: .utf8)
        func quote(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'" }
        let command = quote(binary) + " -f " + quote(config.path) + " \"$@\""
        let script = delayCreate ? "#!/bin/sh\ncase \"$*\" in *new-session*) \(command); sleep 30; exit;; esac\nexec \(command)\n" : "#!/bin/sh\nexec \(command)\n"
        try script.write(to: wrapper, atomically: true, encoding: .utf8); try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: wrapper.path)
        let socket = root.appendingPathComponent("socket").path
        addTeardownBlock {
            let process = Process(); process.executableURL = URL(fileURLWithPath: binary)
            process.arguments = ["-S", socket, "-N", "kill-server"]; process.standardError = FileHandle.nullDevice; process.standardOutput = FileHandle.nullDevice
            try? process.run(); process.waitUntilExit()
            try? FileManager.default.removeItem(at: root)
        }
        return (root, wrapper.path, socket)
    }
    private func chooser(_ model: AppModel, exe: String, socket: String) -> TmuxChooser {
        let chooser = TmuxChooser(model: model, target: .localShell, options: .init(executable: exe, socketPath: socket))
        model.tmuxChooser = chooser; chooser.refresh(); return chooser
    }
    private func pid(_ pane: TerminalPane, prefix: String) -> Int32? {
        pane.surface?.readText()?.components(separatedBy: .newlines).compactMap { line in
            let text = line.trimmingCharacters(in: .whitespaces)
            return text.hasPrefix(prefix) ? Int32(text.dropFirst(prefix.count)) : nil
        }.first
    }
    func testChooserCreatesNewPaneAndExplicitReconnectPreservesTask() async throws {
        let (_, exe, socket) = try fixture()
        let model = isolatedAppModel(); defer { model.dismissTmux(); model.disconnectForTesting() }
        model.connect(to: .localShell)
        let original = try XCTUnwrap(model.activePane)
        original.surface?.sendCommittedText("printf 'ORDINARY:%s\\n' \"$$\"\r")
        try await until { self.pid(original, prefix: "ORDINARY:") != nil }
        let originalPID = pid(original, prefix: "ORDINARY:")
        let browser = chooser(model, exe: exe, socket: socket)
        try await until { !browser.busy }; XCTAssertFalse(browser.requiresRefresh)
        XCTAssertTrue(browser.sessions.isEmpty)
        browser.change(.create, name: "中文 #{pid};")
        try await until { !browser.busy }; XCTAssertEqual(browser.sessions.map(\.name), ["中文 #{pid};"])
        browser.selectedID = browser.sessions.first?.id; browser.connectSelected()
        try await until { model.tmuxChooser == nil && model.tabs.tabs.count == 2 }
        let background = try XCTUnwrap(model.activePane), tab = try XCTUnwrap(model.tabs.activeTab)
        XCTAssertTrue(background !== original); XCTAssertEqual(pid(original, prefix: "ORDINARY:"), originalPID)
        XCTAssertNotNil(background.tmuxContext); XCTAssertEqual(background.childState, .unknown)
        XCTAssertFalse(background.closeNeedsConfirmation)
        background.surface?.sendCommittedText("printf 'BACKGROUND:%s\\n' \"$$\"\r")
        try await until { self.pid(background, prefix: "BACKGROUND:") != nil }
        let taskPID = try XCTUnwrap(pid(background, prefix: "BACKGROUND:"))
        XCTAssertEqual(Darwin.kill(taskPID, 0), 0)
        background.surface?.sendCommittedText("\u{2}d")
        try await until { background.status.isExited }
        XCTAssertTrue(model.tabs.tabs.contains { $0 === tab }, "detach keeps a disconnected pane")
        XCTAssertEqual(Darwin.kill(taskPID, 0), 0, "client EOF is not task exit")
        model.showTmux(for: .localShell, replacing: background)
        let cancelled = try XCTUnwrap(model.tmuxChooser); try await until { !cancelled.busy }
        XCTAssertNil(cancelled.selectedID); XCTAssertTrue(background.status.isExited)
        model.dismissTmux(); XCTAssertTrue(background.status.isExited)
        model.showTmux(for: .localShell, replacing: background)
        let fresh = try XCTUnwrap(model.tmuxChooser); try await until { !fresh.busy }
        XCTAssertNil(fresh.selectedID, "reconnect must never use the old selection automatically")
        fresh.selectedID = fresh.sessions.first?.id; fresh.connectSelected()
        try await until { background.status.isReady && model.tmuxChooser == nil }
        XCTAssertEqual(model.tabs.tabs.count, 2); XCTAssertTrue(tab.activePane === background)
        background.surface?.sendCommittedText("printf 'REJOIN:%s\\n' \"$$\"\r")
        try await until { self.pid(background, prefix: "REJOIN:") != nil }
        XCTAssertEqual(pid(background, prefix: "REJOIN:"), taskPID)
        model.tabs.close(tab)
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(Darwin.kill(taskPID, 0), 0, "closing a tmux pane only closes the client")
        original.surface?.sendCommittedText("printf 'unaffected-%s\\n' alive\r")
        try await until { original.surface?.readText()?.contains("unaffected-alive") == true }
    }
    func testUnknownCreateRequiresRefreshWithoutReplay() async throws {
        let (_, exe, socket) = try fixture(delayCreate: true)
        let model = isolatedAppModel(); defer { model.dismissTmux(); model.disconnectForTesting() }
        let browser = chooser(model, exe: exe, socket: socket)
        try await until { !browser.busy }
        browser.change(.create, name: "created-once")
        try await until { !browser.busy }
        XCTAssertTrue(browser.message.contains("结果未知")); XCTAssertTrue(browser.requiresRefresh)
        XCTAssertFalse(browser.canOperate); XCTAssertTrue(browser.sessions.isEmpty)
        browser.refresh(); try await until { !browser.busy }
        XCTAssertEqual(browser.sessions.map(\.name), ["created-once"], "refresh observes the result, never repeats create")
        XCTAssertNil(browser.selectedID)
    }
    func testChangedEndpointRequiresFreshSelection() async throws {
        let (_, exe, socket) = try fixture()
        let model = isolatedAppModel(); defer { model.dismissTmux(); model.disconnectForTesting() }
        let browser = chooser(model, exe: exe, socket: socket); try await until { !browser.busy }
        browser.change(.create, name: "old-endpoint"); try await until { !browser.busy }
        browser.selectedID = browser.sessions.first?.id
        XCTAssertTrue(browser.canOperate)
        browser.socketPath = socket + "-another"
        XCTAssertTrue(browser.requiresRefresh); XCTAssertFalse(browser.canOperate)
        XCTAssertNil(browser.selectedID); XCTAssertTrue(browser.sessions.isEmpty)
        browser.connectSelected(); XCTAssertTrue(model.tabs.tabs.isEmpty)
    }
    func testCancellingChooserDoesNotReplaceRunningPane() async throws {
        let (_, exe, socket) = try fixture()
        let model = isolatedAppModel(); defer { model.dismissTmux(); model.disconnectForTesting() }
        model.connect(to: .localShell); let pane = try XCTUnwrap(model.activePane)
        let browser = chooser(model, exe: exe, socket: socket); try await until { !browser.busy }
        model.dismissTmux()
        XCTAssertNil(model.tmuxChooser); XCTAssertTrue(model.activePane === pane)
        XCTAssertTrue(pane.isRunning); XCTAssertEqual(model.tabs.tabs.count, 1)
        model.showTmux(for: .localShell, replacing: pane)
        XCTAssertNil(model.tmuxChooser, "a running pane cannot be overwritten by a chooser")
    }
}
