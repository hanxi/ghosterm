import AppKit
import GhostermAppModel
import SwiftUI
import UniformTypeIdentifiers

let workspaceLocalFileType = "com.ghosterm.local-files"
let workspaceRemoteFileType = "com.ghosterm.remote-files"

struct WorkspaceTitleDrag: ViewModifier {
    let provider: (() -> NSItemProvider)?
    @ViewBuilder func body(content: Content) -> some View {
        if let provider { content.overlay(WorkspaceDragSource(provider: provider)).help("拖动以调整窗格位置") }
        else { content }
    }
}

struct WorkspaceHiddenHandle: View {
    @ObservedObject var tab: TerminalTab
    let paneID: UUID
    @State private var hover = false
    var body: some View {
        Color.primary.opacity(hover ? 0.12 : 0)
            .frame(height: 6).contentShape(Rectangle())
            .onHover { hover = $0 }
            .modifier(WorkspaceTitleDrag(provider: { tab.zoomedPaneID = nil; return workspaceDragProvider(tab: tab, paneID: paneID) }))
            .help("拖动以调整窗格位置")
    }
}

struct WorkspaceFileView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tab: TerminalTab
    @ObservedObject var file: WorkspaceFilePane
    let isFocused: Bool
    let opts: PaneChromeOptions
    var chromeChanged: () -> Void = {}
    var isVisible = true
    var showsActions = false
    var preview: (PaneLayout.Axis?) -> Void = { _ in }
    private var actions: PaneActionsView {
        PaneActionsView(model: model, tab: tab, paneID: file.id, revealed: isFocused || showsActions, isAvailable: isVisible, preview: preview)
    }
    var body: some View {
        PaneCard(gap: opts.gap, isFocused: isFocused, isSplit: tab.isSplit,
                 palette: ChromePalette(model: model)) {
            VStack(spacing: 0) {
                if opts.showsTitleBar { title }
                else { WorkspaceHiddenHandle(tab: tab, paneID: file.id).contextMenu { PaneContextMenu(actions: actions) } }
                if let panel = file.panel {
                    FilesPanelView(panel: panel, downloadTo: { download($0) }, uploadFrom: { uploadPicker(panel) },
                                   side: file.remote ? .remote : .local,
                                   quickLook: file.remote ? { e in
                                       if let b = panel.backend as? TransferBackend { QuickLook.previewRemote(entry: e, backend: b) }
                                   } : { e in QuickLook.toggle(localPath: e.path) },
                                   workspaceTab: tab, workspaceFile: file,
                                   onChromeHeightChange: {
                                       file.reportChromeHeight($0)
                                       chromeChanged()
                                   })
                } else {
                    VStack(spacing: 12) {
                        if let error = file.error { Text(error).foregroundStyle(.red).textSelection(.enabled) }
                        else { ProgressView(); Text("正在连接文件窗格…") }
                    }.padding(12).frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .background(Color(nsColor: .textBackgroundColor))
        }
    }
    private var title: some View {
        HStack(spacing: 4) {
            HStack(spacing: 4) {
                Image(systemName: file.remote ? "network" : "folder")
                Text(file.remote ? file.panel?.backend.displayName ?? "远端文件" : "本地文件")
                    .lineLimit(1).truncationMode(.middle)
                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
            .modifier(WorkspaceTitleDrag(provider: { tab.zoomedPaneID = nil; return workspaceDragProvider(tab: tab, paneID: file.id) }))
            .help("拖动以调整窗格位置")
            actions
        }
        .font(DS.Font.captionEmphasis).padding(.leading, 8).padding(.trailing, 4)
        .frame(height: PaneTitleBar.height)
        .modifier(ChromeSurface(palette: ChromePalette(model: model)))
        .contextMenu { PaneContextMenu(actions: actions) }
    }
    private func download(_ entry: FileEntry) {
        guard let b = file.panel?.backend as? TransferBackend else { return }
        let target = tab.localFiles?.panel
        let root = target?.currentPath ?? (FilePaths.defaultDownloadPath(for: entry.name) as NSString).deletingLastPathComponent
        TransferManager.shared.enqueueDownload(b, remote: entry.path,
            local: (root as NSString).appendingPathComponent(entry.name), name: entry.name,
            isDirectory: entry.isDirectory, onFinish: { [weak target] status in if status == .done { target?.refresh() } })
        TransferWindow.open(manager: .shared)
    }
    private func uploadPicker(_ panel: FilesPanelModel) {
        guard let b = panel.backend as? TransferBackend else { return }
        let open = NSOpenPanel(); open.canChooseDirectories = true; open.allowsMultipleSelection = true
        guard open.runModal() == .OK else { return }
        for url in open.urls {
            var dir: ObjCBool = false
            guard FileManager.default.fileExists(atPath: url.path, isDirectory: &dir) else { continue }
            TransferManager.shared.enqueueUpload(b, local: url.path,
                remote: (panel.currentPath as NSString).appendingPathComponent(url.lastPathComponent),
                name: url.lastPathComponent, isDirectory: dir.boolValue,
                onFinish: { [weak panel] status in if status == .done { panel?.refresh() } })
        }
        TransferWindow.open(manager: .shared)
    }
}

struct WorkspaceFileDrop: DropDelegate {
    let tab: TerminalTab
    let target: WorkspaceFilePane
    let directory: String
    private var accepted: [UTType] {
        target.remote ? [UTType(importedAs: workspaceLocalFileType), .fileURL] : [UTType(importedAs: workspaceRemoteFileType)]
    }
    func validateDrop(info: DropInfo) -> Bool {
        guard target.panel != nil else { return false }
        let type = target.remote ? workspaceLocalFileType : workspaceRemoteFileType
        return info.itemProviders(for: accepted).contains { provider in
            if provider.hasItemConformingToTypeIdentifier(type) {
                let ids = (provider.suggestedName ?? "").split(separator: ":")
                guard ids.count == 2, let paneID = UUID(uuidString: String(ids[0])),
                      let endpointID = UUID(uuidString: String(ids[1])) else { return false }
                return tab.filePanes.contains { $0.id == paneID && $0.endpointID == endpointID && $0.panel != nil && $0.remote != target.remote }
            }
            return target.remote && provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier)
        }
    }
    func dropUpdated(info: DropInfo) -> DropProposal? { DropProposal(operation: validateDrop(info: info) ? .copy : .forbidden) }
    func performDrop(info: DropInfo) -> Bool {
        guard validateDrop(info: info) else { return false }
        let snapshot = directory
        for provider in info.itemProviders(for: accepted) {
            let internalType = target.remote ? workspaceLocalFileType : workspaceRemoteFileType
            if provider.hasItemConformingToTypeIdentifier(internalType) {
                provider.loadDataRepresentation(forTypeIdentifier: internalType) { data, _ in
                    guard let data, let drag = try? JSONDecoder().decode(WorkspaceFileDrag.self, from: data) else { return }
                    DispatchQueue.main.async {
                        if WorkspaceFileRouter.perform(drag, onto: target, directory: snapshot, in: tab) {
                            TransferWindow.open(manager: .shared)
                        } else { target.panel?.reportError("文件拖放源已失效或不支持该方向") }
                    }
                }
            } else if target.remote {
                provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier) { value, _ in
                    let url = (value as? URL) ?? (value as? Data).flatMap { URL(dataRepresentation: $0, relativeTo: nil) }
                    guard let url, url.isFileURL else { return }
                    DispatchQueue.main.async {
                        guard let panel = target.panel, let b = panel.backend as? TransferBackend,
                              tab.filePanes.contains(where: { $0 === target }) else { return }
                        var dir: ObjCBool = false
                        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &dir) else { return }
                        TransferManager.shared.enqueueUpload(b, local: url.path,
                            remote: (snapshot as NSString).appendingPathComponent(url.lastPathComponent),
                            name: url.lastPathComponent, isDirectory: dir.boolValue,
                            onFinish: { [weak panel] status in if status == .done { panel?.refresh() } })
                        TransferWindow.open(manager: .shared)
                    }
                }
            }
        }
        return true
    }
}

struct WorkspaceFileDropModifier: ViewModifier {
    let tab: TerminalTab?
    let file: WorkspaceFilePane?
    let directory: String
    @ViewBuilder func body(content: Content) -> some View {
        if let tab, let file { content.onDrop(of: [UTType(importedAs: workspaceLocalFileType), UTType(importedAs: workspaceRemoteFileType), .fileURL],
                                             delegate: WorkspaceFileDrop(tab: tab, target: file, directory: directory)) }
        else { content }
    }
}
