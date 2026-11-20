import AppKit
import SwiftUI
@testable import GhostermApp
@testable import GhostermAppModel
import XCTest

/// Named in-memory document endpoint. The helper result is captured before the
/// outer RPC loss; this is not an SDK, SSH or model-execution fixture.
@MainActor
final class ClaudeObservationFixture {
    var calls: [String] = []
    var failurePoint = "call"
    var inputID = "input-1", responseID = "response-2"
    var statusInput: String?, statusResponse: String?
    var emptyStatus = false
    var exitJSON = "null"
    var body = Data()
    var sequence = 0
    var modifyBody: (Data) -> Data = { $0 }
    var modifyPage: ([String: Any]) -> [String: Any] = { $0 }
    var pauseMethod: String?
    var barrier: CheckedContinuation<Void, Never>?
    func release() { barrier?.resume(); barrier = nil }
    func rpc(_ method: String, _ bytes: Data) async throws -> Data {
        calls.append(method)
        if pauseMethod == method { pauseMethod = nil; await withCheckedContinuation { barrier = $0 } }
        let input = try XCTUnwrap(JSONSerialization.jsonObject(with: bytes) as? [String: Any])
        let value: [String: Any]
        switch method {
        case "claude.input.open":
            if failurePoint == "open" { throw ClaudeRPCClient.Failure.closed }
            value = ["status": "ok", "inputID": inputID, "nextOffset": 0]
        case "claude.input.append":
            value = ["status": "ok", "nextOffset": (input["offset"] as! Int) + (input["content"] as! String).utf16.count]
        case "claude.input.seal": value = ["status": "ok"]
        case "claude.call":
            sequence += 1
            body = modifyBody(Data(ClaudeJSON.fields([
                ("version", .number("1")), ("id", .string("command-\(sequence)")), ("sequence", .number(String(sequence))),
                ("ok", .bool(true)), ("result", .fields([("é", .string("one")), ("e\u{301}", .string("two")),
                    ("text", .string(String(repeating: "中文😀/\n", count: 800)))]))
            ]).encoded().utf8))
            if failurePoint == "call" { throw ClaudeRPCClient.Failure.refused("fixture RPC receipt lost") }
            value = ["status": "ok", "responseID": responseID, "totalUnits": String(decoding: body, as: UTF8.self).utf16.count, "totalBytes": body.count]
        case "claude.status": value = ["phase": "live", "failure": "", "exitJSON": exitJSON, "reaped": false,
            "inputID": emptyStatus ? "" : statusInput ?? inputID, "responseID": emptyStatus ? "" : statusResponse ?? responseID]
        case "claude.response.read":
            let offset = input["offset"] as! Int, units = Array(String(decoding: body, as: UTF8.self).utf16)
            var end = min(offset + 256, units.count)
            if end < units.count && (0xd800...0xdbff).contains(units[end - 1]) { end -= 1 }
            value = modifyPage(["status": "ok", "responseID": responseID, "offset": offset, "nextOffset": end,
                "totalUnits": units.count, "totalBytes": body.count, "content": String(decoding: units[offset..<end], as: UTF16.self), "done": end == units.count])
        case "claude.response.forget":
            if failurePoint == "forget" { throw ClaudeRPCClient.Failure.refused("fixture forget receipt lost") }
            value = ["accepted": true]
        default: throw ClaudeRPCClient.Failure.invalidResponse
        }
        return try JSONSerialization.data(withJSONObject: ["kind": "success", "value": value], options: [.withoutEscapingSlashes])
    }
}

