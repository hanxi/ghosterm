// QuickLook — 空格预览（macOS）。
//
// 本地文件直接预览；远端文件先经传输队列下一个**临时副本**，落盘后再预览。
// 这条路径复用 TransferManager 的 onFinish 回调，不新增 core API。
// QLPreviewPanel 的弹出无法在无头环境断言（诚实标注：这一条无闸门）。
import AppKit
import GhostermAppModel
import UniformTypeIdentifiers

enum QuickLook {
    /// 远端预览的临时副本路径（纯函数，可断言）。
    static func tempPreviewPath(for name: String, token: String = UUID().uuidString) -> String {
        QuickLookPaths.tempPreviewPath(for: name, token: token)
    }

    /// 远端条目：下临时副本（经队列）→ 完成后预览。
    @MainActor
    static func previewRemote(entry: FileEntry, backend: TransferBackend) {
        let name = entry.name.isEmpty ? (entry.path as NSString).lastPathComponent : entry.name
        let dst = tempPreviewPath(for: name)
        try? FileManager.default.createDirectory(
            atPath: (dst as NSString).deletingLastPathComponent,
            withIntermediateDirectories: true)
        TransferManager.shared.enqueueDownload(
            backend, remote: entry.path, local: dst, name: name, isDirectory: false,
            onFinish: { status in
                if case .done = status { QuickLook.show(localPath: dst) }
            })
    }

    /// 本地路径预览。
    @MainActor
    static func show(localPath: String) {
        QuickLookPanel.shared.show([URL(fileURLWithPath: localPath)])
    }

    /// 空格键触发（面板用）。
    @MainActor
    static func toggle(localPath: String) {
        if let panel = QLPreviewPanel.shared(), panel.isVisible {
            panel.orderOut(nil)
        } else {
            show(localPath: localPath)
        }
    }
}

#if canImport(QuickLookUI)
import QuickLookUI

/// QLPreviewPanel 的数据源 / 响应者（进程内单例）。
final class QuickLookPanel: NSObject, QLPreviewPanelDataSource {
    static let shared = QuickLookPanel()
    private var urls: [URL] = []

    func show(_ urls: [URL]) {
        self.urls = urls
        guard let panel = QLPreviewPanel.shared() else { return }
        panel.dataSource = self
        panel.makeKeyAndOrderFront(nil)
    }

    func numberOfPreviewItems(in panel: QLPreviewPanel!) -> Int { urls.count }

    func previewPanel(_ panel: QLPreviewPanel!, previewItemAt index: Int) -> QLPreviewItem! {
        urls[index] as NSURL
    }
}
#else
/// 非 macOS（或没链接 QuickLookUI）：显式报「不支持」——不静默假装成功。
final class QuickLookPanel {
    static let shared = QuickLookPanel()
    func show(_ urls: [URL]) {
        NSLog("QuickLook 在此平台不可用（未链接 QuickLookUI）")
    }
}
#endif
