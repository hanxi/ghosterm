import Combine
import Foundation

@MainActor
public final class ClaudeConversation: ObservableObject {
    public enum Phase: String { case new, starting, ready, running, interrupting, closed, failed, lost, unknown }
    public struct Reference: Equatable {
        public let scope: String; public let requestID: String; public let token: String
        var json: ClaudeJSON { .fields([("scope", .string(scope)), ("requestID", .string(requestID)), ("token", .string(token))]) }
        public var id: String { Data(json.encoded().utf8).base64EncodedString() }
        public static func == (a: Self, b: Self) -> Bool {
            a.scope.utf8.elementsEqual(b.scope.utf8) && a.requestID.utf8.elementsEqual(b.requestID.utf8) && a.token.utf8.elementsEqual(b.token.utf8)
        }
    }
    public struct Question {
        public struct Option { public let label: String; public let description: String }
        public let question: String; public let header: String; public let multiSelect: Bool; public let options: [Option]
    }
    public struct Prompt: Identifiable {
        public let reference: Reference; public let toolName: String; public let turnID: String
        public let inputJSON: String; public let metadataJSON: String; public let questions: [Question]
        public var id: String { reference.id }
        public var isQuestion: Bool { toolName == "AskUserQuestion" }
    }
    public struct Entry: Identifiable {
        public let id: String; public let title: String; public let text: String; public let shortened: Bool
        let turnID: String?
    }
    public struct Record: Identifiable { public let id: Int; public let json: String }
    @Published public private(set) var phase: Phase = .new
    @Published public private(set) var busy = false
    @Published public var draft = ""
    @Published public private(set) var activeMessageID: String?
    @Published public private(set) var pending: [Prompt] = []
    @Published public private(set) var entries: [Entry] = []
    @Published public private(set) var records: [Record] = []
    @Published public private(set) var omittedRecords = 0
    @Published public private(set) var omittedEntries = 0
    @Published public private(set) var issue: String?
    @Published public private(set) var observation: ClaudeRPCClient.Observation?
    @Published public private(set) var observationIssue: String?
    private let observe: (@MainActor () async throws -> ClaudeRPCClient.Observation)?
    private var detached = false
    private let request: ClaudeRPCClient.RPC
    private let isCurrent: @MainActor () -> Bool
    private var valid = true
    private var generation = UUID()
    private var transferring = false
    private var sessionSequence = 0
    private var recordSequence = 0
    private var polling: Task<Void, Never>?
    private var finishedInputs: [Data] = []

    public init(request: @escaping ClaudeRPCClient.RPC, observe: (@MainActor () async throws -> ClaudeRPCClient.Observation)? = nil, isCurrent: @escaping @MainActor () -> Bool = { true }) {
        self.request = request; self.observe = observe; self.isCurrent = isCurrent
    }
    public var canObserve: Bool { !detached && isCurrent() && phase == .unknown && !busy && observe != nil }
    /// This does not adopt a recovered send ID, resume polling or enable actions.
    public func observeOutcome() async {
        guard canObserve, let observe else { return }
        let expected = generation; busy = true; observationIssue = nil
        defer { if expected == generation { busy = false } }
        do {
            let result = try await observe()
            try Task.checkCancellation()
            guard !detached, expected == generation, isCurrent() else { throw CancellationError() }
            observation = result
        } catch {
            if !detached && expected == generation && isCurrent() {
                observationIssue = "回包观察未能确认：\(error)"
            }
        }
    }
    public var canStart: Bool { valid && isCurrent() && phase == .new && !busy }
    public var canSend: Bool { valid && isCurrent() && phase == .ready && activeMessageID == nil && !busy }
    public var canInterrupt: Bool { valid && isCurrent() && phase == .running && activeMessageID != nil && !busy }
    public var canStop: Bool { valid && isCurrent() && ![.new, .closed, .failed, .lost, .unknown].contains(phase) && !busy }
    public func canRespond(_ reference: Reference) -> Bool {
        valid && isCurrent() && !busy && phase == .running && pending.contains { $0.reference == reference }
    }

