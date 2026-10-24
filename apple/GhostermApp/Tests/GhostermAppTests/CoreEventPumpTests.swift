// CoreEventPumpTests — 单一事件泵的扇出语义（无网可跑）。
//
// 存在的理由：`gt_core_poll_events` 是**一次性消费**的。S4 加传输队列前，
// 只有 SFTPBrowser 在轮询；再加一个轮询者就会互相抢事件。本测试钉住
// 「扇出，不抢」：同一批事件，两个订阅者都要**各自收到全部**。
import GhostermCore
@testable import GhostermAppModel
import XCTest

final class CoreEventPumpTests: XCTestCase {
    private func jobDone(_ id: UInt64, _ code: Int32) -> CoreEvent {
        CoreEvent(kind: .jobDone, sessionID: 0, jobID: id, code: code, message: "x")
    }
    private func progress(_ id: UInt64) -> CoreEvent {
        CoreEvent(kind: .progress, sessionID: 0, jobID: id, code: 0, message: "sftp 1/2")
    }

    func testFanOutBothListeners() {
        let pump = CoreEventPump.shared
        var a: [UInt64] = [], b: [UInt64] = []
        let ta = pump.addListener { a.append($0.jobID) }
        let tb = pump.addListener { b.append($0.jobID) }
        defer { pump.removeListener(ta); pump.removeListener(tb) }

        pump.dispatch([jobDone(1, 0), progress(2)])
        XCTAssertEqual(a, [1, 2], "订阅者 A 应收到全部事件")
        XCTAssertEqual(b, [1, 2], "订阅者 B 应收到全部事件（不被 A 抢走）")
    }

    func testUnsubscribeStopsDelivery() {
        let pump = CoreEventPump.shared
        var a = 0
        let ta = pump.addListener { _ in a += 1 }
        pump.removeListener(ta)
        pump.dispatch([jobDone(9, 0)])
        XCTAssertEqual(a, 0, "退订后不应再收到事件")
    }
}
