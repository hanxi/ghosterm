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

enum PluginTableCodec {
    enum Failure: Error { case invalidResponse, unconfirmedResult }

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
        let parsed = try rows.map { row -> PluginTablePage.Row in
            try keys(row, ["id", "cells"])
            guard let cells = row["cells"] as? [String], cells.count == descriptor.columns.count else { throw Failure.invalidResponse }
            return .init(id: try identifier(row["id"]), cells: try cells.map { try text($0, maximum: 256) })
        }
        guard Set(parsed.map(\.id)).count == parsed.count else { throw Failure.invalidResponse }
        return .init(offset: offset, rows: parsed)
    }

    private static func success(_ data: Data) throws -> [String: Any] {
        guard data.count <= 3072, String(data: data, encoding: .utf8) != nil else { throw Failure.invalidResponse }
        try ExtensionRelayBootstrap.validateJSON(data)
        guard let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw Failure.invalidResponse }
        guard envelope["kind"] as? String == "success" else { throw Failure.unconfirmedResult }
        try keys(envelope, ["kind", "value"])
        guard let value = envelope["value"] as? [String: Any], try jsonBytes(value) <= 2048 else { throw Failure.invalidResponse }
        return value
    }

    private static func keys(_ object: [String: Any], _ expected: Set<String>) throws {
        guard Set(object.keys) == expected else { throw Failure.invalidResponse }
    }
    private static func identifier(_ value: Any?) throws -> String {
        guard let value = value as? String, (1...64).contains(value.utf8.count),
              let first = value.utf8.first, alphanumeric(first),
              value.utf8.allSatisfy({ alphanumeric($0) || [45, 46, 95].contains($0) }) else { throw Failure.invalidResponse }
        return value
    }
    private static func alphanumeric(_ c: UInt8) -> Bool { (48...57).contains(c) || (65...90).contains(c) || (97...122).contains(c) }
    private static func revision(_ value: Any?) throws -> String {
        guard let value = value as? String, (1...20).contains(value.utf8.count), value.utf8.first != 48,
              value.utf8.allSatisfy({ (48...57).contains($0) }), let number = UInt64(value), number > 0 else { throw Failure.invalidResponse }
        return value
    }
    private static func integer(_ value: Any?, range: ClosedRange<Int>) throws -> Int {
        guard let value = value as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID(),
              value.doubleValue >= Double(range.lowerBound), value.doubleValue <= Double(range.upperBound),
              value.doubleValue == Double(value.intValue) else { throw Failure.invalidResponse }
        return value.intValue
    }
    private static func text(_ value: Any?, maximum: Int, nonempty: Bool = false) throws -> String {
        guard let value = value as? String, value.unicodeScalars.count <= maximum, !nonempty || !value.isEmpty,
              value.unicodeScalars.allSatisfy({ !((0...8).contains($0.value) || (11...12).contains($0.value) || (14...31).contains($0.value) || $0.value == 127) }) else { throw Failure.invalidResponse }
        return value
    }
    /// JSON.stringify byte accounting, independent of Foundation's optional
    /// slash / Unicode escaping. The raw envelope was already bounded to 3072.
    private static func jsonBytes(_ value: Any) throws -> Int {
        if let string = value as? String {
            return 2 + string.unicodeScalars.reduce(0) { size, scalar in
                size + (scalar.value == 34 || scalar.value == 92 || [8, 9, 10, 12, 13].contains(scalar.value) ? 2
                    : scalar.value < 32 ? 6 : scalar.utf8.count)
            }
        }
        if let array = value as? [Any] { return try 2 + max(0, array.count - 1) + array.reduce(0) { try $0 + jsonBytes($1) } }
        if let object = value as? [String: Any] {
            return try 2 + max(0, object.count - 1) + object.reduce(0) { try $0 + jsonBytes($1.key) + 1 + jsonBytes($1.value) }
        }
        if let number = value as? NSNumber {
            if CFGetTypeID(number) == CFBooleanGetTypeID() { return number.boolValue ? 4 : 5 }
            // Valid table numbers are bounded integers. Their original JSON
            // spelling (1, 1.0, 1e0) does not change JSON.stringify's size.
            return String(number.intValue).utf8.count
        }
        if value is NSNull { return 4 }
        throw Failure.invalidResponse
    }
}

/// One read at a time. A new selection never displays the preceding page as
/// its own; a closed/replaced Runtime cannot apply a late reply or error.
@MainActor
public final class PluginTableModel: ObservableObject {
    public typealias RPC = @MainActor (String, Data) async throws -> Data
    public static let methods = ["view.table.describe", "view.table.page"]
    @Published public private(set) var descriptor: PluginTableDescriptor?
    @Published public private(set) var page: PluginTablePage?
    @Published public private(set) var selectedOffset = 0
    @Published public private(set) var busy = false
    @Published public private(set) var issue: String?
    @Published public private(set) var valid = true
    private let rpc: RPC
    private let isCurrent: @MainActor () -> Bool
    private var generation = UUID()

    public init(rpc: @escaping RPC, isCurrent: @escaping @MainActor () -> Bool) { self.rpc = rpc; self.isCurrent = isCurrent }
    public var available: Bool { valid && isCurrent() }
    public var lastOffset: Int { descriptor.map { $0.total == 0 ? 0 : (($0.total - 1) / $0.pageSize) * $0.pageSize } ?? 0 }
    public var canPrevious: Bool { available && !busy && page != nil && selectedOffset > 0 }
    public var canNext: Bool { available && !busy && page != nil && selectedOffset < lastOffset }

    public func load() async {
        guard available, !busy else { return }
        let token = UUID(); generation = token; busy = true; issue = nil; descriptor = nil; page = nil; selectedOffset = 0
        defer { if generation == token { busy = false } }
        do {
            let data = try await rpc(Self.methods[0], Data("{}".utf8))
            guard current(token) else { return }
            let description = try PluginTableCodec.descriptor(data)
            descriptor = description
            try await readPage(description, offset: 0, token: token)
        } catch { if current(token) { issue = "表格读取失败，未显示新页面：\(error)" } }
    }
    public func showPage(offset: Int) async {
        guard available, !busy, let descriptor, offset >= 0, offset <= lastOffset, offset % descriptor.pageSize == 0 else { return }
        let token = UUID(); generation = token; selectedOffset = offset; page = nil; issue = nil; busy = true
        defer { if generation == token { busy = false } }
        do { try await readPage(descriptor, offset: offset, token: token) }
        catch { if current(token) { issue = "表格读取失败，未显示新页面：\(error)" } }
    }
    public func invalidate() {
        generation = UUID(); valid = false; busy = false; page = nil; issue = "表格所属会话已结束"
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
