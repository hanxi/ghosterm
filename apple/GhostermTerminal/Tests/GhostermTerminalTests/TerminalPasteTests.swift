import AppKit
import XCTest
@testable import GhostermTerminal

private final class PasteBytes: @unchecked Sendable {
    private let lock = NSLock()
    private var value = Data()
    func append(_ bytes: Data) { lock.lock(); value.append(bytes); lock.unlock() }
    var snapshot: Data { lock.lock(); defer { lock.unlock() }; return value }
}

@MainActor final class TerminalPasteTests: XCTestCase {
    override class func setUp() { super.setUp(); _ = NSApplication.shared; NSApp.setActivationPolicy(.prohibited) }
    private func fixture(_ config: String = "") throws -> (TerminalApp, TerminalSurface, TerminalHostView, PasteBytes) {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let directory = root.appendingPathComponent("tmp/paste-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("config").path
        try ("clipboard-paste-protection = true\nclipboard-write = allow\n" + config).write(toFile: path, atomically: true, encoding: .utf8)
        let app = try TerminalApp.shared()
        if let error = app.reloadConfig(path: path) { throw NSError(domain: error, code: 1) }
        addTeardownBlock { _ = app.reloadConfig(path: nil) }
        let surface = try TerminalSurface(app: app), bytes = PasteBytes()
        surface.onInput = { bytes.append($0) }
        let items: [NSPasteboardItem] = NSPasteboard.general.pasteboardItems?.map { item in
            let copy = NSPasteboardItem()
            for type in item.types { if let bytes = item.data(forType: type) { copy.setData(bytes, forType: type) } }
            return copy
        } ?? []
        addTeardownBlock { NSPasteboard.general.clearContents(); NSPasteboard.general.writeObjects(items) }
        return (app, surface, TerminalHostView(surface: surface), bytes)
    }
    private func setClipboard(_ text: String) {
        NSPasteboard.general.clearContents(); NSPasteboard.general.setString(text, forType: .string)
    }
    @discardableResult private func pump(_ app: TerminalApp, seconds: Double = 2, until done: () -> Bool) -> Bool {
        let end = Date().addingTimeInterval(seconds)
        while Date() < end { app.tick(); if done() { return true }; RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        return done()
    }
    private func settle(_ app: TerminalApp) { _ = pump(app, seconds: 0.12) { false } }
    private func expected(_ text: String) -> Data { Data(text.replacingOccurrences(of: "\n", with: "\r").utf8) }

    func testUnsafeMenuPasteWaitsForApprovalAndUsesReviewedSnapshot() throws {
        let (app, surface, host, bytes) = try fixture()
        let text = "first line\nreviewed second line"
        setClipboard(text)
        var decision: ((Bool) -> Void)?, reviewed: String?, kind: TerminalSurface.ClipboardOperation?
        surface.clipboardConfirmation = { operation, snapshot, done in kind = operation; reviewed = snapshot; decision = done }
        host.paste(nil)
        _ = pump(app) { decision != nil || !bytes.snapshot.isEmpty }
        let pending = bytes.snapshot.isEmpty && decision != nil && reviewed == text && kind == .paste
        setClipboard("changed while permission was open\nnever reviewed")
        decision?(true)
        _ = pump(app) { !bytes.snapshot.isEmpty }
        XCTAssertTrue(pending && bytes.snapshot == expected(text), "menu paste must review before writing and approve exactly the captured text")
    }
    func testDeniedUnsafePasteProducesNoInput() throws {
        let (app, surface, host, bytes) = try fixture()
        setClipboard("never execute\nthis line")
        var asked = false
        surface.clipboardConfirmation = { _, _, done in asked = true; done(false) }
        host.paste(nil); settle(app)
        XCTAssertTrue(asked && bytes.snapshot.isEmpty, "denied paste must not inject any command bytes")
    }
    func testSecondPendingRequestCannotReplaceReviewedPaste() throws {
        let (app, surface, host, bytes) = try fixture()
        let first = "first reviewed\nonly this"
        var decisions: [(Bool) -> Void] = []
        surface.clipboardConfirmation = { _, _, done in decisions.append(done) }
        setClipboard(first); host.paste(nil); _ = pump(app) { !decisions.isEmpty || !bytes.snapshot.isEmpty }
        setClipboard("second request\nnot reviewed"); host.paste(nil); settle(app)
        let single = decisions.count == 1 && bytes.snapshot.isEmpty
        decisions.first?(true); _ = pump(app) { !bytes.snapshot.isEmpty }
        XCTAssertTrue(single && bytes.snapshot == expected(first), "an outstanding review cannot silently switch to a newer clipboard request")
    }
    func testOldApprovalCannotApproveANewerRequest() throws {
        let (app, surface, host, bytes) = try fixture()
        var decisions: [(Bool) -> Void] = []
        surface.clipboardConfirmation = { _, _, done in decisions.append(done) }
        setClipboard("old\ncancelled"); host.paste(nil); _ = pump(app) { decisions.count == 1 || !bytes.snapshot.isEmpty }
        decisions.first?(false)
        let next = "new\nreviewed separately"
        setClipboard(next); host.paste(nil); _ = pump(app) { decisions.count == 2 || !bytes.snapshot.isEmpty }
        decisions.first?(true); settle(app)
        let pending = decisions.count == 2 && bytes.snapshot.isEmpty
        if decisions.count > 1 { decisions[1](true) }
        _ = pump(app) { !bytes.snapshot.isEmpty }
        XCTAssertTrue(pending && bytes.snapshot == expected(next), "a stale confirmation cannot grant the next request")
    }
    func testChangingActivePaneRevokesItsPendingPaste() throws {
        let (app, surface, host, bytes) = try fixture()
        host.paneFocused = true
        var decision: ((Bool) -> Void)?
        surface.clipboardConfirmation = { _, _, done in decision = done }
        setClipboard("old pane\nmust not run"); host.paste(nil)
        _ = pump(app) { decision != nil || !bytes.snapshot.isEmpty }
        let asked = decision != nil
        host.paneFocused = false
        decision?(true); settle(app)
        XCTAssertTrue(asked && bytes.snapshot.isEmpty, "switching panes revokes the old pane's approval")
    }
    func testHiddenSurfaceRevokesItsPendingPaste() throws {
        let (app, surface, host, bytes) = try fixture()
        var decision: ((Bool) -> Void)?
        surface.clipboardConfirmation = { _, _, done in decision = done }
        setClipboard("hidden pane\nmust not run"); host.paste(nil)
        _ = pump(app) { decision != nil || !bytes.snapshot.isEmpty }
        let asked = decision != nil
        surface.setVisible(false)
        decision?(true); settle(app)
        XCTAssertTrue(asked && bytes.snapshot.isEmpty, "hiding a surface revokes the pending paste")
    }
    func testSafeAndBracketedPastesUseEngineEncodingWithoutPrompt() throws {
        let (app, surface, host, bytes) = try fixture()
        var prompts = 0
        surface.clipboardConfirmation = { _, _, done in prompts += 1; done(false) }
        setClipboard("plain café"); host.paste(nil); _ = pump(app) { !bytes.snapshot.isEmpty }
        let first = bytes.snapshot
        surface.feed(Data("\u{1b}[?2004h".utf8))
        let text = "bracketed\n中文"
        setClipboard(text); host.paste(nil)
        let expected = Data(("plain café\u{1b}[200~" + text + "\u{1b}[201~").utf8)
        _ = pump(app) { bytes.snapshot.count >= expected.count }
        XCTAssertTrue(first == Data("plain café".utf8) && bytes.snapshot == expected && prompts == 0,
            "safe clipboard input and framed paste must retain native encoding")
    }
    func testExplicitPasteProtectionSettingStillControlsTheEngine() throws {
        let (app, surface, host, bytes) = try fixture("clipboard-paste-protection = false\n")
        var prompts = 0
        surface.clipboardConfirmation = { _, _, done in prompts += 1; done(false) }
        let text = "explicit preference\nsecond line"
        setClipboard(text); host.paste(nil); _ = pump(app) { !bytes.snapshot.isEmpty }
        XCTAssertTrue(prompts == 0 && bytes.snapshot == expected(text), "the wrapper must use the configured engine policy")
    }
    func testCopyOnSelectWritesOnlyTheActualSelection() throws {
        let (app, surface, _, _) = try fixture("copy-on-select = true\n")
        setClipboard("before selection")
        surface.feed(Data("SELECTED_ROW\r\nNOT_SELECTED".utf8))
        surface.sendMousePos(x: 1, y: 5); surface.sendMouseButton(.press, button: .left)
        surface.sendMousePos(x: 300, y: 5); surface.sendMouseButton(.release, button: .left)
        _ = pump(app) { NSPasteboard.general.string(forType: .string) != "before selection" }
        XCTAssertEqual(NSPasteboard.general.string(forType: .string)?.trimmingCharacters(in: .whitespacesAndNewlines), "SELECTED_ROW")
    }
    func testClipboardWriteAskKeepsWriteIntentAndDenialPreservesClipboard() throws {
        let (app, surface, _, _) = try fixture("clipboard-write = ask\n")
        setClipboard("preserved before program write")
        var kind: TerminalSurface.ClipboardOperation?, reviewed: String?
        surface.clipboardConfirmation = { operation, text, done in kind = operation; reviewed = text; done(false) }
        let text = "program clipboard write"
        surface.feed(Data(("\u{1b}]52;c;" + Data(text.utf8).base64EncodedString() + "\u{7}").utf8))
        _ = pump(app) { kind != nil }
        XCTAssertTrue(kind == .write && reviewed == text && NSPasteboard.general.string(forType: .string) == "preserved before program write",
            "a program write must remain distinct from a user paste and preserve the clipboard when denied")
    }
    func testNativePastePreviewUsesAByteBudgetForCombiningText() throws {
        let (app, surface, _, bytes) = try fixture()
        let window = NSWindow(contentRect: surface.view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.alphaValue = 0; window.contentView = surface.view
        window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000)); window.order(.below, relativeTo: 0)
        defer { window.close() }
        let text = "a" + String(repeating: "\u{301}", count: 10_000) + "\nsecond line"
        setClipboard(text); surface.pasteFromClipboard()
        _ = pump(app) { window.attachedSheet != nil || !bytes.snapshot.isEmpty }
        var informative: String?, deny: NSButton?
        func visit(_ view: NSView) {
            if let field = view as? NSTextField, field.stringValue.contains("这段文本包含") { informative = field.stringValue }
            if let button = view as? NSButton, button.title == "拒绝" { deny = button }
            for child in view.subviews { visit(child) }
        }
        if let content = window.attachedSheet?.contentView { visit(content) }
        let bounded = informative.map { $0.utf8.count < 5000 && $0.contains("预览已截断") } ?? false
        deny?.performClick(nil); settle(app)
        XCTAssertTrue(bounded && bytes.snapshot.isEmpty, "one huge grapheme must not bypass the native preview's byte limit")
    }
    func testNativePasteSheetShowsPasteIntentAndApprovesCapturedText() throws {
        let (app, surface, _, bytes) = try fixture()
        let window = NSWindow(contentRect: surface.view.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.alphaValue = 0
        window.contentView = surface.view
        window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000)); window.order(.below, relativeTo: 0)
        defer { window.close() }
        let text = "native review\nsecond line"
        setClipboard(text); surface.pasteFromClipboard()
        _ = pump(app) { window.attachedSheet != nil || !bytes.snapshot.isEmpty }
        var labels: [String] = [], allow: NSButton?
        func visit(_ view: NSView) {
            if let field = view as? NSTextField { labels.append(field.stringValue) }
            if let button = view as? NSButton, button.title == "允许一次" { allow = button }
            for child in view.subviews { visit(child) }
        }
        if let content = window.attachedSheet?.contentView { visit(content) }
        let reviewed = labels.contains("确认粘贴到当前终端？") && labels.contains(where: { $0.contains(text) }) && bytes.snapshot.isEmpty
        setClipboard("changed\nnot approved")
        allow?.performClick(nil); _ = pump(app) { !bytes.snapshot.isEmpty }
        XCTAssertTrue(reviewed && bytes.snapshot == expected(text), "a native paste confirmation must describe paste and keep its original bytes")
    }
}
