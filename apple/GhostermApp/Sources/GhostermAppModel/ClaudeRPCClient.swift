import Foundation

/// Native adapter for the candidate's document RPC. Vendor replies remain whole
/// JSON records; this layer does not translate Claude events into Codex events.
@MainActor
public final class ClaudeRPCClient {
    public typealias RPC = @MainActor (String, Data) async throws -> Data
    public enum Failure: Error, Equatable {
        case busy, closed, invalidParameters, invalidResponse, refused(String)
    }
    public static let methods = ["claude.input.open", "claude.input.append", "claude.input.seal",
        "claude.input.discard", "claude.call", "claude.response.read", "claude.response.forget", "claude.status"]
    private let rpc: RPC
    private var valid = true
    private var busy = false
    private var sequence = 0
    public private(set) var lastInputID: String?
    public private(set) var lastResponseID: String?

    public init(rpc: @escaping RPC) { self.rpc = rpc }
    public func invalidate() { valid = false }

    /// Parameters use the child's compact JSON representation and are never
    /// reserialized through Swift dictionaries (which can merge Unicode keys).
    /// After an uncertain operation this adapter refuses further effects. The
    /// recorded IDs remain observations, not authority to replay that action.
    public func request(method: String, parameters: Data) async throws -> Data {
        try check()
        guard !busy else { throw Failure.busy }
        guard ["start", "send", "interrupt", "approve", "answer", "poll", "stop"].contains(method),
              parameters.count <= 64_000,
              let object = try? JSONSerialization.jsonObject(with: parameters), object is [String: Any] else {
            throw Failure.invalidParameters
        }
        try ExtensionRelayBootstrap.validateJSON(parameters)
        guard let text = String(data: parameters, encoding: .utf8) else { throw Failure.invalidParameters }
        busy = true
        defer { busy = false }
        do {
            let opened: Opened = try await call("claude.input.open", ["totalUnits": text.utf16.count])
            try requireOK(opened.status)
            guard Self.identifier(opened.inputID, prefix: "input-"), opened.nextOffset == 0 else { throw Failure.invalidResponse }
            lastInputID = opened.inputID; lastResponseID = nil
            let units = Array(text.utf16)
            var offset = 0
            while offset < units.count {
                var end = min(offset + 256, units.count)
                if end < units.count && (0xd800...0xdbff).contains(units[end - 1]) { end -= 1 }
                let content = String(decoding: units[offset..<end], as: UTF16.self)
                let appended: Appended = try await call("claude.input.append",
                    ["inputID": opened.inputID, "offset": offset, "content": content])
                try requireOK(appended.status)
                guard appended.nextOffset == end else { throw Failure.invalidResponse }
                offset = end
            }
            let sealed: Status = try await call("claude.input.seal", ["inputID": opened.inputID])
            try requireOK(sealed.status)
            let receipt: Receipt = try await call("claude.call", ["method": method, "inputID": opened.inputID])
            try requireOK(receipt.status)
            guard Self.identifier(receipt.responseID, prefix: "response-"),
                  (1...600_000).contains(receipt.totalUnits),
                  receipt.totalBytes >= receipt.totalUnits, receipt.totalBytes <= 600_000 else { throw Failure.invalidResponse }
            lastResponseID = receipt.responseID
            var result = Data(), received = 0
            while received < receipt.totalUnits {
                let page: Page = try await call("claude.response.read", ["responseID": receipt.responseID, "offset": received])
                try requireOK(page.status)
                let count = page.content.utf16.count
                guard page.responseID == receipt.responseID, page.offset == received,
                      page.totalUnits == receipt.totalUnits, page.totalBytes == receipt.totalBytes,
                      count > 0, count <= 256, page.nextOffset == received + count,
                      page.nextOffset <= receipt.totalUnits, page.done == (page.nextOffset == receipt.totalUnits),
                      result.count + page.content.utf8.count <= receipt.totalBytes else { throw Failure.invalidResponse }
                result.append(contentsOf: page.content.utf8); received = page.nextOffset
            }
            guard result.count == receipt.totalBytes else { throw Failure.invalidResponse }
            try ExtensionRelayBootstrap.validateJSON(result)
            let response = try JSONDecoder().decode(ChildResponse.self, from: result)
            let fields = try JSONSerialization.jsonObject(with: result) as? [String: Any]
            let expected: Set<String> = response.ok ? ["version", "id", "sequence", "ok", "result"]
                : ["version", "id", "sequence", "ok", "error", "phase"]
            let next = sequence + 1
            guard response.version == 1, response.sequence == next, response.id == "command-\(next)",
                  fields.map({ Set($0.keys) }) == expected,
                  response.ok || response.error == "ACTION_REJECTED" else { throw Failure.invalidResponse }
            let forgotten: Accepted = try await call("claude.response.forget", ["responseID": receipt.responseID])
            guard forgotten.accepted else { throw Failure.invalidResponse }
            sequence = next
            return result
        } catch {
            valid = false
            throw error
        }
    }

    private func check() throws {
        try Task.checkCancellation()
        guard valid else { throw Failure.closed }
    }
    private func requireOK(_ status: String) throws {
        guard status == "ok" else { throw Failure.refused(status) }
    }
    private func call<Value: Decodable>(_ method: String, _ input: [String: Any]) async throws -> Value {
        try check()
        let encoded = try JSONSerialization.data(withJSONObject: input, options: [.sortedKeys, .withoutEscapingSlashes])
        guard encoded.count <= 3072 else { throw Failure.invalidParameters }
        let result = try await rpc(method, encoded)
        try check()
        guard result.count <= 3072 else { throw Failure.invalidResponse }
        try ExtensionRelayBootstrap.validateJSON(result)
        let envelope = try JSONDecoder().decode(Envelope<Value>.self, from: result)
        guard envelope.kind == "success", let value = envelope.value else { throw Failure.invalidResponse }
        return value
    }
    private static func identifier(_ id: String, prefix: String) -> Bool {
        guard id.hasPrefix(prefix), let number = UInt64(id.dropFirst(prefix.count)),
              number > 0, number <= 9_007_199_254_740_991 else { return false }
        return id == prefix + String(number)
    }
    private struct Envelope<Value: Decodable>: Decodable { let kind: String; let value: Value? }
    private struct Opened: Decodable { let status: String; let inputID: String; let nextOffset: Int }
    private struct Appended: Decodable { let status: String; let nextOffset: Int }
    private struct Status: Decodable { let status: String }
    private struct Accepted: Decodable { let accepted: Bool }
    private struct Receipt: Decodable { let status: String; let responseID: String; let totalUnits: Int; let totalBytes: Int }
    private struct Page: Decodable {
        let status: String; let responseID: String; let offset: Int; let nextOffset: Int
        let totalUnits: Int; let totalBytes: Int; let content: String; let done: Bool
    }
    private struct ChildResponse: Decodable {
        let version: Int; let id: String; let sequence: Int; let ok: Bool; let error: String?
    }
}
