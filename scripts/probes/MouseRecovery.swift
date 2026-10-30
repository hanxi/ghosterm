// Disposable native probe: production SessionController + real full libghostty + isolated tmux.
import AppKit
import Foundation
import Darwin
import GhostermTerminal
import GhostermCore

let recovering = ProcessInfo.processInfo.environment["GHOSTERM_MOUSE_PROMPT_RECOVERY"] == "1"
let evidencePrefix = recovering ? "recovery" : "native"
let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let work = URL(fileURLWithPath: ProcessInfo.processInfo.environment["GHOSTERM_MOUSE_TEST_ROOT"]!)
let testShell = ProcessInfo.processInfo.environment["GHOSTERM_MOUSE_TEST_SHELL"] ?? "bash"
let testCodex = ProcessInfo.processInfo.environment["GHOSTERM_MOUSE_TEST_CODEX"] == "1"
let shellCommand = testShell == "zsh" ? "/bin/zsh -f -i" : "/bin/bash --noprofile --norc"
let socket = work.appendingPathComponent("native.sock").path
let tmuxBinary = ProcessInfo.processInfo.environment["TMUX_BINARY"] ?? "/opt/homebrew/bin/tmux"
func quote(_ value: String) -> String { "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'" }
func terminalCommand(_ command: String) -> String {
    guard let value = ProcessInfo.processInfo.environment["GHOSTERM_MOUSE_SSH_PREFIX"] else { return command }
    let args = try! JSONSerialization.jsonObject(with: Data(value.utf8)) as! [String]
    return args.map(quote).joined(separator: " ") + " " + quote(command)
}
@discardableResult
func tmux(_ args: [String], required: Bool = true) -> String {
    let p = Process(); p.executableURL = URL(fileURLWithPath: tmuxBinary)
    p.arguments = ["-S", socket] + args
    let pipe = Pipe(); p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
    try! p.run(); let output = pipe.fileHandleForReading.readDataToEndOfFile(); p.waitUntilExit()
    if required && p.terminationStatus != 0 { fatalError("tmux command failed: \(args)") }
    return String(decoding: output, as: UTF8.self)
}
func check(_ label: String, _ condition: Bool) {
    guard condition else { print("FAIL",label); exit(1) }
    print("PASS",label); fflush(stdout)
}
func settle(_ seconds: Double = 0.25) { RunLoop.main.run(until: Date().addingTimeInterval(seconds)) }
func until(_ label: String, _ condition: () -> Bool) {
    let end = Date().addingTimeInterval(5)
    while !condition(), Date() < end { settle(0.02) }
    check(label, condition())
}
_ = NSApplication.shared
NSApp.setActivationPolicy(.accessory)
let app = try TerminalApp(); MainActor.assumeIsolated { app.startEventLoop() }
let surface = try TerminalSurface(app: app)
let host = TerminalHostView(surface: surface)
let window = NSWindow(contentRect: NSRect(x: 50, y: 50, width: 800, height: 600), styleMask: [.titled], backing: .buffered, defer: false)
window.isReleasedWhenClosed = false; window.contentView = host
host.paneVisible = true; host.paneFocused = true
window.orderBack(nil)
var session: SessionController? = try SessionController.localShell(surface: surface, executable: "/bin/bash", argv: ["--noprofile", "--norc", "-i"], kind: .direct)
defer { _ = tmux(["kill-server"], required: false); session?.stop(); session = nil; window.close() }
session!.start(); settle(1)

let conf = work.appendingPathComponent("native-tmux.conf")
try "set -g mouse on\nset -g assume-paste-time 0\nset -g default-shell /bin/\(testShell)\nset -g default-command '\(shellCommand)'\n".write(to: conf, atomically: true, encoding: .utf8)
surface.sendCommittedText(terminalCommand("\(quote(tmuxBinary)) -S \(quote(socket)) -f \(quote(conf.path)) new-session -s probe") + "\r")
settle(1)

