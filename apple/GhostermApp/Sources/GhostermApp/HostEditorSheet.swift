// SSH parsing and ownership come from Core; the form emits only dirty patches.
import Foundation
import GhostermAppModel
import SwiftUI

struct HostEditorSheet: View {
    @ObservedObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    let alias: String
    let isNew: Bool
    @State private var draft: HostEditorDraft?
    @State private var errorText: String?
    @State private var loaded = false
    @State private var importing = false
    @State private var importNotice: String?
    private struct PendingSave {
        let replacement: String
        let edits: [(String, [String]?)]
        let expected: Data?
        let preview: String
    }
    @State private var pendingSave: PendingSave?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(isNew ? "新建 Host" : "编辑 \(alias)").font(DS.Font.titleLarge)
            if let draft = Binding($draft) {
                ScrollView {
                    VStack(alignment: .leading, spacing: 14) {
                        TextField("别名（Host 模式）", text: draft.alias)
                            .textFieldStyle(.roundedBorder).accessibilityIdentifier("host-editor-alias")
                        if let notice = draft.wrappedValue.notice { Text(notice).font(DS.Font.callout).foregroundStyle(.secondary) }
                        if let reason = draft.wrappedValue.readOnlyReason { Text(reason).foregroundStyle(.orange) }
                        field(draft.hostname)
                        field(draft.user)
                        field(draft.port)
                        field(draft.identities, multiline: true)
                        let references = Array(draft.wrappedValue.identities.value.components(separatedBy: "\n").filter { !$0.isEmpty }.prefix(64))
                        Group {
                            if references.count == 1 {
                                Button("从文件更新钥匙串私钥…") { reimport(references[0]) }
                            } else {
                                Menu("从文件更新钥匙串私钥…") {
                                    ForEach(Array(references.enumerated()), id: \.offset) { index, reference in
                                        Button(reference) { reimport(reference) }
                                            .accessibilityIdentifier("host-editor-import-key-\(index)")
                                    }
                                }
                            }
                        }
                        .disabled(importing || references.isEmpty || !draft.wrappedValue.identities.enabled)
                        .accessibilityIdentifier("host-editor-import-key")
                        Text("首次使用会将文件导入钥匙串，之后从钥匙串认证。导入独立保存，取消登录或表单不删除；来源文件仍由你管理。此动作主动更新已存材料，一次最多显示前 64 条引用。")
                            .font(DS.Font.callout).foregroundStyle(.secondary)
                        if importing { ProgressView("正在更新钥匙串…").controlSize(.small) }
                        if let importNotice { Text(importNotice).font(DS.Font.callout).foregroundStyle(.secondary).accessibilityIdentifier("host-editor-import-result") }
                        if !draft.wrappedValue.extras.isEmpty {
                            Text("其他指令").font(DS.Font.title)
                            ForEach(draft.extras) { extra in
                                field(extra.field, multiline: extra.wrappedValue.perLine)
                                    .disabled(extra.wrappedValue.readOnlyReason != nil)
                                if let reason = extra.wrappedValue.readOnlyReason { Text(reason).font(DS.Font.callout).foregroundStyle(.secondary) }
                            }
                        }
                    }
                    .disabled(draft.wrappedValue.readOnlyReason != nil || pendingSave != nil || importing)
                }
                .frame(maxHeight: 480)
            }
            if let pendingSave {
                Text(pendingSave.preview).font(DS.Font.callout).textSelection(.enabled)
                HStack {
                    Button("返回编辑") { self.pendingSave = nil }.accessibilityIdentifier("host-editor-back-to-edit")
                    Spacer()
                    Button("确认改名并保存") { commit(pendingSave) }.accessibilityIdentifier("host-editor-confirm-save")
                }
            }
            if let errorText { Text(errorText).foregroundStyle(.red).font(DS.Font.callout).textSelection(.enabled).accessibilityIdentifier("host-editor-error") }
            HStack {
                Spacer()
                Button("取消") { dismiss() }.disabled(importing).accessibilityIdentifier("host-editor-cancel")
                Button("保存") { save() }
                    .disabled(draft == nil || draft?.readOnlyReason != nil || pendingSave != nil || importing)
                    .keyboardShortcut(.defaultAction).accessibilityIdentifier("host-editor-save")
            }
        }
        .padding(20).frame(width: 550)
        .interactiveDismissDisabled(importing)
        .onAppear {
            guard !loaded else { return }
            loaded = true
            do { draft = try HostEditorDraft(model: model.configModel, alias: alias, isNew: isNew) }
            catch { errorText = "\(error)" }
        }
    }

    private func field(_ value: Binding<HostEditorDraft.Field>, multiline: Bool = false) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Toggle("\(value.wrappedValue.keyword)：本块显式设置", isOn: value.enabled)
                .accessibilityIdentifier("host-editor-\(value.wrappedValue.keyword)-enabled")
            if multiline {
                TextEditor(text: value.value).font(.system(.body, design: .monospaced))
                    .frame(minHeight: 60, maxHeight: 110).border(.secondary.opacity(0.3))
                    .disabled(!value.wrappedValue.enabled).accessibilityIdentifier("host-editor-\(value.wrappedValue.keyword)")
            } else {
                TextField(value.wrappedValue.keyword, text: value.value).textFieldStyle(.roundedBorder)
                    .disabled(!value.wrappedValue.enabled).accessibilityIdentifier("host-editor-\(value.wrappedValue.keyword)")
            }
            Text(value.wrappedValue.source).font(DS.Font.callout).foregroundStyle(.secondary).textSelection(.enabled)
        }
    }

    private func reimport(_ reference: String) {
        guard !importing else { return }
        importing = true; errorText = nil; importNotice = nil
        Task { @MainActor in
            do {
                try await model.reimportPrivateKey(reference)
                importNotice = "钥匙串已更新；来源文件未改写。"
            } catch { errorText = "\(error)" }
            importing = false
        }
    }

    private func commit(_ request: PendingSave) {
        do {
            try model.saveHost(alias: alias, replacement: request.replacement, edits: request.edits, expectedConfiguration: request.expected)
            pendingSave = nil
            dismiss()
        } catch { pendingSave = nil; errorText = "\(error)" }
    }

    private func save() {
        guard let draft else { return }
        do {
            let edits = try draft.edits()
            if isNew { try model.addHost(alias: draft.alias, edits, expectedConfiguration: draft.originalConfiguration) }
            else if Data(alias.utf8) != Data(draft.alias.utf8) {
                let preview = try model.previewHostRename(alias: alias, replacement: draft.alias, edits: edits, expectedConfiguration: draft.originalConfiguration)
                pendingSave = PendingSave(replacement: draft.alias, edits: edits, expected: draft.originalConfiguration, preview: preview)
                errorText = nil
                return
            } else { try model.saveHost(alias: alias, replacement: draft.alias, edits: edits, expectedConfiguration: draft.originalConfiguration) }
            dismiss()
        } catch { errorText = "\(error)" }
    }
}
