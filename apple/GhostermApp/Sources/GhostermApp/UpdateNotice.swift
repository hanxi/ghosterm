import AppKit
import GhostermAppModel
import SwiftUI

/// An overlay never becomes a key window or requests focus from the terminal.
struct UpdateNotice: View {
    @ObservedObject var updater: UpdateChecker

    var body: some View {
        if updater.noticeVisible {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Text("Ghosterm 更新").font(.headline)
                    Spacer()
                    Button { updater.noticeVisible = false } label: {
                        Image(systemName: "xmark")
                    }
                    .buttonStyle(.plain)
                    .help("收起提示；下载会继续，可在菜单或关于窗口查看")
                    .accessibilityLabel("收起更新提示")
                }
                if updater.isChecking {
                    HStack { ProgressView().controlSize(.small); Text("正在检查更新…") }
                } else if let query = updater.query {
                    Text(query.statusText).font(.subheadline).foregroundStyle(.secondary)
                }
                UpdateActions(updater: updater)
            }
            .padding(14)
            .frame(width: 300, alignment: .leading)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
            .overlay(RoundedRectangle(cornerRadius: 12).stroke(.primary.opacity(0.1)))
            .shadow(color: .black.opacity(0.15), radius: 8, y: 2)
        }
    }
}

struct UpdateActions: View {
    @ObservedObject var updater: UpdateChecker
    private var remote: RemoteBuild? {
        if case .newer(let remote) = updater.query { return remote }
        return nil
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            switch updater.phase {
            case .downloading(let received, let total):
                if let total, total > 0 {
                    ProgressView(value: min(Double(received) / Double(total), 1))
                    Text("下载中 \(Int(min(Double(received) / Double(total), 1) * 100))% · \(bytes(received)) / \(bytes(total))")
                } else {
                    ProgressView()
                    Text("下载中 · \(bytes(received))")
                }
                Button("取消下载") { updater.cancelAction?() }
            case .extracting(let progress):
                ProgressView(value: max(0, min(progress, 1)))
                Text("正在校验并准备更新…")
            case .ready:
                Text("更新已就绪，重启后完成更新。")
                HStack {
                    Button("重启并更新") { updater.restartAction?() }
                    Button("稍后") { updater.noticeVisible = false }
                }
            case .installing:
                ProgressView()
                Text("正在安装更新…")
            case .failed(let detail):
                Text("更新未完成：\(detail)").foregroundStyle(.secondary)
                HStack {
                    Button("重新检查") { updater.checkNow() }
                    downloadLink
                }
            case .idle:
                if let remote {
                    Text("\(remote.version) · \(String(remote.gitCommit.prefix(12)))")
                    HStack {
                        Button("下载更新") { updater.download() }
                        downloadLink
                    }
                    if remote.installBlocked {
                        Text("此发布未配置更新签名，需手动下载安装。")
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
        .font(.system(size: 12))
        .controlSize(.small)
        .fixedSize(horizontal: false, vertical: true)
    }

    @ViewBuilder private var downloadLink: some View {
        if let url = remote?.downloadURL {
            Button("打开下载页") { NSWorkspace.shared.open(url) }
        }
    }

    private func bytes(_ count: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: count), countStyle: .file)
    }
}
