import CoreFoundation
import Foundation

public struct PluginTaskDescriptor: Equatable, Sendable {
    public let viewID: String
    public let revision: String
    public let title: String
    public let unit: String
    public let cancellable: Bool
}
public struct PluginTaskSnapshot: Equatable, Sendable {
    public enum Phase: String, Sendable {
        case queued, running, cancelling, succeeded, failed, cancelled, unknown
        public var terminal: Bool { self == .succeeded || self == .failed || self == .cancelled }
        public var label: String {
            switch self {
            case .queued: return "等待开始"
            case .running: return "正在运行"
            case .cancelling: return "等待任务确认停止"
            case .succeeded: return "已完成"
            case .failed: return "已失败"
            case .cancelled: return "已确认取消"
            case .unknown: return "结果不明"
            }
        }
    }
    public enum Result: Equatable, Sendable {
        case none, success(String), failed(String, String), cancelled(String), unknown(String)
        public var text: String? {
            switch self {
            case .none: return nil
            case .success(let summary): return summary
            case .failed(let code, let message): return "\(code)：\(message)"
            case .cancelled(let effect): return effect == "none" ? "任务已停止，无外部影响" : "任务已停止，可能已有外部影响"
            case .unknown(let reason): return reason
            }
        }
    }
    public let viewID: String
    public let revision: String
    public let sequence: String
    public let phase: Phase
    public let completed: String
    /// nil is unknown; "0" is an explicitly known empty task.
    public let total: String?
    public let message: String
    public let result: Result
}