@MainActor
final class ClaudeObservationTests: XCTestCase {
    private func lost(_ fixture: ClaudeObservationFixture, method: String = "send") async -> ClaudeRPCClient {
        let client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        do { _ = try await client.request(method: method, parameters: Data("{}".utf8)); XCTFail("Fixture loss expected") }
        catch { }
        return client
    }
    private func wait(_ predicate: @escaping () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(nanoseconds: 1_000_000) }
        guard predicate() else { throw ClaudeRPCClient.Failure.invalidResponse }
    }
    func testLostCallReceiptReadsExactOriginalWithoutEffectsOrReplay() async throws {
        let fixture = ClaudeObservationFixture(), client = await lost(fixture)
        let before = fixture.calls.count
        let observation = try await client.observe()
        XCTAssertEqual(observation.source, .executor)
        XCTAssertEqual(observation.response, fixture.body)
        XCTAssertEqual(observation.inputID, "input-1"); XCTAssertEqual(observation.responseID, "response-2")
        XCTAssertGreaterThan(observation.response?.count ?? 0, 3072)
        XCTAssertTrue(fixture.calls.dropFirst(before).allSatisfy { ["claude.status", "claude.response.read"].contains($0) }, "Observation must be read-only")
        let count = fixture.calls.count
        let cached = try await client.observe()
        XCTAssertEqual(cached.response, fixture.body)
        XCTAssertEqual(fixture.calls.count, count, "Validated local observation must not need another RPC")
        var rejected = false
        do { _ = try await client.request(method: "send", parameters: Data("{}".utf8)) }
        catch { rejected = error as? ClaudeRPCClient.Failure == .closed }
        XCTAssertTrue(rejected && fixture.calls.filter { $0 == "claude.call" }.count == 1, "Observation cannot reopen effects")
    }
    func testLostForgetReceiptUsesAlreadyValidatedBytesWithoutAnotherRPC() async throws {
        let fixture = ClaudeObservationFixture(); fixture.failurePoint = "forget"
        let client = await lost(fixture); let before = fixture.calls.count
        let observation = try await client.observe()
        XCTAssertTrue(observation.source == .received && observation.response == fixture.body && fixture.calls.count == before,
            "An unknown forget must use the validated bytes without another release or action")
    }
    func testNoRetainedReceiptNeverMeansNotExecuted() async throws {
        let fixture = ClaudeObservationFixture(); fixture.emptyStatus = true
        let client = await lost(fixture), result = try await client.observe()
        XCTAssertEqual(result.source, .unavailable); XCTAssertNil(result.response)
        XCTAssertFalse(fixture.calls.contains("claude.response.forget"))
    }
    func testForeignInputOrResponseCannotBeAdopted() async throws {
        for mode in ["input", "response", "known-response"] {
            let fixture = ClaudeObservationFixture()
            if mode == "input" { fixture.statusInput = "input-999" }
            else if mode == "response" { fixture.statusResponse = "response-02" }
            else { fixture.statusResponse = "response-999"; fixture.failurePoint = "page" }
            // A receipt has already bound response-2 before the first read fails.
            if mode == "known-response" { fixture.modifyPage = { var page = $0; page["done"] = true; return page } }
            let client = await lost(fixture); let before = fixture.calls.count
            var rejected = false
            do { _ = try await client.observe() } catch { rejected = true }
            XCTAssertTrue(rejected && fixture.calls.dropFirst(before) == ["claude.status"], "Foreign observation must not read or adopt a response: \(mode)")
        }
    }
    func testReadFailureDoesNotAllowTheResponseIdentityToChange() async throws {
        let fixture = ClaudeObservationFixture(), client = await lost(fixture)
        fixture.modifyPage = { var page = $0; page["done"] = true; return page }
        do { _ = try await client.observe(); XCTFail("Expected incomplete first read") } catch { }
        fixture.modifyPage = { $0 }; fixture.responseID = "response-999"
        let before = fixture.calls.count
        var rejected = false
        do { _ = try await client.observe() } catch { rejected = true }
        XCTAssertTrue(rejected && fixture.calls.dropFirst(before) == ["claude.status"], "A failed read cannot authorize a different response ID")
    }
    func testObservedPagesAndChildIdentityKeepAllExistingChecks() async throws {
        for field in ["responseID", "offset", "nextOffset", "totalUnits", "totalBytes", "done", "content", "id", "sequence", "duplicate"] {
            let fixture = ClaudeObservationFixture()
            if ["id", "sequence", "duplicate"].contains(field) {
                fixture.modifyBody = { data in
                    let old = field == "id" ? "command-1" : field == "sequence" ? "\"sequence\":1" : "\"ok\":true"
                    let new = field == "id" ? "command-9" : field == "sequence" ? "\"sequence\":9" : "\"ok\":true,\"ok\":true"
                    return Data(String(decoding: data, as: UTF8.self).replacingOccurrences(of: old, with: new).utf8)
                }
            } else {
                fixture.modifyPage = { page in
                    var changed = page
                    if field == "responseID" { changed[field] = "response-999" }
                    else if field == "done" { changed[field] = !(page[field] as! Bool) }
                    else if field == "content" { changed[field] = "" }
                    else { changed[field] = (page[field] as! Int) + 1 }
                    return changed
                }
            }
            let client = await lost(fixture)
            var rejected = false
            do { _ = try await client.observe() } catch { rejected = true }
            XCTAssertTrue(rejected && !fixture.calls.contains("claude.response.forget"), "Invalid observed page/child must remain unadopted: \(field)")
        }
    }
    func testInvalidationRejectsObservationBeforeAndAfterAwait() async throws {
        var allRejected = true
        for stage in ["claude.status", "claude.response.read"] {
            let fixture = ClaudeObservationFixture(), client = await lost(fixture)
            fixture.pauseMethod = stage
            let task = Task { try await client.observe() }
            try await wait { fixture.barrier != nil }
            client.invalidate(); fixture.release()
            var rejected = false
            do { _ = try await task.value } catch { rejected = true }
            let count = fixture.calls.count
            do { _ = try await client.observe(); rejected = false } catch { }
            allRejected = allRejected && rejected && count == fixture.calls.count
        }
        XCTAssertTrue(allRejected, "Detached Runtime must reject both late and new observations")
    }
    func testBusyAndCancelledObserversCannotOverlapOrDispatch() async throws {
        let fixture = ClaudeObservationFixture(), client = await lost(fixture)
        fixture.pauseMethod = "claude.status"
        let task = Task { try await client.observe() }; try await wait { fixture.barrier != nil }
        var busy = false
        do { _ = try await client.observe() } catch { busy = error as? ClaudeRPCClient.Failure == .busy }
        task.cancel(); fixture.release()
        var cancelled = false
        do { _ = try await task.value } catch { cancelled = error is CancellationError }
        XCTAssertTrue(busy && cancelled && fixture.calls.filter { $0 == "claude.call" }.count == 1 && !fixture.calls.contains("claude.response.read"))
    }
    func testFailedNextOpenClearsThePreviousOperationIdentity() async throws {
        let fixture = ClaudeObservationFixture(); fixture.failurePoint = ""
        let client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        _ = try await client.request(method: "poll", parameters: Data("{}".utf8))
        fixture.failurePoint = "open"
        do { _ = try await client.request(method: "send", parameters: Data("{}".utf8)); XCTFail("Expected failed open") } catch { }
        let before = fixture.calls.count, observation = try await client.observe()
        XCTAssertTrue(observation.source == .unavailable && observation.inputID == nil && observation.response == nil && fixture.calls.count == before,
            "A failed new upload cannot adopt the previous action's identity or cached body")
    }
    func testMalformedExitObservationCannotAuthorizeReading() async throws {
        for value in ["", "null,null", "{\"state\":1,\"state\":2}", "null trailing"] {
            let fixture = ClaudeObservationFixture(); fixture.exitJSON = value
            let client = await lost(fixture)
            var rejected = false
            do { _ = try await client.observe() } catch { rejected = true }
            XCTAssertTrue(rejected && !fixture.calls.contains("claude.response.read"))
        }
    }
    func testLiveAdapterAndInactiveAppModelCannotObserve() async throws {
        let fixture = ClaudeObservationFixture(), client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        do { _ = try await client.observe(); XCTFail("An active adapter is not an unknown-outcome observer") } catch { }
        XCTAssertTrue(fixture.calls.isEmpty)
        let model = isolatedAppModel()
        do { _ = try await model.observeClaudeOutcome(); XCTFail("Inactive AppModel cannot observe") } catch { }
    }
}

