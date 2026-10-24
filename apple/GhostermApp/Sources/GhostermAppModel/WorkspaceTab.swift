import Foundation

@MainActor public extension TerminalTab {
    func addFilePane(_ pane: WorkspaceFilePane, beside target: UUID? = nil, axis: PaneLayout.Axis = .horizontal) {
        workspacePanes.append(.files(pane))
        filesPanelVisible = true
        if !layoutCustomized, target == nil { restoreDefaultWorkspace() }
        else if let layout = workspaceLayout, let id = target ?? activeWorkspacePaneID {
            workspaceLayout = layout.replacing(id, with: .group([.leaf(id), .leaf(pane.id)], axis: axis))
        } else { workspaceLayout = .leaf(pane.id) }
        activeWorkspacePaneID = pane.id; zoomedPaneID = nil
    }
    func restoreDefaultWorkspace() {
        let terminals = workspacePanes.compactMap(\.terminal).map { WorkspaceLayout.leaf($0.id) }
        let local = filePanes.filter { !$0.remote }.map { WorkspaceLayout.leaf($0.id) }
        let remote = filePanes.filter(\.remote).map { WorkspaceLayout.leaf($0.id) }
        let files = local + remote
        if terminals.isEmpty {
            workspaceLayout = files.isEmpty ? nil : .group(files, axis: .horizontal)
        } else if files.isEmpty { workspaceLayout = .group(terminals, axis: .horizontal) }
        else {
            workspaceLayout = .group([.group(terminals, axis: .horizontal), .group(files, axis: .vertical)],
                                     axis: .horizontal, weights: [2, 1])
        }
        layoutCustomized = false; zoomedPaneID = nil
    }
    func duplicateFilePane(_ source: WorkspaceFilePane, axis: PaneLayout.Axis) {
        guard let panel = source.panel else { return }
        let copy = FilesPanelModel(backend: panel.backend, initialPath: panel.currentPath)
        copy.start()
        copy.viewMode = panel.viewMode; copy.showHidden = panel.showHidden
        addFilePane(WorkspaceFilePane(endpointID: source.endpointID, remote: source.remote, panel: copy),
                    beside: source.id, axis: axis)
        layoutCustomized = true
    }
    func removeFilePane(_ id: UUID) {
        guard workspacePanes.contains(where: { $0.id == id && $0.files != nil }) else { return }
        workspaceLayout = workspaceLayout?.removing(id, equalize: true)
        workspacePanes.removeAll { $0.id == id }
        if activeWorkspacePaneID == id { activeWorkspacePaneID = visibleWorkspaceLayout?.paneIDs.first }
        if zoomedPaneID == id || workspacePanes.count < 2 { zoomedPaneID = nil }
    }
    func toggleWorkspaceZoom(_ id: UUID) {
        guard isSplit, visibleWorkspaceLayout?.paneIDs.contains(id) == true else { zoomedPaneID = nil; return }
        zoomedPaneID = zoomedPaneID == id ? nil : id
        activeWorkspacePaneID = id
    }
    func dock(_ drag: WorkspacePaneDrag, onto target: UUID, edge: WorkspaceLayout.Edge,
              available: CGSize, minimum: (UUID) -> CGSize) -> Bool {
        guard drag.tabID == id, let next = workspaceLayout?.docking(source: drag.paneID, onto: target, edge: edge) else { return false }
        let size = next.minimumSize(minimum)
        guard size.width <= available.width, size.height <= available.height else { return false }
        workspaceLayout = next; layoutCustomized = true; zoomedPaneID = nil
        activeWorkspacePaneID = drag.paneID
        return true
    }
}
