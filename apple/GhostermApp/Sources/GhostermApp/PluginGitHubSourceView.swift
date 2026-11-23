import SwiftUI
import GhostermAppModel

struct PluginGitHubSourceView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var address = ""
    @State private var loading = false
    @State private var issue: String?
    @State private var index: PluginIndex?
    private let client = PluginGitHubClient()
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("从 GitHub 添加插件").font(.title2)
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
                    VStack(alignment: .leading) { Text(item.name); Text("\(item.version) · \(item.role) · \(item.archiveSHA256)").font(.caption).textSelection(.enabled) }
                }
            }
            Spacer()
            HStack { Spacer(); Button("关闭") { dismiss() } }
        }
        .padding(24).frame(minWidth: 520, minHeight: 360)
    }
    private func load() {
        guard let source = try? PluginGitHubSource(address) else { issue = "GitHub 地址格式无效。"; index = nil; return }
        loading = true; issue = nil; index = nil
        Task {
            do { let value = try await client.fetchIndex(for: source); await MainActor.run { index = value; loading = false } }
            catch { await MainActor.run { issue = (error as? LocalizedError)?.errorDescription ?? String(describing: error); loading = false } }
        }
    }
}
