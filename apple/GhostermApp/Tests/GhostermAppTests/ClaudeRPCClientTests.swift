import Foundation
@testable import GhostermAppModel
import XCTest

/// Named document-RPC fixture, not a Claude SDK/CLI or SSH execution.
@MainActor
private final class ClaudeDocumentFixture {
    var calls: [String] = []
    var uploaded = ""
    var reply = Data()
    var sequence = 0
    var modifyPage: ([String: Any]) -> [String: Any] = { $0 }
    var modifyReply: (Data) -> Data = { $0 }
    var barrier: CheckedContinuation<Void, Never>?
    var pause = false
    var failAction = false
    var responseID = "response-2"
    var inputID = "input-1"

    func rpc(_ method: String, _ bytes: Data) async throws -> Data {
        XCTAssertLessThanOrEqual(bytes.count, 3072)
        calls.append(method)
        let input = try XCTUnwrap(JSONSerialization.jsonObject(with: bytes) as? [String: Any])
        let value: [String: Any]
        switch method {
        case "claude.input.open":
            uploaded = ""
            value = ["status": "ok", "inputID": inputID, "nextOffset": 0]
        case "claude.input.append":
            XCTAssertEqual(input["inputID"] as? String, inputID)
            XCTAssertEqual(input["offset"] as? Int, uploaded.utf16.count)
            uploaded += try XCTUnwrap(input["content"] as? String)
            value = ["status": "ok", "nextOffset": uploaded.utf16.count]
        case "claude.input.seal": value = ["status": "ok"]
        case "claude.call":
            if pause { await withCheckedContinuation { barrier = $0 } }
            if failAction { throw ClaudeRPCClient.Failure.refused("fixture outcome unknown") }
            sequence += 1
            let record: [String: Any] = ["version": 1, "id": "command-\(sequence)", "sequence": sequence, "ok": true,
                "result": ["originalText": uploaded, "longText": String(repeating: "中文😀/\n", count: 700)]]
            reply = modifyReply(try JSONSerialization.data(withJSONObject: record, options: [.sortedKeys, .withoutEscapingSlashes]))
            value = ["status": "ok", "responseID": responseID, "totalUnits": String(decoding: reply, as: UTF8.self).utf16.count,
                     "totalBytes": reply.count]
        case "claude.response.read":
            let offset = try XCTUnwrap(input["offset"] as? Int)
            let units = Array(String(decoding: reply, as: UTF8.self).utf16)
            var end = min(units.count, offset + 256)
            if end < units.count && (0xd800...0xdbff).contains(units[end - 1]) { end -= 1 }
            value = modifyPage(["status": "ok", "responseID": responseID, "offset": offset, "nextOffset": end,
                "totalUnits": units.count, "totalBytes": reply.count,
                "content": String(decoding: units[offset..<end], as: UTF16.self), "done": end == units.count])
        case "claude.response.forget": value = ["accepted": true]
        default: XCTFail("Unexpected fixture method \(method)"); throw ClaudeRPCClient.Failure.invalidResponse
        }
        return try JSONSerialization.data(withJSONObject: ["kind": "success", "value": value], options: [.withoutEscapingSlashes])
    }
}

final class ClaudeRPCClientTests: XCTestCase {
    @MainActor
    func testCompleteUnicodeParametersAndReplyAreNeverReserializedOrTruncated() async throws {
        let fixture = ClaudeDocumentFixture(), client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        // These keys are distinct UTF-8 despite Swift String canonical equality.
        let input = "{\"answers\":{\"é\":\"one\",\"e\u{301}\":\"two\"},\"text\":\"" + String(repeating: "中文😀/", count: 900) + "\"}"
        let result = try await client.request(method: "answer", parameters: Data(input.utf8))
        XCTAssertTrue(fixture.uploaded.utf8.elementsEqual(input.utf8), "Claude upload must preserve exact UTF8 and distinct Unicode keys")
        XCTAssertEqual(result, fixture.reply, "Claude reply pages must preserve the complete record")
        XCTAssertEqual(fixture.calls.filter { $0 == "claude.call" }.count, 1)
        XCTAssertEqual(fixture.calls.last, "claude.response.forget")
        _ = try await client.request(method: "poll", parameters: Data("{}".utf8))
        XCTAssertEqual(fixture.sequence, 2)
    }