until("real tmux session exists") { tmux(["list-sessions", "-F", "#{session_name}"], required: false).contains("probe") }
// Load the actual delivered hook, never a second implementation of it.
if recovering {
    let script = ProcessInfo.processInfo.environment["GHOSTERM_MOUSE_RECOVERY_SCRIPT"]!
    tmux(["send-keys", "-t", "probe:0", "-l", "source " + quote(script)])
    tmux(["send-keys", "-t", "probe:0", "Enter"]);settle(0.3)
}
func text() -> String { surface.readText() ?? "" }
func reports(_ value: String) -> Int {
    let re = try! NSRegularExpression(pattern: "35;[0-9]+;[0-9]+M")
    return re.numberOfMatches(in: value, range: NSRange(value.startIndex..., in: value))
}
func leak() -> Bool { reports(text()) > 0 }
func mode(_ stage: String) { print("TMUX MODE", stage, tmux(["display-message", "-p", "-t", "probe:0", "any=#{mouse_any_flag},sgr=#{mouse_sgr_flag}"]).trimmingCharacters(in: .whitespacesAndNewlines)) }
var eventNumber = 1
func move() {
    for _ in 0..<8 {
        let event = NSEvent.mouseEvent(with: .mouseMoved, location: NSPoint(x: 80 + eventNumber * 12 % 600, y: 250), modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber, context: nil, eventNumber: eventNumber, clickCount: 0, pressure: 0)!
        host.mouseMoved(with: event); eventNumber += 1
    }
    settle()
}
func startTUI() -> Int32 {
    let pidfile = work.appendingPathComponent("native-tui.pid")
    try? FileManager.default.removeItem(at: pidfile)
    tmux(["send-keys", "-t", "probe:0", "-l", "/usr/bin/python3 \(quote(work.appendingPathComponent("tui.py").path)) \(quote(pidfile.path))"])
    tmux(["send-keys", "-t", "probe:0", "Enter"])
    until("TUI enabled mouse reporting") {
        guard let pid = try? String(contentsOf: pidfile).trimmingCharacters(in: .whitespacesAndNewlines), !pid.isEmpty else { return false }
        return text().contains("TUI_READY:" + pid) && surface.isMouseCaptured
    }
    return Int32(try! String(contentsOf: pidfile).trimmingCharacters(in: .whitespacesAndNewlines))!
}
move();check("clean shell receives no mouse text", !leak());mode("clean")
let healthy = startTUI(); move()
func mouseBytesReceived() -> Int {
    let input = work.appendingPathComponent("native-tui.pid.input")
    return (try? Data(contentsOf: input).count) ?? 0
}
until("healthy TUI really receives mouse reports") { mouseBytesReceived() > 0 }
let beforePauseBytes = mouseBytesReceived()
check("running TUI receives mouse without shell leakage", !leak())
let serverPID = Int32(tmux(["display-message", "-p", "#{pid}"]).trimmingCharacters(in: .whitespacesAndNewlines))!
check("pause isolated tmux event loop", Darwin.kill(serverPID, SIGSTOP) == 0)
NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.willSleepNotification, object: nil)
surface.setFocused(false);surface.setVisible(false);move()
NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.didWakeNotification, object: nil)
surface.setVisible(true);surface.setFocused(true)
check("resume isolated tmux event loop", Darwin.kill(serverPID, SIGCONT) == 0)
move();check("server pause/resume and synthetic wake alone do not leak", !leak())
until("healthy mouse reporting still works after synthetic wake") { mouseBytesReceived() > beforePauseBytes }
check("normal TUI cleanup", Darwin.kill(healthy, SIGTERM) == 0)
settle(0.5);move();check("normal exit leaves shell usable without mouse leakage", !leak());mode("normal exit")
let broken = startTUI()
check("abrupt TUI exit", Darwin.kill(broken, SIGKILL) == 0)
settle(0.5);move();check(recovering ? "FIX automatic cleanup after crash" : "REPRO mouse reports reach shell after crash", recovering ? !leak() : leak());mode("crashed program")
try text().write(to: work.appendingPathComponent("\(evidencePrefix)-after-crash.txt"), atomically: true, encoding: .utf8)
print("LEAK", text().components(separatedBy: .newlines).filter { $0.contains("35;") }.joined(separator: " | "))
surface.sendCommittedText("\u{3}");settle()
if testCodex {
let codexDir = work.path
let codexCommand = "/usr/bin/env PATH=" + quote(ProcessInfo.processInfo.environment["PATH"] ?? "/opt/homebrew/bin:/usr/bin:/bin") + " " + quote(ProcessInfo.processInfo.environment["GHOSTERM_MOUSE_CODEX_BINARY"] ?? "/opt/homebrew/bin/codex") + " --no-daemon -c check_for_update_on_startup=false --sandbox read-only --ask-for-approval never -C " + quote(codexDir) + " -c " + quote("projects.\"" + codexDir + "\".trust_level=\"trusted\"")
surface.sendCommittedText(codexCommand + "\r")
settle(5)
if text().contains("Update available") { surface.sendCommittedText("\u{1b}");settle(4) }
try text().write(to: work.appendingPathComponent("codex-start-screen.txt"), atomically: true, encoding: .utf8)
print("CODEX startup captured; mode:", tmux(["display-message", "-p", "-t", "probe:0", "any=#{mouse_any_flag},sgr=#{mouse_sgr_flag},command=#{pane_current_command}"]))
check("Codex TUI started", text().contains(">_ OpenAI Codex"))
let beforeCodex = reports(text())
move();settle(1)
try text().write(to: work.appendingPathComponent("codex-after-mouse.txt"), atomically: true, encoding: .utf8)
print("CODEX mouse report text delta:",reports(text()) - beforeCodex)
surface.sendCommittedText("GHOSTERM_INPUT_PROBE")
settle(0.4)
print("CODEX keyboard probe visible:", text().contains("GHOSTERM_INPUT_PROBE"))
try text().write(to: work.appendingPathComponent("codex-after-keyboard.txt"), atomically: true, encoding: .utf8)
check("Codex composer is ready",text().contains(">_ OpenAI Codex") && text().contains("GHOSTERM_INPUT_PROBE"))
let codexBeforeWake = reports(text())
NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.willSleepNotification, object: nil)
surface.setFocused(false);surface.setVisible(false)
check("pause isolated server with Codex active", Darwin.kill(serverPID, SIGSTOP) == 0)
move()
NSWorkspace.shared.notificationCenter.post(name: NSWorkspace.didWakeNotification, object: nil)
surface.setVisible(true);surface.setFocused(true)
check("resume isolated server with Codex active", Darwin.kill(serverPID, SIGCONT) == 0)
move();settle(0.5)
check("Codex mouse sequences are not inserted after synthetic wake",reports(text()) == codexBeforeWake)
check("Codex draft survives synthetic wake",text().contains("GHOSTERM_INPUT_PROBE"))
tmux(["detach-client"]);settle(0.5)
surface.sendCommittedText(terminalCommand("\(quote(tmuxBinary)) -S \(quote(socket)) attach-session -t probe") + "\r")
settle(0.7);move()
check("Codex draft survives tmux reattach",text().contains("GHOSTERM_INPUT_PROBE"))
check("Codex mouse reports are not inserted after reattach",reports(text()) == codexBeforeWake)
surface.sendCommittedText("_AFTER_WAKE");settle(0.4)
check("Codex keyboard still works after reattach",text().contains("GHOSTERM_INPUT_PROBE_AFTER_WAKE"))
print("DONE: Codex input verified without submitting prompts")
} else {
let originalPane = tmux(["display-message", "-p", "-t", "probe:0", "#{pane_id}:#{pane_pid}"]).trimmingCharacters(in: .whitespacesAndNewlines)
tmux(["detach-client"]);settle(0.5)
surface.sendCommittedText(terminalCommand("\(quote(tmuxBinary)) -S \(quote(socket)) attach-session -t probe") + "\r")
settle(0.7)
let before = text()
move();settle()
check(recovering ? "FIX reattach has no mouse report leakage" : "REPRO reattach leaks new reports", recovering ? reports(text()) == 0 : reports(text()) > reports(before))
try text().write(to: work.appendingPathComponent("\(evidencePrefix)-after-reattach.txt"), atomically: true, encoding: .utf8)
surface.sendCommittedText("\u{2}c");settle(0.5)
check("tmux keyboard prefix remains operational", tmux(["list-windows", "-t", "probe"]).split(separator: "\n").count == 2)
check("original pane and shell PID survived reconnect", tmux(["display-message", "-p", "-t", "probe:0", "#{pane_id}:#{pane_pid}"]).trimmingCharacters(in: .whitespacesAndNewlines) == originalPane)
move();check("new clean tmux pane is unaffected", !leak())
tmux(["detach-client"]);settle(0.3)
let freshSurface = try TerminalSurface(app: app)
let freshHost = TerminalHostView(surface: freshSurface)
window.contentView = freshHost;freshHost.paneVisible = true
let freshSession = try SessionController.localShell(surface: freshSurface, executable: "/bin/bash", argv: ["--noprofile", "--norc", "-i"], kind: .direct)
defer { freshSession.stop() }
freshSession.start();settle(0.5)
freshSurface.sendCommittedText(terminalCommand("\(quote(tmuxBinary)) -S \(quote(socket)) attach-session -t probe:0") + "\r")
settle(0.7)
let priorFresh = reports(freshSurface.readText() ?? "")
for i in 1...8 {
    let e = NSEvent.mouseEvent(with: .mouseMoved, location: NSPoint(x: 100 + i * 15, y: 250), modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window.windowNumber, context: nil, eventNumber: i, clickCount: 0, pressure: 0)!
    freshHost.mouseMoved(with: e)
}
settle(0.4)
check(recovering ? "FIX fresh terminal reattach stays clean" : "REPRO fresh terminal reattach leaks", recovering ? reports(freshSurface.readText() ?? "") == 0 : reports(freshSurface.readText() ?? "") > priorFresh)
mode("fresh terminal reattach")
try (freshSurface.readText() ?? "").write(to: work.appendingPathComponent("\(evidencePrefix)-fresh-surface.txt"), atomically: true, encoding: .utf8)
print("DONE: recovery=\(recovering); actual system sleep not performed")

}
