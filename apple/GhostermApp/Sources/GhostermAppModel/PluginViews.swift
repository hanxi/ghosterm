import Foundation

public struct PluginViewRegistration: Equatable, Sendable {
    public enum Kind: String, Sendable, Hashable { case table, diff, task }
    public let handle: String
    public let viewID: String
    public let kind: Kind
    public let describe: String
    public let read: String
    public let cancel: String?
    var methods: Set<String> { Set([describe, read] + (cancel.map { [$0] } ?? [])) }
    var counter: UInt64 { UInt64(handle.split(separator: ".")[1])! }
    var nonce: String { String(handle.prefix(32)) }
}

struct PluginViewRegistrySnapshot: Equatable {
    let instance: String, epoch: String
    let revision: UInt64
    let entries: [PluginViewRegistration]
    static func parse(_ data: Data) throws -> Self {
        typealias V = PluginViewValue
        guard !data.isEmpty, data.count <= 8192, String(data: data, encoding: .utf8) != nil else { throw V.Failure.invalidResponse }
        try ExtensionRelayBootstrap.validateJSON(data)
        guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw V.Failure.invalidResponse }
        try V.keys(root, ["instanceId", "epoch", "revision", "views"])
        guard let instance = root["instanceId"] as? String, hex(instance), let entries = root["views"] as? [[String: Any]], entries.count <= 8 else { throw V.Failure.invalidResponse }
        let epoch = try V.revision(root["epoch"]), revision = try V.revision(root["revision"])
        let parsed = try entries.map { entry -> PluginViewRegistration in
            try V.keys(entry, ["handle", "viewId", "kind", "methods"])
            guard let handle = entry["handle"] as? String, handle.utf8.count <= 64,
                  let dot = handle.firstIndex(of: "."), hex(String(handle[..<dot])),
                  let kind = (entry["kind"] as? String).flatMap(PluginViewRegistration.Kind.init(rawValue:)),
                  let methods = entry["methods"] as? [String: Any] else { throw V.Failure.invalidResponse }
            _ = try V.revision(String(handle[handle.index(after: dot)...]))
            try V.keys(methods, kind == .task ? ["describe", "read", "cancel"] : ["describe", "read"])
            let describe = try method(methods["describe"]), read = try method(methods["read"])
            let cancel = kind == .task ? try method(methods["cancel"]) : nil
            guard Set([describe, read] + (cancel.map { [$0] } ?? [])).count == (kind == .task ? 3 : 2) else { throw V.Failure.invalidResponse }
            return .init(handle: handle, viewID: try V.identifier(entry["viewId"]), kind: kind, describe: describe, read: read, cancel: cancel)
        }
        guard Set(parsed.map(\.handle)).count == parsed.count, Set(parsed.map(\.viewID)).count == parsed.count,
              Set(parsed.map(\.nonce)).count <= 1 else { throw V.Failure.invalidResponse }
        return .init(instance: instance, epoch: epoch, revision: UInt64(revision)!, entries: parsed)
    }
    private static func hex(_ value: String) -> Bool {
        value.utf8.count == 32 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    private static func method(_ value: Any?) throws -> String {
        guard let value = value as? String, (1...128).contains(value.utf8.count), let first = value.utf8.first,
              (97...122).contains(first), value.utf8.allSatisfy({ PluginViewValue.alphanumeric($0) || [45, 46, 95].contains($0) }) else { throw PluginViewValue.Failure.invalidResponse }
        return value
    }
}

@MainActor public final class PluginViewItem: ObservableObject, Identifiable {
    public enum Content { case table(PluginTableModel), diff(PluginDiffModel), task(PluginTaskModel) }
    public let registration: PluginViewRegistration
    public nonisolated var id: String { registration.handle }
    public let content: Content
    @Published public private(set) var closing = false
    @Published public private(set) var closeIssue: String?
    private final class Gate { var live = true, closing = false }
    private let gate: Gate
    init(registration r: PluginViewRegistration, rpc: @escaping PluginViewsModel.RPC, history: PluginTaskHistory, isCurrent: @escaping @MainActor () -> Bool) {
        registration = r
        let gate = Gate(); self.gate = gate
        let live: @MainActor () -> Bool = { gate.live && !gate.closing && isCurrent() }
        let mapped: PluginViewsModel.RPC = { method, input in
            guard live() else { throw CancellationError() }
            let name: String?
            switch method {
            case "view.table.describe", "view.diff.describe", "view.task.describe": name = r.describe
            case "view.table.page", "view.diff.page", "view.task.read": name = r.read
            case "view.task.cancel": name = r.cancel
            default: name = nil
            }
            guard let name else { throw PluginViewValue.Failure.invalidResponse }
            return try await rpc(name, input)
        }
        switch r.kind {
        case .table: content = .table(PluginTableModel(rpc: mapped, isCurrent: live, expectedViewID: r.viewID))
        case .diff: content = .diff(PluginDiffModel(rpc: mapped, isCurrent: live, expectedViewID: r.viewID))
        case .task: content = .task(PluginTaskModel(rpc: mapped, isCurrent: live, expectedViewID: r.viewID, history: history))
        }
    }
    func beginClose() { closing = true; gate.closing = true; closeIssue = nil }
    func closeFailed() { closing = false; gate.closing = false; closeIssue = "视图关闭未确认，可显式重试。任务取消仍需单独操作。" }
    func invalidate() {
        gate.live = false
        switch content { case .table(let m): m.invalidate(); case .diff(let m): m.invalidate(); case .task(let m): m.invalidate() }
    }
}