    public func start() async {
        guard canStart else { return }
        await action {
            guard self.phase == .new else { throw InvalidAction() }
            let reply = try await self.call("start", .object([]))
            guard reply["requested"]?.boolean == true,
                  reply["backend"]?.text == "cli",
                  reply["cliVersion"]?.text == "2.1.323" else { throw InvalidReply() }
            self.phase = .starting
        }
        if phase == .starting { beginPolling() }
    }
    public func send() async {
        guard canSend, !draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        let text = draft
        await action {
            guard self.phase == .ready && self.activeMessageID == nil else { throw InvalidAction() }
            let reply = try await self.call("send", .fields([("text", .string(text))]))
            guard let id = reply["messageID"]?.text, Self.identity(id) else { throw InvalidReply() }
            self.activeMessageID = id; self.phase = .running
            self.append(id: "input:\(id)", title: "你 · 已提交", text: text, turn: id)
            if self.draft.utf8.elementsEqual(text.utf8) { self.draft = "" }
        }
    }
    public func interrupt() async {
        guard canInterrupt else { return }
        let id = activeMessageID
        await action {
            guard self.activeMessageID == id && self.phase == .running else { throw InvalidAction() }
            let reply = try await self.call("interrupt", .object([]))
            guard reply["requested"]?.boolean == true else { throw InvalidReply() }
            self.phase = .interrupting; self.pending = []
        }
    }
    public func respond(_ reference: Reference, allow: Bool) async {
        guard canRespond(reference), let prompt = pending.first(where: { $0.reference == reference }),
              !allow || !prompt.isQuestion else { return }
        await action {
            try self.checkPrompt(reference)
            let reply = try await self.call("approve", .fields([("reference", reference.json), ("allow", .bool(allow))]))
            guard let accepted = reply["accepted"]?.boolean else { throw InvalidReply() }
            self.pending.removeAll { $0.reference == reference }
            if !accepted { self.issue = "此请求已失效，后端未接受决定。" }
        }
    }
    public func answer(_ reference: Reference, answers: [String]) async {
        guard canRespond(reference), let prompt = pending.first(where: { $0.reference == reference }), prompt.isQuestion,
              answers.count == prompt.questions.count,
              answers.allSatisfy({ !$0.isEmpty && $0.utf8.count <= 2048 }) else { return }
        let values = zip(prompt.questions, answers).map { ($0.0.question, ClaudeJSON.string($0.1)) }
        await action {
            try self.checkPrompt(reference)
            let reply = try await self.call("answer", .fields([("reference", reference.json), ("answers", .fields(values))]))
            guard let accepted = reply["accepted"]?.boolean else { throw InvalidReply() }
            self.pending.removeAll { $0.reference == reference }
            if !accepted { self.issue = "此问题已失效，后端未接受回答。" }
        }
    }
    public func stop() async {
        guard canStop else { return }
        await action {
            let reply = try await self.call("stop", .object([]))
            guard let name = reply["phase"]?.text, ["closed", "failed", "lost"].contains(name),
                  let phase = Phase(rawValue: name) else { throw InvalidReply() }
            self.phase = phase; self.pending = []
            if self.activeMessageID != nil { self.issue = "会话已停止；最后一条输入的结果尚未确认。" }
            self.valid = false; self.polling?.cancel(); self.polling = nil
        }
    }
    /// A detached/replaced Runtime cannot keep sending or accept late replies.
    public func invalidate() {
        generation = UUID(); detached = true; valid = false; polling?.cancel(); polling = nil; busy = false; pending = []
        if ![.closed, .failed, .lost].contains(phase) { phase = .unknown; issue = "连接观察已结束；未确认的结果保持未知。" }
    }

