// StopBoundedTests — SessionController.stop() 在子进程不读 stdin 时必须有界返回。
//
// 用户实测「卡住的程序拖垮整个 app」的 Swift 侧回归：后台/前台程序永远不读
// stdin → pty 输入队列满 → 任何后续 transport.write 触发 bounded-write
// 路径（旧实现是 poll(-1) 死等）；此时 SessionController.stop() 必须仍能
// 在 ~2s 内返回。旧实现的 stop() 卡在 queue.sync 等会话线程，会话线程
// 卡在 transport.write 的 poll(-1) → 主线程一起卡死（整个 app 无响应）。
//
// 与 C 侧 test_transport §13 互补：那里验「写要有界」（同进程直接看 elapsed），
// 这里验「stop 要有界」（跨线程通过 queue.sync 屏障），是同一契约的 Swift 端。
//
// 关键：write 必须走**生产链路**（surface.sendText → libghostty 编码 →
// onInput → SessionController.enqueue(.write) → 会话线程 drainPending →
// transport.write），不能从测试线程直接调 transport.write——core 契约是
// read/write 由同一会话线程独占（libssh2 session 非线程安全；pty 的 wtail
// 被 read 的 flush 和 write 的 append 共享），跨线程直接写是竞态。走生产
// 链路才能让 stop() 的 queue.sync 屏障真正卡在会话线程的 write 上。
//
// 子进程必须 `stty -icanon -echo` 关掉 canonical 模式：canonical 下超长无
// 换行输入会被内核**丢弃**而非 EAGAIN，永远走不到 poll 分支（这正是第一版
// 测试无牙的原因）。raw 模式下 ~1 KiB 输入队列填满才产生 EAGAIN → 触发
// bounded-write 的 poll。与 C §13a 同构。
//
// 不依赖窗口 / sshd。
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
        let session = try SessionController.localShell(
            surface: surface, executable: "/bin/sh",
            argv: ["-c", "stty -icanon -echo; sleep 99999"], kind: .direct)
        let pane = TerminalPane()
        pane.attach(surface: surface, session: session)
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

        // 关键断言：stop() 必须在 2s 内返回。
        // 走 pane.stop()（生产路径：关标签页 → pane.stop → session.stop →
        // queue.sync）。旧实现的 stop() 卡在 queue.sync（会话线程被
        // transport.write 的 poll(-1) 钉死 → 整个 app 无响应）；新实现的
        // 会话线程最多 20ms 一次 read 超时，write 在 100ms 内有界返回 →
        // queue.sync 立刻通过。
        let t0 = Date()
        pane.stop()
        let elapsed = Date().timeIntervalSince(t0)
        XCTAssertLessThan(elapsed, 2.0,
            "stop() 必须 < 2s（旧实现会卡死整个 app）；实际 \(String(format: "%.2f", elapsed))s")
        XCTAssertFalse(pane.isRunning, "stop() 后 pane 应不再 running")
    }
}
