import Foundation

public struct PluginDiffDescriptor: Equatable, Sendable {
    public let viewID: String
    public let revision: String
    public let title: String
    public let beforeLabel: String
    public let afterLabel: String
    public let beforeLines: Int
    public let afterLines: Int
    public let added: Int
    public let removed: Int
    public let total: Int
    public let pageSize: Int
}
public struct PluginDiffCursor: Equatable, Sendable {
    public let before: Int
    public let after: Int
    public static let zero = Self(before: 0, after: 0)
}
public struct PluginDiffPage: Equatable, Sendable {
    public struct Row: Equatable, Sendable {
        public enum Kind: String, Sendable { case equal, delete, insert }
        public enum Ending: String, Sendable {
            case lf, crlf, cr, none
            public var label: String {
                switch self { case .lf: return "LF"; case .crlf: return "CRLF"; case .cr: return "CR"; case .none: return "无末尾换行" }
            }
        }
        public let kind: Kind
        public let beforeLine: Int
        public let afterLine: Int
        public let text: String
        public let ending: Ending
    }
    public let offset: Int
    public let start: PluginDiffCursor
    public let end: PluginDiffCursor
    public let rows: [Row]
}

enum PluginDiffCodec {
    typealias V = PluginViewValue
    static func descriptor(_ outcome: Data) throws -> PluginDiffDescriptor {
        let value = try V.success(outcome)
        try V.keys(value, ["viewId", "revision", "title", "beforeLabel", "afterLabel", "beforeLines", "afterLines", "added", "removed", "total", "pageSize"])
        let before = try V.integer(value["beforeLines"], range: 0...128)
        let after = try V.integer(value["afterLines"], range: 0...128)
        let added = try V.integer(value["added"], range: 0...128)
        let removed = try V.integer(value["removed"], range: 0...128)
        let total = try V.integer(value["total"], range: 0...256)
        guard total - added == before, total - removed == after, added <= after, removed <= before else { throw V.Failure.invalidResponse }
        return .init(viewID: try V.identifier(value["viewId"]), revision: try V.revision(value["revision"]),
            title: try V.text(value["title"], maximum: 128, nonempty: true),
            beforeLabel: try V.text(value["beforeLabel"], maximum: 64, nonempty: true),
            afterLabel: try V.text(value["afterLabel"], maximum: 64, nonempty: true),
            beforeLines: before, afterLines: after, added: added, removed: removed, total: total,
            pageSize: try V.integer(value["pageSize"], range: 1...3))
    }
    static func page(_ outcome: Data, descriptor d: PluginDiffDescriptor, offset: Int, prefix: PluginDiffCursor) throws -> PluginDiffPage {
        let value = try V.success(outcome)
        try V.keys(value, ["viewId", "revision", "offset", "total", "beforeOffset", "afterOffset", "rows"])
        guard try V.identifier(value["viewId"]) == d.viewID, try V.revision(value["revision"]) == d.revision,
              try V.integer(value["offset"], range: 0...256) == offset,
              try V.integer(value["total"], range: 0...256) == d.total,
              offset >= 0, offset <= d.total,
              try V.integer(value["beforeOffset"], range: 0...128) == prefix.before,
              try V.integer(value["afterOffset"], range: 0...128) == prefix.after,
              prefix.before <= d.beforeLines, prefix.after <= d.afterLines,
              prefix.before + prefix.after >= offset, prefix.before + prefix.after <= 2 * offset,
              let rows = value["rows"] as? [[String: Any]], rows.count <= 3,
              rows.count == min(d.pageSize, d.total - offset) else { throw V.Failure.invalidResponse }
        var before = prefix.before, after = prefix.after
        let parsed = try rows.map { value -> PluginDiffPage.Row in
            try V.keys(value, ["kind", "beforeLine", "afterLine", "text", "ending"])
            guard let rawKind = value["kind"] as? String, let kind = PluginDiffPage.Row.Kind(rawValue: rawKind),
                  let rawEnding = value["ending"] as? String, let ending = PluginDiffPage.Row.Ending(rawValue: rawEnding) else { throw V.Failure.invalidResponse }
            let old = try V.integer(value["beforeLine"], range: 0...128), new = try V.integer(value["afterLine"], range: 0...128)
            if kind != .insert { before += 1 }
            if kind != .delete { after += 1 }
            let text = try V.text(value["text"], maximum: 128)
            guard old == (kind == .insert ? 0 : before), new == (kind == .delete ? 0 : after),
                  before <= d.beforeLines, after <= d.afterLines, !text.utf8.contains(10), !text.utf8.contains(13),
                  ending != .none || ((kind == .insert || before == d.beforeLines) && (kind == .delete || after == d.afterLines)) else { throw V.Failure.invalidResponse }
            return .init(kind: kind, beforeLine: old, afterLine: new, text: text, ending: ending)
        }
        guard offset + parsed.count != d.total || (before == d.beforeLines && after == d.afterLines) else { throw V.Failure.invalidResponse }
        return .init(offset: offset, start: prefix, end: .init(before: before, after: after), rows: parsed)
    }
}