    private struct InvalidReply: Error {}
    private struct InvalidAction: Error {}
    private struct Rejected: Error {}
    private struct TooLarge: Error {}
    private func check(_ expected: UUID) throws {
        try Task.checkCancellation()
        guard valid, expected == generation, isCurrent() else { throw CancellationError() }
    }
    private func checkPrompt(_ reference: Reference) throws {
        guard phase == .running, let prompt = pending.first(where: { $0.reference == reference }),
              activeMessageID?.utf8.elementsEqual(prompt.turnID.utf8) == true else { throw InvalidAction() }
    }
    private func action(_ body: () async throws -> Void) async {
        guard valid, isCurrent(), !busy else { return }
        let expected = generation; busy = true; issue = nil
        defer { if expected == generation { busy = false } }
        do {
            while transferring { try check(expected); try await Task.sleep(nanoseconds: 5_000_000) }
            try check(expected); transferring = true
            defer { transferring = false }
            try await body()
        } catch is Rejected { if expected == generation { issue = "后端未接受此操作；请核对当前会话状态。" } }
        catch is InvalidAction { if expected == generation { issue = "当前输入或问题已改变，未发送此操作。" } }
        catch is TooLarge { if expected == generation { issue = "参数超过 64,000 字节上限，未发送此操作。" } }
        catch { if expected == generation { fail("操作未能确认：\(error)") } }
    }
    private func call(_ method: String, _ parameters: ClaudeJSON) async throws -> ClaudeJSON {
        let expected = generation; try check(expected)
        let bytes = Data(parameters.encoded().utf8)
        guard bytes.count <= 64_000 else { throw TooLarge() }
        let raw = try await request(method, bytes)
        try check(expected)
        let response = try ClaudeJSON.parse(raw)
        guard let ok = response["ok"]?.boolean else { throw InvalidReply() }
        if !ok {
            guard response["error"]?.text == "ACTION_REJECTED" else { throw InvalidReply() }
            throw Rejected()
        }
        guard let result = response["result"] else { throw InvalidReply() }
        if method == "poll", result["source"]?.text != "none",
           result["source"]?.text != "batch" || result["events"]?.elements?.isEmpty == false { retain(raw) }
        return result
    }
    private func beginPolling() {
        guard polling == nil else { return }
        polling = Task { [weak self] in
            while !Task.isCancelled {
                guard let self, self.valid else { return }
                await self.pollOnce()
                do { try await Task.sleep(nanoseconds: 100_000_000) } catch { return }
            }
        }
    }
    func pollOnce() async {
        guard valid, !busy, !transferring else { return }
        let expected = generation; transferring = true
        defer { transferring = false }
        do {
            let result = try await call("poll", .fields([("maxEvents", .number("32"))]))
            if result["source"]?.text == "batch" {
                guard let events = result["events"]?.elements, events.count <= 32 else { throw InvalidReply() }
                if events.isEmpty { try consume(.fields([("source", .string("none")), ("phase", result["phase"] ?? .null)])) }
                for event in events { guard valid else { break }; try consume(event) }
            } else { try consume(result) }
        }
        catch { if expected == generation && !Task.isCancelled { fail("后端观察中断：\(error)") } }
    }
    private func fail(_ message: String, phase: Phase = .unknown) {
        self.phase = phase; issue = message; valid = false; pending = []; polling?.cancel(); polling = nil
    }
    private func retain(_ bytes: Data) {
        recordSequence += 1; records.append(Record(id: recordSequence, json: String(decoding: bytes, as: UTF8.self)))
        while records.count > 128 || records.reduce(0, { $0 + $1.json.utf8.count }) > 2_097_152 {
            records.removeFirst(); omittedRecords += 1
        }
    }
    private func append(id: String, title: String, text: String, turn: String?) {
        guard text.utf8.count <= 600_000 else { fail("单条消息超出本机保留上限；当前输入结果保持未知。"); return }
        let shortened = text.unicodeScalars.count > 8192 || text.utf8.filter({ $0 == 10 }).count >= 8
        entries.removeAll { $0.id == id }
        entries.append(Entry(id: id, title: title, text: text, shortened: shortened, turnID: turn))
        while entries.count > 128 || entries.reduce(0, { $0 + $1.text.utf8.count }) > 2_097_152 { entries.removeFirst(); omittedEntries += 1 }
    }
    private static func identity(_ text: String) -> Bool { !text.isEmpty && text.utf8.count <= 128 }

