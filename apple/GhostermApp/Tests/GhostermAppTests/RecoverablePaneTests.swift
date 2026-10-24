import AppKit
import Combine
import Foundation
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermAppModel
@testable import GhostermApp

@MainActor final class RecoverablePaneTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }
    private func until(_ app: TerminalApp, _ predicate: () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(12)
        while !predicate(), Date() < deadline { app.tick(); try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertTrue(predicate(), "pane did not reach its observable state")
    }
    func testRecoveryStatusPreservesUnknownAndRequiresCloseConfirmation() {
        XCTAssertTrue(TerminalPane.closeNeedsConfirmation(status: .recovering("恢复中"), childState: .unknown))
        XCTAssertFalse(TerminalPane.closeNeedsConfirmation(status: .exitedUnknown, childState: .unknown))
        XCTAssertEqual(PaneStatus.exitedUnknown.label, "已结束（退出码未知）")
        XCTAssertEqual(TerminalTab.worst([.ready, .recovering("恢复中")]), .recovering("恢复中"))
        XCTAssertEqual(TerminalTab.worst([.ready, .exitedUnknown]), .exitedUnknown)
    }
    func testRealPaneResumesFromDurableRecordAndRemountsSurface() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let runtime = env["GHOSTERM_REPLICA_RUNTIME"], let helpers = env["GHOSTERM_REPLICA_HELPERS"],
              let hex = env["GHOSTERM_REPLICA_CONTRACT"] else { throw XCTSkip("run native probe --pane") }
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let hash = stride(from: 0, to: hex.count, by: 2).compactMap { n -> UInt8? in
            let a = hex.index(hex.startIndex, offsetBy: n)
            return UInt8(hex[a..<hex.index(a, offsetBy: 2)], radix: 16)
        }
        let directory = URL(fileURLWithPath: runtime).appendingPathComponent("pane-records")
        let nonce = withUnsafeBytes(of: UUID().uuid) { Array($0) }
        let record = SessionRecord(kind: .local, endpoint: .init(proxy: helpers + "/ghosterm-session-proxy", daemon: helpers + "/ghosterm-sessiond", runtime: runtime, contract: hex), nonce: nonce)
        var options = RecoverableSession.Options(kind: .local, proxy: record.endpoint.proxy, daemon: record.endpoint.daemon, runtime: runtime, compatibility: hash, format: 0x47545643,
            create: .init(nonce: nonce, executable: "/bin/sh", cwd: "/", arguments: ["-c", "printf 'PANE-PID:%s\\n' \"$$\"; IFS= read -r one; printf 'FIRST:%s\\n' \"$one\"; IFS= read -r two; printf 'SECOND:%s\\n' \"$two\""], size: .init(cols: 80, rows: 24), direct: true))
        let pane = TerminalPane()
        // Use the same pane identity across both attachments; it remains in the
        // actual production RootView throughout the exchange.
        let model = isolatedAppModel(configPath: runtime + "/ssh-config", knownHostsPath: runtime + "/known_hosts")
        let actualTab = TerminalTab(kind: .localShell, pane: pane)
        model.tabs.add(actualTab)
        let host = makeRootHostingView(model: model, initialShowsSidebar: false)
        host.frame = NSRect(x: 0, y: 0, width: 800, height: 600)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled, .closable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.contentView = host; TestWindows.presentOffscreenAsKey(window)
        defer { pane.stop(); window.orderOut(nil) }
        let configPath = runtime + "/ordered-native-style"
        try "background = #224466\nforeground = #eeccee\nfont-size = 17\npalette = 1=#102030\n".write(toFile: configPath, atomically: true, encoding: .utf8)
        XCTAssertNil(app.reloadConfig(path: configPath))
        defer { _ = app.reloadConfig(path: nil) }
        var publishedBackgrounds: [UInt32] = []
        let publication = pane.$surface.sink { surface in
            if let surface, let g = surface.replicaGeneration, let color = surface.replicaAppearance(generation: g)?.background { publishedBackgrounds.append(color) }
        }
        defer { publication.cancel() }
        try pane.attachRecoverable(app: app, record: record, directory: directory, options: options)
        try await until(app) { pane.surface?.readText()?.contains("PANE-PID:") == true && (pane.sessionRecord?.session ?? 0) > 0 && pane.status == .ready || pane.status.isFailed }
        guard !pane.status.isFailed, let first = pane.surface, let saved = pane.sessionRecord, saved.session > 0 else { return XCTFail("first durable pane failed: \(pane.status)") }
        XCTAssertEqual(first.replicaAppearance(generation: first.replicaGeneration!)?.background, 0x224466, "ready must wait for the Source STYLE and actual engine apply, not just queue admission")
        XCTAssertEqual(publishedBackgrounds.first, 0x224466, "Surface publication must itself wait for actual STYLE apply")
        XCTAssertTrue(saved.admitted); XCTAssertEqual(saved.nonce, nonce)
        try await until(app) { first.view.window === window }
        let pid = first.readText()?.components(separatedBy: .newlines).first { $0.contains("PANE-PID:") }
        XCTAssertNotNil(pid)
        first.sendCommittedText("one\r")
        try await until(app) { first.readText()?.contains("FIRST:one") == true }
        pane.stop(); XCTAssertFalse(pane.isRunning)
        let invalid = TerminalPane()
        defer { invalid.stop() }
        XCTAssertThrowsError(try invalid.attachRecoverable(app: app, record: saved, directory: directory, options: options), "an admitted record cannot CREATE again")
        options.create = nil; options.session = saved.session; options.instance = saved.instance
        try pane.attachRecoverable(app: app, record: saved, directory: directory, options: options)
        try await until(app) { (pane.surface !== first && pane.surface?.readText()?.contains("FIRST:one") == true && pane.status == .ready) || pane.status.isFailed }
        guard !pane.status.isFailed, let second = pane.surface, second !== first else { return XCTFail("durable pane resume failed: \(pane.status)") }
        try await until(app) { second.view.window === window && first.view.window == nil }
        XCTAssertTrue(second.view.window === window, "RootView must mount the new Surface rather than keep the previous NSViewRepresentable")
        XCTAssertNil(first.view.window)
        guard let container = second.view.superview else { return XCTFail("new Surface has no native host") }
        let responder = window.firstResponder as? NSView
        XCTAssertTrue(responder === container || responder?.isDescendant(of: container) == true, "keyboard focus must move to the remounted Surface")
        XCTAssertEqual(model.tabs.activeTab?.id, actualTab.id); XCTAssertEqual(actualTab.activePane?.id, pane.id)
        XCTAssertEqual(pane.sessionRecord?.instance, saved.instance); XCTAssertEqual(pane.sessionRecord?.session, saved.session)
        XCTAssertTrue(second.readText()?.contains(pid ?? "missing PID") == true)
        XCTAssertEqual(second.replicaAppearance(generation: second.replicaGeneration!)?.background, 0x224466)
        try "background = #663311\nforeground = #eeccee\nfont-size = 19\npalette = 1=#405060\n".write(toFile: configPath, atomically: true, encoding: .utf8)
        XCTAssertNil(app.reloadConfig(path: configPath))
        app.setColorScheme(dark: true)
        // Deliberately hold main briefly: Source/Transport advance, but the
        // main-owner tagged config cannot apply. This is a LIVE same-generation
        // input queue, not disconnected replay. Typing must survive the barrier.
        Thread.sleep(forTimeInterval: 0.1)
        first.sendCommittedText("stale\r"); second.sendCommittedText("two\r")
        try await until(app) {
            let actual = second.replicaAppearance(generation: second.replicaGeneration!)
            return (actual?.background == 0x663311 && actual?.dark == true) || pane.status.isFailed
        }
        XCTAssertFalse(pane.status.isFailed, "global config/theme updates must not send unsequenced replica mutations")
        XCTAssertTrue(second.replicaAppearance(generation: second.replicaGeneration!)?.dark == true)
        // A new window/model reloads the shared app config and scheme too.
        let otherWindow = isolatedAppModel(configPath: runtime + "/other-ssh-config", knownHostsPath: runtime + "/known_hosts")
        XCTAssertNotNil(otherWindow.terminalApp)
        try await Task.sleep(nanoseconds: 100_000_000); app.tick()
        XCTAssertNotNil(second.replicaAppearance(generation: second.replicaGeneration!))
        try await until(app) { second.readText()?.contains("SECOND:two") == true }
        XCTAssertFalse(second.readText()?.contains("SECOND:stale") == true)
        if testRun?.failureCount == 0 { print("PASS durable native pane: original record/PID/tab/pane, real RootView remount, ordered theme/font updates, resumed input and stale Surface rejection") }
    }
}