enum PluginTaskCodec {
    typealias V = PluginViewValue
    enum RPCFailure: Error {
        case failed(String, String, String), cancelled(String)
        var message: String {
            switch self {
            case .failed(let code, let message, let effect): return "请求失败（\(code)）：\(message)；\(effect == "none" ? "未产生执行效果" : "可能已有执行效果")"
            case .cancelled(let effect): return "请求已取消；\(effect == "none" ? "未产生执行效果" : "可能已有执行效果")，任务状态需另行读取"
            }
        }
    }
    static func success(_ data: Data) throws -> [String: Any] {
        // Generic wire errors allow 1024 scalars (including four-byte Unicode
        // and escaped controls). Only successful task values have the 2 KiB cap.
        guard data.count <= 8192, String(data: data, encoding: .utf8) != nil else { throw V.Failure.invalidResponse }
        try ExtensionRelayBootstrap.validateJSON(data)
        guard let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw V.Failure.invalidResponse }
        if envelope["kind"] as? String == "failed" {
            try V.keys(envelope, ["kind", "error"])
            guard let error = envelope["error"] as? [String: Any] else { throw V.Failure.invalidResponse }
            try V.keys(error, ["code", "message", "effect"])
            guard let code = error["code"] as? String,
                  ["UNSUPPORTED", "DENIED", "EXPIRED", "INVALID_INPUT", "RESOURCE_LIMIT", "CANCELLED", "IO", "PROTOCOL", "INTERNAL", "BUSY", "INTEGRITY"].contains(code),
                  let effect = error["effect"] as? String, ["none", "possible"].contains(effect),
                  let message = error["message"] as? String, (1...1024).contains(message.unicodeScalars.count) else { throw V.Failure.invalidResponse }
            let visible = message.unicodeScalars.map { scalar in
                scalar.value < 32 || scalar.value == 127 ? String(format: "\\u%04x", scalar.value) : String(scalar)
            }.joined()
            throw RPCFailure.failed(code, visible, effect)
        }
        if envelope["kind"] as? String == "cancelled" {
            try V.keys(envelope, ["kind", "effect"])
            guard let effect = envelope["effect"] as? String, ["none", "possible"].contains(effect) else { throw V.Failure.invalidResponse }
            throw RPCFailure.cancelled(effect)
        }
        return try V.success(data)
    }
    static func counter(_ value: Any?) throws -> String {
        if value as? String == "0" { return "0" }
        return try V.revision(value)
    }
    static func descriptor(_ data: Data) throws -> PluginTaskDescriptor {
        let v = try success(data)
        try V.keys(v, ["viewId", "revision", "title", "unit", "cancellable"])
        guard let flag = v["cancellable"] as? NSNumber, CFGetTypeID(flag) == CFBooleanGetTypeID() else { throw V.Failure.invalidResponse }
        return .init(viewID: try V.identifier(v["viewId"]), revision: try V.revision(v["revision"]),
            title: try V.text(v["title"], maximum: 128, nonempty: true),
            unit: try V.text(v["unit"], maximum: 64, nonempty: true), cancellable: flag.boolValue)
    }
    static func scope(_ value: [String: Any], _ d: PluginTaskDescriptor) throws {
        guard try V.identifier(value["viewId"]) == d.viewID,
              try V.revision(value["revision"]) == d.revision else { throw V.Failure.invalidResponse }
    }
    static func snapshot(_ data: Data, descriptor d: PluginTaskDescriptor, previous: PluginTaskSnapshot? = nil) throws -> PluginTaskSnapshot {
        let v = try success(data)
        try V.keys(v, ["viewId", "revision", "sequence", "phase", "completed", "total", "message", "result"])
        try scope(v, d)
        guard let phase = (v["phase"] as? String).flatMap(PluginTaskSnapshot.Phase.init),
              let total = v["total"] as? [String: Any], let result = v["result"] as? [String: Any] else { throw V.Failure.invalidResponse }
        let units: String?
        if total["kind"] as? String == "known" { try V.keys(total, ["kind", "units"]); units = try counter(total["units"]) }
        else { try V.keys(total, ["kind"]); guard total["kind"] as? String == "unknown" else { throw V.Failure.invalidResponse }; units = nil }
        let completed = try counter(v["completed"])
        guard units == nil || UInt64(completed)! <= UInt64(units!)!, phase != .queued || completed == "0",
              d.cancellable || (phase != .cancelling && phase != .cancelled) else { throw V.Failure.invalidResponse }
        let parsed: PluginTaskSnapshot.Result
        switch phase {
        case .succeeded:
            try V.keys(result, ["kind", "summary"])
            guard result["kind"] as? String == "success", units == nil || completed == units else { throw V.Failure.invalidResponse }
            parsed = .success(try V.text(result["summary"], maximum: 256, nonempty: true))
        case .failed:
            try V.keys(result, ["kind", "code", "message"])
            guard result["kind"] as? String == "failed" else { throw V.Failure.invalidResponse }
            parsed = .failed(try V.text(result["code"], maximum: 64, nonempty: true), try V.text(result["message"], maximum: 256, nonempty: true))
        case .cancelled:
            try V.keys(result, ["kind", "effect"])
            guard result["kind"] as? String == "cancelled", let effect = result["effect"] as? String, ["none", "possible"].contains(effect) else { throw V.Failure.invalidResponse }
            parsed = .cancelled(effect)
        case .unknown:
            try V.keys(result, ["kind", "reason"])
            guard result["kind"] as? String == "unknown" else { throw V.Failure.invalidResponse }
            parsed = .unknown(try V.text(result["reason"], maximum: 128, nonempty: true))
        default:
            try V.keys(result, ["kind"])
            guard result["kind"] as? String == "none" else { throw V.Failure.invalidResponse }
            parsed = .none
        }
        let p = PluginTaskSnapshot(viewID: d.viewID, revision: d.revision, sequence: try V.revision(v["sequence"]),
            phase: phase, completed: completed, total: units, message: try V.text(v["message"], maximum: 128), result: parsed)
        if let old = previous {
            guard old.viewID == d.viewID, old.revision == d.revision,
                  UInt64(p.sequence)! >= UInt64(old.sequence)!, p.sequence != old.sequence || p == old,
                  UInt64(p.completed)! >= UInt64(old.completed)!, old.total == nil || p.total == old.total,
                  !old.phase.terminal || p == old,
                  old.phase != .running || p.phase != .queued,
                  old.phase != .cancelling || (p.phase != .queued && p.phase != .running) else { throw V.Failure.invalidResponse }
        }
        return p
    }
    static func cancellation(_ data: Data, descriptor d: PluginTaskDescriptor, requestID: String) throws -> String {
        let v = try success(data)
        try V.keys(v, ["viewId", "revision", "requestId", "kind"]); try scope(v, d)
        guard d.cancellable, try V.identifier(v["requestId"]) == requestID,
              let kind = v["kind"] as? String, ["requested", "alreadyFinished"].contains(kind) else { throw V.Failure.invalidResponse }
        return kind
    }
}

