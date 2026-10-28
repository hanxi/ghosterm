import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

@MainActor final class SidebarHoverTests: XCTestCase {
    override class func setUp() {
        _ = NSApplication.shared
        NSApplication.shared.setActivationPolicy(.accessory)
    }

    func testHoverChangesRowRendering() throws {
        let tab = TerminalTab(kind: .localFiles)
        let host = NSHostingView(rootView: TabRow(tab: tab, isActive: true))
        host.frame = NSRect(x: 0, y: 0, width: 220, height: 40)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        func settle() { RunLoop.main.run(until: Date().addingTimeInterval(0.2)) }
        func image() throws -> Data {
            host.layoutSubtreeIfNeeded()
            let rep = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
            host.cacheDisplay(in: host.bounds, to: rep)
            return try XCTUnwrap(rep.representation(using: .png, properties: [:]))
        }
        func event(_ type: NSEvent.EventType) throws -> NSEvent {
            try XCTUnwrap(NSEvent.enterExitEvent(with: type, location: NSPoint(x: 100, y: 20),
                                                modifierFlags: [], timestamp: 0, windowNumber: window.windowNumber,
                                                context: nil, eventNumber: 0, trackingNumber: 0, userData: nil))
        }
        settle()
        let before = try image()
        func tracker(_ view: NSView) -> HoverTrackingView? {
            if let tracker = view as? HoverTrackingView { return tracker }
            return view.subviews.lazy.compactMap { tracker($0) }.first
        }
        let tracking = try XCTUnwrap(tracker(host))
        XCTAssertTrue(tracking.trackingAreas.contains { $0.options.contains(.mouseEnteredAndExited) })
        var closed: UUID?
        let observer = NotificationCenter.default.addObserver(forName: .ghostermRequestCloseTab,
                                                               object: nil, queue: nil) { note in
            closed = note.object as? UUID
        }
        defer { NotificationCenter.default.removeObserver(observer) }
        func click() throws {
            let point = host.convert(NSPoint(x: host.bounds.width - 12, y: host.bounds.midY), to: nil)
            for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
                let click = try XCTUnwrap(NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                                                            timestamp: 0, windowNumber: window.windowNumber,
                                                            context: nil, eventNumber: 1, clickCount: 1, pressure: 1))
                window.sendEvent(click)
            }
            settle()
        }
        try click()
        XCTAssertNil(closed, "隐藏的关闭按钮不得接收点击")
        tracking.mouseEntered(with: try event(.mouseEntered)); settle()
        let hovered = try image()
        XCTAssertNotEqual(before, hovered, "悬停时侧栏关闭按钮应画出来")
        try click()
        XCTAssertEqual(closed, tab.id, "关闭按钮必须作用于对应标签")
        tracking.mouseExited(with: try event(.mouseExited)); settle()
        XCTAssertEqual(before, try image(), "移出后关闭按钮应隐藏")
    }
}