    @MainActor
    func testReplyPageIdentityOffsetsCountsAndCompletionMustMatch() async throws {
        for field in ["responseID", "offset", "nextOffset", "totalUnits", "totalBytes", "done", "content"] {
            let fixture = ClaudeDocumentFixture()
            fixture.modifyPage = { page in
                var bad = page
                if field == "responseID" { bad[field] = "response-999" }
                else if field == "done" { bad[field] = !(page[field] as! Bool) }
                else if field == "content" { bad[field] = "" }
                else { bad[field] = (page[field] as! Int) + 1 }
                return bad
            }
            let client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
            do { _ = try await client.request(method: "poll", parameters: Data("{}".utf8)); XCTFail("Claude must reject mismatched reply metadata: \(field)"); continue }
            catch { XCTAssertEqual(error as? ClaudeRPCClient.Failure, .invalidResponse, field) }
            XCTAssertFalse(fixture.calls.contains("claude.response.forget"), "An invalid receipt must not be discarded")
        }
    }

    @MainActor
    func testChildReplyIdentityAndDuplicateFieldsCannotBecomeSuccess() async throws {
        for mode in ["id", "sequence", "duplicate", "result"] {
            let fixture = ClaudeDocumentFixture()
            fixture.modifyReply = { bytes in
                var text = String(decoding: bytes, as: UTF8.self)
                if mode == "id" { text = text.replacingOccurrences(of: "command-1", with: "command-9") }
                else if mode == "sequence" { text = text.replacingOccurrences(of: "\"sequence\":1", with: "\"sequence\":9") }
                else if mode == "duplicate" { text = text.replacingOccurrences(of: "\"ok\":true", with: "\"ok\":true,\"ok\":true") }
                else { text = "{\"version\":1,\"id\":\"command-1\",\"sequence\":1,\"ok\":true}" }
                return Data(text.utf8)
            }
            let client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
            do { _ = try await client.request(method: "poll", parameters: Data("{}".utf8)); XCTFail("Claude must reject invalid child envelope: \(mode)"); continue }
            catch { }
            XCTAssertFalse(fixture.calls.contains("claude.response.forget"))
        }
    }

    @MainActor
    func testInvalidationRejectsLateReceiptAndBusyCannotDispatchSecondAction() async throws {
        let fixture = ClaudeDocumentFixture(); fixture.pause = true
        let client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        let first = Task { try await client.request(method: "send", parameters: Data("{}".utf8)) }
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while fixture.barrier == nil && ProcessInfo.processInfo.systemUptime < deadline {
            try await Task.sleep(nanoseconds: 1_000_000)
        }
        guard fixture.barrier != nil else { first.cancel(); XCTFail("Fixture action was not reached"); return }
        fixture.pause = false
        do { _ = try await client.request(method: "send", parameters: Data("{}".utf8)); XCTFail("Claude must reject overlapping action") }
        catch { XCTAssertEqual(error as? ClaudeRPCClient.Failure, .busy) }
        client.invalidate(); fixture.barrier?.resume()
        var rejected = false
        do { _ = try await first.value }
        catch { rejected = error as? ClaudeRPCClient.Failure == .closed }
        XCTAssertEqual(fixture.calls.filter { $0 == "claude.call" }.count, 1)
        XCTAssertTrue(rejected && !fixture.calls.contains("claude.response.read"), "Claude invalidated scope must reject a late receipt")
    }

