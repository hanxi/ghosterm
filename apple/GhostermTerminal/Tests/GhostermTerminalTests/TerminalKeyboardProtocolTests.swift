import AppKit
import GhostermTerminal
import XCTest

final class TerminalKeyboardProtocolTests: XCTestCase {
    private func waitUntil(_ condition: () -> Bool) -> Bool {
        let deadline = Date().addingTimeInterval(5)
        while !condition() && Date() < deadline {
            RunLoop.main.run(until: Date().addingTimeInterval(0.01))
        }
        return condition()
    }

    /// Exercise the actual NSEvent → NSTextInputClient → encoder → transport path.
    /// AppKit's function-key PUA characters must never become text, including when
    /// a TUI negotiates Kitty keyboard flags (plain shell tests miss this failure).
    @MainActor
    func testFunctionKeysAcrossKeyboardProtocols() throws {
        _ = NSApplication.shared
        let surface = try TerminalSurface(app: TerminalApp.shared())
        let host = TerminalHostView(surface: surface)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 300),
                              styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        window.makeFirstResponder(host)
        var received = Data()
        surface.onInput = { received.append($0) }
        // Physical macOS keycode, NSEvent character, Kitty/normal sequence,
        // application-cursor sequence. Keep expectations independent of our encoder.
        let keys: [(UInt16, UInt32, String, String)] = [
            (126, 0xF700, "[A", "OA"), (125, 0xF701, "[B", "OB"),
            (123, 0xF702, "[D", "OD"), (124, 0xF703, "[C", "OC"),
            (115, 0xF729, "[H", "OH"), (119, 0xF72B, "[F", "OF"),
            (116, 0xF72C, "[5~", "[5~"), (121, 0xF72D, "[6~", "[6~"),
            (117, 0xF728, "[3~", "[3~"), (122, 0xF704, "OP", "OP"),
        ]
        for (flags, application) in [(0, false), (0, true), (1, false),
                                      (3, false), (5, false), (9, false),
                                      (17, false), (31, false), (0, false)] {
            received.removeAll()
            surface.feed(Data("\u{1b}[?1\(application ? "h" : "l")\u{1b}[=\(flags)u\u{1b}[?u".utf8))
            XCTAssertTrue(waitUntil { received == Data("\u{1b}[?\(flags)u".utf8) },
                          "Engine must acknowledge keyboard flags \(flags)")
            for (code, scalar, normal, app) in keys {
                received.removeAll()
                let chars = String(UnicodeScalar(scalar)!)
                let event = try XCTUnwrap(NSEvent.keyEvent(
                    with: .keyDown, location: .zero, modifierFlags: [.function, .numericPad],
                    timestamp: ProcessInfo.processInfo.systemUptime,
                    windowNumber: window.windowNumber, context: nil,
                    characters: chars, charactersIgnoringModifiers: chars,
                    isARepeat: false, keyCode: code))
                host.keyDown(with: event)
                XCTAssertTrue(waitUntil { !received.isEmpty })
                // Allow any erroneously synthesized release to arrive as well.
                RunLoop.main.run(until: Date().addingTimeInterval(0.02))
                var sequence = flags != 0 && code == 122 ? "[P" : (application ? app : normal)
                // Kitty's special-key form includes the press event when flag 2
                // is set; tilde keys use the full form with an implicit press.
                if flags & 2 != 0 && !normal.hasSuffix("~") {
                    sequence = "[1;1:1" + String(normal.suffix(1))
                }
                XCTAssertEqual(received, Data(("\u{1b}" + sequence).utf8),
                               "flags=\(flags), application=\(application), key=\(code), bytes=\(Array(received))")
            }
        }
    }
}
