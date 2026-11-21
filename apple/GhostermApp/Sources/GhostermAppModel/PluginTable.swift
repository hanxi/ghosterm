import CoreFoundation
import Foundation

/// Native values for the standard table contribution. These limits mirror the
/// Portable table codecs; they do not depend on a particular plugin or command.
public struct PluginTableDescriptor: Equatable, Sendable {
    public struct Column: Equatable, Sendable, Identifiable {
        public let id: String
        public let title: String
    }
    public let viewID: String
    public let revision: String
    public let title: String
    public let columns: [Column]
    public let total: Int
    public let pageSize: Int
}

public struct PluginTablePage: Equatable, Sendable {
    public struct Row: Equatable, Sendable, Identifiable {
        public let id: String
        public let cells: [String]
    }
    public let offset: Int
    public let rows: [Row]
}

public struct PluginTableSearchPage: Equatable, Sendable {
    public let cursor: Int
    public let latest: Int
    public let truncated: Bool
    public let rows: [PluginTablePage.Row]
}

enum PluginTableCodec {
    typealias Failure = PluginViewValue.Failure

    static func descriptor(_ outcome: Data) throws -> PluginTableDescriptor {
        let value = try success(outcome)
        try keys(value, ["viewId", "revision", "title", "columns", "total", "pageSize"])
        let viewID = try identifier(value["viewId"]), revision = try revision(value["revision"])
        let title = try text(value["title"], maximum: 128, nonempty: true)
        guard let columns = value["columns"] as? [[String: Any]], (1...8).contains(columns.count) else { throw Failure.invalidResponse }
        let parsed = try columns.map { column -> PluginTableDescriptor.Column in
            try keys(column, ["id", "title"])
            return .init(id: try identifier(column["id"]), title: try text(column["title"], maximum: 64, nonempty: true))
        }
        guard Set(parsed.map(\.id)).count == parsed.count else { throw Failure.invalidResponse }
        return .init(viewID: viewID, revision: revision, title: title, columns: parsed,
            total: try integer(value["total"], range: 0...1_000_000), pageSize: try integer(value["pageSize"], range: 1...16))
    }

    static func page(_ outcome: Data, descriptor: PluginTableDescriptor, offset: Int) throws -> PluginTablePage {
        let value = try success(outcome)
        try keys(value, ["viewId", "revision", "offset", "total", "rows"])
        guard try identifier(value["viewId"]) == descriptor.viewID,
              try revision(value["revision"]) == descriptor.revision,
              try integer(value["offset"], range: 0...1_000_000) == offset,
              try integer(value["total"], range: 0...1_000_000) == descriptor.total,
              offset >= 0, offset <= descriptor.total,
              let rows = value["rows"] as? [[String: Any]], rows.count <= 16,
              rows.count == min(descriptor.pageSize, descriptor.total - offset) else { throw Failure.invalidResponse }
        return .init(offset: offset, rows: try parseRows(rows, descriptor: descriptor))
    }

    static func search(_ outcome: Data, descriptor: PluginTableDescriptor, after: Int, through: Int) throws -> PluginTableSearchPage {
        let value = try success(outcome)
        try keys(value, ["viewId", "revision", "cursor", "latest", "truncated", "rows"])
        let limit = 0...9_007_199_254_740_991
        let cursor = try integer(value["cursor"], range: limit)
        let latest = try integer(value["latest"], range: limit)
        guard try identifier(value["viewId"]) == descriptor.viewID,
              try revision(value["revision"]) == descriptor.revision,
              after <= cursor, cursor <= latest, cursor > after || cursor == latest,
              through == 0 || through == latest,
              let flag = value["truncated"] as? NSNumber, CFGetTypeID(flag) == CFBooleanGetTypeID(),
              let rows = value["rows"] as? [[String: Any]], rows.count <= 4,
              cursor > after || rows.isEmpty else { throw Failure.invalidResponse }
        return .init(cursor: cursor, latest: latest, truncated: flag.boolValue,
                     rows: try parseRows(rows, descriptor: descriptor))
    }

    private static func parseRows(_ rows: [[String: Any]], descriptor: PluginTableDescriptor) throws -> [PluginTablePage.Row] {
        let parsed = try rows.map { row -> PluginTablePage.Row in
            try keys(row, ["id", "cells"])
            guard let cells = row["cells"] as? [String], cells.count == descriptor.columns.count else { throw Failure.invalidResponse }
            return .init(id: try identifier(row["id"]), cells: try cells.map { try text($0, maximum: 256) })
        }
        guard Set(parsed.map(\.id)).count == parsed.count else { throw Failure.invalidResponse }
        return parsed
    }

    private static func success(_ data: Data) throws -> [String: Any] { try PluginViewValue.success(data) }
    private static func keys(_ value: [String: Any], _ expected: Set<String>) throws { try PluginViewValue.keys(value, expected) }
    private static func identifier(_ value: Any?) throws -> String { try PluginViewValue.identifier(value) }
    private static func revision(_ value: Any?) throws -> String { try PluginViewValue.revision(value) }
    private static func integer(_ value: Any?, range: ClosedRange<Int>) throws -> Int { try PluginViewValue.integer(value, range: range) }
    private static func text(_ value: Any?, maximum: Int, nonempty: Bool = false) throws -> String {
        try PluginViewValue.text(value, maximum: maximum, nonempty: nonempty)
    }

}

