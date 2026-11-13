import Foundation

public struct CodexChangeReference: Sendable, Equatable {
    public let snapshotID: String
    public let threadID: String
    public let turnID: String
    public let itemID: String
    public let totalUnits: Int
    public let totalBytes: Int
    static let maximumBytes = 524288

    static func decode(marker: Any?, thread: Any?, turn: Any?, item: Any?) -> Self? {
        struct Marker: Decodable { let snapshotID: String; let snapshotUnits: Int; let snapshotBytes: Int }
        guard let marker, JSONSerialization.isValidJSONObject(marker),
              let bytes = try? JSONSerialization.data(withJSONObject: marker),
              let value = try? JSONDecoder().decode(Marker.self, from: bytes),
              value.snapshotID.hasPrefix("change-"), value.snapshotID.utf8.count <= 128,
              let number = UInt64(value.snapshotID.dropFirst(7)), number > 0, number <= 9_007_199_254_740_991,
              String(number) == value.snapshotID.dropFirst(7),
              value.snapshotUnits > 0, value.snapshotBytes >= value.snapshotUnits,
              value.snapshotBytes <= maximumBytes,
              let thread = thread as? String, validIdentity(thread),
              let turn = turn as? String, validIdentity(turn),
              let item = item as? String, validIdentity(item) else { return nil }
        return Self(snapshotID: value.snapshotID, threadID: thread, turnID: turn, itemID: item,
                    totalUnits: value.snapshotUnits, totalBytes: value.snapshotBytes)
    }

    private static func validIdentity(_ value: String) -> Bool { !value.isEmpty && value.utf8.count <= 128 }

    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.snapshotID == rhs.snapshotID && lhs.totalUnits == rhs.totalUnits && lhs.totalBytes == rhs.totalBytes &&
        lhs.threadID.utf8.elementsEqual(rhs.threadID.utf8) && lhs.turnID.utf8.elementsEqual(rhs.turnID.utf8) &&
        lhs.itemID.utf8.elementsEqual(rhs.itemID.utf8)
    }
}

public struct CodexChangeDocument: Decodable, Sendable, Equatable {
    public struct File: Decodable, Sendable, Equatable {
        public struct Kind: Decodable, Sendable, Equatable {
            public let type: String
            public let move_path: String?
        }
        public let path: String
        public let kind: Kind
        public let diff: String
    }
    public let schemaVersion: Int
    public let threadId: String
    public let turnId: String
    public let itemId: String
    public let changes: [File]
}

struct CodexChangeChunk: Decodable, Sendable {
    let status: String
    let snapshotID: String
    let offset: Int
    let nextOffset: Int
    let totalUnits: Int
    let totalBytes: Int
    let content: String
    let done: Bool
}

enum CodexChangeReadError: LocalizedError {
    case invalid, expired, incomplete
    var errorDescription: String? {
        switch self {
        case .invalid: return "完整补丁的身份、分块或内容校验失败。"
        case .expired: return "补丁快照已过期，请读取当前版本。"
        case .incomplete: return "完整补丁尚未读完。"
        }
    }
}

/// No partial document is exposed. Validation uses the advertised reference,
/// the exact received UTF-8 bytes, and UTF-16 offsets, without normalizing text.
struct CodexChangeAssembler: Sendable {
    let reference: CodexChangeReference
    private(set) var nextOffset = 0
    private(set) var receivedBytes = 0
    private(set) var done = false
    private var bytes = Data()

    init(reference: CodexChangeReference) { self.reference = reference }

    mutating func append(_ chunk: CodexChangeChunk) throws {
        guard !done else { throw CodexChangeReadError.invalid }
        if chunk.status == "expired" { throw CodexChangeReadError.expired }
        guard chunk.status == "ok", chunk.snapshotID == reference.snapshotID,
              chunk.totalUnits == reference.totalUnits, chunk.totalBytes == reference.totalBytes,
              chunk.offset == nextOffset, chunk.nextOffset >= chunk.offset,
              chunk.nextOffset <= reference.totalUnits,
              chunk.nextOffset == chunk.offset + chunk.content.utf16.count,
              chunk.done == (chunk.nextOffset == reference.totalUnits),
              chunk.done || chunk.nextOffset > chunk.offset else { throw CodexChangeReadError.invalid }
        let data = Data(chunk.content.utf8)
        guard data.count <= 3072, data.count <= reference.totalBytes - receivedBytes else {
            throw CodexChangeReadError.invalid
        }
        if chunk.done && receivedBytes + data.count != reference.totalBytes { throw CodexChangeReadError.invalid }
        bytes.append(data)
        receivedBytes += data.count
        nextOffset = chunk.nextOffset
        done = chunk.done
    }

    func document() throws -> CodexChangeDocument {
        guard done, receivedBytes == reference.totalBytes, nextOffset == reference.totalUnits else {
            throw CodexChangeReadError.incomplete
        }
        let value = try JSONDecoder().decode(CodexChangeDocument.self, from: bytes)
        guard value.schemaVersion == 1, value.threadId.utf8.elementsEqual(reference.threadID.utf8),
              value.turnId.utf8.elementsEqual(reference.turnID.utf8), value.itemId.utf8.elementsEqual(reference.itemID.utf8),
              value.changes.count <= 8192 else { throw CodexChangeReadError.invalid }
        for file in value.changes {
            guard !file.path.isEmpty, file.path.utf8.count <= 16384,
                  ["add", "delete", "update"].contains(file.kind.type),
                  file.kind.move_path.map({ !$0.isEmpty && $0.utf8.count <= 16384 }) ?? true else {
                throw CodexChangeReadError.invalid
            }
        }
        return value
    }
}

public struct CodexChangeReviewState: Sendable {
    public let reference: CodexChangeReference
    public let receivedBytes: Int
    public let document: CodexChangeDocument?
    public let issue: String?
    public var loading: Bool { document == nil && issue == nil }
}
