import AppKit
import Foundation
import GhostermAppModel
import SwiftUI

@MainActor
final class PluginInstalledVersionsDraft: ObservableObject {
    @Published private(set) var catalog: ExtensionController.InstalledCatalog?
    @Published private(set) var busy = false
    private var activities = 0
    private func beginActivity() { activities += 1; busy = true }
    private func endActivity() { activities -= 1; busy = activities != 0 }
    @Published private(set) var issue: String?
    @Published private(set) var notice: String?
    @Published private(set) var affectedChannelInstances = 0
    @Published private(set) var removal: ExtensionController.RemovalPreview?
    @Published private(set) var pendingRemoval: ExtensionController.RemovalPreview?
    @Published private(set) var residues: ExtensionController.InstallationResidues?
    @Published private(set) var unclassified: [ExtensionController.UnclassifiedEntry] = []
    @Published var acknowledgedUnknown = false
    @Published private(set) var selectionReview: ExtensionController.SelectionReview?
    @Published var acknowledgedSelection = false
    private var selectionGeneration = UUID()
    private var selectionOwner: ExtensionController?
    private func invalidateSelection(cancelNative: Bool = true) {
        let id = selectionReview?.id, owner = selectionOwner
        selectionGeneration = UUID(); selectionReview = nil; acknowledgedSelection = false; selectionOwner = nil
        if cancelNative, let id, let owner {
            beginActivity()
            Task { defer { endActivity() }; await owner.cancelInstalledSelection(id: id) }
        }
    }
    func cancelSelection(_ controller: ExtensionController) async {
        let id = selectionReview?.id
        invalidateSelection(cancelNative: false)
        if let id {
            beginActivity(); defer { endActivity() }
            await controller.cancelInstalledSelection(id: id)
        }
    }
    private var generation = UUID()
    @Published private(set) var policyReview: ExtensionController.PolicyReview?
    @Published var policyDisableIDs: Set<String> = []
    private var policyGeneration = UUID()
    @Published var policyPath = "" { didSet { trustedPolicy = false; invalidatePolicy() } }
    @Published var trustedPolicy = false { didSet { invalidatePolicy() } }
    private var canReviewVersion: Bool {
        !busy && removal == nil && pendingRemoval == nil && selectionReview == nil && catalog != nil
    }
    var canSelect: Bool { canReviewVersion && trustedPolicy && policyPath.hasPrefix("/") && !policyPath.utf8.contains(0) }
    func canSelect(_ version: ExtensionController.InstalledVersion) -> Bool {
        version.requiresChannelPolicy ? canReviewVersion : canSelect
    }
    private func invalidatePolicy() {
        policyGeneration = UUID(); policyReview = nil; policyDisableIDs = []
        invalidateSelection()
    }
    func cancelPolicy(_ controller: ExtensionController) async {
        let id = policyReview?.id; invalidatePolicy()
        if let id { await controller.cancelInstalledPolicyReview(id: id) }
    }
    func reviewPolicy(_ controller: ExtensionController) async {
        guard canSelect else { return }
        invalidatePolicy(); let request = policyGeneration
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        do {
            let value = try await controller.previewInstalledPolicy(trustedPolicy: URL(fileURLWithPath: policyPath))
            guard request == policyGeneration else { await controller.cancelInstalledPolicyReview(id: value.id); return }
            policyReview = value
        } catch { if request == policyGeneration { issue = message(error) } }
    }
    func applyPolicy(_ controller: ExtensionController) async -> ExtensionController.PolicyDisableResult? {
        guard canSelect, let review = policyReview, !policyDisableIDs.isEmpty else { return nil }
        let ids = policyDisableIDs.sorted()
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        do {
            let result = try await controller.disableReviewedPlugins(id: review.id, pluginIDs: ids)
            invalidatePolicy(); catalog = result.selection.catalog
            notice = "已禁用所选 \(result.disabled.count) 个插件的新建；现有运行需另行停止。" +
                (result.selection.durabilityConfirmed ? "" : "选择已写入，但持久性未确认；请刷新核对。")
            return result
        } catch { invalidatePolicy(); issue = message(error); return nil }
    }
    var canRefreshChannelPolicy: Bool {
        !busy && removal == nil && pendingRemoval == nil && selectionReview == nil && policyReview == nil
    }