    private func consume(_ result: ClaudeJSON) throws {
        switch result["source"]?.text {
        case "none":
            guard let name = result["phase"]?.text, let next = Phase(rawValue: name), next != .new else { throw InvalidReply() }
            if next == .ready && activeMessageID != nil { throw InvalidReply() }
            phase = next
            if [.closed, .failed, .lost].contains(next) {
                if activeMessageID != nil { issue = "后端会话已结束；当前输入的结果尚未确认。" }
                else if next != .closed { issue = "后端初始化或连接失败；请检查执行端配置和依赖。" }
                pending = []; valid = false; polling?.cancel(); polling = nil
            }
        case "approval":
            guard let event = result["event"], let kind = event["kind"]?.text else { throw InvalidReply() }
            if kind == "requested" {
                guard let value = event["request"] else { throw InvalidReply() }
                let prompt = try Self.prompt(value)
                if finishedInputs.contains(Data(prompt.turnID.utf8)) { return }
                if phase == .interrupting && activeMessageID?.utf8.elementsEqual(prompt.turnID.utf8) == true { return }
                guard phase == .running, activeMessageID?.utf8.elementsEqual(prompt.turnID.utf8) == true else { throw InvalidReply() }
                if let old = pending.first(where: { $0.reference == prompt.reference }) {
                    guard old.metadataJSON.utf8.elementsEqual(prompt.metadataJSON.utf8) else { throw InvalidReply() }
                } else {
                    guard pending.count < 32 else { throw InvalidReply() }
                    pending.append(prompt)
                }
            } else if kind == "resolved" || kind == "expired" {
                let ref = try Self.reference(event["reference"])
                pending.removeAll { $0.reference == ref }
            } else if kind == "closed" { pending = [] }
            else { throw InvalidReply() }
        case "session":
            guard let event = result["event"], event["sequence"]?.integer == sessionSequence + 1,
                  let kind = event["kind"]?.text else { throw InvalidReply() }
            sessionSequence += 1
            if kind == "sdk-message", let message = event["message"], let type = message["type"]?.text {
                try consumeMessage(message, type: type)
            } else if kind == "interrupt-receipt" {
                guard let id = event["messageID"]?.text else { throw InvalidReply() }
                let current = activeMessageID?.utf8.elementsEqual(id.utf8) == true
                guard current || finishedInputs.contains(Data(id.utf8)) else { throw InvalidReply() }
                append(id: "receipt:\(sessionSequence)", title: current ? "已收到中断回执 · 等待最终结果" : "先前输入的中断回执",
                    text: event["receipt"]?.encoded() ?? "", turn: id)
            } else if kind == "closed" {
                guard let reason = event["reason"]?.text, ["stopped", "failed", "lost"].contains(reason) else { throw InvalidReply() }
                phase = reason == "stopped" ? .closed : reason == "failed" ? .failed : .lost
                if activeMessageID != nil { issue = "后端会话已结束；当前输入的结果尚未确认。" }
                else if reason != "stopped" { issue = "后端初始化或连接失败；请检查执行端配置和依赖。" }
                pending = []; valid = false; polling?.cancel(); polling = nil
            } else { throw InvalidReply() }
        default: throw InvalidReply()
        }
    }

