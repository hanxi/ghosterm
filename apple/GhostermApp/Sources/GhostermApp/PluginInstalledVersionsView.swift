import Foundation
import GhostermAppModel
import SwiftUI

@MainActor
final class PluginInstalledVersionsDraft: ObservableObject {
    @Published private(set) var catalog: ExtensionController.InstalledCatalog?
    @Published private(set) var busy = false
    @Published private(set) var issue: String?
    @Published private(set) var notice: String?
    @Published var policyPath = "" { didSet { trustedPolicy = false } }
    @Published var trustedPolicy = false
    var canSelect: Bool { !busy && catalog != nil && trustedPolicy && policyPath.hasPrefix("/") && !policyPath.utf8.contains(0) }
    func refresh(_ controller: ExtensionController) async {
        guard !busy else { return }
        busy = true; defer { busy = false }
        do { catalog = try await controller.installedVersions(); issue = nil }
        catch { catalog = nil; issue = message(error) }
    }
    func select(_ version: ExtensionController.InstalledVersion, controller: ExtensionController) async -> ExtensionController.VersionSelectionResult? {
        guard canSelect, let catalog else { return nil }
        busy = true; issue = nil; notice = nil; defer { busy = false }
        do {
            let result = try await controller.selectInstalledVersion(releaseID: version.releaseID,
                expectedRevision: catalog.revision, trustedPolicy: URL(fileURLWithPath: policyPath))
            self.catalog = result.catalog
            notice = result.durabilityConfirmed ? "活动版本已更新；精确来源和本次 Session 仍需分别批准。" : "活动选择已写入，但持久性未确认；请刷新核对。"
            return result
        } catch { issue = message(error); return nil }
    }
    func disable(_ pluginID: String, controller: ExtensionController) async -> ExtensionController.VersionSelectionResult? {
        guard !busy, let catalog else { return nil }
        busy = true; issue = nil; notice = nil; defer { busy = false }
        do {
            let result = try await controller.disableInstalledPlugin(pluginID: pluginID, expectedRevision: catalog.revision)
            self.catalog = result.catalog
            notice = result.durabilityConfirmed ? "已禁用新建；现有运行可继续，也可单独停止。" : "禁用选择已写入，但持久性未确认；请刷新核对。"
            return result
        } catch { issue = message(error); return nil }
    }
    private func message(_ error: Error) -> String { (error as? LocalizedError)?.errorDescription ?? String(describing: error) }
}

struct PluginInstalledVersionsView: View {
    let controller: ExtensionController?
    let didChange: (String, ExtensionController.VersionSelectionResult) -> Void
    @StateObject private var draft = PluginInstalledVersionsDraft()
    @State private var expanded = false
    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.s) {
            Button(expanded ? "收起已安装版本" : "管理已安装版本…") { expanded.toggle() }
                .disabled(controller == nil).accessibilityIdentifier("plugin-installed-expand")
            if expanded {
                Button(draft.busy ? "正在读取…" : "刷新已安装列表") {
                    if let controller { Task { await draft.refresh(controller) } }
                }.disabled(draft.busy).accessibilityIdentifier("plugin-installed-refresh")
                Group {
                    TextField("当前独立可信渠道策略绝对路径", text: $draft.policyPath).accessibilityIdentifier("plugin-installed-policy")
                    Toggle("我已独立核对并信任此策略，用于重新验证所选版本", isOn: $draft.trustedPolicy)
                        .accessibilityIdentifier("plugin-installed-trust")
                }.disabled(draft.busy)
                if let catalog = draft.catalog {
                    if catalog.versions.isEmpty { Text("未读到完整的已安装版本。") }
                    ForEach(catalog.versions) { version in
                        VStack(alignment: .leading, spacing: DS.Padding.xs) {
                            Text("\(version.pluginID) · \(version.version) · \(version.selected ? (version.enabled ? "活动选择" : "已禁用") : "已安装")")
                                .accessibilityIdentifier("plugin-installed-row-\(version.releaseID)")
                            Text("包内声明发布者：\(version.publisher)（当前策略待核对）")
                                .font(DS.Font.caption)
                            Text(version.packageSHA256).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                            HStack {
                                Button(version.selected && version.enabled ? "重新核对并载入" : "核对并设为活动版本") {
                                    if let controller { Task { if let result = await draft.select(version, controller: controller) { didChange(version.pluginID, result) } } }
                                }.disabled(!draft.canSelect).accessibilityIdentifier("plugin-installed-select-\(version.releaseID)")
                                if version.selected && version.enabled {
                                    Button("禁用新建") {
                                        if let controller { Task { if let result = await draft.disable(version.pluginID, controller: controller) { didChange(version.pluginID, result) } } }
                                    }.disabled(draft.busy).accessibilityIdentifier("plugin-installed-disable-\(version.releaseID)")
                                }
                            }
                        }
                    }
                    ForEach(catalog.selections.filter { choice in !catalog.versions.contains { $0.releaseID == choice.releaseID && $0.pluginID == choice.pluginID } }) { choice in
                        VStack(alignment: .leading) {
                            Text("\(choice.pluginID)：所选版本查不出来（\(choice.releaseID)）").foregroundStyle(.red)
                            if choice.enabled {
                                Button("禁用此记录的新建") {
                                    if let controller { Task { if let result = await draft.disable(choice.pluginID, controller: controller) { didChange(choice.pluginID, result) } } }
                                }.disabled(draft.busy).accessibilityIdentifier("plugin-installed-disable-missing-\(choice.pluginID)")
                            } else { Text("此记录已禁用新建。") }
                        }
                    }
                    ForEach(Array(catalog.issues.enumerated()), id: \.offset) { _, issue in Text(issue).foregroundStyle(.red).textSelection(.enabled) }
                    if catalog.maintenanceCount > 0 { Text("另有 \(catalog.maintenanceCount) 个待核对条目；不会按名称自动删除。") }
                }
                if let issue = draft.issue { Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-installed-error") }
                if let notice = draft.notice { Text(notice).accessibilityIdentifier("plugin-installed-notice") }
                Text("列表只读本机记录，不认证包自报身份。选择旧版本即显式回滚，也须重新验签和读取内容；不会迁移来源批准、Session 授权或重放操作。禁用仅阻止新建，已运行实例仍持有原快照。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }.task(id: expanded) { if expanded, let controller { await draft.refresh(controller) } }
    }
}
