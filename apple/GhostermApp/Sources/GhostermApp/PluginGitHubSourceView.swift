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

    private var editableAddress: Binding<String> {
        Binding(get: { address }, set: { value in
            guard !loading, value != address else { return }
            address = value
            clearIndex()
        })
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("从 GitHub 添加插件").font(.title2)
            Text("精选插件").font(.headline)
            ForEach(PluginCatalog.builtIn, id: \.displayName) { entry in
                Button {
                    load(address: entry.source.repositoryPath + (entry.source.plugin.map { ":\($0)" } ?? ""))
                } label: {
                    VStack(alignment: .leading) {
                        Text(entry.displayName)
                        Text("\(entry.source.repositoryPath) · \(entry.defaultRole)").font(.caption)
                    }
                }
                .buttonStyle(.plain)
                .disabled(loading)
                .accessibilityIdentifier("plugin-featured-\(entry.source.repository)")
            }
            TextField("owner/repo 或 GitHub 仓库 URL", text: editableAddress)
                .textFieldStyle(.roundedBorder)
                .disabled(loading)
                .accessibilityIdentifier("plugin-github-address")
            Button(loading ? "正在读取…" : "读取仓库") { load(address: address) }
                .disabled(loading || address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                .accessibilityIdentifier("plugin-github-read")
            if let issue { Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-github-error") }
            if let index {
                Text("\(index.repository) · Release \(index.releaseTag) · \(index.publisher)").font(.headline)
                ForEach(index.plugins, id: \.id) { item in
                    Toggle(isOn: Binding(get: { selected.contains(item.id) }, set: { value in if value { selected.insert(item.id) } else { selected.remove(item.id) } })) {
                        VStack(alignment: .leading) { Text(item.name); Text("\(item.version) · \(item.role) · \(item.archiveSHA256)").font(.caption).textSelection(.enabled) }
                    }
                    .disabled(loading)
                }
                Toggle("固定当前 Release tag", isOn: $pinned)
                Text("安装计划：已选 \(selected.count) 个，来源 \(index.repository)，tag \(index.releaseTag)").font(.caption)
                    .accessibilityIdentifier("plugin-github-plan")
                Button("安装选中插件") { install() }
                    .disabled(selected.isEmpty || loading || controller?.hasConfiguredChannel != true)
                    .accessibilityIdentifier("plugin-github-install")
                if controller?.hasConfiguredChannel == false {
                    Text("此版本未配置在线插件信任渠道。已有可信签名发布包可在设置中通过「安装签名发布包」安装。")
                        .font(.caption)
                }
            }
            Spacer()
            HStack { Spacer(); Button("关闭") { dismiss() } }
        }
        .padding(24).frame(minWidth: 520, minHeight: 360)
    }
    private func install() {
        guard !loading, let index, !selected.isEmpty,
              let controller, controller.hasConfiguredChannel,
              let source = try? PluginGitHubSource(address) else { return }
        let selectedEntries = index.plugins.filter { selected.contains($0.id) }
        loading = true; issue = nil
        Task {
            do {
                for entry in selectedEntries {
                    let assets = try await client.fetchAssets(for: entry, source: source, releaseTag: index.releaseTag)
                    let result = try await PluginGitHubInstallCoordinator().prepareAndInstallUsingConfiguredChannel(assets, using: controller)
                    await MainActor.run { didInstall(result) }
                }
                await MainActor.run { loading = false; dismiss() }
            } catch { await MainActor.run { show(error) } }
        }
    }

    private func clearIndex() {
        issue = nil
        index = nil
        selected.removeAll()
        pinned = false
    }

    private func show(_ error: Error) {
        issue = (error as? LocalizedError)?.errorDescription ?? String(describing: error)
        loading = false
    }

    private func load(address: String) {
        guard !loading else { return }
        self.address = address
        clearIndex()
        guard let source = try? PluginGitHubSource(address) else { issue = "GitHub 地址格式无效。"; return }
        loading = true
        Task {
            do { let value = try await client.fetchIndex(for: source); await MainActor.run { index = value; loading = false } }
            catch { await MainActor.run { show(error) } }
        }
    }
}
