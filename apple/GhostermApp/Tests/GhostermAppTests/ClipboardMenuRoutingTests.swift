import AppKit
import GhostermAppModel
@testable import GhostermTerminal
import XCTest
import ObjectiveC.runtime
@testable import GhostermApp

// XCTest's prohibited app cannot acquire an OS key window. Supply only that
// environment query; NSMenu, nil targets, NSApplication's responder search and
// the real window/view responders remain production objects. Never activate.
@MainActor private enum ClipboardMenuWindowContext {
    static weak var window: NSWindow?
    static func install() -> () -> Void {
        let selectors: [(Selector, Selector)] = [
            (#selector(getter: NSApplication.keyWindow), #selector(NSApplication.ghostermClipboardKeyWindow)),
            (#selector(getter: NSApplication.mainWindow), #selector(NSApplication.ghostermClipboardMainWindow)),
        ]
        let methods = selectors.map { (class_getInstanceMethod(NSApplication.self, $0.0)!, class_getInstanceMethod(NSApplication.self, $0.1)!) }
        for pair in methods { method_exchangeImplementations(pair.0, pair.1) }
        return { window = nil; for pair in methods { method_exchangeImplementations(pair.0, pair.1) } }
    }
}
private extension NSApplication {
    @objc func ghostermClipboardKeyWindow() -> NSWindow? { ClipboardMenuWindowContext.window ?? ghostermClipboardKeyWindow() }
    @objc func ghostermClipboardMainWindow() -> NSWindow? { ClipboardMenuWindowContext.window ?? ghostermClipboardMainWindow() }
}

private final class MenuClipboardBytes: @unchecked Sendable {
    private let lock = NSLock(); private var bytes = Data()
    func append(_ value: Data) { lock.lock(); bytes.append(value); lock.unlock() }
    var snapshot: Data { lock.lock(); defer { lock.unlock() }; return bytes }
}

// Preserve NSTextView selection insertion while supplying the fixture pasteboard.
private final class MenuClipboardTextView: NSTextView {
    var clipboard: NSPasteboard?
    override func validateUserInterfaceItem(_ item: NSValidatedUserInterfaceItem) -> Bool {
        if item.action == #selector(NSText.paste(_:)) {
            return isEditable && clipboard?.string(forType: .string) != nil
        }
        return super.validateUserInterfaceItem(item)
    }
    override func paste(_ sender: Any?) {
        guard let clipboard else { return }
        _ = readSelection(from: clipboard)
    }
}

@MainActor final class ClipboardMenuRoutingTests: XCTestCase {
    override class func setUp() { super.setUp(); _ = NSApplication.shared; TestWindows.enterAutomationMode() }
    private func installMenu() throws -> AppModel {
        RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        let directory = sessionTestDirectory()
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let model = isolatedAppModel(configPath: directory.appendingPathComponent("config").path,
            knownHostsPath: directory.appendingPathComponent("known_hosts").path,
            settings: SettingsStore(terminalConfigPath: directory.appendingPathComponent("ghostty").path))
        AppMenu.install(model: model)
        addTeardownBlock { Task { @MainActor in NSApp.mainMenu = nil } }
        return model
    }
    private func item(_ title: String, in menu: NSMenu?) -> NSMenuItem? {
        for entry in menu?.items ?? [] {
            if entry.title == title { return entry }
            if let found = item(title, in: entry.submenu) { return found }
        }
        return nil
    }
    private func window(_ view: NSView) -> NSWindow {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 480), styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false; window.contentView = view
        TestWindows.presentOffscreen(window)
        ClipboardMenuWindowContext.window = window
        return window
    }
    private func paste(_ text: String, window: NSWindow, clipboard: NSPasteboard) -> Bool {
        clipboard.clearContents(); clipboard.setString(text, forType: .string)
        let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .command,
            timestamp: 0, windowNumber: window.windowNumber, context: nil,
            characters: "v", charactersIgnoringModifiers: "v", isARepeat: false, keyCode: 9)!
        NSApp.mainMenu?.update()
        return NSApp.mainMenu?.performKeyEquivalent(with: event) ?? false
    }
    @discardableResult private func pump(_ apps: [TerminalApp], _ done: () -> Bool) -> Bool {
        let end = Date().addingTimeInterval(3)
        while Date() < end {
            for app in apps { app.tick() }
            if done() { return true }
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        return done()
    }
    func testProcessMenuUsesInjectedWindowResponderWithoutRetargetingItems() throws {
        let restoreWindowContext = ClipboardMenuWindowContext.install(); defer { restoreWindowContext() }
        let model = try installMenu(); defer { model.disconnectForTesting() }
        let clipboard = NSPasteboard.withUniqueName()
        defer { clipboard.releaseGlobally() }
        let app = try TerminalApp.shared(), a = try TerminalSurface(app: app, clipboard: clipboard), b = try TerminalSurface(app: app, clipboard: clipboard)
        let hostA = TerminalHostView(surface: a), hostB = TerminalHostView(surface: b)
        let bytesA = MenuClipboardBytes(), bytesB = MenuClipboardBytes()
        a.onInput = { bytesA.append($0) }; b.onInput = { bytesB.append($0) }
        let windowA = window(hostA), windowB = window(hostB)
        defer { windowA.close(); windowB.close() }
        ClipboardMenuWindowContext.window = windowA; hostA.paneFocused = true
        let pasteItem = item("粘贴", in: NSApp.mainMenu)
        let routedA = pasteItem?.action.map { NSApp.target(forAction: $0, to: pasteItem?.target, from: pasteItem) as AnyObject? === hostA } ?? false
        let first = paste("WINDOW_A_ONLY", window: windowA, clipboard: clipboard)
        _ = pump([app]) { !bytesA.snapshot.isEmpty }
        let isolatedA = bytesA.snapshot == Data("WINDOW_A_ONLY".utf8) && bytesB.snapshot.isEmpty
        ClipboardMenuWindowContext.window = windowB; hostB.paneFocused = true
        let routedB = pasteItem?.action.map { NSApp.target(forAction: $0, to: pasteItem?.target, from: pasteItem) as AnyObject? === hostB } ?? false
        let second = paste("WINDOW_B_ONLY", window: windowB, clipboard: clipboard)
        _ = pump([app]) { !bytesB.snapshot.isEmpty }
        XCTAssertTrue(pasteItem != nil && pasteItem?.target == nil && routedA && routedB && first && second && isolatedA
            && bytesA.snapshot == Data("WINDOW_A_ONLY".utf8) && bytesB.snapshot == Data("WINDOW_B_ONLY".utf8),
            "the real process menu must follow key-window responders without test-assigned targets")
    }
    func testProcessMenuFollowsPaneFocusAndTextEditor() throws {
        let restoreWindowContext = ClipboardMenuWindowContext.install(); defer { restoreWindowContext() }
        let model = try installMenu(); defer { model.disconnectForTesting() }
        let clipboard = NSPasteboard.withUniqueName()
        defer { clipboard.releaseGlobally() }
        let app = try TerminalApp.shared(), a = try TerminalSurface(app: app, clipboard: clipboard), b = try TerminalSurface(app: app, clipboard: clipboard)
        let hostA = TerminalHostView(surface: a), hostB = TerminalHostView(surface: b)
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 640, height: 480))
        container.addSubview(hostA); container.addSubview(hostB)
        let window = window(container); defer { window.close() }
        let bytesA = MenuClipboardBytes(), bytesB = MenuClipboardBytes()
        a.onInput = { bytesA.append($0) }; b.onInput = { bytesB.append($0) }
        hostA.paneFocused = true
        let first = paste("PANE_A", window: window, clipboard: clipboard); _ = pump([app]) { !bytesA.snapshot.isEmpty }
        hostA.paneFocused = false; hostB.paneFocused = true
        let second = paste("PANE_B", window: window, clipboard: clipboard); _ = pump([app]) { !bytesB.snapshot.isEmpty }
        let editor = MenuClipboardTextView(frame: container.bounds)
        editor.clipboard = clipboard
        container.addSubview(editor); window.makeFirstResponder(editor)
        let third = paste("EDITOR_ONLY", window: window, clipboard: clipboard)
        _ = pump([app]) { editor.string == "EDITOR_ONLY" }
        XCTAssertTrue(first, "Paste must dispatch for pane A")
        XCTAssertTrue(second, "Paste must dispatch for pane B")
        XCTAssertTrue(third, "Paste must dispatch for the text editor")
        XCTAssertTrue(window.firstResponder === editor, "The editor must retain first responder")
        XCTAssertEqual(editor.string, "EDITOR_ONLY")
        XCTAssertEqual(bytesA.snapshot, Data("PANE_A".utf8))
        XCTAssertEqual(bytesB.snapshot, Data("PANE_B".utf8))
    }
    func testProcessMenuPasteReachesTheRealLocalPTY() throws {
        let restoreWindowContext = ClipboardMenuWindowContext.install(); defer { restoreWindowContext() }
        let model = try installMenu(); defer { model.disconnectForTesting() }
        let clipboard = NSPasteboard.withUniqueName()
        defer { clipboard.releaseGlobally() }
        let app = try TerminalApp.shared(), surface = try TerminalSurface(app: app, clipboard: clipboard)
        let host = TerminalHostView(surface: surface), window = window(NSView())
        window.contentView = host; host.paneFocused = true
        defer { window.close() }
        let session = try SessionController.localShell(surface: surface, executable: "/bin/sh",
            argv: ["-c", #"IFS= read -r line; printf 'MENU_LOCAL_<%s>\n' "$line""#], kind: .direct)
        defer { session.stop() }
        session.start()
        let text = "clip_中文_café"
        let invoked = paste(text, window: window, clipboard: clipboard)
        surface.sendKey(.return)
        let arrived = pump([app]) { surface.readText()?.contains("MENU_LOCAL_<" + text + ">") == true }
        XCTAssertTrue(invoked && arrived, "process menu clipboard bytes must reach the actual local PTY and return through the grid")
    }
    func testProcessCopyUsesSelectionFromInjectedWindow() throws {
        let restoreWindowContext = ClipboardMenuWindowContext.install(); defer { restoreWindowContext() }
        let model = try installMenu(); defer { model.disconnectForTesting() }
        let clipboard = NSPasteboard.withUniqueName()
        defer { clipboard.releaseGlobally() }
        let app = try TerminalApp.shared(), surface = try TerminalSurface(app: app, clipboard: clipboard)
        let host = TerminalHostView(surface: surface)
        let window = window(host); defer { window.close() }
        host.paneFocused = true
        surface.feed(Data("COPY_SELECTED\r\nDO_NOT_COPY".utf8))
        surface.sendMousePos(x: 1, y: 5); surface.sendMouseButton(.press, button: .left)
        surface.sendMousePos(x: 300, y: 5); surface.sendMouseButton(.release, button: .left)
        clipboard.clearContents(); clipboard.setString("before copy", forType: .string)
        let copy = item("拷贝", in: NSApp.mainMenu)
        let action = copy?.action
        let invoked = action.map { NSApp.sendAction($0, to: copy?.target, from: copy) } ?? false
        XCTAssertTrue(copy != nil && copy?.target == nil && invoked
            && clipboard.string(forType: .string)?.trimmingCharacters(in: .whitespacesAndNewlines) == "COPY_SELECTED",
            "the real menu copy action must resolve the current terminal selection")
    }
}
