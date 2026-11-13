import Foundation

/// Ordered JSON keeps byte-distinct Unicode keys separate. Foundation/Swift
/// dictionaries cannot represent every valid Claude question/answer map.
indirect enum ClaudeJSON {
    struct Member { let name: String; let value: ClaudeJSON }
    case object([Member]), array([ClaudeJSON]), string(String), number(String), bool(Bool), null

    subscript(_ name: String) -> ClaudeJSON? {
        guard case .object(let members) = self else { return nil }
        return members.first { $0.name.utf8.elementsEqual(name.utf8) }?.value
    }
    var text: String? { if case .string(let value) = self { return value }; return nil }
    var boolean: Bool? { if case .bool(let value) = self { return value }; return nil }
    var integer: Int? { if case .number(let value) = self { return Int(value) }; return nil }
    var elements: [ClaudeJSON]? { if case .array(let value) = self { return value }; return nil }
    static func fields(_ values: [(String, ClaudeJSON)]) -> Self { .object(values.map { Member(name: $0.0, value: $0.1) }) }

    func encoded() -> String {
        switch self {
        case .object(let members):
            // JSON.stringify emits canonical integer-index keys before other
            // keys, even if a question happens to be named "10" or "2".
            func index(_ name: String) -> UInt32? {
                guard let value = UInt32(name), value != UInt32.max, String(value) == name else { return nil }
                return value
            }
            let numeric = members.filter { index($0.name) != nil }.sorted { index($0.name)! < index($1.name)! }
            let ordered = numeric + members.filter { index($0.name) == nil }
            return "{" + ordered.map { Self.quote($0.name) + ":" + $0.value.encoded() }.joined(separator: ",") + "}"
        case .array(let values): return "[" + values.map { $0.encoded() }.joined(separator: ",") + "]"
        case .string(let value): return Self.quote(value)
        case .number(let value): return value
        case .bool(let value): return value ? "true" : "false"
        case .null: return "null"
        }
    }
    private static func quote(_ value: String) -> String {
        var result = "\""
        for scalar in value.unicodeScalars {
            switch scalar.value {
            case 34: result += "\\\""
            case 92: result += "\\\\"
            case 8: result += "\\b"
            case 9: result += "\\t"
            case 10: result += "\\n"
            case 12: result += "\\f"
            case 13: result += "\\r"
            case 0...31: result += String(format: "\\u%04x", scalar.value)
            default: result.unicodeScalars.append(scalar)
            }
        }
        return result + "\""
    }

    static func parse(_ data: Data) throws -> Self {
        guard data.count <= 600_000 else { throw ClaudeRPCClient.Failure.invalidResponse }
        try ExtensionRelayBootstrap.validateJSON(data)
        var parser = Parser(bytes: Array(data))
        let result = try parser.value(depth: 0)
        parser.whitespace()
        guard parser.at == parser.bytes.count else { throw ClaudeRPCClient.Failure.invalidResponse }
        return result
    }
    private struct Parser {
        let bytes: [UInt8]; var at = 0; var nodes = 0
        mutating func whitespace() { while at < bytes.count && [9, 10, 13, 32].contains(bytes[at]) { at += 1 } }
        mutating func value(depth: Int) throws -> ClaudeJSON {
            nodes += 1; whitespace()
            guard depth <= 40, nodes <= 10_000, at < bytes.count else { throw ClaudeRPCClient.Failure.invalidResponse }
            switch bytes[at] {
            case 123:
                at += 1; whitespace(); var members: [Member] = []
                if bytes[at] == 125 { at += 1; return .object(members) }
                while true {
                    let name = try string(); whitespace(); at += 1 // validated colon
                    members.append(Member(name: name, value: try value(depth: depth + 1))); whitespace()
                    let end = bytes[at] == 125; at += 1
                    if end { return .object(members) }
                    whitespace()
                }
            case 91:
                at += 1; whitespace(); var values: [ClaudeJSON] = []
                if bytes[at] == 93 { at += 1; return .array(values) }
                while true {
                    values.append(try value(depth: depth + 1)); whitespace()
                    let end = bytes[at] == 93; at += 1
                    if end { return .array(values) }
                }
            case 34: return .string(try string())
            case 116: at += 4; return .bool(true)
            case 102: at += 5; return .bool(false)
            case 110: at += 4; return .null
            default:
                let start = at
                while at < bytes.count && ![9, 10, 13, 32, 44, 93, 125].contains(bytes[at]) { at += 1 }
                return .number(String(decoding: bytes[start..<at], as: UTF8.self))
            }
        }
        mutating func string() throws -> String {
            let start = at; at += 1
            while at < bytes.count {
                if bytes[at] == 34 { at += 1; return try JSONDecoder().decode(String.self, from: Data(bytes[start..<at])) }
                if bytes[at] == 92 { at += 1 }
                at += 1
            }
            throw ClaudeRPCClient.Failure.invalidResponse
        }
    }
}