/// One observation and one cancellation identity per task. Unconfirmed actions
/// remain unconfirmed even when a later explicit read supplies fresh facts.
@MainActor public final class PluginTaskModel: ObservableObject {
    public typealias RPC = @MainActor (String, Data) async throws -> Data
    public static let methods = ["view.task.cancel", "view.task.describe", "view.task.read"]
    public enum Cancellation: Sendable { case idle, pending, requested, alreadyFinished, unconfirmed, failed, cancelled }
    @Published public private(set) var descriptor: PluginTaskDescriptor?
    @Published public private(set) var snapshot: PluginTaskSnapshot?
    @Published public private(set) var busy = false
    @Published public private(set) var valid = true
    @Published public private(set) var issue: String?
    @Published public private(set) var cancellation = Cancellation.idle
    @Published public private(set) var cancelRequestID: String?
    private var generation = UUID()
    private let rpc: RPC
    private let isCurrent: @MainActor () -> Bool
    private let expectedViewID: String?
    private let history: PluginTaskHistory?
    public init(rpc: @escaping RPC, isCurrent: @escaping @MainActor () -> Bool, expectedViewID: String? = nil, history: PluginTaskHistory? = nil) {
        self.rpc = rpc; self.isCurrent = isCurrent; self.expectedViewID = expectedViewID; self.history = history
    }
    public var available: Bool { valid && isCurrent() }
    public var canCancel: Bool { available && !busy && descriptor?.cancellable == true && cancelRequestID == nil &&
        (snapshot?.phase == .queued || snapshot?.phase == .running) }
    public var canAutoRefresh: Bool { available && !busy && issue == nil && cancellation != .unconfirmed &&
        snapshot.map { !$0.phase.terminal && $0.phase != .unknown } == true }
    public func load() async {
        guard available, !busy else { return }
        let token = UUID(); generation = token; busy = true; issue = nil
        defer { if generation == token { busy = false } }
        do {
            if descriptor == nil {
                let data = try await rpc("view.task.describe", Data("{}".utf8))
                guard current(token) else { return }
                let description = try PluginTaskCodec.descriptor(data)
                guard expectedViewID == nil || expectedViewID == description.viewID else { throw PluginViewValue.Failure.invalidResponse }
                if let prior = try history?.restore(description) {
                    snapshot = prior.snapshot; cancelRequestID = prior.requestID
                    cancellation = prior.cancellation == .pending ? .unconfirmed : prior.cancellation
                }
                descriptor = description
            }
            if let descriptor { try await read(descriptor, token: token) }
        } catch let failure as PluginTaskHistory.Failure {
            if current(token) { issue = failure == .full ? "本次任务观察记录已满，请停止并释放插件后再试。" : "同一任务修订的身份描述已改变，未显示新状态。" }
        } catch let failure as PluginTaskCodec.RPCFailure {
            if current(token) { issue = "\(failure.message)。保留上次观察。" }
        } catch { if current(token) { issue = "读取结果不明，保留上次观察。可显式重新读取：\(error)" } }
    }
    public func cancel() async {
        guard canCancel, let descriptor else { return }
        let token = UUID(); generation = token; busy = true; issue = nil
        let requestID = UUID().uuidString; cancelRequestID = requestID; cancellation = .pending
        history?.action(descriptor, requestID: requestID, state: cancellation)
        defer {
            if generation == token {
                busy = false
                if cancellation == .pending {
                    cancellation = .unconfirmed; issue = "取消结果不明，不会再次发送。请显式读取任务状态。"
                }
            }
            history?.action(descriptor, requestID: requestID, state: cancellation)
        }
        do {
            let input = try JSONSerialization.data(withJSONObject: ["viewId": descriptor.viewID, "revision": descriptor.revision, "requestId": requestID], options: [.sortedKeys])
            let data = try await rpc("view.task.cancel", input)
            guard current(token) else { return }
            let kind = try PluginTaskCodec.cancellation(data, descriptor: descriptor, requestID: requestID)
            cancellation = kind == "requested" ? .requested : .alreadyFinished
        } catch let failure as PluginTaskCodec.RPCFailure {
            if current(token) {
                switch failure { case .failed: cancellation = .failed; case .cancelled: cancellation = .cancelled }
                issue = "取消\(failure.message)。不会自动再次发送。"
            }
            return
        } catch {
            if current(token) { cancellation = .unconfirmed; issue = "取消结果不明，不会再次发送。请显式读取任务状态。" }
            return
        }
        do { try await read(descriptor, token: token) }
        catch let failure as PluginTaskCodec.RPCFailure { if current(token) { issue = "取消回执已收到；读取\(failure.message)" } }
        catch { if current(token) { issue = "取消回执已收到，任务状态读取不明。请显式重新读取。" } }
    }
    public func invalidate() {
        generation = UUID(); valid = false; busy = false
        if cancellation == .pending { cancellation = .unconfirmed }
        if let descriptor, let cancelRequestID { history?.action(descriptor, requestID: cancelRequestID, state: cancellation) }
        issue = "任务所属会话已结束；保留最后观察，不能据此确认仍在运行或已停止"
    }
    private func current(_ token: UUID) -> Bool { generation == token && available && !Task.isCancelled }
    private func read(_ d: PluginTaskDescriptor, token: UUID) async throws {
        let input = try JSONSerialization.data(withJSONObject: ["viewId": d.viewID, "revision": d.revision], options: [.sortedKeys])
        let data = try await rpc("view.task.read", input)
        guard current(token) else { return }
        snapshot = try PluginTaskCodec.snapshot(data, descriptor: d, previous: snapshot)
        if let snapshot { history?.observe(d, snapshot: snapshot) }
    }
}
