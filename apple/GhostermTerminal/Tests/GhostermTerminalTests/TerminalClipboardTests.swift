import AppKit
import GhostermTerminal
import XCTest

@MainActor final class TerminalClipboardTests: XCTestCase {
    func testMenuCopyAndPaste() throws {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.accessory)
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        let host = TerminalHostView(surface: surface)
        let window = NSWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000))
        window.orderFrontRegardless()
        window.makeFirstResponder(host)
        let previous = NSPasteboard.general.string(forType: .string)
        defer {
            window.close()
            NSPasteboard.general.clearContents()
            if let previous { NSPasteboard.general.setString(previous, forType: .string) }
        }
        let menu = NSMenu()
        menu.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        menu.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        func shortcut(_ key: String, _ code: UInt16) -> Bool {
            let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .command,
                timestamp: 0, windowNumber: window.windowNumber, context: nil,
                characters: key, charactersIgnoringModifiers: key, isARepeat: false, keyCode: code)!
            // XCTest 无 NSApplication.run 的全局 key-window 路由；把菜单目标
            // 设为真实窗口的 firstResponder，仍经过标准 selector 与快捷键匹配。
            for item in menu.items { item.target = window.firstResponder }
            menu.update()
            return menu.performKeyEquivalent(with: event)
        }
        XCTAssertTrue(window.firstResponder === host)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString("unchanged", forType: .string)
        XCTAssertTrue(shortcut("c", 8))
        XCTAssertEqual(NSPasteboard.general.string(forType: .string), "unchanged")
        surface.feed(Data("CLIPBOARD_SELECTED\r\nNOT_SELECTED".utf8))
        // 拖过首行；下一行必须不在复制结果里。
        surface.sendMousePos(x: 1, y: 5)
        surface.sendMouseButton(.press, button: .left)
        surface.sendMousePos(x: 300, y: 5)
        surface.sendMouseButton(.release, button: .left)
        XCTAssertTrue(shortcut("c", 8))
        XCTAssertEqual(NSPasteboard.general.string(forType: .string)?.trimmingCharacters(in: .whitespacesAndNewlines), "CLIPBOARD_SELECTED")
        let text = "粘贴 café\nsecond line"
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        surface.feed(Data("\u{1b}[?2004h".utf8))
        let received = expectation(description: "bracketed paste reaches transport")
        let lock = NSLock()
        var bytes = Data()
        let expected = Data(("\u{1b}[200~" + text + "\u{1b}[201~").utf8)
        surface.onInput = { data in
            lock.lock(); defer { lock.unlock() }
            bytes.append(data)
            if bytes.count >= expected.count { received.fulfill() }
        }
        XCTAssertTrue(shortcut("v", 9))
        wait(for: [received], timeout: 5)
        lock.lock(); let actual = bytes; lock.unlock()
        XCTAssertEqual(actual, expected)
    }
}