/// At most eight models and one counter high-water mark. A closed registration
/// cannot resurrect, without keeping an unbounded tombstone set. Model identity
/// survives unchanged snapshots; replacement invalidates old async callbacks.
@MainActor public final class PluginViewsModel: ObservableObject {
    public typealias RPC = @MainActor (String, Data) async throws -> Data
    public typealias Close = @MainActor (String) async throws -> Data
    @Published public private(set) var items: [PluginViewItem] = []
    @Published public private(set) var issue: String?
    public private(set) var valid = true
    private let instance: String, epoch: String, kinds: Set<PluginViewRegistration.Kind>, methods: Set<String>
    private let rpc: RPC, close: Close, isCurrent: @MainActor () -> Bool
    private var last: PluginViewRegistrySnapshot?
    private var nonce: String?
    private var highWater: UInt64 = 0
    private let taskHistory = PluginTaskHistory()
    public init(instance: String, epoch: String, kinds: Set<PluginViewRegistration.Kind>, capabilities: Set<String>, methods: Set<String>,
                rpc: @escaping RPC, isCurrent: @escaping @MainActor () -> Bool, close: @escaping Close) throws {
        guard !kinds.isEmpty, ["ui.views", "session.rpc"].allSatisfy(capabilities.contains),
              kinds.allSatisfy({ capabilities.contains("ui." + $0.rawValue) }) else { throw PluginViewValue.Failure.invalidResponse }
        self.instance = instance; self.epoch = epoch; self.kinds = kinds; self.methods = methods
        self.rpc = rpc; self.close = close; self.isCurrent = isCurrent
    }
    public var available: Bool { valid && isCurrent() }
    public func apply(_ data: Data) throws {
        guard available else { return }
        do {
            let snapshot = try PluginViewRegistrySnapshot.parse(data)
            guard snapshot.instance == instance, snapshot.epoch == epoch else { throw PluginViewValue.Failure.invalidResponse }
            if let last {
                if snapshot.revision < last.revision { return } // queued before a confirmed Native close
                if snapshot.revision == last.revision {
                    guard snapshot == last else { throw PluginViewValue.Failure.invalidResponse }
                    return
                }
            }
            let prior = Dictionary(uniqueKeysWithValues: items.map { ($0.id, $0) })
            for r in snapshot.entries {
                guard kinds.contains(r.kind), r.methods.isSubset(of: methods), nonce == nil || nonce == r.nonce else { throw PluginViewValue.Failure.invalidResponse }
                if let old = prior[r.handle] {
                    guard old.registration == r else { throw PluginViewValue.Failure.invalidResponse }
                } else {
                    guard r.counter > highWater else { throw PluginViewValue.Failure.invalidResponse }
                }
            }
            let selected = snapshot.entries.map { r in
                prior[r.handle] ?? PluginViewItem(registration: r, rpc: rpc, history: taskHistory, isCurrent: { [weak self] in self?.available == true })
            }
            let handles = Set(snapshot.entries.map(\.handle))
            for old in items where !handles.contains(old.id) { old.invalidate() }
            highWater = max(highWater, snapshot.entries.map(\.counter).max() ?? 0)
            nonce = nonce ?? snapshot.entries.first?.nonce
            if items.map(\.id) != selected.map(\.id) { items = selected }
            last = snapshot
        } catch {
            invalidate(); issue = "视图注册快照无效，已停用本次视图。可停止并释放插件。"
            throw error
        }
    }
    public func closeView(handle: String) async {
        guard available, let entry = items.first(where: { $0.id == handle }), !entry.closing else { return }
        entry.beginClose()
        do {
            let snapshot = try await close(handle)
            guard available, items.contains(where: { $0 === entry }) else { return }
            try apply(snapshot)
            guard !items.contains(where: { $0 === entry }) else { entry.closeFailed(); return }
        } catch { if available && items.contains(where: { $0 === entry }) { entry.closeFailed() } }
    }
    public func invalidate() { valid = false; items.forEach { $0.invalidate() }; items = [] }
}
