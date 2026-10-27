// Engine-free management Transport. Explicit stop is never inferred from
// detach, view destruction or connection failure; local/SSH share this owner.
import Foundation
import GhostermCore

public final class SessionControlTransport {
    public enum Operation { case inspect, terminate }
    public enum Outcome {
        case observed(RecoverableSession.Row?)
        case termination(RecoverableSession.Stop, RecoverableSession.Row?)
    }
    public let owner = DispatchQueue(label: "dev.ghosterm.session-control")
    private let context: SessionRecordContext
    private let operation: Operation
    private let completion: (Result<Outcome, Swift.Error>) -> Void
    private var record: SessionRecord
    private var session: RecoverableSession?
    private var authentication: RecoverableSession.Authentication?
    private var timer: DispatchSourceTimer?
    private enum Stage { case initial, lookup, switching, catalog, persisting, stopping, observing }
    private var stage = Stage.initial
    private var finished = false
    private var deadline: UInt64 = 0
    private var nextRefresh: UInt64 = 0
    private var lastStop: RecoverableSession.Stop?
    private var finalResult: Result<Outcome, Swift.Error>?

    public init(record: SessionRecord, directory: URL, operation: Operation,
                recordChanged: ((SessionRecord) -> Void)? = nil,
                completion: @escaping (Result<Outcome, Swift.Error>) -> Void) {
        self.record = record; self.operation = operation; self.completion = completion
        context = SessionRecordContext(record: record, directory: directory)
        context.onChange = recordChanged
    }
    /// Metadata callbacks only; credential and hostkey completions use owner.
    public func start(authentication: RecoverableSession.Authentication? = nil) {
        owner.async { [self] in
            guard stage == .initial, !finished else { return }
            self.authentication = authentication
            deadline = DispatchTime.now().uptimeNanoseconds + 60_000_000_000
            context.snapshot { [self] result in owner.async { [self] in
                guard !finished else { return }
                do {
                    record = try result.get()
                    guard record.instance.contains(where: { $0 != 0 }), record.session > 0 || record.admitted else {
                        throw GhostermError.failed("创建尚未发送，无可查询或终止的会话")
                    }
                    try open(catalog: record.session > 0)
                    let timer = DispatchSource.makeTimerSource(queue: owner)
                    timer.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .milliseconds(1))
                    timer.setEventHandler { [weak self] in self?.tick() }
                    self.timer = timer; timer.resume()
                } catch { finish(.failure(error)) }
            } }
        }
    }
    public func withSession(_ body: (RecoverableSession) -> Void) {
        dispatchPrecondition(condition: .onQueue(owner))
        if let session, !finished { body(session) }
    }
    public func cancel() { owner.async { [self] in finish(.failure(GhostermError.failed("会话管理操作已取消；未确认的停止结果仍保留为未知"))) } }
    private func open(catalog: Bool) throws {
        let hex = record.endpoint.contract
        guard hex.count == 64 else { throw GhostermError.failed("会话协议标识无效") }
        let hash = stride(from: 0, to: hex.count, by: 2).compactMap { n -> UInt8? in
            let a = hex.index(hex.startIndex, offsetBy: n)
            return UInt8(hex[a..<hex.index(a, offsetBy: 2)], radix: 16)
        }
        guard hash.count == 32 else { throw GhostermError.failed("会话协议标识无效") }
        var options = RecoverableSession.Options(kind: record.kind == .local ? .local : .ssh,
            proxy: record.endpoint.proxy, daemon: record.endpoint.daemon, runtime: record.endpoint.runtime,
            compatibility: hash, format: catalog ? 0 : 0x47545643)
        options.instance = record.instance; options.catalogOnly = catalog
        let sink: RecoverableSession.Sink?
        if catalog { sink = nil } else {
            guard let nonce = record.nonce, record.admitted else { throw GhostermError.failed("创建查询缺少原 nonce") }
            options.pendingNonce = nonce
            // Query-only recovery may reach ATTACH inside the shared coordinator.
            // It never claims a snapshot was consumed and never enables input.
            sink = .init(snapshot: { _, _, _, _, _ in .busy }, event: { _, _, _ in .failed(7) }, applied: { nil }, invalidated: { _, _ in }, receipt: { _ in })
        }
        session = try RecoverableSession(options: options, sink: sink, authentication: authentication, owner: owner)
        stage = catalog ? .catalog : .lookup
    }
    private func tick() {
        guard !finished else { return }
        let now = DispatchTime.now().uptimeNanoseconds
        guard now < deadline else { finish(.failure(GhostermError.failed(timeoutMessage()))); return }
        if stage == .persisting { return }
        if stage == .switching {
            guard session?.close() ?? true else { return }
            session = nil
            do { try open(catalog: true) } catch { finish(.failure(error)) }
            return
        }
        guard let session else { return }
        let state = session.step()
        guard let info = session.info else { finish(.failure(GhostermError.failed("无法读取会话管理状态"))); return }
        if info.instance.contains(where: { $0 != 0 }), info.instance != record.instance {
            finish(.failure(GhostermError.failed("会话管理连接不属于原 daemon 实例"))); return
        }
        if stage == .lookup, info.session > 0 {
            record.session = info.session; context.observe(info)
            session.cancel(); stage = .switching
            return
        }
        if info.stop.state == .unknown {
            context.observe(info)
            finish(.success(.termination(info.stop, nil))); return
        }
        if state == .failed {
            context.observe(info)
            finish(.failure(GhostermError.status(info.error, failureMessage(info.error)))); return
        }
        if stage == .stopping {
            guard info.stop.state == .known else { return }
            context.observe(info); lastStop = info.stop
            guard info.stop.status == 0 else { finish(.success(.termination(info.stop, nil))); return }
            stage = .observing; nextRefresh = 0
        }
        guard state == .catalog, stage == .catalog || stage == .observing else { return }
        do {
            guard let rows = try session.catalog() else { return }
            let row = rows.first { $0.session == record.session }
            if let row { context.observe(row: row, instance: info.instance) }
            if row == nil {
                stage = .persisting
                context.observeAbsent(instance: info.instance, session: record.session) { [self] result in owner.async { [self] in
                    guard !finished else { return }
                    switch result {
                    case .failure(let error): finish(.failure(error))
                    case .success:
                        if let stop = lastStop { finish(.success(.termination(stop, nil))) }
                        else { finish(.success(.observed(nil))) }
                    }
                } }
                return
            }
            if operation == .inspect {
                finish(.success(.observed(row))); return
            }
            if row?.hasResult == true {
                if let stop = lastStop { finish(.success(.termination(stop, row))) }
                else { finish(.success(.observed(row))) }
                return
            }
            if stage == .catalog {
                if row?.terminating == true { stage = .observing; nextRefresh = 0 }
                else {
                    stage = .persisting
                    context.prepareStop { [self] result in owner.async { [self] in
                        guard !finished, stage == .persisting else { return }
                        do {
                            try result.get()
                            guard DispatchTime.now().uptimeNanoseconds < deadline else { throw GhostermError.failed("\(record.displayTitle) 停止意图已保存但管理超时；未发送停止请求") }
                            guard self.session?.terminate(session: record.session) == .queued else { throw GhostermError.failed("停止请求未能入队；请查询实际状态") }
                            stage = .stopping
                        } catch { finish(.failure(error)) }
                    } }
                    return
                }
            }
            if now >= nextRefresh {
                guard session.refreshCatalog() == .queued else { throw GhostermError.failed("无法刷新结束状态") }
                nextRefresh = now + 250_000_000
            }
        } catch { finish(.failure(error)) }
    }
    private func failureMessage(_ status: Int32) -> String {
        let op = operation == .inspect ? "检查状态" : "终止会话"
        // GT_ERR_NOT_FOUND 不是连接没建上——而是 daemon 明确答复没有此会话。
        // 常见因：daemon 已重启/catalog 丢；会话被别的客户端清理完回收；保存
        // 记录的 session id 跟实际 catalog 对不上。此时「终止会话」已经没有
        // 后台可杀，用户的安全出路是「遗忘记录（不终止后端）」。
        if status == 2 {
            return "\(record.displayTitle) \(op)：daemon 已无此会话（可能已结束或 daemon 重启）；可用「遗忘记录（不终止后端）」清理本机记录"
        }
        // 其他码：仍是“连接层/协议层失败”的统一表述，但带上会话与操作。
        return "\(record.displayTitle) \(op)连接失败；可稍后再试或「遗忘记录（不终止后端）」"
    }
    private func timeoutMessage() -> String {
        let op = operation == .inspect ? "检查状态" : "终止会话"
        let stageText: String
        switch stage {
        case .initial: stageText = "尚未开始"
        case .lookup: stageText = "等待 daemon 查询"
        case .switching: stageText = "切换到清单查询"
        case .catalog: stageText = "读取会话清单"
        case .persisting: stageText = "持久化停止意图"
        case .stopping: stageText = "等待停止回执"
        case .observing: stageText = "等待停止后状态刷新"
        }
        // 既告知哪条、哪一步，又保留“未自动重发停止请求”这条终止语义——
        // 顶部横幅不应把“超时”折叠成“已终止”或“已失败”。
        return "\(record.displayTitle) \(op)超时（卡在：\(stageText)）；请重新查询实际状态，未自动重发停止请求"
    }
    private func finish(_ result: Result<Outcome, Swift.Error>) {
        guard !finished else { return }; finished = true; timer?.cancel(); timer = nil
        finalResult = result
        session?.cancel()
        // Cancellation itself can turn a submitted request into UNKNOWN. Store
        // that fact before publishing completion and never replay the operation.
        if let info = session?.info, info.stop.state != .none { context.observe(info) }
        reap()
    }
    private func reap() {
        if let session, !session.close() { owner.asyncAfter(deadline: .now() + .milliseconds(10)) { [self] in reap() }; return }
        session = nil
        guard let result = finalResult else { return }; finalResult = nil
        context.snapshot { [self] saved in
            let output: Result<Outcome, Swift.Error>
            switch saved { case .success: output = result; case .failure(let error): output = .failure(error) }
            DispatchQueue.main.async { [self] in completion(output) }
        }
    }
    deinit { timer?.cancel() }
}
