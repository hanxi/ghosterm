import GhostermAppModel
import SwiftUI
import UniformTypeIdentifiers

/// Native review and explicit in-memory source decision for a fixed package.
/// This page does not claim to install, persist trust, or activate a plugin.
struct ExtensionsPane: View {
    @ObservedObject var model: AppModel
    @State private var importing = false
    @State private var entries: [Entry] = []
    @State private var issue: String?

    private struct Entry: Identifiable {
        let review: ExtensionController.PackageReview
        var decision: String = "尚未批准"
        var id: String { review.id + ":" + review.digest }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: DS.Padding.m) {
            HStack {
                Text("插件包").font(DS.Font.labelEmphasis)
                Spacer()
                Button("选择文件夹…") { importing = true }
                    .disabled(model.extensionController == nil)
            }

            if model.extensionController == nil {
                Label("此构建未启用 Portable JS 插件运行时。", systemImage: "exclamationmark.circle")
                    .foregroundStyle(.secondary)
                SettingsNote("普通 Ghosterm 构建默认关闭插件运行时；当前不能审查或启动 Client 插件。")
            } else if entries.isEmpty {
                SettingsNote("选择插件包文件夹后，Ghosterm 会检查固定内容摘要、兼容性和命令。审查不会自动批准或启动插件。")
            }

            if let issue {
                Text(issue).foregroundStyle(.red).textSelection(.enabled)
            }

            ForEach($entries) { $entry in
                VStack(alignment: .leading, spacing: DS.Padding.s) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(entry.review.id).font(DS.Font.labelEmphasis)
                        Spacer()
                        Text(entry.decision).foregroundStyle(.secondary)
                    }
                    Text("版本 \(entry.review.version) · 发布者 \(entry.review.publisher) · \(entry.review.license)")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("运行时：\(entry.review.runtime) · 发布者身份：\(entry.review.publisherAuthenticated ? "已认证" : "未认证")")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("声明的必需能力：\(entry.review.requiredCapabilities.isEmpty ? "无" : entry.review.requiredCapabilities.joined(separator: ", "))")
                        .font(DS.Font.caption)
                    Text("缺失的可选能力：\(entry.review.optionalMissingCapabilities.isEmpty ? "无" : entry.review.optionalMissingCapabilities.joined(separator: ", "))")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    Text("摘要：\(entry.review.digest)").font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                    Text("命令：\(entry.review.commands.map(\.path).joined(separator: ", "))")
                        .font(DS.Font.caption)
                    Text("能力授权：\(entry.review.authorizationGranted ? "已授予" : "未授予")；来源批准不会授予能力。")
                        .font(DS.Font.caption).foregroundStyle(.secondary)
                    HStack {
                        Button("批准此摘要") { decide(entry: entry, approve: true) }
                            .disabled(entry.decision == "已批准（本进程）")
                        Button("拒绝") { decide(entry: entry, approve: false) }
                    }
                }
                .padding(DS.Padding.m)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(.quaternary, in: RoundedRectangle(cornerRadius: DS.Radius.control))
            }

            SettingsNote("批准仅作用于当前 AppModel 生命周期中的精确包摘要；安装、持久信任和运行时激活尚未接入。")
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.folder], allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                Task { await review(url) }
            case .failure(let error):
                issue = "无法选择插件包：\(error.localizedDescription)"
            }
        }
    }

    private func review(_ url: URL) async {
        guard let controller = model.extensionController else { return }
        issue = nil
        do {
            let value = try await controller.review(directory: url)
            guard !entries.contains(where: { $0.id == value.id + ":" + value.digest }) else { return }
            entries.append(Entry(review: value))
        } catch {
            issue = "插件包审查失败：\(error)"
        }
    }

    private func decide(entry: Entry, approve: Bool) {
        guard let controller = model.extensionController else { return }
        Task {
            do {
                let status = approve
                    ? try await controller.approve(id: entry.review.id, digest: entry.review.digest)
                    : try await controller.deny(id: entry.review.id, digest: entry.review.digest)
                guard let index = entries.firstIndex(where: { $0.id == entry.id }) else { return }
                entries[index].decision = status == 0 ? (approve ? "已批准（本进程）" : "已拒绝") : "裁决失败（\(status)）"
            } catch {
                issue = "无法保存来源裁决：\(error)"
            }
        }
    }
}
