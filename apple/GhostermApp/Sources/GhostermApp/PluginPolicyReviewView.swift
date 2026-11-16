import GhostermAppModel
import SwiftUI

struct PluginPolicyReviewView: View {
    @ObservedObject var draft: PluginInstalledVersionsDraft
    let controller: ExtensionController?
    let didDisable: (ExtensionController.PolicyDisableResult) -> Void
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Button("按此策略复核全部已启用选择") {
                if let controller { Task { await draft.reviewPolicy(controller) } }
            }.disabled(!draft.canSelect).accessibilityIdentifier("plugin-policy-review-open")
            if let review = draft.policyReview {
                Text("策略渠道：" + review.channel + " · 只读快照，尚未更改选择")
                Text(review.policySHA256).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                Text("这是你选择的策略快照，无联网新鲜度或全局撤销保证。其它渠道也可能不被此策略接受。读取失败保留‘查不出来’，不当作已撤销。")
                if review.entries.isEmpty { Text("没有已启用选择需要复核。") }
                ForEach(review.entries) { entry in
                    VStack(alignment: .leading) {
                        Text(entry.pluginID + " · " + label(entry.state)).accessibilityIdentifier("plugin-policy-state-" + entry.id)
                        Text(entry.releaseID).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                        Text(entry.reason).font(DS.Font.caption)
                        if entry.state != .accepted {
                            Toggle("我选择禁用此插件的新建", isOn: Binding(
                                get: { draft.policyDisableIDs.contains(entry.id) },
                                set: { if $0 { draft.policyDisableIDs.insert(entry.id) } else { draft.policyDisableIDs.remove(entry.id) } }))
                                .disabled(draft.busy).accessibilityIdentifier("plugin-policy-select-" + entry.id)
                        }
                    }
                }
                Text("确认只禁用勾选的同一活动选择；不会授予权限、删除包或停止现有运行。确认前再次核对策略文件和选择记录。")
                HStack {
                    Button("确认禁用以上勾选项") {
                        if let controller { Task { if let result = await draft.applyPolicy(controller) { didDisable(result) } } }
                    }.disabled(!draft.canSelect || draft.policyDisableIDs.isEmpty).accessibilityIdentifier("plugin-policy-review-apply")
                    Button("保留选择并取消复核") {
                        if let controller { Task { await draft.cancelPolicy(controller) } }
                    }.disabled(draft.busy).accessibilityIdentifier("plugin-policy-review-cancel")
                }
            }
        }
    }
    private func label(_ state: ExtensionController.PolicyReview.State) -> String {
        switch state {
        case .accepted: return "此策略接受"
        case .rejected: return "此策略拒绝"
        case .unknown: return "查不出来"
        }
    }
}
