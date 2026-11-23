import Foundation
@testable import GhostermAppModel
import XCTest

/// Explicit private-protocol fixture. These tests do not execute an SDK/model.
@MainActor
final class ClaudeChatFixture {
    struct Call { let method: String; let input: ClaudeJSON }
    var calls: [Call] = []
    var events: [ClaudeJSON] = []
    var state = "ready"
    var inputID = "input-one"
    var helperSequence = 0
    var eventSequence = 0
    var pauseSend = false
    var pausePoll = false
    var sendBarrier: CheckedContinuation<Void, Never>?
    var pollBarrier: CheckedContinuation<Void, Never>?
    var failSend = false
    var accepted = true
    var rejectSend = false

    func request(_ method: String, _ input: Data) async throws -> Data {
        let value = try ClaudeJSON.parse(input)
        calls.append(Call(method: method, input: value)); helperSequence += 1
        let sequence = helperSequence
        let result: ClaudeJSON
        switch method {
        case "start": result = .fields([("requested", .bool(true)), ("backend", .string("cli")), ("cliVersion", .string("2.1.323"))])
        case "send":
            if pauseSend { await withCheckedContinuation { sendBarrier = $0 } }
            if failSend { throw ClaudeRPCClient.Failure.refused("fixture transport unknown") }
            if rejectSend {
                rejectSend = false
                return Data(ClaudeJSON.fields([("version", .number("1")), ("id", .string("command-\(sequence)")),
                    ("sequence", .number(String(sequence))), ("ok", .bool(false)), ("error", .string("ACTION_REJECTED")), ("phase", .string(state))]).encoded().utf8)
            }
            state = "running"; result = .fields([("messageID", .string(inputID))])
        case "interrupt":
            state = "interrupting"
            session(.fields([("kind", .string("interrupt-receipt")), ("messageID", .string(inputID)), ("receipt", .object([]))]))
            result = .fields([("requested", .bool(true))])
        case "approve", "answer": result = .fields([("accepted", .bool(accepted))])
        case "stop": state = "closed"; result = .fields([("phase", .string("closed")), ("pendingMessageID", .string(inputID))])
        case "poll":
            if pausePoll { pausePoll = false; await withCheckedContinuation { pollBarrier = $0 } }
            if value["maxEvents"] != nil {
                let batch = Array(events.prefix(4)); events.removeFirst(batch.count)
                result = .fields([("source", .string("batch")), ("events", .array(batch)), ("phase", .string(state))])
            } else { result = events.isEmpty ? .fields([("source", .string("none")), ("phase", .string(state))]) : events.removeFirst() }
        default: throw ClaudeRPCClient.Failure.invalidParameters
        }
        return Data(ClaudeJSON.fields([("version", .number("1")), ("id", .string("command-\(sequence)")),
            ("sequence", .number(String(sequence))), ("ok", .bool(true)), ("result", result)]).encoded().utf8)
    }
    func session(_ event: ClaudeJSON) {
        eventSequence += 1
        guard case .object(let members) = event else { preconditionFailure("fixture event must be an object") }
        let fields = members + [.init(name: "sequence", value: .number(String(eventSequence)))]
        events.append(.fields([("source", .string("session")), ("event", .object(fields))]))
    }
    func message(_ value: ClaudeJSON) { session(.fields([("kind", .string("sdk-message")), ("message", value)])) }
    func result(id: String? = nil, error: Bool = false) {
        state = "ready"
        message(.fields([("type", .string("result")), ("user_message_uuid", .string(id ?? inputID)),
            ("is_error", .bool(error)), ("subtype", .string("success")),
            ("result", .string(error ? "fixture backend error" : "fixture result"))]))
    }
    static func ref(_ name: String, token: String = "token-one") -> ClaudeJSON {
        .fields([("scope", .string("fixture")), ("requestID", .string(name)), ("token", .string(token))])
    }
    static func question(_ name: String, multi: Bool = false) -> ClaudeJSON {
        .fields([("question", .string(name)), ("header", .string("选择")), ("multiSelect", .bool(multi)),
            ("options", .array([.fields([("label", .string("Blue")), ("description", .string("First"))]),
                                .fields([("label", .string("Green")), ("description", .string("Second"))])]))])
    }
    func prompt(_ name: String = "request-one", token: String = "token-one", questions: [ClaudeJSON]? = nil, turn: String? = nil) {
        let input: ClaudeJSON = questions.map { .fields([("questions", .array($0))]) } ?? .fields([("command", .string("fixture only; " + String(repeating: "中😀", count: 1200)))])
        var fields: [(String, ClaudeJSON)] = [("reference", Self.ref(name, token: token)), ("kind", .string(questions == nil ? "tool" : "question")),
            ("toolName", .string(questions == nil ? "Bash" : "AskUserQuestion")), ("turnID", .string(turn ?? inputID)), ("input", input)]
        if let questions { fields.append(("questions", .array(questions))) }
        events.append(.fields([("source", .string("approval")), ("event", .fields([("kind", .string("requested")), ("request", .fields(fields))]))]))
    }
    func expire(_ name: String, token: String = "token-one") {
        events.append(.fields([("source", .string("approval")), ("event", .fields([("kind", .string("expired")), ("reference", Self.ref(name, token: token))]))]))
    }
    func release() { sendBarrier?.resume(); sendBarrier = nil; pollBarrier?.resume(); pollBarrier = nil }
}

