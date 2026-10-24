import AppKit
import Foundation
import XCTest
@testable import GhostermTerminal

@MainActor final class TerminalEventLoopTests: XCTestCase {
    func testHostLoopDeliversQueuedTitleWithoutManualTick() async throws {
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        app.startEventLoop(); app.startEventLoop()
        let surface = try TerminalSurface(app: app)
        var title: String?, offMain = false
        surface.onTitle = { value in title = value; offMain = !Thread.isMainThread }
        surface.feed(Data("\u{1b}]0;HOST-LOOP-READY\u{7}".utf8))
        let deadline = Date().addingTimeInterval(3)
        while title != "HOST-LOOP-READY", Date() < deadline { try await Task.sleep(nanoseconds: 10_000_000) }
        XCTAssertEqual(title, "HOST-LOOP-READY", "native host must consume app mailbox without a probe calling tick")
        XCTAssertFalse(offMain, "queued application metadata belongs to the main host loop")
    }
}