/// One selected page and bounded cursor history; no prior row cache. Every
/// reachable page is tied to the validated cursor of its preceding page.
@MainActor public final class PluginDiffModel: ObservableObject {
    public typealias RPC = @MainActor (String, Data) async throws -> Data
    public static let methods = ["view.diff.describe", "view.diff.page"]
    @Published public private(set) var descriptor: PluginDiffDescriptor?
    @Published public private(set) var page: PluginDiffPage?
    @Published public private(set) var selectedOffset = 0
    @Published public private(set) var busy = false
    @Published public private(set) var valid = true
    @Published public private(set) var issue: String?
    private var generation = UUID()
    private var cursors: [Int: PluginDiffCursor] = [0: .zero]
    private let rpc: RPC
    private let isCurrent: @MainActor () -> Bool
    private let expectedViewID: String?
    public init(rpc: @escaping RPC, isCurrent: @escaping @MainActor () -> Bool, expectedViewID: String? = nil) {
        self.rpc = rpc; self.isCurrent = isCurrent; self.expectedViewID = expectedViewID
    }
    public var available: Bool { valid && isCurrent() }
    public var canPrevious: Bool { available && !busy && page != nil && selectedOffset > 0 }
    public var canNext: Bool { available && !busy && page.map { $0.offset + $0.rows.count < (descriptor?.total ?? 0) } == true }
    public func load() async {
        guard available, !busy else { return }
        let token = UUID(); generation = token; busy = true; descriptor = nil; page = nil; issue = nil; selectedOffset = 0; cursors = [0: .zero]
        defer { if generation == token { busy = false } }
        do {
            let data = try await rpc(Self.methods[0], Data("{}".utf8))
            guard current(token) else { return }
            let value = try PluginDiffCodec.descriptor(data)
            guard expectedViewID == nil || expectedViewID == value.viewID else { throw PluginViewValue.Failure.invalidResponse }
            descriptor = value
            try await readPage(value, offset: 0, prefix: .zero, token: token)
        } catch { if current(token) { issue = "差异读取失败，未显示新页面：\(error)" } }
    }
    public func showPage(offset: Int) async {
        guard available, !busy, let descriptor, offset >= 0, offset < max(1, descriptor.total),
              offset % descriptor.pageSize == 0, let prefix = cursors[offset] else { return }
        let token = UUID(); generation = token; selectedOffset = offset; page = nil; issue = nil; busy = true
        defer { if generation == token { busy = false } }
        do { try await readPage(descriptor, offset: offset, prefix: prefix, token: token) }
        catch { if current(token) { issue = "差异读取失败，未显示新页面：\(error)" } }
    }
    public func invalidate() {
        generation = UUID(); valid = false; busy = false; page = nil; cursors.removeAll(); issue = "差异所属会话已结束"
    }
    private func current(_ token: UUID) -> Bool { generation == token && available && !Task.isCancelled }
    private func readPage(_ d: PluginDiffDescriptor, offset: Int, prefix: PluginDiffCursor, token: UUID) async throws {
        let input = try JSONSerialization.data(withJSONObject: ["viewId": d.viewID, "revision": d.revision, "offset": offset, "limit": d.pageSize], options: [.sortedKeys])
        let data = try await rpc(Self.methods[1], input)
        guard current(token) else { return }
        let value = try PluginDiffCodec.page(data, descriptor: d, offset: offset, prefix: prefix)
        page = value; cursors[offset + value.rows.count] = value.end
    }
}