/// One read at a time. A new selection never displays the preceding page as
/// its own; a closed/replaced Runtime cannot apply a late reply or error.
@MainActor
public final class PluginTableModel: ObservableObject {
    public typealias RPC = @MainActor (String, Data) async throws -> Data
    public static let methods = ["view.table.describe", "view.table.page"]
    public static let searchMethod = "view.table.search"
    public let supportsSearch: Bool
    @Published public private(set) var descriptor: PluginTableDescriptor?
    @Published public private(set) var page: PluginTablePage?
    @Published public private(set) var selectedOffset = 0
    @Published public private(set) var busy = false
    @Published public private(set) var issue: String?
    @Published public private(set) var valid = true
    @Published public private(set) var searchPage: PluginTableSearchPage?
    @Published public private(set) var searchQuery: String?
    @Published public private(set) var searchTruncated = false
    private let rpc: RPC
    private let isCurrent: @MainActor () -> Bool
    private let expectedViewID: String?
    private var generation = UUID()

    public init(rpc: @escaping RPC, isCurrent: @escaping @MainActor () -> Bool, expectedViewID: String? = nil,
                supportsSearch: Bool = false) {
        self.rpc = rpc; self.isCurrent = isCurrent; self.expectedViewID = expectedViewID
        self.supportsSearch = supportsSearch
    }
    public var available: Bool { valid && isCurrent() }
    public var lastOffset: Int { descriptor.map { $0.total == 0 ? 0 : (($0.total - 1) / $0.pageSize) * $0.pageSize } ?? 0 }
    public var canPrevious: Bool { available && !busy && page != nil && selectedOffset > 0 }
    public var canNext: Bool { available && !busy && page != nil && selectedOffset < lastOffset }

    public func load() async {
        guard available, !busy else { return }
        searchQuery = nil; searchPage = nil; searchTruncated = false
        let token = UUID(); generation = token; busy = true; issue = nil; descriptor = nil; page = nil; selectedOffset = 0
        defer { if generation == token { busy = false } }
        do {
            let data = try await rpc(Self.methods[0], Data("{}".utf8))
            guard current(token) else { return }
            let description = try PluginTableCodec.descriptor(data)
            guard expectedViewID == nil || expectedViewID == description.viewID else { throw PluginViewValue.Failure.invalidResponse }
            descriptor = description
            try await readPage(description, offset: 0, token: token)
        } catch { if current(token) { issue = "表格读取失败，未显示新页面：\(error)" } }
    }
    public func showPage(offset: Int) async {
        guard available, !busy, searchQuery == nil, let descriptor, offset >= 0, offset <= lastOffset,
              offset % descriptor.pageSize == 0 else { return }
        let token = UUID(); generation = token; selectedOffset = offset; page = nil; issue = nil; busy = true
        defer { if generation == token { busy = false } }
        do { try await readPage(descriptor, offset: offset, token: token) }
        catch { if current(token) { issue = "表格读取失败，未显示新页面：\(error)" } }
    }
    public func search(_ query: String) async {
        guard !query.isEmpty, query.unicodeScalars.count <= 64, supportsSearch, available, !busy,
              let descriptor else { return }
        searchQuery = query; searchPage = nil; searchTruncated = false
        await readSearch(descriptor, query: query, after: 0, through: 0)
    }
    public func continueSearch() async {
        guard available, !busy, let descriptor, let searchQuery, let searchPage,
              searchPage.cursor < searchPage.latest else { return }
        await readSearch(descriptor, query: searchQuery, after: searchPage.cursor, through: searchPage.latest)
    }
    private func readSearch(_ descriptor: PluginTableDescriptor, query: String, after: Int, through: Int) async {
        let token = UUID(); generation = token; busy = true; issue = nil
        defer { if generation == token { busy = false } }
        do {
            let input = try JSONSerialization.data(withJSONObject: ["viewId": descriptor.viewID,
                "revision": descriptor.revision, "query": query, "after": after, "through": through])
            let result = try await rpc(Self.searchMethod, input)
            guard current(token) else { return }
            let page = try PluginTableCodec.search(result, descriptor: descriptor, after: after, through: through)
            searchPage = page
            searchTruncated = searchTruncated || page.truncated
        } catch { if current(token) { issue = "搜索失败：\(error)" } }
    }
    public func invalidate() {
        generation = UUID(); valid = false; busy = false; page = nil; issue = "表格所属会话已结束"
        searchPage = nil; searchQuery = nil; searchTruncated = false
    }
    private func current(_ token: UUID) -> Bool { generation == token && available && !Task.isCancelled }
    private func readPage(_ descriptor: PluginTableDescriptor, offset: Int, token: UUID) async throws {
        let input = try JSONSerialization.data(withJSONObject: ["viewId": descriptor.viewID, "revision": descriptor.revision,
            "offset": offset, "limit": descriptor.pageSize], options: [.sortedKeys])
        let data = try await rpc(Self.methods[1], input)
        guard current(token) else { return }
        page = try PluginTableCodec.page(data, descriptor: descriptor, offset: offset)
    }
}
