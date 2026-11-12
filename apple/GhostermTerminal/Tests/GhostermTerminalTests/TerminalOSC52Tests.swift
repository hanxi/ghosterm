import AppKit
@testable import GhostermTerminal
import XCTest

@MainActor final class TerminalOSC52Tests: XCTestCase {
    private func preserveClipboard() -> () -> Void {
        let items: [NSPasteboardItem] = NSPasteboard.general.pasteboardItems?.map { item in
            let copy = NSPasteboardItem()
            for type in item.types { if let data = item.data(forType: type) { copy.setData(data, forType: type) } }
            return copy
        } ?? []
        return { NSPasteboard.general.clearContents(); NSPasteboard.general.writeObjects(items) }
    }

    private func pump(_ app: TerminalApp, until done: () -> Bool) {
        let deadline = Date().addingTimeInterval(5)
        while !done() && Date() < deadline {
            app.tick()
            RunLoop.current.run(until: Date().addingTimeInterval(0.01))
        }
        XCTAssertTrue(done())
    }

    func testOSC52WriteAndReadUseSystemClipboardAndEngineReply() throws {
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let clipboard = NSPasteboard.general
        let restore = preserveClipboard(); defer { restore() }
        let text = "OSC 52 中文 café\nsecond line"
        let encoded = Data(text.utf8).base64EncodedString()
        clipboard.clearContents(); clipboard.setString("before", forType: .string)
        // Fragmented output exercises the real stream parser and both terminators.
        surface.feed(Data("\u{1b}]52;c;\(encoded.prefix(4))".utf8))
        app.tick()
        XCTAssertEqual(clipboard.string(forType: .string), "before")
        surface.feed(Data("\(encoded.dropFirst(4))\u{7}".utf8))
        pump(app) { clipboard.string(forType: .string) == text }
        let lock = NSLock()
        var reply = Data()
        surface.onInput = { bytes in lock.lock(); reply.append(bytes); lock.unlock() }
        var permission: ((Bool) -> Void)?
        surface.clipboardConfirmation = { read, snapshot, decide in
            XCTAssertTrue(read); XCTAssertEqual(snapshot, text); permission = decide
        }
        surface.feed(Data("\u{1b}]52;c;?\u{1b}\\".utf8))
        pump(app) { permission != nil }
        lock.lock(); XCTAssertTrue(reply.isEmpty); lock.unlock()
        // Approval serves the reviewed snapshot, even if another app changes it.
        clipboard.clearContents(); clipboard.setString("changed", forType: .string)
        permission?(true)
        pump(app) { lock.lock(); defer { lock.unlock() }; return !reply.isEmpty }
        lock.lock(); let actual = reply; lock.unlock()
        XCTAssertEqual(String(decoding: actual, as: UTF8.self), "\u{1b}]52;c;\(encoded)\u{1b}\\")
    }

    func testWriteAndReadAboveOneMiBUseEngineAndClipboard() throws {
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let restore = preserveClipboard(); defer { restore() }
        let text = String(repeating: "x", count: 1024 * 1024 + 1) + "中文"
        let encoded = Data(text.utf8).base64EncodedString()
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString("before-large-write", forType: .string)
        let sequence = Data(("\u{1b}]52;c;" + encoded + "\u{7}").utf8)
        for offset in stride(from: 0, to: sequence.count, by: 4096) {
            surface.feed(sequence.subdata(in: offset..<min(offset + 4096, sequence.count)))
        }
        pump(app) { NSPasteboard.general.string(forType: .string) == text }
        let expected = Data(("\u{1b}]52;c;" + encoded + "\u{1b}\\").utf8)
        let lock = NSLock(); var reply = Data()
        surface.onInput = { data in lock.lock(); reply.append(data); lock.unlock() }
        surface.clipboardConfirmation = { read, snapshot, decide in
            XCTAssertTrue(read); XCTAssertTrue(snapshot == text); decide(true)
        }
        surface.feed(Data("\u{1b}]52;c;?\u{7}".utf8))
        pump(app) { lock.lock(); defer { lock.unlock() }; return reply.count >= expected.count }
        lock.lock(); let actual = reply; lock.unlock()
        XCTAssertTrue(actual == expected, "Large clipboard reply must preserve every byte")
    }

    func testNativeSheetApprovalUsesCapturedText() throws {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.prohibited)
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let window = NSWindow(contentRect: surface.view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.alphaValue = 0
        window.contentView = surface.view
        window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000)); window.order(.below, relativeTo: 0)
        let restore = preserveClipboard(); defer { window.close(); restore() }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString("OSC_NATIVE_SHEET", forType: .string)
        let lock = NSLock(); var reply = Data()
        surface.onInput = { data in lock.lock(); reply.append(data); lock.unlock() }
        surface.feed(Data("\u{1b}]52;c;?\u{7}".utf8))
        pump(app) { window.attachedSheet != nil }
        lock.lock(); XCTAssertTrue(reply.isEmpty); lock.unlock()
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString("CHANGED_AFTER_PROMPT", forType: .string)
        func allowButton(_ view: NSView) -> NSButton? {
            if let button = view as? NSButton, button.title == "允许一次" { return button }
            return view.subviews.lazy.compactMap { allowButton($0) }.first
        }
        let content = try XCTUnwrap(window.attachedSheet?.contentView)
        try XCTUnwrap(allowButton(content)).performClick(nil)
        pump(app) { lock.lock(); defer { lock.unlock() }; return !reply.isEmpty }
        lock.lock(); let actual = reply; lock.unlock()
        let encoded = Data("OSC_NATIVE_SHEET".utf8).base64EncodedString()
        XCTAssertEqual(String(decoding: actual, as: UTF8.self), "\u{1b}]52;c;\(encoded)\u{1b}\\")
    }

    func testExplicitDenialNeverDisclosesText() throws {
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let clipboard = NSPasteboard.general
        let restore = preserveClipboard(); defer { restore() }
        clipboard.clearContents(); clipboard.setString("OSC_DENIED_FIXTURE", forType: .string)
        surface.clipboardConfirmation = { _, _, decide in decide(false) }
        let lock = NSLock(); var reply = Data()
        surface.onInput = { data in lock.lock(); reply.append(data); lock.unlock() }
        surface.feed(Data("\u{1b}]52;c;?\u{7}".utf8))
        pump(app) { lock.lock(); defer { lock.unlock() }; return !reply.isEmpty }
        lock.lock(); let actual = reply; lock.unlock()
        XCTAssertEqual(String(decoding: actual, as: UTF8.self), "\u{1b}]52;c;\u{1b}\\")
    }

    func testDeniedAndHiddenRequestsNeverDiscloseClipboard() throws {
        _ = NSApplication.shared
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let restore = preserveClipboard(); defer { restore() }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString("OSC_HIDDEN_FIXTURE", forType: .string)
        var decision: ((Bool) -> Void)?
        surface.clipboardConfirmation = { _, _, callback in decision = callback }
        let lock = NSLock(); var reply = Data()
        surface.onInput = { data in lock.lock(); reply.append(data); lock.unlock() }
        surface.feed(Data("\u{1b}]52;c;?\u{7}".utf8))
        pump(app) { decision != nil }
        surface.setVisible(false)
        decision?(true) // Late permission cannot undo cancellation.
        pump(app) { lock.lock(); defer { lock.unlock() }; return !reply.isEmpty }
        lock.lock(); let actual = reply; lock.unlock()
        XCTAssertFalse(String(decoding: actual, as: UTF8.self).contains(";?"))
        XCTAssertEqual(String(decoding: actual, as: UTF8.self), "\u{1b}]52;c;\u{1b}\\")
    }
}