    func readChannelPolicy(_ controller: ExtensionController, refresh: Bool, automatic: Bool = false) async {
        guard canRefreshChannelPolicy, refresh ? controller.canRefreshChannelPolicy : controller.hasConfiguredChannel else { return }
        beginActivity(); defer { endActivity() }
        if !automatic { issue = nil; notice = nil }
        do {
            let policy = try await (refresh ? controller.refreshConfiguredChannelPolicy() : controller.cachedChannelPolicy())
            let count = try await controller.runningInstancesWithRevokedPublisher(in: policy)
            guard !Task.isCancelled else { return }
            affectedChannelInstances = count
            if !automatic {
                notice = refresh ? "已更新 \(policy.channel) 渠道策略至第 \(policy.generation) 代。"
                    : "本机已验证 \(policy.channel) 渠道策略第 \(policy.generation) 代。"
            }
        } catch {
            guard !Task.isCancelled else { return }
            affectedChannelInstances = 0
            if !automatic { issue = message(error) }
        }
    }

    func stopAffectedChannelInstances(_ controller: ExtensionController, stop: () async throws -> AppModel.ChannelStopResult) async {
        guard canRefreshChannelPolicy, affectedChannelInstances > 0 else { return }
        beginActivity(); issue = nil; notice = nil
        do {
            let result = try await stop()
            notice = "已确认停止 \(result.stopped) 个实例。"
            if let firstFailure = result.failures.first {
                issue = "\(result.failures.count) 个实例未能完成核对或停止：\(firstFailure)"
            }
        } catch { issue = message(error) }
        endActivity()
        if !Task.isCancelled { await readChannelPolicy(controller, refresh: false, automatic: true) }
    }

