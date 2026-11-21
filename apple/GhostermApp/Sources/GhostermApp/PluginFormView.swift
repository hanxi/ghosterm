import GhostermAppModel
import SwiftUI

/// Inline, scoped to the active plugin run. The caller supplies a view identity
/// bound to the native ticket so drafts cannot carry over between requests.
struct PluginFormView: View {
    let request: PluginFormRequest
    let pluginName: String
    let answering: Bool
    let answer: (PluginFormRequest.Answer) -> Void
    @State private var values: [String: PluginFormRequest.Value] = [:]

    private var submission: PluginFormRequest.Answer {
        var supplied = values
        for field in request.fields where supplied[field.id] == nil {
            switch field.kind {
            case .text: supplied[field.id] = .text("")
            case .toggle: supplied[field.id] = .toggle(false)
            case .choice: break
            }
        }
        return .submitted(supplied)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("插件 \(pluginName) 请求输入").font(.caption)
            Text(request.title).font(.headline).accessibilityAddTraits(.isHeader)
            ForEach(request.fields) { field in
                switch field.kind {
                case .text(let required, _):
                    TextField(required ? "\(field.label)（必填）" : field.label, text: text(field.id))
                case .toggle:
                    Toggle(field.label, isOn: toggle(field.id))
                case .choice(let options):
                    Picker(field.label, selection: text(field.id)) {
                        Text("请选择").tag("")
                        ForEach(options) { Text($0.label).tag($0.id) }
                    }
                }
            }
            ViewThatFits(in: .horizontal) {
                HStack { actions }
                VStack(alignment: .leading) { actions }
            }
        }
        .disabled(answering)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("plugin-form")
    }

    @ViewBuilder private var actions: some View {
        Button("提交") { answer(submission) }
            .disabled((try? request.outcome(submission)) == nil)
            .accessibilityIdentifier("plugin-form-submit")
        Button("取消表单") { answer(.dismissed) }
            .accessibilityIdentifier("plugin-form-dismiss")
    }

    private func text(_ id: String) -> Binding<String> {
        Binding(get: {
            if case .text(let text) = values[id] { return text }
            return ""
        }, set: { values[id] = .text($0) })
    }

    private func toggle(_ id: String) -> Binding<Bool> {
        Binding(get: {
            if case .toggle(let enabled) = values[id] { return enabled }
            return false
        }, set: { values[id] = .toggle($0) })
    }
}
