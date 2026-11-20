import AppKit
import XCTest

final class TestWindowsTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        TestWindows.enterAutomationMode()
    }

    func testFrontOrderingRemainsInvisibleAndDoesNotActivateApplication() {
        let first = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 240, height: 160),
                             styleMask: [.titled], backing: .buffered, defer: false)
        let second = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 180, height: 120),
                             styleMask: [.titled, .utilityWindow], backing: .buffered, defer: false)
        first.isReleasedWhenClosed = false
        second.isReleasedWhenClosed = false
        defer { first.close(); second.close() }

        first.orderFrontRegardless()
        second.makeKeyAndOrderFront(nil)

        XCTAssertEqual(first.alphaValue, 0)
        XCTAssertEqual(second.alphaValue, 0)
        XCTAssertFalse(NSApp.isActive)
    }
}
