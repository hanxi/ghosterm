import AppKit
import Darwin
import GhostermCore
import GhostermTerminal
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

/// Real window → full surface → local PTY → system ssh → tmux. Read sizes
/// through a separate fixture command, without typing or switching tmux windows.
@MainActor final class TmuxResizeTests: XCTestCase {
    private func until(_ predicate: () -> Bool, file: StaticString = #filePath, line: UInt = #line) async throws {
        let deadline = Date().addingTimeInterval(8)
        while !predicate(), Date() < deadline { try await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertTrue(predicate(), file: file, line: line)
    }

    private func quote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    func testLocalTmuxResizesWithoutInput() async throws {
        try await exercise(sshArguments: nil)
    }

    func testLocalShellSSHResizesRemoteTmuxWithoutInput() async throws {
        let env = ProcessInfo.processInfo.environment
        guard env["GHOSTERM_SSH_HOST"] == "127.0.0.1",
              let port = env["GHOSTERM_SSH_PORT"], let user = env["GHOSTERM_SSH_USER"],
              let key = env["GHOSTERM_SSH_KEY"] else {
            throw XCTSkip("Requires the isolated loopback sshd from scripts/apple-app-gate.sh")
        }
        try await exercise(sshArguments: ["/usr/bin/ssh", "-F", "/dev/null", "-tt",
            "-i", key, "-p", port, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            "-o", "ConnectTimeout=5", user + "@127.0.0.1"])
    }

    private func exercise(sshArguments: [String]?) async throws {
        _ = NSApplication.shared; NSApplication.shared.setActivationPolicy(.accessory)
        let root = sessionTestDirectory().deletingLastPathComponent()
            .appendingPathComponent("resize-" + String(UUID().uuidString.prefix(8)))
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let binary = ProcessInfo.processInfo.environment["TMUX_BINARY"] ?? "/opt/homebrew/bin/tmux"
        let socket = root.appendingPathComponent("socket").path
        XCTAssertLessThan(socket.utf8.count, 104)
        let config = root.appendingPathComponent("tmux.conf")
        try "set -g default-shell /bin/sh\nset -g default-command 'exec /bin/sh'\nset -g status off\n".write(to: config, atomically: true, encoding: .utf8)

        func tmux(_ args: [String]) throws -> String {
            let process = Process(), output = Pipe()
            process.executableURL = URL(fileURLWithPath: binary)
            process.arguments = ["-f", config.path, "-S", socket] + args
            process.standardOutput = output; process.standardError = FileHandle.nullDevice
            try process.run()
            let data = output.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
            guard process.terminationStatus == 0 else { throw GhostermError.failed("tmux fixture command failed: \(args)") }
            return String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        _ = try tmux(["new-session", "-d", "-s", "resize"])
        defer { _ = try? tmux(["-N", "kill-server"]) }
        let paneID = try tmux(["list-panes", "-t", "resize", "-F", "#{pane_id}:#{pane_pid}"])
        let model = isolatedAppModel(); defer { model.disconnectForTesting() }
        model.preferences.defaultShell = "/bin/sh"
        // Deterministically reproduce a signal-blocking GUI/worker caller.
        // The production PTY must clear the child mask without changing ours.
        var blocked: sigset_t = 0, previous: sigset_t = 0
        sigemptyset(&blocked); sigaddset(&blocked, SIGWINCH)
        XCTAssertEqual(pthread_sigmask(SIG_BLOCK, &blocked, &previous), 0)
        model.connect(to: .localShell)
        XCTAssertEqual(pthread_sigmask(SIG_SETMASK, &previous, nil), 0)
        let pane = try XCTUnwrap(model.activePane), surface = try XCTUnwrap(pane.surface)
        let rootView = makeRootHostingView(model: model)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 520), styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        window.contentView = rootView; TestWindows.presentOffscreenAsKey(window)
        defer { window.orderOut(nil); window.contentView = nil }
        let attach = [binary, "-S", socket, "-N", "attach-session", "-t", "resize"].map(quote).joined(separator: " ")
        let command = sshArguments.map { $0.map(quote).joined(separator: " ") + " " + quote(attach) } ?? attach
        surface.sendCommandLine(command)
        func sizesMatch() -> Bool {
            let expected = "\(surface.gridSize.cols) \(surface.gridSize.rows)"
            return (try? tmux(["list-clients", "-F", "#{client_width} #{client_height}"])) == expected
                && (try? tmux(["list-panes", "-t", "resize", "-F", "#{pane_width} #{pane_height}"])) == expected
        }
        try await until { rootView.layoutSubtreeIfNeeded(); return sizesMatch() }
        for size in [NSSize(width: 1100, height: 680), NSSize(width: 760, height: 440)] {
            let before = surface.gridSize
            window.setContentSize(size)
            try await until { rootView.layoutSubtreeIfNeeded(); return surface.gridSize != before }
            try await until { sizesMatch() }
            XCTAssertEqual(try tmux(["list-panes", "-t", "resize", "-F", "#{pane_id}:#{pane_pid}"]), paneID)
            XCTAssertTrue(pane.status.isReady)
        }
    }
}
