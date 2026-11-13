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
    public struct Observation: Equatable {
        public enum Source: Equatable { case unavailable, executor, received }
        public let source: Source
        public let method: String
        public let inputID: String?
        public let responseID: String?
        public let response: Data?
        public let backendPhase: String?
        public let failure: String?
        public let exitJSON: String?
        public let reaped: Bool?
    }
    private var scopeOpen = true
    private var lastMethod: String?
    private var dispatched = false
    private var completeResponse: Data?
    private var valid = true
    private var busy = false
    private var sequence = 0
    public private(set) var lastInputID: String?
    public private(set) var lastResponseID: String?

    public init(rpc: @escaping RPC) { self.rpc = rpc }
    public func invalidate() { scopeOpen = false; valid = false }

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
        lastInputID = nil; lastResponseID = nil; completeResponse = nil
        lastMethod = method; dispatched = false
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
            dispatched = true
            let receipt: Receipt = try await call("claude.call", ["method": method, "inputID": opened.inputID])
            try requireOK(receipt.status)
            guard Self.identifier(receipt.responseID, prefix: "response-"),
                  (1...600_000).contains(receipt.totalUnits),
                  receipt.totalBytes >= receipt.totalUnits, receipt.totalBytes <= 600_000 else { throw Failure.invalidResponse }
            lastResponseID = receipt.responseID
            let result = try await readResponse(receipt)
            completeResponse = result
            let forgotten: Accepted = try await call("claude.response.forget", ["responseID": receipt.responseID])
            guard forgotten.accepted else { throw Failure.invalidResponse }
            sequence += 1
            return result
        } catch {
            valid = false
            throw error
        }
    }

    /// Explicit observation only. The original RPC and conversation remain
    /// unknown; reading never dispatches, forgets, advances sequence or reopens effects.
    public func observe() async throws -> Observation {
        try checkObservation()
        guard !busy else { throw Failure.busy }
        guard !valid, let method = lastMethod else { throw Failure.closed }
        busy = true
        defer { busy = false }
        func snapshot(_ source: Observation.Source, response: Data? = nil, backend: Backend? = nil) -> Observation {
            Observation(source: source, method: method, inputID: lastInputID, responseID: lastResponseID,
                response: response, backendPhase: backend?.phase, failure: backend?.failure,
                exitJSON: backend?.exitJSON, reaped: backend?.reaped)
        }
        if let completeResponse { return snapshot(.received, response: completeResponse) }
        guard dispatched, let inputID = lastInputID else { return snapshot(.unavailable) }
        let backend: Backend = try await call("claude.status", [:], observing: true)
        guard !backend.phase.isEmpty, backend.phase.utf8.count <= 32, backend.failure.utf8.count <= 32,
              backend.exitJSON.utf8.count <= 256 else { throw Failure.invalidResponse }
        // The protocol permits null. Validate exactly one JSON value through
        // the existing strict container parser without accepting trailing data.
        let exit = try ClaudeJSON.parse(Data(("[" + backend.exitJSON + "]").utf8))
        guard exit.elements?.count == 1 else { throw Failure.invalidResponse }
        if backend.inputID.isEmpty && backend.responseID.isEmpty { return snapshot(.unavailable, backend: backend) }
        guard backend.inputID.utf8.elementsEqual(inputID.utf8),
              Self.identifier(backend.responseID, prefix: "response-"),
              lastResponseID == nil || lastResponseID == backend.responseID else { throw Failure.invalidResponse }
        lastResponseID = backend.responseID
        // A zero-offset read supplies bounded lengths; the shared reader checks
        // every subsequent page and the original child command identity.
        let first: Page = try await call("claude.response.read", ["responseID": backend.responseID, "offset": 0], observing: true)
        try requireOK(first.status)
        let receipt = Receipt(status: "ok", responseID: backend.responseID,
            totalUnits: first.totalUnits, totalBytes: first.totalBytes)
        let raw = try await readResponse(receipt, observing: true, first: first)
        try checkObservation()
        completeResponse = raw
        return snapshot(.executor, response: raw, backend: backend)
    }

    private func checkObservation() throws {
        try Task.checkCancellation()
        guard scopeOpen else { throw Failure.closed }
    }
    private func readResponse(_ receipt: Receipt, observing: Bool = false, first: Page? = nil) async throws -> Data {
        guard Self.identifier(receipt.responseID, prefix: "response-"),
              (1...600_000).contains(receipt.totalUnits), receipt.totalBytes >= receipt.totalUnits,
              receipt.totalBytes <= 600_000 else { throw Failure.invalidResponse }
        var result = Data(), received = 0
        while received < receipt.totalUnits {
            let page: Page
            if received == 0, let first { page = first }
            else { page = try await call("claude.response.read", ["responseID": receipt.responseID, "offset": received], observing: observing) }
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
        return result
    }

    private func check() throws {
        try Task.checkCancellation()
        guard valid else { throw Failure.closed }
    }
    private func requireOK(_ status: String) throws {
        guard status == "ok" else { throw Failure.refused(status) }
    }
    private func call<Value: Decodable>(_ method: String, _ input: [String: Any], observing: Bool = false) async throws -> Value {
        guard !observing || ["claude.status", "claude.response.read"].contains(method) else { throw Failure.closed }
        if observing { try checkObservation() } else { try check() }
        let encoded = try JSONSerialization.data(withJSONObject: input, options: [.sortedKeys, .withoutEscapingSlashes])
        guard encoded.count <= 3072 else { throw Failure.invalidParameters }
        let result = try await rpc(method, encoded)
        if observing { try checkObservation() } else { try check() }
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
    private struct Backend: Decodable {
        let phase: String; let failure: String; let exitJSON: String; let reaped: Bool
        let responseID: String; let inputID: String
    }
    private struct ChildResponse: Decodable {
        let version: Int; let id: String; let sequence: Int; let ok: Bool; let error: String?
    }
}