    func refresh(_ controller: ExtensionController) async {
        guard !busy, removal == nil else { return }
        beginActivity(); defer { endActivity() }
        await cancelSelection(controller)
        await cancelPolicy(controller)
        do {
            catalog = try await controller.installedVersions()
            if catalog?.removalPending == true { pendingRemoval = try await controller.pendingVersionRemoval() }
            else { pendingRemoval = nil }
            residues = try await controller.installationResidues(); issue = nil
            unclassified = try await controller.unclassifiedEntries()
        }
        catch { catalog = nil; issue = message(error) }
    }
    func previewSelection(_ version: ExtensionController.InstalledVersion, controller: ExtensionController) async {
        guard canSelect(version), let catalog else { return }
        invalidateSelection(); selectionOwner = controller; let request = selectionGeneration
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        do {
            let value = try await controller.previewInstalledSelection(releaseID: version.releaseID,
                expectedRevision: catalog.revision, trustedPolicy: version.requiresChannelPolicy ? nil : URL(fileURLWithPath: policyPath))
            guard selectionGeneration == request else { await controller.cancelInstalledSelection(id: value.id); return }
            selectionReview = value
        } catch { if selectionGeneration == request { issue = message(error) } }
    }
    var canConfirmSelection: Bool {
        !busy && (trustedPolicy || selectionReview?.channelGeneration != nil) && selectionReview != nil && acknowledgedSelection && removal == nil && pendingRemoval == nil
    }
    func confirmSelection(_ controller: ExtensionController) async -> ExtensionController.VersionSelectionResult? {
        guard canConfirmSelection, let value = selectionReview else { return nil }
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        do {
            let result = try await controller.confirmInstalledSelection(id: value.id, acknowledged: acknowledgedSelection)
            invalidateSelection(cancelNative: false); catalog = result.catalog
            notice = result.durabilityConfirmed ? "活动版本已更新；精确来源和本次 Session 仍需分别批准。" : "活动选择已写入，但持久性未确认；请刷新核对。"
            return result
        } catch { invalidateSelection(cancelNative: false); issue = message(error); return nil }
    }
    func disable(_ pluginID: String, controller: ExtensionController) async -> ExtensionController.VersionSelectionResult? {
        guard !busy, removal == nil, let catalog else { return nil }
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        do {
            let result = try await controller.disableInstalledPlugin(pluginID: pluginID, expectedRevision: catalog.revision)
            self.catalog = result.catalog
            notice = result.durabilityConfirmed ? "已禁用新建；现有运行可继续，也可单独停止。" : "禁用选择已写入，但持久性未确认；请刷新核对。"
            return result
        } catch { issue = message(error); return nil }
    }
    func preview(_ version: ExtensionController.InstalledVersion, controller: ExtensionController) async {
        guard !busy, removal == nil, pendingRemoval == nil, let catalog else { return }
        let request = generation
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        do {
            let value = try await controller.previewVersionRemoval(releaseID: version.releaseID, expectedRevision: catalog.revision)
            guard generation == request else { await controller.cancelVersionRemoval(id: value.id); return }
            removal = value
        } catch { if generation == request { issue = message(error) } }
    }
    func cancel(_ controller: ExtensionController) async {
        generation = UUID()
        await cancelSelection(controller)
        await cancelPolicy(controller)
        guard let removal else { return }
        self.removal = nil
        acknowledgedUnknown = false
        await controller.cancelVersionRemoval(id: removal.id)
    }
    func remove(_ controller: ExtensionController, resume: Bool) async -> ExtensionController.RemovalResult? {
        guard !busy, let value = resume ? pendingRemoval : removal else { return nil }
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        removal = nil
        var result: ExtensionController.RemovalResult?
        do {
            let value = try await (resume ? controller.resumeVersionRemoval(id: value.id) : controller.confirmVersionRemoval(id: value.id, acknowledgedUnknown: acknowledgedUnknown))
            result = value
            notice = "本次删除 \(value.removed) 个条目；观察到已不存在 \(value.observedAbsent) 个。" +
                (value.complete ? "回收已完成。" : "删除记录已保留，请显式继续处理。") +
                (value.durabilityConfirmed ? "" : "持久性未确认，请刷新核对。")
            if let cleanup = value.cleanupIssue { issue = "目录操作已返回；后续维护未完成：" + cleanup }
        } catch { issue = message(error) }
        do {
            catalog = try await controller.installedVersions(); pendingRemoval = try await controller.pendingVersionRemoval()
            residues = try await controller.installationResidues()
            unclassified = try await controller.unclassifiedEntries()
        }
        catch { issue = [issue, "刷新失败：" + message(error)].compactMap { $0 }.joined(separator: "\n") }
        return result
    }
    func previewUnknown(_ entry: ExtensionController.UnclassifiedEntry, controller: ExtensionController) async {
        guard !busy, removal == nil, pendingRemoval == nil, entry.canPreview else { return }
        let request = generation
        beginActivity(); issue = nil; notice = nil; acknowledgedUnknown = false; defer { endActivity() }
        do {
            let value = try await controller.previewUnclassifiedDirectory(name: entry.name, expectedIdentity: entry.identity)
            guard request == generation else { await controller.cancelVersionRemoval(id: value.id); return }
            removal = value
        } catch { if request == generation { issue = message(error) } }
    }
    func previewResidue(_ value: ExtensionController.InstallationResidue, controller: ExtensionController) async {
        guard !busy, removal == nil, pendingRemoval == nil else { return }
        let request = generation
        beginActivity(); issue = nil; notice = nil; defer { endActivity() }
        do {
            let preview = try await controller.previewInstallationResidue(id: value.id, expectedDigest: value.digest)
            guard request == generation else { await controller.cancelVersionRemoval(id: preview.id); return }
            removal = preview
        } catch { if request == generation { issue = message(error) } }
    }
    func retireResidue(_ value: ExtensionController.InstallationResidue, controller: ExtensionController) async {
        guard !busy, removal == nil, pendingRemoval == nil else { return }
        beginActivity(); issue = nil; defer { endActivity() }
        do {
            try await controller.retireInstallationRecord(id: value.id, expectedDigest: value.digest)
            notice = "已移除完成核对的安装记录；没有删除版本或其它目录。"
        } catch { issue = message(error) }
        do { residues = try await controller.installationResidues() } catch { issue = message(error) }
    }
    func cancelPending(_ controller: ExtensionController) async {
        guard !busy, let pendingRemoval else { return }
        beginActivity(); issue = nil; defer { endActivity() }
        do {
            let durable = try await controller.cancelPendingVersionRemoval(id: pendingRemoval.id)
            self.pendingRemoval = nil
            notice = "已撤销尚未开始的删除，版本与禁用记录保留。" + (durable ? "" : "持久性未确认，请刷新核对。")
            catalog = try await controller.installedVersions()
        } catch { issue = message(error) }
    }
    private func message(_ error: Error) -> String { (error as? LocalizedError)?.errorDescription ?? String(describing: error) }
}

