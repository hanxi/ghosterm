import AppKit
import GhostermAppModel
import Sparkle
import SwiftUI
import XCTest
@testable import GhostermApp

@MainActor
final class NativeUpdaterTests: XCTestCase {
    func testDownloadProgressCancellationAndHiddenNoticeKeepSharedState() {
        let model = UpdateChecker()
        let driver = NativeUpdater(model: model)
        var cancelled = 0
        driver.showDownloadInitiated { cancelled += 1 }
        driver.showDownloadDidReceiveExpectedContentLength(100)
        driver.showDownloadDidReceiveData(ofLength: 30)
        model.noticeVisible = false
        driver.showDownloadDidReceiveData(ofLength: 25)
        XCTAssertEqual(model.phase, .downloading(received: 55, total: 100))
        XCTAssertEqual(cancelled, 0, "Closing a notification must not cancel a download")
        model.cancelAction?()
        XCTAssertEqual(cancelled, 1)
        XCTAssertEqual(model.phase, .idle)
        driver.showDownloadDidReceiveData(ofLength: 10)
        XCTAssertEqual(model.phase, .idle, "Late progress must not revive a cancelled operation")
    }

    func testReadyWaitsForExplicitRestartAndCanRetryAfterTerminationIsCancelled() {
        let model = UpdateChecker()
        let driver = NativeUpdater(model: model)
        var choices: [SPUUserUpdateChoice] = []
        driver.showDownloadDidStartExtractingUpdate()
        driver.showReady { choices.append($0) }
        XCTAssertEqual(model.phase, .ready)
        XCTAssertTrue(choices.isEmpty, "Downloading must never terminate the application")
        model.noticeVisible = false
        XCTAssertTrue(choices.isEmpty, "Later only hides the notice")
        model.restartAction?()
        XCTAssertEqual(choices, [.install])
        var retries = 0
        driver.showInstallingUpdate(withApplicationTerminated: false) { retries += 1 }
        XCTAssertEqual(model.phase, .ready)
        model.restartAction?()
        model.restartAction?()
        XCTAssertEqual(retries, 2, "A cancelled normal quit must keep a usable restart action")
    }

    func testFailureIsVisibleAndDoesNotTurnIntoUpToDateOrEraseOnDismiss() {
        let model = UpdateChecker()
        let driver = NativeUpdater(model: model)
        var acknowledged = false
        driver.showUpdaterError(NSError(domain: "fixture-signature", code: 1,
                                       userInfo: [NSLocalizedDescriptionKey: "签名不匹配"])) { acknowledged = true }
        XCTAssertTrue(acknowledged)
        XCTAssertEqual(model.phase, .failed("签名不匹配"))
        XCTAssertTrue(model.noticeVisible)
        XCTAssertNil(model.query)
        driver.dismissUpdateInstallation()
        XCTAssertEqual(model.phase, .failed("签名不匹配"))
        XCTAssertNil(model.restartAction)
    }

    func testNoticeIsAnOverlayAndDoesNotStealTerminalFocus() throws {
        _ = NSApplication.shared
        TestWindows.enterAutomationMode()
        let model = UpdateChecker()
        model.noticeVisible = true
        model.phase = .downloading(received: 30, total: 100)
        let field = NSTextField()
        let host = NSHostingView(rootView: Color.clear.overlay(alignment: .bottomTrailing) {
            UpdateNotice(updater: model).padding(12)
        })
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 600),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        field.frame = NSRect(x: 20, y: 400, width: 100, height: 30)
        host.addSubview(field)
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        window.makeFirstResponder(field)
        let responder = window.firstResponder
        host.layoutSubtreeIfNeeded()
        model.phase = .ready
        RunLoop.main.run(until: Date().addingTimeInterval(0.15))
        XCTAssertTrue(window.firstResponder === responder)
        XCTAssertTrue(model.noticeVisible)
    }
}
