// CoreEventPump — core 事件队列的**唯一消费者**。
//
// 为什么必须有它（S4 前置）：`gt_core_poll_events` 是**一次性消费**的——
// 每取一条就从环形缓冲里移除。先前只有 `SFTPBrowser` 自己在轮询；一旦再加
// 一个轮询者（传输队列），两者会互相**抢事件**：谁先 poll 谁拿走，另一个
// 永远看不到。故把唯一那条取事件的线收进这里，再扇出给所有订阅者。
//
// 线程：主线程 Timer 驱动；订阅者的回调都在主线程（@MainActor）。
import Foundation
import GhostermCore

// 刻意**不**标 @MainActor：它要能在 deinit（非隔离上下文）里退订。
// 线程安全靠一把锁；Listener 的回调都在**主线程**跑（Timer 在主 RunLoop，
// 测试也在主线程调 drain）——故回调里可安全碰 @MainActor 状态。
public final class CoreEventPump: @unchecked Sendable {
    public static let shared = CoreEventPump()

    private let lock = NSLock()
    private var listeners: [UUID: (CoreEvent) -> Void] = [:]
    private var timer: Timer?

    init() {}

    /// 订阅事件（首次订阅即启动轮询）。返回订阅票，用于退订。
    @discardableResult
    public func addListener(_ fn: @escaping (CoreEvent) -> Void) -> UUID {
        let id = UUID()
        lock.lock(); listeners[id] = fn; lock.unlock()
        start()
        return id
    }

    public func removeListener(_ id: UUID) {
        lock.lock(); listeners.removeValue(forKey: id); lock.unlock()
        if isEmpty { stop() }
    }

    private var isEmpty: Bool {
        lock.lock(); defer { lock.unlock() }; return listeners.isEmpty
    }

    /// 供测试**直接驱动**：取一批事件并同步派发给订阅者，返回条数。
    /// Timer 也走它——所以测试不必等 Timer，只调 drain 即可（确定性）。
    @discardableResult
    public func drain(max: Int = 16) -> Int {
        let events = Ghosterm.pollEvents(max: max)
        dispatch(events)
        return events.count
    }

    /// 把一批事件**扇出**给全部订阅者（每个订阅者都收到每一条——这就是
    /// 「不互相抢事件」的落点）。drain 用它；测试也用它注入合成事件。
    func dispatch(_ events: [CoreEvent]) {
        lock.lock(); let fns = Array(listeners.values); lock.unlock()
        for e in events { for fn in fns { fn(e) } }
    }

    private func start() {
        guard timer == nil else { return }
        // 0.1s：低频事件（PROGRESS 已按 ~16ms 合帧、JOB_DONE 一次性），
        // 这个节奏足够跟手，又不刷主线程。
        let t = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in
            self?.drain()
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func stop() {
        timer?.invalidate()
        timer = nil
    }
}