struct PluginInstalledVersionsView: View {
    let controller: ExtensionController?
    var didRemove: (String) -> Void = { _ in }
    var stopRevoked: (() async throws -> AppModel.ChannelStopResult)? = nil
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
                }.disabled(draft.busy || draft.removal != nil).accessibilityIdentifier("plugin-installed-refresh")
                if let controller, controller.canRefreshChannelPolicy {
                    Button("刷新渠道策略") {
                        Task { await draft.readChannelPolicy(controller, refresh: true) }
                    }.disabled(!draft.canRefreshChannelPolicy).accessibilityIdentifier("plugin-installed-channel-refresh")
                }
                if let controller, controller.hasConfiguredChannel {
                    Button("核对本机渠道缓存") {
                        Task { await draft.readChannelPolicy(controller, refresh: false) }
                    }.disabled(!draft.canRefreshChannelPolicy).accessibilityIdentifier("plugin-installed-channel-cache")
                }
                if draft.affectedChannelInstances > 0 {
                    Text("本应用仍持有 \(draft.affectedChannelInstances) 个受撤销影响的实例。")
                        .accessibilityIdentifier("plugin-installed-channel-affected")
                }
                if let controller, let stopRevoked, draft.affectedChannelInstances > 0 {
                    Button("停止受渠道撤销影响的实例") {
                        Task { await draft.stopAffectedChannelInstances(controller, stop: stopRevoked) }
                    }.disabled(!draft.canRefreshChannelPolicy).accessibilityIdentifier("plugin-installed-channel-stop")
                }
                Group {
                    TextField("当前独立可信渠道策略绝对路径", text: $draft.policyPath).accessibilityIdentifier("plugin-installed-policy")
                    Toggle("我已独立核对并信任此策略，用于重新验证所选版本", isOn: $draft.trustedPolicy)
                        .accessibilityIdentifier("plugin-installed-trust")
                }.disabled(draft.busy)
                PluginPolicyReviewView(draft: draft, controller: controller) { result in
                    for id in result.disabled { didChange(id, result.selection) }
                }
                if let catalog = draft.catalog {
                    if catalog.versions.isEmpty { Text("未读到完整的已安装版本。") }
                    ForEach(catalog.versions) { version in
                        VStack(alignment: .leading, spacing: DS.Padding.xs) {
                            Text("\(version.pluginID) · \(version.version) · \(version.selected ? (version.enabled ? "活动选择" : "已禁用") : "已安装")")
                                .accessibilityIdentifier("plugin-installed-row-\(version.releaseID)")
                            Text("包内声明发布者：\(version.publisher)（当前策略待核对）")
                                .font(DS.Font.caption)
                            Text(version.packageSHA256).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                            Text(version.inUse.map { $0 ? "有运行或维护预览引用；先停止或取消后回收。" : "未观察到运行引用。" } ?? "运行引用状态查不出来；回收时必须重新核对。")
                                .font(DS.Font.caption)
                            HStack {
                                Button(version.selected && version.enabled ? "预览重新载入" : "预览切换到此版本") {
                                    if let controller { Task { await draft.previewSelection(version, controller: controller) } }
                                }.disabled(!draft.canSelect(version)).accessibilityIdentifier("plugin-installed-select-\(version.releaseID)")
                                if version.selected && version.enabled {
                                    Button("禁用新建") {
                                        if let controller { Task { if let result = await draft.disable(version.pluginID, controller: controller) { didChange(version.pluginID, result) } } }
                                    }.disabled(draft.busy || draft.removal != nil).accessibilityIdentifier("plugin-installed-disable-\(version.releaseID)")
                                }
                                Button("预览回收此版本") {
                                    if let controller { Task { await draft.preview(version, controller: controller) } }
                                }.disabled(draft.busy || draft.removal != nil || draft.pendingRemoval != nil || (version.selected && version.enabled))
                                    .accessibilityIdentifier("plugin-installed-remove-preview-\(version.releaseID)")
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
                PluginSelectionReviewView(draft: draft, controller: controller) { id, result in didChange(id, result) }
                if let preview = draft.removal { removalReview(preview, confirmed: false) }
                if let pending = draft.pendingRemoval { removalReview(pending, confirmed: true) }
                if let residues = draft.residues {
                    ForEach(residues.entries) { entry in residueRow(entry) }
                    ForEach(Array(residues.issues.enumerated()), id: \.offset) { _, issue in Text(issue).foregroundStyle(.red) }
                }
                ForEach(draft.unclassified) { entry in
                    VStack(alignment: .leading, spacing: DS.Padding.xs) {
                        Text("归属未知：" + String(reflecting: entry.name)).accessibilityIdentifier("plugin-unknown-row-" + entry.id)
                        Text("不认定为安装残留，不按名称授权删除。").font(DS.Font.caption)
                        if let issue = entry.issue { Text(issue).foregroundStyle(.red) }
                        if entry.canPreview {
                            Button("逐项核对此目录的完整内容") {
                                if let controller { Task { await draft.previewUnknown(entry, controller: controller) } }
                            }.disabled(draft.busy || draft.removal != nil || draft.pendingRemoval != nil)
                                .accessibilityIdentifier("plugin-unknown-preview-" + entry.id)
                        }
                    }
                }
                if let issue = draft.issue { Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-installed-error") }
                if let notice = draft.notice { Text(notice).accessibilityIdentifier("plugin-installed-notice") }
                Text("列表只读本机记录，不认证包自报身份。选择旧版本即显式回滚，也须重新验签和读取内容；不会迁移来源批准、Session 授权或重放操作。禁用仅阻止新建，已运行实例仍持有原快照。")
                    .font(DS.Font.caption).foregroundStyle(.secondary)
            }
        }.task(id: expanded) {
            if let controller {
                if expanded {
                    await draft.refresh(controller)
                    if !Task.isCancelled { await draft.readChannelPolicy(controller, refresh: false, automatic: true) }
                } else {
                    await draft.cancel(controller)
                }
            }
        }.onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            if expanded, let controller { Task { await draft.readChannelPolicy(controller, refresh: false, automatic: true) } }
        }.onDisappear { if let controller { Task { await draft.cancel(controller) } } }
    }
    private func residueRow(_ entry: ExtensionController.InstallationResidue) -> some View {
        VStack(alignment: .leading, spacing: DS.Padding.xs) {
            Text("安装记录：" + residueState(entry.state)).accessibilityIdentifier("plugin-install-residue-" + entry.id)
            Text(entry.directory.path).font(DS.Font.caption).textSelection(.enabled)
            if let target = entry.publishedDirectory { Text("记录的发布目标：" + target.path).font(DS.Font.caption).textSelection(.enabled) }
            if entry.state == .abandoned {
                Button("预览此安装暂存的完整内容") {
                    if let controller { Task { await draft.previewResidue(entry, controller: controller) } }
                }.disabled(draft.busy || draft.removal != nil || draft.pendingRemoval != nil)
                    .accessibilityIdentifier("plugin-install-residue-preview-" + entry.id)
            } else if entry.state == .published || entry.state == .absent {
                Button("仅移除此条已核对记录（保留目录）") {
                    if let controller { Task { await draft.retireResidue(entry, controller: controller) } }
                }.disabled(draft.busy || draft.removal != nil || draft.pendingRemoval != nil)
                    .accessibilityIdentifier("plugin-install-residue-retire-" + entry.id)
            }
        }
    }
    private func residueState(_ state: ExtensionController.InstallationResidueState) -> String {
        switch state {
        case .active: return "仍在处理，保留暂存"
        case .abandoned: return "目录身份已匹配，未观察到活动引用；可预览回收"
        case .published: return "同一目录已在发布目标出现；仅可移除记录"
        case .absent: return "未在原暂存位置观察到目录；仅可移除记录"
        case .unconfirmed: return "只有预约，目录归属尚未确认；保留"
        case .changed: return "目录身份已变化；保留"
        }
    }
    private func removalReview(_ preview: ExtensionController.RemovalPreview, confirmed: Bool) -> some View {
        VStack(alignment: .leading, spacing: DS.Padding.xs) {
            Text(confirmed ? "此前已确认的目录回收尚待核对" : "确认回收此私有目录及以下全部内容")
            Text(preview.directory.path).textSelection(.enabled)
            if preview.unknownOrigin {
                Text("此目录归属未知。删除只依据本次核对和确认，不证明它来自安装，也不会移除旧归属记录。")
                if !confirmed {
                    Toggle("我已核对完整清单，并选择删除这个归属未知的目录", isOn: $draft.acknowledgedUnknown)
                        .accessibilityIdentifier("plugin-unknown-acknowledge")
                }
            }
            Text("\(preview.files.count) 个成员，\(preview.bytes) 字节；运行引用和内容会在执行前再核对。")
            ScrollView {
                LazyVStack(alignment: .leading) {
                    ForEach(preview.files) { file in
                        Text(file.path + (file.directory ? "/" : " · \(file.bytes) 字节 · " + (file.sha256 ?? "摘要查不出来")))
                            .font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                    }
                }
            }.frame(maxHeight: 180)
            HStack {
                Button(confirmed ? "继续处理此已确认删除" : "确认删除以上目录与文件", role: .destructive) {
                    if let controller { Task {
                        if let result = await draft.remove(controller, resume: confirmed), result.complete { didRemove(result.releaseID) }
                    } }
                }.disabled(draft.busy || (!confirmed && preview.unknownOrigin && !draft.acknowledgedUnknown)).accessibilityIdentifier("plugin-installed-remove-confirm")
                if !confirmed {
                    Button("保留并取消") { if let controller { Task { await draft.cancel(controller) } } }
                        .disabled(draft.busy).accessibilityIdentifier("plugin-installed-remove-cancel")
                } else {
                    Button("撤销尚未开始的删除") { if let controller { Task { await draft.cancelPending(controller) } } }
                        .disabled(draft.busy).accessibilityIdentifier("plugin-installed-remove-pending-cancel")
                }
            }
            Text(preview.unknownOrigin ? "只删除这份清单内的本机目录与文件；旧归属和选择记录保留。" : "回收只处理这份本机版本，不触碰远端执行端、工作区或业务文件；禁用记录仍保留。")
                .font(DS.Font.caption)
        }
    }
}
