import GhostermAppModel
import SwiftUI

struct PluginSelectionReviewView: View {
    @ObservedObject var draft: PluginInstalledVersionsDraft
    let controller: ExtensionController?
    let didChange: (String, ExtensionController.VersionSelectionResult) -> Void
    var body: some View {
        if let value = draft.selectionReview {
            VStack(alignment: .leading, spacing: DS.Padding.s) {
                Text("活动选择切换预览").font(DS.Font.caption).accessibilityIdentifier("plugin-selection-review")
                Text("候选：\(value.candidate.review.id) · \(value.candidate.review.version) · \(value.candidate.review.publisher)")
                Text("发布 SHA-256：" + value.candidateReleaseID).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                Text("本次策略：" + value.signature.channel + " · " + value.signature.policySHA256).font(DS.Font.caption).textSelection(.enabled)
                if let previous = value.previous {
                    Text("旧选择：\(previous.review.version) · \(value.previousEnabled == true ? "启用" : "禁用")")
                } else if value.previousState == .none { Text("没有旧活动选择；这是首次选择。") }
                else { Text(value.previousIssue ?? "旧版状态查不出来；无法计算完整声明差异。").foregroundStyle(.orange) }
                Text("SDK：\(value.candidate.sdkVersion) · 协议：\(value.candidate.protocols.map(String.init).joined(separator: ", "))")
                Text("所需能力：" + value.candidate.review.requiredCapabilities.joined(separator: ", "))
                Text("未提供的可选能力：" + value.candidate.review.optionalMissingCapabilities.joined(separator: ", "))
                if let added = value.addedCapabilities, let removed = value.removedCapabilities {
                    Text("所需能力声明新增：" + added.joined(separator: ", ") + " · 移除：" + removed.joined(separator: ", "))
                } else { Text("旧能力声明查不出来，无法比较。").foregroundStyle(.orange) }
                if let added = value.addedPermissions, let removed = value.removedPermissions {
                    Text("新增或修改的权限声明 \(added.count) 项；移除或修改的旧声明 \(removed.count) 项。")
                    ForEach(Array(added.enumerated()), id: \.offset) { _, permission in
                        Text("＋ " + permission.side + " · " + permission.capability + " · " + permission.scope + " · " + permission.purpose + " · " + permission.programs.joined(separator: ", "))
                    }
                    ForEach(Array(removed.enumerated()), id: \.offset) { _, permission in
                        Text("－ " + permission.side + " · " + permission.capability + " · " + permission.scope + " · " + permission.purpose + " · " + permission.programs.joined(separator: ", "))
                    }
                } else { Text("权限差异查不出来；请核对以下完整候选声明。").foregroundStyle(.orange) }
                DisclosureGroup("完整候选权限声明") {
                    ForEach(Array(value.candidate.permissions.enumerated()), id: \.offset) { _, permission in
                        Text(permission.side + " · " + permission.capability + " · " + permission.scope + " · " + permission.purpose)
                        if !permission.programs.isEmpty { Text("声明的程序：" + permission.programs.joined(separator: ", ")) }
                    }
                }
                DisclosureGroup("候选命令描述") {
                    ForEach(value.candidate.review.commands, id: \.path) { command in
                        Text(command.path + " · " + command.title + " · " + command.argsSchema + " · " + command.views.joined(separator: ", "))
                    }
                }
                if let executor = value.candidate.executorDeclarations {
                    DisclosureGroup("完整候选执行端与依赖声明") {
                        Text(executor).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                    }
                }
                if let previous = value.previous {
                    DisclosureGroup("完整旧版声明（由本次策略核验）") {
                        Text("SDK：" + previous.sdkVersion + " · 协议：" + previous.protocols.map(String.init).joined(separator: ", "))
                        Text("所需能力：" + previous.review.requiredCapabilities.joined(separator: ", "))
                        ForEach(Array(previous.permissions.enumerated()), id: \.offset) { _, permission in
                            Text(permission.side + " · " + permission.capability + " · " + permission.scope + " · " + permission.purpose + " · " + permission.programs.joined(separator: ", "))
                        }
                        if let executor = previous.executorDeclarations { Text(executor).font(.system(size: 11, design: .monospaced)).textSelection(.enabled) }
                    }
                }
                Text("这里只比较声明，不判断代码行为或版本先后。确认只更换活动选择，现有运行保留原快照；仍需新来源批准和本次 Session 授权。")
                Toggle("我已核对候选声明与旧版本状态，确认本次切换", isOn: $draft.acknowledgedSelection)
                    .disabled(draft.busy).accessibilityIdentifier("plugin-selection-acknowledge")
                HStack {
                    Button("确认切换活动选择") {
                        if let controller { Task {
                            if let result = await draft.confirmSelection(controller) { didChange(value.candidate.review.id, result) }
                        } }
                    }.disabled(!draft.canConfirmSelection).accessibilityIdentifier("plugin-selection-confirm")
                    Button("保留原选择并取消") {
                        if let controller { Task { await draft.cancelSelection(controller) } }
                    }.disabled(draft.busy).accessibilityIdentifier("plugin-selection-cancel")
                }
            }
        }
    }
}