    @MainActor
    func testUnknownActionCannotBeRetriedAndObservationIdentityRemains() async throws {
        let fixture = ClaudeDocumentFixture(); fixture.failAction = true
        let client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        do { _ = try await client.request(method: "send", parameters: Data("{}".utf8)); XCTFail("Expected fixture loss") }
        catch { }
        XCTAssertEqual(client.lastInputID, fixture.inputID); XCTAssertNil(client.lastResponseID)
        var closed = false
        do { _ = try await client.request(method: "send", parameters: Data("{}".utf8)) }
        catch { closed = error as? ClaudeRPCClient.Failure == .closed }
        XCTAssertTrue(closed && fixture.calls.filter { $0 == "claude.call" }.count == 1, "Claude unknown action must never replay")
    }

    @MainActor
    func testInvalidInputAndInactiveAppModelRejectBeforeRPC() async throws {
        let fixture = ClaudeDocumentFixture(), client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        for (method, parameters) in [("install", "{}"), ("send", "[]"), ("send", "{\"text\":1,\"text\":2}"),
                                      ("send", String(repeating: "x", count: 64_001))] {
            do { _ = try await client.request(method: method, parameters: Data(parameters.utf8)); XCTFail("Invalid parameters accepted") }
            catch { }
        }
        XCTAssertTrue(fixture.calls.isEmpty)
        let model = isolatedAppModel()
        do { _ = try await model.requestClaudeAction(method: "start", parameters: Data("{}".utf8)); XCTFail("Inactive AppModel cannot start Claude") }
        catch { }
    }

    func testRelayNodePrefixArgumentsAndSelectedEnvironmentAreLiteralAndOrdered() throws {
        let digest = String(repeating: "a", count: 64)
        let programs = try ExtensionRelayBootstrap.parseProcessProgramsJSON("[{\"name\":\"claude-node\",\"executable\":\"/n\",\"version\":\"v26.4.0\",\"sha256\":\"\(digest)\",\"prefixArguments\":[\"/entry.js\",\"--profile\",\"/it's 中文.json\",\"\"]}]")
        let command = try ExtensionRelayBootstrap.build(.init(executable: "gterm", executorDirectory: "/p",
            executorDigest: digest, clientDigest: digest, methods: [], scopedRoots: [.init(id: "workspace", directory: "/w")],
            processPrograms: programs, processEnvironmentNames: ["CLAUDE_CONFIG_DIR"], command: "ai claude"))
        let prefix = "'--process-argument' 'claude-node' '/entry.js' '--process-argument' 'claude-node' '--profile' " +
            "'--process-argument' 'claude-node' '/it'\\''s 中文.json' '--process-argument' 'claude-node' ''"
        XCTAssertTrue(command.contains(prefix), "Native relay must preserve fixed arguments and literal quoting")
        XCTAssertTrue(command.contains("'--process-environment' 'CLAUDE_CONFIG_DIR'"), "Native relay must select execution-machine environment by name")
        XCTAssertEqual(programs[0].prefixArguments, ["/entry.js", "--profile", "/it's 中文.json", ""])
    }

    func testRelayRejectsInvalidPrefixAndEnvironmentBeforeLaunch() throws {
        let digest = String(repeating: "a", count: 64)
        for name in ["HOME", "PATH", "XDG_CONFIG_HOME", "CODEX_HOME", "TMPDIR", "BAD-NAME", "1NAME", "", "中文"] {
            XCTAssertThrowsError(try ExtensionRelayBootstrap.parseProcessEnvironmentJSON("[\"\(name)\"]"))
        }
        XCTAssertThrowsError(try ExtensionRelayBootstrap.parseProcessEnvironmentJSON("[\"A\",\"A\"]"))
        let base = "\"name\":\"n\",\"executable\":\"/n\",\"version\":\"v26\",\"sha256\":\"\(digest)\""
        for prefix in ["null", "[1]", "[\"a\\n\"]", "[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\",\"m\",\"n\",\"o\",\"p\",\"q\"]"] {
            XCTAssertThrowsError(try ExtensionRelayBootstrap.parseProcessProgramsJSON("[{\(base),\"prefixArguments\":\(prefix)}]"))
        }
    }
}
