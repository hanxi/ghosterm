// Closing a pane cancels on main, then its IO owner must finish and release.
// An infinite write must fail the resource-lifetime assertion even though
// stop() itself no longer waits on the IO queue.
@testable import GhostermAppModel
import GhostermCore
import GhostermTerminal
import XCTest

@MainActor
final class StopBoundedTests: XCTestCase {
    /// 子进程永不读 stdin（raw 模式）。经生产链路把大 payload 交给会话线程
    /// 写，塞满 pty 输入队列触发 bounded-write，然后 stop() 必须 2s 内返回。
    /// 这一条最贴近用户症状——旧实现会卡死整个 app。
    func testStopReturnsWhenChildNeverReadsStdin() throws {
        let app = try TerminalApp.shared()
        let surface = try TerminalSurface(app: app)
        // stty -icanon -echo：关 canonical（否则超长输入被丢弃而非 EAGAIN，
        // 走不到 poll 分支）；sleep 99999：子进程永远不读 stdin。
        var session: SessionController? = try SessionController.localShell(
            surface: surface, executable: "/bin/sh",
            argv: ["-c", "stty -icanon -echo; sleep 99999"], kind: .direct)
        let pane = TerminalPane()
        pane.attach(surface: surface, session: session!)
        weak var releasedSession = session
        session = nil
        pane.start()

        // 预热：等 stty 在子进程侧生效（与 C §13a 的 300ms nanosleep 同构）。
        // 会话线程的 readLoop 每 20ms 自动排空 stty 的输出，无需手动 pre-drain。
        Thread.sleep(forTimeInterval: 0.3)

        // 经生产链路注入大 payload：sendText → libghostty 编码 → onInput →
        // enqueue(.write) → 会话线程 drainPending → transport.write。
        // 300 KB 远超 raw 模式下 ~1 KiB 的 pty 输入队列，必然触发 EAGAIN →
        // bounded-write 的 poll。旧实现（P4：poll(-1)）会让会话线程在此死等。
        let payload = String(repeating: "x", count: 300_000)
        let w0 = Date()
        surface.sendText(payload)
        // 等会话线程 drainPending 拾起这笔 write 并进入 bounded-write。
        // 正常实现：write 在 GT_PTY_WRITE_GRACE_MS (100ms) 内有界返回，余量
        // 攒进 wtail；旧实现：会话线程卡在 poll(-1)，永远不返回。
        Thread.sleep(forTimeInterval: 0.3)
        let wElapsed = Date().timeIntervalSince(w0)
        XCTAssertLessThan(wElapsed, 1.0,
            "注入 + 会话线程写必须 < 1s（旧实现会话线程会无限等）；实际 \(String(format: "%.2f", wElapsed))s")

        let t0 = Date()
        pane.stop()
        let elapsed = Date().timeIntervalSince(t0)
        XCTAssertLessThan(elapsed, 2.0,
            "stop() 必须 < 2s（旧实现会卡死整个 app）；实际 \(String(format: "%.2f", elapsed))s")
        XCTAssertFalse(pane.isRunning, "stop() 后 pane 应不再 running")
        let deadline = Date().addingTimeInterval(3)
        while releasedSession != nil && Date() < deadline { Thread.sleep(forTimeInterval: 0.01) }
        XCTAssertNil(releasedSession, "取消后的 IO owner 必须完成；不能把无限写藏到后台")
    }
}