@MainActor
final class ClaudeConversationTests: XCTestCase {
    func until(_ message: String = "Expected Native state", _ predicate: @escaping () -> Bool) async throws {
        let deadline = ProcessInfo.processInfo.systemUptime + 5
        while !predicate() && ProcessInfo.processInfo.systemUptime < deadline { try await Task.sleep(nanoseconds: 5_000_000) }
        XCTAssertTrue(predicate(), message)
        if !predicate() { throw ClaudeRPCClient.Failure.invalidResponse }
    }
    func ready() async throws -> (ClaudeConversation, ClaudeChatFixture) {
        let fixture = ClaudeChatFixture(), model = ClaudeConversation(request: { try await fixture.request($0, $1) })
        XCTAssertTrue(fixture.calls.isEmpty, "Activation must not start a backend")
        await model.start(); try await until { model.phase == .ready }
        return (model, fixture)
    }

    func testMessagesStreamAndCorrelatedResultsRetireTheExplicitInput() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "literal input 中文😀\n"; await model.send()
        XCTAssertEqual(fixture.calls.first { $0.method == "send" }?.input["text"]?.text, "literal input 中文😀\n")
        for delta in ["Hello ", "中文😀"] {
            fixture.message(.fields([("type", .string("stream_event")), ("event", .fields([("type", .string("content_block_delta")),
                ("index", .number("0")), ("delta", .fields([("type", .string("text_delta")), ("text", .string(delta))]))]))]))
        }
        try await until { model.entries.contains { $0.title == "Claude · 正在生成" && $0.text == "Hello 中文😀" } }
        fixture.message(.fields([("type", .string("assistant")), ("message", .fields([("content", .array([.fields([("type", .string("text")), ("text", .string("Hello 中文😀"))])]))]))]))
        fixture.result(); try await until { model.phase == .ready }
        XCTAssertNil(model.activeMessageID)
        XCTAssertFalse(model.entries.contains { $0.id.hasPrefix("stream:") })
        XCTAssertTrue(model.entries.contains { $0.title == "本轮已完成" })
        fixture.inputID = "input-two"; model.draft = "second explicit input"; await model.send(); fixture.result(error: true)
        try await until { model.phase == .ready }
        XCTAssertTrue(model.entries.contains { $0.title == "本轮未成功" }, "Known backend errors must not become transport success")
    }

    func testToolApprovalIsExplicitOnceAndByteDistinctReferencesStaySeparate() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "fixture"; await model.send(); fixture.prompt("é"); fixture.prompt("e\u{301}")
        try await until { model.pending.count == 2 }
        let first = model.pending[0], second = model.pending[1]
        XCTAssertNotEqual(first.id, second.id)
        XCTAssertTrue(first.inputJSON.contains(String(repeating: "中😀", count: 1200)), "Tool review must preserve complete input")
        XCTAssertFalse(fixture.calls.contains { $0.method == "approve" })
        await model.respond(first.reference, allow: true); await model.respond(first.reference, allow: true)
        XCTAssertTrue(fixture.calls.filter { $0.method == "approve" }.count == 1 && model.pending.count == 1 && model.pending[0].reference == second.reference,
                      "Native approval must resolve exactly one byte-exact reference once")
    }

    func testQuestionsNeedEveryExplicitAnswerAndPreserveUnicodeAndNumericKeys() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "fixture"; await model.send()
        fixture.prompt(questions: [ClaudeChatFixture.question("é"), ClaudeChatFixture.question("e\u{301}"), ClaudeChatFixture.question("10"), ClaudeChatFixture.question("2")])
        try await until { model.pending.count == 1 }
        let prompt = model.pending[0]
        await model.respond(prompt.reference, allow: true); await model.answer(prompt.reference, answers: ["partial"])
        XCTAssertFalse(fixture.calls.contains { ["approve", "answer"].contains($0.method) }, "Questions must not accept generic allow or partial answers")
        await model.answer(prompt.reference, answers: ["first", "second", "ten", "two"])
        let answer = try XCTUnwrap(fixture.calls.first { $0.method == "answer" }?.input["answers"])
        XCTAssertEqual(answer["é"]?.text, "first"); XCTAssertEqual(answer["e\u{301}"]?.text, "second")
        XCTAssertTrue(answer.encoded().hasPrefix("{\"2\":\"two\",\"10\":\"ten\""), "Integer-like question keys must match JSON.stringify ordering")
        XCTAssertTrue(model.pending.isEmpty)
    }

    func testInterruptReceiptDoesNotCompleteInputAndOldPromptCannotRun() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "fixture"; await model.send(); fixture.prompt(); try await until { model.pending.count == 1 }
        let reference = model.pending[0].reference
        await model.interrupt(); await model.interrupt()
        try await until { model.entries.contains { $0.title.hasPrefix("已收到中断回执") } }
        let unresolved = model.phase == .interrupting && model.activeMessageID == fixture.inputID && model.pending.isEmpty
        XCTAssertTrue(unresolved,
                      "Interrupt receipt must preserve unresolved input and retire prompts")
        guard unresolved else { return }
        await model.respond(reference, allow: true)
        XCTAssertFalse(fixture.calls.contains { $0.method == "approve" })
        fixture.result(); try await until { model.phase == .ready }
    }

    func testQueuedApprovalRechecksAfterAnInFlightPollExpiresIt() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "fixture"; await model.send(); fixture.prompt(); try await until { model.pending.count == 1 }
        let reference = model.pending[0].reference
        fixture.pausePoll = true; try await until { fixture.pollBarrier != nil }
        let decision = Task { await model.respond(reference, allow: true) }
        try await until { model.busy }
        fixture.expire("request-one"); fixture.pollBarrier?.resume(); fixture.pollBarrier = nil
        await decision.value
        XCTAssertTrue(!fixture.calls.contains { $0.method == "approve" } && model.phase == .running,
                      "Queued approval must recheck the live reference after polling")
    }

    func testForeignResultKeepsInputUnknownAndWrongTurnCannotOfferApproval() async throws {
        for wrongPrompt in [false, true] {
            let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
            model.draft = "fixture"; await model.send()
            if wrongPrompt { fixture.prompt(turn: "foreign-input") } else { fixture.result(id: "foreign-input") }
            try await until { !model.records.isEmpty }
            XCTAssertTrue([.lost, .unknown].contains(model.phase) && model.activeMessageID == fixture.inputID && !model.canSend && model.pending.isEmpty,
                          "Foreign backend identity must never complete or authorize the active input")
        }
    }

    func testConfirmedSendKeepsTheDraftEditedWhileWaiting() async throws {
        let (model, fixture) = try await ready()
        defer { model.invalidate(); fixture.release() }
        let submitted = "submitted Claude input"
        model.draft = submitted
        fixture.pauseSend = true
        let send = Task { await model.send() }
        try await until { fixture.sendBarrier != nil }
        model.draft = "draft for the next Claude turn"
        fixture.release()
        await send.value

        XCTAssertEqual(fixture.calls.last(where: { $0.method == "send" })?.input["text"]?.text, submitted)
        XCTAssertEqual(model.activeMessageID, fixture.inputID)
        XCTAssertEqual(model.phase, .running)
        XCTAssertEqual(model.draft, "draft for the next Claude turn")
    }

    func testInvalidationAndUnknownSendDoNotClearDraftOrReplay() async throws {
        for loss in [false, true] {
            let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
            model.draft = "preserve this draft"; fixture.pauseSend = !loss; fixture.failSend = loss
            let send = Task { await model.send() }
            if !loss {
                try await until { fixture.sendBarrier != nil }; model.invalidate()
                fixture.sendBarrier?.resume(); fixture.sendBarrier = nil
            }
            await send.value; await model.send()
            XCTAssertTrue(model.phase == .unknown && model.draft == "preserve this draft" &&
                fixture.calls.filter { $0.method == "send" }.count == 1 && model.activeMessageID == nil,
                "Late or unknown send must preserve the draft without replay")
        }
    }

    func testStopPreservesUnconfirmedInputAndJSONStringsKeepExactEscapes() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "fixture"; await model.send(); await model.stop()
        XCTAssertTrue(model.phase == .closed && model.activeMessageID == fixture.inputID && model.issue != nil)
        let text = "quote\" slash/ back\\ null\u{0} separator\u{2028} emoji😀"
        let json = ClaudeJSON.fields([("text", .string(text))]).encoded()
        XCTAssertTrue(json.contains("\\u0000") && !json.contains("\\/"))
        XCTAssertTrue(try XCTUnwrap(ClaudeJSON.parse(Data(json.utf8))["text"]?.text).utf8.elementsEqual(text.utf8))
    }

    func testLateOldTurnPromptsAndInterruptReceiptsDoNotAlterANewInput() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "first"; await model.send(); fixture.result()
        try await until { model.phase == .ready }
        fixture.inputID = "second-input"; model.draft = "second"; await model.send()
        fixture.prompt(turn: "input-one")
        fixture.session(.fields([("kind", .string("interrupt-receipt")), ("messageID", .string("input-one")), ("receipt", .object([]))]))
        try await until { model.entries.contains { $0.title == "先前输入的中断回执" } }
        XCTAssertTrue(model.phase == .running && model.activeMessageID == "second-input" && model.pending.isEmpty,
                      "Late old-turn observations must not change a new input or offer approval")
    }

    func testKnownRejectionPreservesDraftWithoutAutomaticRetry() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        XCTAssertTrue(model.records.isEmpty, "Empty batches must not displace real conversation records")
        fixture.rejectSend = true; model.draft = "try explicitly"; await model.send()
        XCTAssertTrue(model.phase == .ready && model.draft == "try explicitly" && model.issue != nil &&
            fixture.calls.filter { $0.method == "send" }.count == 1, "Known rejection must stay distinct from unknown or successful input")
    }

    func testSequenceGapsAndBoundedHistoryRemainVisible() async throws {
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        for _ in 0..<70 { fixture.message(.fields([("type", .string("system")), ("fixture", .string(String(repeating: "x", count: 40_000)))])) }
        for _ in 0..<30 { await model.pollOnce() }
        try await until { fixture.events.isEmpty }
        XCTAssertTrue(model.records.reduce(0, { $0 + $1.json.utf8.count }) <= 2_097_152 &&
            model.entries.reduce(0, { $0 + $1.text.utf8.count }) <= 2_097_152,
            "Native retained history must enforce both byte budgets")
        fixture.eventSequence += 1; fixture.message(.fields([("type", .string("system")), ("probe", .string("sequence-gap"))]))
        try await until { model.records.contains { $0.json.contains("sequence-gap") } }
        XCTAssertTrue(model.phase == .unknown && !model.canSend, "Missing ordered SDK events must not pretend the conversation is current")
    }

    func testReferencesUseExactBytesAndQuestionsNeverAcceptGenericAllow() async throws {
        let first = ClaudeConversation.Reference(scope: "fixture", requestID: "é", token: "same-token")
        let second = ClaudeConversation.Reference(scope: "fixture", requestID: "e\u{301}", token: "same-token")
        XCTAssertNotEqual(first, second, "Opaque approval identities must compare exact UTF8")
        let (model, fixture) = try await ready(); defer { model.invalidate(); fixture.release() }
        model.draft = "fixture"; await model.send(); fixture.prompt(questions: [ClaudeChatFixture.question("Choose")])
        try await until { model.pending.count == 1 }
        await model.respond(model.pending[0].reference, allow: true)
        XCTAssertFalse(fixture.calls.contains { $0.method == "approve" }, "A question must never accept ordinary allow")
    }

    func testOwnerChangeDuringSendRejectsTheLateReply() async throws {
        let fixture = ClaudeChatFixture(); var current = true
        let model = ClaudeConversation(request: { try await fixture.request($0, $1) }, isCurrent: { current })
        defer { model.invalidate(); fixture.release() }
        await model.start(); try await until { model.phase == .ready }
        model.draft = "preserve owner draft"; fixture.pauseSend = true
        let sending = Task { await model.send() }; try await until { fixture.sendBarrier != nil }
        current = false; fixture.sendBarrier?.resume(); fixture.sendBarrier = nil
        await sending.value
        XCTAssertTrue(model.phase == .unknown && model.draft == "preserve owner draft" && model.activeMessageID == nil,
                      "Owner must still be current after an asynchronous reply")
    }

    func testStartupFailureIsVisibleWithoutAnActiveInput() async throws {
        for event in [false, true] {
            let fixture = ClaudeChatFixture(), model = ClaudeConversation(request: { try await fixture.request($0, $1) })
            defer { model.invalidate(); fixture.release() }
            if event { fixture.session(.fields([("kind", .string("closed")), ("reason", .string("failed")), ("pendingMessageID", .null)])) }
            else { fixture.state = "failed" }
            await model.start(); try await until { model.phase == .failed }
            XCTAssertTrue(model.issue != nil && !model.canStart && !model.canSend, "Startup failure must be visible without pretending ready")
        }
    }
}
