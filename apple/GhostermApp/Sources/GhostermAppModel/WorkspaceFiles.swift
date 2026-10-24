import Foundation

public struct WorkspaceFileDrag: Codable, Equatable {
    public struct Entry: Codable, Equatable {
        public let path: String
        public let name: String
        public let isDirectory: Bool
        public init(path: String, name: String, isDirectory: Bool) {
            self.path = path; self.name = name; self.isDirectory = isDirectory
        }
    }
    public let paneID: UUID
    public let endpointID: UUID
    public let entries: [Entry]
    public init(paneID: UUID, endpointID: UUID, entries: [Entry]) {
        self.paneID = paneID; self.endpointID = endpointID; self.entries = entries
    }
}

@MainActor public enum WorkspaceFileRouter {
    /// 只从源注册表解析下载端点，绝不借用目标或最近使用的后端。
    public static func source(_ drag: WorkspaceFileDrag, in tab: TerminalTab) -> WorkspaceFilePane? {
        guard !drag.entries.isEmpty, drag.entries.allSatisfy({
            !$0.path.isEmpty && !$0.name.isEmpty && !$0.name.contains("/") && $0.name != "." && $0.name != ".."
        }), let pane = tab.filePanes.first(where: { $0.id == drag.paneID && $0.endpointID == drag.endpointID }),
              pane.panel != nil else { return nil }
        return pane
    }
    @discardableResult public static func perform(_ drag: WorkspaceFileDrag, onto target: WorkspaceFilePane,
                                                 directory: String, in tab: TerminalTab,
                                                 manager: TransferManager? = nil) -> Bool {
        let manager = manager ?? TransferManager.shared
        guard let source = source(drag, in: tab), source.remote != target.remote,
              tab.filePanes.contains(where: { $0 === target }), let targetPanel = target.panel else { return false }
        let backend = (source.remote ? source.panel?.backend : targetPanel.backend) as? TransferBackend
        guard let backend else { return false }
        for item in drag.entries {
            let dst = (directory as NSString).appendingPathComponent(item.name)
            let finish: (TransferManager.Status) -> Void = { [weak targetPanel] status in
                if status == .done { targetPanel?.refresh() }
            }
            if source.remote {
                manager.enqueueDownload(backend, remote: item.path, local: dst, name: item.name,
                                        isDirectory: item.isDirectory, onFinish: finish)
            } else {
                manager.enqueueUpload(backend, local: item.path, remote: dst, name: item.name,
                                      isDirectory: item.isDirectory, onFinish: finish)
            }
        }
        return true
    }
}