    private func consumeMessage(_ message: ClaudeJSON, type: String) throws {
        if type == "result" {
            guard let id = message["user_message_uuid"]?.text, activeMessageID?.utf8.elementsEqual(id.utf8) == true,
                  let error = message["is_error"]?.boolean, let subtype = message["subtype"]?.text,
                  error || subtype == "success" else {
                fail("收到无法关联当前输入的后端结果；当前输入保持未知。", phase: .lost); return
            }
            let detail = message["result"]?.text ?? message["errors"]?.encoded() ?? message.encoded()
            append(id: "result:\(sessionSequence)", title: error ? "本轮未成功" : "本轮已完成", text: detail, turn: id)
            finishedInputs.append(Data(id.utf8)); if finishedInputs.count > 64 { finishedInputs.removeFirst() }
            activeMessageID = nil; pending = []; phase = .ready
        } else if type == "stream_event", let event = message["event"], event["type"]?.text == "content_block_delta",
                  let index = event["index"]?.integer, index >= 0, index <= 4096,
                  event["delta"]?["type"]?.text == "text_delta", let delta = event["delta"]?["text"]?.text,
                  let turn = activeMessageID {
            let id = "stream:\(turn):\(index)"
            let previous = entries.first { $0.id == id }
            append(id: id, title: "Claude · 正在生成", text: (previous?.text ?? "") + delta, turn: turn)
        } else if type == "assistant" {
            let content = message["message"]?["content"]
            let text = content?.elements?.compactMap { $0["type"]?.text == "text" ? $0["text"]?.text : nil }.joined(separator: "\n")
            if let turn = activeMessageID { entries.removeAll { $0.id.hasPrefix("stream:\(turn):") } }
            append(id: "sdk:\(sessionSequence)", title: "Claude", text: text?.isEmpty == false ? text! : message.encoded(), turn: activeMessageID)
        } else {
            append(id: "sdk:\(sessionSequence)", title: "Claude · \(type)", text: message.encoded(), turn: activeMessageID)
        }
    }

    private static func reference(_ value: ClaudeJSON?) throws -> Reference {
        guard let scope = value?["scope"]?.text, identity(scope),
              let id = value?["requestID"]?.text, identity(id),
              let token = value?["token"]?.text, identity(token) else { throw InvalidReply() }
        return Reference(scope: scope, requestID: id, token: token)
    }
    private static func prompt(_ value: ClaudeJSON) throws -> Prompt {
        let ref = try reference(value["reference"])
        guard let tool = value["toolName"]?.text, identity(tool), let turn = value["turnID"]?.text, identity(turn),
              let input = value["input"], case .object = input,
              let kind = value["kind"]?.text, kind == (tool == "AskUserQuestion" ? "question" : "tool"),
              input.encoded().utf8.count <= 65_536 else { throw InvalidReply() }
        let questions = tool == "AskUserQuestion" ? try Self.questions(value["questions"]) : []
        if tool == "AskUserQuestion" {
            let original = try Self.questions(input["questions"])
            func projection(_ questions: [Question]) -> String {
                var result: [ClaudeJSON] = []
                for q in questions {
                    let options: [ClaudeJSON] = q.options.map { .fields([("label", .string($0.label)), ("description", .string($0.description))]) }
                    result.append(.fields([("question", .string(q.question)), ("header", .string(q.header)),
                        ("multiSelect", .bool(q.multiSelect)), ("options", .array(options))]))
                }
                return ClaudeJSON.array(result).encoded()
            }
            guard projection(original).utf8.elementsEqual(projection(questions).utf8) else { throw InvalidReply() }
        }
        return Prompt(reference: ref, toolName: tool, turnID: turn, inputJSON: input.encoded(), metadataJSON: value.encoded(), questions: questions)
    }
    private static func questions(_ value: ClaudeJSON?) throws -> [Question] {
        guard let values = value?.elements, (1...4).contains(values.count) else { throw InvalidReply() }
        var keys = Set<Data>()
        return try values.map { value in
            guard let text = value["question"]?.text, !text.isEmpty, text.utf8.count <= 2048,
                  keys.insert(Data(text.utf8)).inserted, let header = value["header"]?.text, !header.isEmpty,
                  header.unicodeScalars.count <= 12, header.utf8.count <= 128,
                  let multi = value["multiSelect"]?.boolean, let choices = value["options"]?.elements,
                  (2...4).contains(choices.count) else { throw InvalidReply() }
            var labels = Set<Data>()
            let options = try choices.map { choice -> Question.Option in
                guard let label = choice["label"]?.text, !label.isEmpty, label.utf8.count <= 256,
                      labels.insert(Data(label.utf8)).inserted, let detail = choice["description"]?.text,
                      detail.utf8.count <= 2048 else { throw InvalidReply() }
                return .init(label: label, description: detail)
            }
            return Question(question: text, header: header, multiSelect: multi, options: options)
        }
    }
}
