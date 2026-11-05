// P0 only: production full libghostty surface, with no bridge mux or Client.
import AppKit
import Foundation
import GhostermTerminal

struct Case: Decodable {
    let name: String
    let file: String
    let expected: String
    let chunk: Int
    let replyPrefix: String
}

_ = NSApplication.shared
NSApplication.shared.setActivationPolicy(.accessory)
let directory = URL(fileURLWithPath: ProcessInfo.processInfo.environment["PROBE_INPUT"]!)
let cases = try JSONDecoder().decode([Case].self, from: Data(contentsOf: directory.appendingPathComponent("cases.json")))
let app = try TerminalApp.shared()
let io = DispatchQueue(label: "plugin-no-client-native-io")
var failures = 0
for item in cases {
    let surface = try TerminalSurface(app: app, frame: NSRect(x: 0, y: 0, width: 800, height: 400))
    let window = NSWindow(contentRect: NSRect(x: -30000, y: -30000, width: 800, height: 400),
                          styleMask: [.titled], backing: .buffered, defer: false)
    window.contentView = surface.view
    window.orderFrontRegardless()
    surface.setSize(width: 800, height: 400)
    let lock = NSLock()
    var reply = Data()
    surface.onInput = { data in lock.lock(); reply.append(data); lock.unlock() }
    func replies() -> Data { lock.lock(); defer { lock.unlock() }; return reply }
    let input = try Data(contentsOf: directory.appendingPathComponent(item.file))
    for start in stride(from: 0, to: input.count, by: item.chunk) {
        let bytes = input.subdata(in: start..<min(input.count, start + item.chunk))
        io.sync { surface.feed(bytes) }
    }
    // Causal barrier: this response proves the engine processed after the
    // candidate frame. No fixed quiet-period assertion substitutes for it.
    io.sync { surface.feed(Data("\u{1b}[6n".utf8)) }
    let expectedReply = Data("\(item.replyPrefix)\u{1b}[1;\(item.expected.utf8.count + 1)R".utf8)
    let deadline = Date().addingTimeInterval(8)
    while replies().last != 0x52 && Date() < deadline {
        app.tick()
        RunLoop.main.run(until: Date().addingTimeInterval(0.01))
    }
    let text = surface.readText()?.trimmingCharacters(in: .whitespacesAndNewlines)
    let ok = text == item.expected && replies() == expectedReply
    print("\(ok ? "PASS" : "FAIL") \(item.name)")
    if !ok {
        failures += 1
        print("OBSERVED text=\(String(reflecting: text)) replies=\(Array(replies())) expected=\(Array(expectedReply))")
    }
    window.orderOut(nil)
    window.contentView = nil
}
print("RESULT cases=\(cases.count) failures=\(failures)")
exit(failures == 0 ? 0 : 1)