@MainActor
final class ClaudeObservationConversationTests: XCTestCase {
    private func wait(_ predicate: @escaping () -> Bool) async throws {
        let end = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < end { try await Task.sleep(nanoseconds: 1_000_000) }
        guard predicate() else { throw ClaudeRPCClient.Failure.invalidResponse }
    }
    func testObservedReceiptCannotResumeConversationOrClearTheDraft() async throws {
        let fixture = ClaudeObservationFixture(), client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        let model = ClaudeConversation(request: { try await client.request(method: $0, parameters: $1) }, observe: { try await client.observe() })
        defer { model.invalidate(); client.invalidate() }
        model.draft = "unconfirmed input 中文😀"; await model.start()
        let originalIssue = model.issue
        await model.observeOutcome()
        XCTAssertEqual(model.observation?.response, fixture.body)
        XCTAssertEqual(model.phase, .unknown); XCTAssertEqual(model.draft, "unconfirmed input 中文😀")
        XCTAssertEqual(model.issue, originalIssue)
        await model.send(); await model.start(); await model.stop(); await model.interrupt()
        XCTAssertTrue(!model.canSend && !model.canStart && !model.canStop && !model.canInterrupt && fixture.calls.filter { $0 == "claude.call" }.count == 1,
            "Observed data cannot authorize another business action")
    }
    func testDetachedOrReplacedOwnerRejectsTheLateObservation() async throws {
        for detach in [true, false] {
            let fixture = ClaudeObservationFixture(), client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
            var owner = true
            let model = ClaudeConversation(request: { try await client.request(method: $0, parameters: $1) },
                observe: { try await client.observe() }, isCurrent: { owner })
            defer { model.invalidate(); client.invalidate(); fixture.release() }
            await model.start(); fixture.pauseMethod = "claude.status"
            let task = Task { await model.observeOutcome() }; try await wait { fixture.barrier != nil }
            if detach { model.invalidate() } else { owner = false }
            fixture.release(); await task.value
            XCTAssertTrue(model.observation == nil && !model.canObserve, "A late read cannot update a detached/replaced conversation")
        }
    }
    func testObservationFailurePreservesTheUnknownBusinessIssue() async throws {
        let model = ClaudeConversation(request: { _, _ in throw ClaudeRPCClient.Failure.closed },
            observe: { throw ClaudeRPCClient.Failure.invalidResponse })
        defer { model.invalidate() }
        await model.start(); let original = model.issue
        await model.observeOutcome()
        XCTAssertTrue(model.issue == original && model.observationIssue != nil && model.phase == .unknown && model.observation == nil)
    }
    func testRenderedObservationButtonShowsCompleteReadonlyReceiptWithoutResend() async throws {
        TestWindows.enterAutomationMode()
        let enhanced = NSAccessibility.Attribute(rawValue: "AXEnhancedUserInterface")
        NSApplication.shared.accessibilitySetValue(true, forAttribute: enhanced)
        defer { NSApplication.shared.accessibilitySetValue(false, forAttribute: enhanced) }
        let fixture = ClaudeObservationFixture(), client = ClaudeRPCClient(rpc: { try await fixture.rpc($0, $1) })
        let model = ClaudeConversation(request: { try await client.request(method: $0, parameters: $1) }, observe: { try await client.observe() })
        model.draft = "原输入保持未知"
        await model.start()
        let probe = ConversationUIProbe(root: NSHostingView(rootView: ClaudeConversationView(model: model).padding().background(Color(nsColor: .windowBackgroundColor))))
        defer { model.invalidate(); client.invalidate(); probe.close() }
        func control(_ id: String) -> NSObject? {
            probe.root.layoutSubtreeIfNeeded()
            return PluginTableRendering.elements(probe.root).first {
                PluginTableRendering.attribute($0, "accessibilityIdentifier") == id
            }
        }
        try await wait { control("plugin-claude-observe") != nil }
        XCTAssertTrue(PluginTableRendering.press(try XCTUnwrap(control("plugin-claude-observe"))))
        do { try await wait { model.observation != nil } }
        catch { XCTFail("Claude rendered observation must read the retained receipt"); return }
        try await wait { probe.textViews().contains { $0.identifier?.rawValue == "plugin-claude-observed-response" } }
        let text = try XCTUnwrap(probe.textViews().first { $0.identifier?.rawValue == "plugin-claude-observed-response" })
        XCTAssertTrue(text.string.utf8.elementsEqual(String(decoding: fixture.body, as: UTF8.self).utf8))
        XCTAssertTrue(text.isSelectable && !text.isEditable)
        let send = try XCTUnwrap(control("plugin-claude-send"))
        let selector = NSSelectorFromString("isAccessibilityEnabled")
        let implementation = try XCTUnwrap(send.method(for: selector))
        typealias Enabled = @convention(c) (AnyObject, Selector) -> Bool
        XCTAssertFalse(unsafeBitCast(implementation, to: Enabled.self)(send, selector))
        XCTAssertTrue(model.phase == .unknown && fixture.calls.filter { $0 == "claude.call" }.count == 1, "Rendered observation cannot resend an unknown action")
    }
}
