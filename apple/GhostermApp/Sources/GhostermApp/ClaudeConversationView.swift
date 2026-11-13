import GhostermAppModel
import SwiftUI

struct ClaudeConversationView: View {
    @ObservedObject var model: ClaudeConversation
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Text("Claude 对话").font(DS.Font.labelEmphasis)
            Text(phaseLabel).font(DS.Font.caption).foregroundStyle(.secondary)
                .accessibilityIdentifier("plugin-claude-phase")
            ForEach(model.entries) { entry in
                VStack(alignment: .leading, spacing: DS.Padding.xs) {
                    Text(entry.title).font(DS.Font.labelEmphasis)
                    Text(String(entry.text.unicodeScalars.prefix(8192))).lineLimit(8).textSelection(.enabled)
                    if !entry.text.isEmpty {
                        DisclosureGroup("查看本条完整内容") {
                            ConversationReadOnlyText(text: entry.text, identifier: "plugin-claude-entry-\(entry.id)", label: "本条完整内容")
                                .frame(height: 200)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading).padding(DS.Padding.s)
                .background(.background.opacity(0.55), in: RoundedRectangle(cornerRadius: DS.Radius.control))
            }
            ForEach(model.pending) { prompt in
                ClaudePromptView(model: model, prompt: prompt).id(prompt.id)
            }
            if model.omittedEntries > 0 {
                Text("较早的 \(model.omittedEntries) 条对话预览已超出本次内存保留范围。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
            if model.phase == .new {
                Button(model.busy ? "正在启动…" : "启动 Claude 对话") { Task { await model.start() } }
                    .disabled(!model.canStart).accessibilityIdentifier("plugin-claude-start")
            } else {
                ConversationComposer(placeholder: "发送给 Claude", draft: $model.draft, busy: model.busy,
                    canSend: model.canSend, prefix: "plugin-claude") { Task { await model.send() } }
                HStack {
                    Button("中断当前输入") { Task { await model.interrupt() } }
                        .disabled(!model.canInterrupt).accessibilityIdentifier("plugin-claude-interrupt")
                    Button("停止 Claude 会话") { Task { await model.stop() } }
                        .disabled(!model.canStop).accessibilityIdentifier("plugin-claude-stop")
                }
            }
            if let issue = model.issue { Text(issue).foregroundStyle(.red).textSelection(.enabled) }
            if !model.records.isEmpty {
                DisclosureGroup("原始 Claude 记录（\(model.records.count)）") {
                    ForEach(model.records) { record in
                        ConversationReadOnlyText(text: record.json, identifier: "plugin-claude-record-\(record.id)", label: "完整 Claude 记录")
                            .frame(height: 160)
                    }
                }
            }
            if model.omittedRecords > 0 {
                Text("较早的 \(model.omittedRecords) 条原始记录已超出本次内存保留范围。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }.frame(maxWidth: .infinity, alignment: .leading)
    }
    private var phaseLabel: String {
        switch model.phase {
        case .new: return "尚未启动"
        case .starting: return "正在初始化后端"
        case .ready: return "后端已就绪；认证与模型调用结果需由实际请求确认"
        case .running: return "正在处理当前输入"
        case .interrupting: return "已请求中断，等待后端最终结果"
        case .closed: return "后端会话已关闭"
        case .failed: return "后端会话失败"
        case .lost: return "后端连接已丢失，未确认结果保持未知"
        case .unknown: return "无法确认当前会话状态"
        }
    }
}

private struct ClaudePromptView: View {
    @ObservedObject var model: ClaudeConversation
    let prompt: ClaudeConversation.Prompt
    @State private var drafts: [AnswerDraft] = []
    struct AnswerDraft { var selected: Set<Int> = []; var freeText = false; var text = "" }
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Text(prompt.isQuestion ? "Claude 需要你的回答" : "Claude 请求执行工具：\(prompt.toolName)")
                .font(DS.Font.labelEmphasis)
            if prompt.isQuestion {
                ForEach(Array(prompt.questions.enumerated()), id: \.offset) { index, question in
                    questionView(question, index: index)
                }
            } else {
                ConversationReadOnlyText(text: prompt.inputJSON, identifier: "plugin-claude-tool-input", label: "本次工具请求的完整参数")
                    .frame(height: 160)
            }
            DisclosureGroup("完整请求信息") {
                ConversationReadOnlyText(text: prompt.metadataJSON, identifier: "plugin-claude-prompt-metadata", label: "完整 Claude 审批请求")
                    .frame(height: 160)
            }
            ConversationDecisionButtons(prefix: "plugin-claude-approval", busy: !model.canRespond(prompt.reference),
                canAllow: !prompt.isQuestion || answers != nil, allowLabel: prompt.isQuestion ? "提交回答" : "批准一次",
                deny: { Task { await model.respond(prompt.reference, allow: false) } },
                allow: { Task {
                    if prompt.isQuestion, let answers { await model.answer(prompt.reference, answers: answers) }
                    else if !prompt.isQuestion { await model.respond(prompt.reference, allow: true) }
                } })
        }
        .padding(DS.Padding.s)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: DS.Radius.control))
        .onAppear { drafts = prompt.questions.map { _ in AnswerDraft() } }
    }
    @ViewBuilder private func questionView(_ question: ClaudeConversation.Question, index: Int) -> some View {
        if drafts.indices.contains(index) {
            Text(question.header).font(DS.Font.labelEmphasis)
            Text(question.question).textSelection(.enabled)
            ForEach(Array(question.options.enumerated()), id: \.offset) { optionIndex, option in
                Toggle(isOn: Binding(get: { drafts[index].selected.contains(optionIndex) }, set: { selected in
                    if !question.multiSelect { drafts[index].selected = [] }
                    if selected { drafts[index].selected.insert(optionIndex) } else { drafts[index].selected.remove(optionIndex) }
                })) {
                    VStack(alignment: .leading) { Text(option.label); Text(option.description).font(DS.Font.caption).foregroundStyle(.secondary) }
                }
                .disabled(drafts[index].freeText || !model.canRespond(prompt.reference))
                .accessibilityIdentifier("plugin-claude-question-\(index)-option-\(optionIndex)")
            }
            Toggle("填写自己的回答", isOn: $drafts[index].freeText).disabled(!model.canRespond(prompt.reference))
                .accessibilityIdentifier("plugin-claude-question-\(index)-free")
            if drafts[index].freeText {
                TextField("你的回答", text: $drafts[index].text, axis: .vertical).lineLimit(1...4)
                    .disabled(!model.canRespond(prompt.reference))
                    .accessibilityIdentifier("plugin-claude-question-\(index)-text")
            }
        }
    }
    private var answers: [String]? {
        guard drafts.count == prompt.questions.count else { return nil }
        var result: [String] = []
        for (index, question) in prompt.questions.enumerated() {
            let draft = drafts[index]
            let value = draft.freeText ? draft.text : question.options.enumerated()
                .filter { draft.selected.contains($0.offset) }.map { $0.element.label }.joined(separator: ", ")
            guard !value.isEmpty, value.utf8.count <= 2048 else { return nil }
            result.append(value)
        }
        return result
    }
}
