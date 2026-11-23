import SwiftUI
import GhostermAppModel

struct PluginGitHubSourceView: View {
    let controller: ExtensionController?
    let didInstall: (ExtensionController.InstalledRelease) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var address = ""
    @State private var loading = false
    @State private var issue: String?
    @State private var index: PluginIndex?
    @State private var selected = Set<String>()
    @State private var pinned = false
    private let client = PluginGitHubClient()
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("从 GitHub 添加插件").font(.title2)
            Text("精选插件").font(.headline)
            ForEach(PluginCatalog.builtIn, id: \.displayName) { entry in
                Button { address = entry.source.repositoryPath; load() } label: {
                    VStack(alignment: .leading) { Text(entry.displayName); Text("${entry.source.repositoryPath} · ${entry.defaultRole}").font(.caption) }
                }.buttonStyle(.plain).accessibilityIdentifier("plugin-featured-\(entry.source.repository)")
            }
            TextField("owner/repo 或 GitHub 仓库 URL", text: $address)
                .textFieldStyle(.roundedBorder)
                .accessibilityIdentifier("plugin-github-address")
            Button(loading ? "正在读取…" : "读取仓库") { load() }
                .disabled(loading || address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                .accessibilityIdentifier("plugin-github-read")
            if let issue { Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-github-error") }
            if let index {
                Text("\(index.repository) · Release \(index.releaseTag) · \(index.publisher)").font(.headline)
                ForEach(index.plugins, id: \.id) { item in
                    Toggle(isOn: Binding(get: { selected.contains(item.id) }, set: { value in if value { selected.insert(item.id) } else { selected.remove(item.id) } })) {
                        VStack(alignment: .leading) { Text(item.name); Text("\(item.version) · \(item.role) · \(item.archiveSHA256)").font(.caption).textSelection(.enabled) }
                    }
                }
                Toggle("固定当前 Release tag", isOn: $pinned)
                Text("安装计划：已选 \(selected.count) 个，来源 \(index.repository)，tag \(index.releaseTag)").font(.caption)
                    .accessibilityIdentifier("plugin-github-plan")
                Button("安装选中插件") { install(index: index) }
                    .disabled(selected.isEmpty || loading || controller == nil || controller?.hasConfiguredChannel != true)
                    .accessibilityIdentifier("plugin-github-install")
            }
            Spacer()
            HStack { Spacer(); Button("关闭") { dismiss() } }
        }
        .padding(24).frame(minWidth: 520, minHeight: 360)
    }
    private func install(index: PluginIndex) {
        guard let controller, let source = try? PluginGitHubSource(address) else { return }
        loading = true; issue = nil
        Task {
            do {
                let selectedEntries = index.plugins.filter { selected.contains($0.id) }
                for entry in selectedEntries {
                    let assets = try await client.fetchAssets(for: entry, source: source, releaseTag: index.releaseTag)
                    let result = try await PluginGitHubInstallCoordinator().prepareAndInstallUsingConfiguredChannel(assets, using: controller)
                    await MainActor.run { didInstall(result) }
                }
                await MainActor.run { loading = false; dismiss() }
            } catch { await MainActor.run { issue = String(describing: error); loading = false } }
        }
    }
    private func load() {
        guard let source = try? PluginGitHubSource(address) else { issue = "GitHub 地址格式无效。"; index = nil; return }
        loading = true; issue = nil; index = nil; selected.removeAll(); pinned = false
        Task {
            do { let value = try await client.fetchIndex(for: source); await MainActor.run { index = value; loading = false } }
            catch { await MainActor.run { issue = (error as? LocalizedError)?.errorDescription ?? String(describing: error); loading = false } }
        }
    }
}
