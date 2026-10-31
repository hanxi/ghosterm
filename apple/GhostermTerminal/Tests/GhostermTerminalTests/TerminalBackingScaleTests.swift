import AppKit
import GhostermTerminal
import XCTest

/// Inject only the screen's backing scale. The view, Metal layer, terminal
/// grid and resize callbacks are real; this does not claim a physical monitor move.
@MainActor final class TerminalBackingScaleTests: XCTestCase {
    private final class ScreenWindow: NSWindow {
        var screenScale: CGFloat = 2
        override var backingScaleFactor: CGFloat { screenScale }
    }

    private func exercise(screenNotification: Bool) throws {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.accessory)
        let app = try TerminalApp.shared()
        app.startEventLoop()
        let surface = try TerminalSurface(app: app)
        let host = TerminalHostView(surface: surface)
        let window = ScreenWindow(contentRect: host.frame, styleMask: [.titled, .resizable],
                                  backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000))
        window.orderFrontRegardless()
        defer { window.close() }
        host.layout()
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        let initial = surface.gridSize
        let originalSize = host.bounds.size
        let lock = NSLock()
        var reported: (UInt16, UInt16)?
        surface.onResize = { cols, rows in
            lock.lock(); reported = (cols, rows); lock.unlock()
        }

        for scale: CGFloat in [1, 2, 1] {
            window.screenScale = scale
            // Keep point geometry unchanged: layout() alone cannot observe this.
            if screenNotification {
                NotificationCenter.default.post(name: NSWindow.didChangeScreenNotification, object: window)
            } else {
                host.viewDidChangeBackingProperties()
            }
            RunLoop.main.run(until: Date().addingTimeInterval(0.1))
            XCTAssertEqual(surface.size.width, UInt32(originalSize.width * scale))
            XCTAssertEqual(surface.size.height, UInt32(originalSize.height * scale))
            XCTAssertEqual(try XCTUnwrap(surface.view.layer).contentsScale, scale,
                           "The engine-owned Metal layer must use the destination screen's scale")
            // Rasterized font metrics round to integral pixels at each DPI.
            // Allow that small difference, but never a half/double-size grid.
            XCTAssertEqual(Double(surface.gridSize.cols), Double(initial.cols), accuracy: Double(initial.cols) * 0.1)
            XCTAssertEqual(Double(surface.gridSize.rows), Double(initial.rows), accuracy: Double(initial.rows) * 0.1)

            // Subsequent window resizing must fill the whole rendering anchor
            // and reach the transport callback, without recreating the surface.
            for factor: CGFloat in [1.5, 1] {
                window.setContentSize(NSSize(width: originalSize.width * factor,
                                            height: originalSize.height * factor))
                host.layoutSubtreeIfNeeded()
                RunLoop.main.run(until: Date().addingTimeInterval(0.1))
                XCTAssertEqual(surface.view.frame, host.bounds)
                XCTAssertEqual(surface.size.width, UInt32(host.bounds.width * scale))
                XCTAssertEqual(surface.size.height, UInt32(host.bounds.height * scale))
                XCTAssertEqual(try XCTUnwrap(surface.view.layer).contentsScale, scale)
                XCTAssertEqual(Double(surface.gridSize.cols), Double(initial.cols) * Double(factor),
                               accuracy: Double(initial.cols) * 0.1)
                XCTAssertEqual(Double(surface.gridSize.rows), Double(initial.rows) * Double(factor),
                               accuracy: Double(initial.rows) * 0.1)
                let deadline = Date().addingTimeInterval(5)
                var actual: (UInt16, UInt16)?
                repeat {
                    lock.lock(); actual = reported; lock.unlock()
                    if actual?.0 == surface.gridSize.cols && actual?.1 == surface.gridSize.rows { break }
                    RunLoop.main.run(until: Date().addingTimeInterval(0.02))
                } while Date() < deadline
                XCTAssertEqual(actual?.0, surface.gridSize.cols)
                XCTAssertEqual(actual?.1, surface.gridSize.rows)
            }
        }
    }

    func testBackingScaleChangeAndSubsequentWindowResize() throws {
        try exercise(screenNotification: false)
    }

    func testScreenChangeRefreshesBackingPropertiesWithoutLayout() throws {
        try exercise(screenNotification: true)
    }

    func testMovingHostIntoAnotherWindowUsesItsScaleImmediately() throws {
        _ = NSApplication.shared
        let surface = try TerminalSurface(app: TerminalApp.shared())
        let host = TerminalHostView(surface: surface)
        let first = ScreenWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        let second = ScreenWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        first.isReleasedWhenClosed = false; second.isReleasedWhenClosed = false
        defer { first.close(); second.close() }
        first.screenScale = 1; second.screenScale = 2
        first.contentView = host
        XCTAssertEqual(surface.size.width, UInt32(host.bounds.width))
        XCTAssertEqual(try XCTUnwrap(surface.view.layer).contentsScale, 1)
        first.contentView = nil
        second.contentView = host
        XCTAssertEqual(surface.size.width, UInt32(host.bounds.width * 2))
        XCTAssertEqual(try XCTUnwrap(surface.view.layer).contentsScale, 2)
    }
}
