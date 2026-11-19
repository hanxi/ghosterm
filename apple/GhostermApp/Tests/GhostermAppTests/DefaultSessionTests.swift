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
        XCTAssertTrue(panes.allSatisfy { $0.isRunning })
        for pane in panes { pane.surface?.sendCommittedText("printf 'LOCAL-PID:%s\\n' \"$$\"\r") }
        try await until { panes.allSatisfy { self.pid($0) != nil } }
        let processes = panes.compactMap(pid)
        XCTAssertEqual(Set(processes).count, 6)
        for tab in model.tabs.tabs { model.tabs.close(tab) }
        try await until { processes.allSatisfy { Darwin.kill($0, 0) == -1 && errno == ESRCH } }
        XCTAssertTrue(panes.allSatisfy { !$0.isRunning })
    }
    func testPassiveDiscoveryKeepsOrdinaryPTYOutput() async throws {
        try await verifyPassiveDiscovery(ending: .exit)
    }

    func testPassiveDiscoveryExpiresWhileTerminalIsIdle() async throws {
        try XCTSkipUnless(ExtensionRuntime.supportStatus == 0)
        try await verifyPassiveDiscovery(ending: .idle)
    }

    func testPassiveDiscoveryIsClearedWhenPaneFails() async throws {
        try XCTSkipUnless(ExtensionRuntime.supportStatus == 0)
        try await verifyPassiveDiscovery(ending: .failure)
    }

    func testPassiveDiscoveryChangesOwnerWhenPaneReattaches() async throws {
        try XCTSkipUnless(ExtensionRuntime.supportStatus == 0)
        try await verifyPassiveDiscovery(ending: .reattach)
    }

    func testLateTerminalExitDoesNotInvalidateReplacementCandidates() async throws {
        try XCTSkipUnless(ExtensionRuntime.supportStatus == 0)
        _ = NSApplication.shared
        let model = isolatedAppModel()
        defer { model.disconnectForTesting() }
        model.preferences.defaultShell = "/bin/sh"
        model.connect(to: .localShell)
        let pane = try XCTUnwrap(model.activePane)
        let app = try XCTUnwrap(model.terminalApp)

        let oldSurface = try TerminalSurface(app: app)
        let oldSession = try SessionController.localShell(surface: oldSurface, executable: "/bin/sh", kind: .shell)
        defer { oldSession.stop() }
        pane.attach(surface: oldSurface, session: oldSession)
        pane.start()
        let deliverOldState = try XCTUnwrap(oldSession.onStateChange)

        let newSurface = try TerminalSurface(app: app)
        let newSession = try SessionController.localShell(surface: newSurface, executable: "/bin/sh", kind: .shell)
        pane.attach(surface: newSurface, session: newSession)
        pane.start()
        try await sendDiscoveryOffer(to: newSurface)
        try await until { !model.frontendCandidates.isEmpty }
        let candidate = try XCTUnwrap(model.frontendCandidates.first)

        withExtendedLifetime(oldSession) {
            deliverOldState(.exited(code: 0))
        }
        guard case .ready = pane.status,
              pane.isRunning, model.activePane === pane,
              model.frontendCandidates.map(\.id) == [candidate.id] else {
            return XCTFail("late exit from the old terminal must leave the replacement and its candidate intact")
        }
    }

    func testRuntimeFailureClearsOnlyItsOwnPassiveCandidates() async throws {
        try XCTSkipUnless(ExtensionRuntime.supportStatus == 0)
        _ = NSApplication.shared
        let model = isolatedAppModel()
        defer { model.disconnectForTesting() }
        model.preferences.defaultShell = "/bin/sh"
        model.connect(to: .localShell)
        let unaffected = try XCTUnwrap(model.activePane)
        try await sendDiscoveryOffer(to: XCTUnwrap(unaffected.surface))
        try await until { model.frontendCandidates.contains { $0.sourceID == unaffected.id } }
        let otherID = try XCTUnwrap(model.frontendCandidates.first?.id)

        model.connect(to: .localShell)
        let pane = try XCTUnwrap(model.activePane)
        XCTAssertFalse(pane === unaffected)
        let surface = try TerminalSurface(app: XCTUnwrap(model.terminalApp))
        let session = try SessionController.localShell(surface: surface, executable: "/bin/sh", kind: .shell)
        pane.attach(surface: surface, session: session)
        pane.start()
        try await sendDiscoveryOffer(to: surface)
        try await until { model.frontendCandidates.contains { $0.sourceID == pane.id } }

        let report = try XCTUnwrap(session.onStateChange)
        session.stop()
        report(.failed("Fixture transport failure"))
        XCTAssertEqual(model.frontendCandidates.map(\.id), [otherID],
                       "runtime transport failure must remove only its pane's candidates")
        guard case .failed("Fixture transport failure") = pane.status else {
            return XCTFail("runtime error must remain visible in its pane")
        }
        XCTAssertTrue(model.activePane === pane)
        XCTAssertTrue(unaffected.isRunning)
        XCTAssertNil(model.extensionRun)
    }

    private enum DiscoveryEnding { case exit, idle, failure, reattach }

    private func sendDiscoveryOffer(to surface: TerminalSurface) async throws {
        surface.sendCommittedText("stty -echo; PS1=''; printf '\\033[2J\\033[HPTY_%s' READY\r")
        try await until { surface.readText()?.contains("PTY_READY") == true }
        let payload = "EREREREREREREREREREREaqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqAAsvd29yay9mcm9udA=="
        var nested = try XCTUnwrap(Data(base64Encoded: payload))
        nested.replaceSubrange(0..<16, with: Data(repeating: 0x22, count: 16))
        let nestedOffer = "\\033]777;ghosterm-discovery;1;\(nested.base64EncodedString())\\007"
        let offer = "\\033]777;ghosterm-discovery;1;\(payload)\\007"
        surface.sendCommittedText("printf '%b%s' '\\033P\(nestedOffer)\\033\\0134\(offer)\\033[2J\\033[HPTY_' OUTPUT\r")
        try await until { surface.readText()?.contains("PTY_OUTPUT") == true }
    }

    private func verifyPassiveDiscovery(ending: DiscoveryEnding) async throws {
        _ = NSApplication.shared
        let model = isolatedAppModel(); defer { model.disconnectForTesting() }
        model.preferences.defaultShell = "/bin/sh"
        model.connect(to: .localShell)
        let pane = try XCTUnwrap(model.activePane)
        let surface = try XCTUnwrap(pane.surface)
        try await sendDiscoveryOffer(to: surface)
        guard ExtensionRuntime.supportStatus == 0 else {
            XCTAssertTrue(model.frontendCandidates.isEmpty)
            return
        }
        let deadline = ProcessInfo.processInfo.systemUptime + 8
        while model.frontendCandidates.isEmpty, ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(for: .milliseconds(20))
        }
        XCTAssertFalse(model.frontendCandidates.isEmpty, "top-level discovery must arrive after ordinary PTY output")
        guard !model.frontendCandidates.isEmpty else { return }
        XCTAssertEqual(model.frontendCandidates.map(\.offer.instance), [String(repeating: "1", count: 32)])
        XCTAssertEqual(model.frontendCandidates.first?.sourceID, pane.id)
        XCTAssertNil(model.extensionRun)
        if ending == .idle {
            let candidate = try XCTUnwrap(model.frontendCandidates.first)
            let clock = ProcessInfo.processInfo
            let earlyDelay = max(0, candidate.expiresAt - clock.systemUptime - 2)
            try await Task.sleep(for: .seconds(earlyDelay))
            XCTAssertEqual(model.frontendCandidates.map(\.id), [candidate.id])
            while !model.frontendCandidates.isEmpty, clock.systemUptime < candidate.expiresAt + 4 {
                try await Task.sleep(for: .milliseconds(20))
            }
            XCTAssertTrue(model.frontendCandidates.isEmpty, "idle passive candidate must expire without another offer")
            XCTAssertTrue(pane.isRunning)
            XCTAssertTrue(surface.readText()?.contains("PTY_OUTPUT") == true)
            return
        }
        if ending == .reattach {
            let original = try XCTUnwrap(model.frontendCandidates.first)
            surface.sendCommittedText("printf '\\nLOCAL-PID:%s\\n' \"$$\"\r")
            try await until { self.pid(pane) != nil }
            let oldProcess = try XCTUnwrap(pid(pane))
            let replacement = try TerminalSurface(app: XCTUnwrap(model.terminalApp))
            let session = try SessionController.localShell(
                surface: replacement, executable: "/bin/sh", cwd: NSHomeDirectory(), kind: .shell)
            pane.attach(surface: replacement, session: session)
            XCTAssertTrue(model.frontendCandidates.isEmpty, "reattaching a pane must immediately remove its old candidates")
            pane.start()
            try await sendDiscoveryOffer(to: replacement)
            try await until { model.frontendCandidates.contains { $0.id != original.id } }
            let current = try XCTUnwrap(model.frontendCandidates.first)
            XCTAssertEqual(model.frontendCandidates.count, 1)
            XCTAssertEqual(current.sourceID, original.sourceID)
            XCTAssertEqual(current.offer.instance, original.offer.instance)
            try await until { Darwin.kill(oldProcess, 0) == -1 && errno == ESRCH }
            XCTAssertTrue(pane.isRunning)
            return
        }
        if ending == .failure {
            pane.markFailed("Fixture connection failure")
            XCTAssertTrue(model.frontendCandidates.isEmpty, "failed pane must immediately remove its passive candidates")
            XCTAssertFalse(pane.isRunning)
            XCTAssertTrue(model.tabs.tabs.contains { $0.panes.contains { $0 === pane } })
            guard case .failed("Fixture connection failure") = pane.status else {
                return XCTFail("pane must retain its connection error")
            }
            return
        }
        surface.sendCommittedText("exit\r")
        try await until { !pane.isRunning || model.frontendCandidates.isEmpty }
        XCTAssertTrue(model.frontendCandidates.isEmpty, "terminal exit must remove its passive discovery candidates")
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
        XCTAssertTrue(next.isRunning)
        next.surface?.sendCommittedText("printf 'LOCAL-PID:%s\\n' \"$$\"\r")
        try await until { self.pid(next) != nil }
        XCTAssertEqual(next.currentDirectory, directory.path)
        XCTAssertNotEqual(pid(next), process); XCTAssertEqual(pid(original), process)
        original.surface?.sendCommittedText("sleep 20\r")
        try await until { original.childState == .job }
        XCTAssertFalse(model.shouldTerminate(), "普通终端前台任务必须保留退出确认")
    }
}
