import Foundation

@MainActor public extension AppModel {
    /// The generic workspace owns the instance; command names do not become
    /// layout enum cases. Refocusing reuses its UUID and existing native hosts.
    func showExtensionWorkspace() {
        if let tab = tabs.tabs.first(where: { $0.workspacePanes.contains(where: { $0.id == extensionInstance.id }) }) {
            tabs.activate(tab); tab.activeWorkspacePaneID = extensionInstance.id; tab.zoomedPaneID = nil
            return
        }
        let tab = tabs.activeTab ?? TerminalTab(kind: .workspace)
        if !tabs.tabs.contains(where: { $0.id == tab.id }) { tabs.add(tab) }
        tab.addExtensionPane(extensionInstance)
        tabs.activate(tab)
    }
    func openLocalFiles(newTab: Bool = false) {
        let tab: TerminalTab
        if !newTab, let current = tabs.activeTab { tab = current }
        else { tab = TerminalTab(kind: .localFiles); tabs.add(tab) }
        if let existing = tab.localFiles {
            tab.filesPanelVisible = true; tab.activeWorkspacePaneID = existing.id; return
        }
        let root = tab.kind.isLocalShell ? tab.currentDirectory ?? NSHomeDirectory() : NSHomeDirectory()
        let panel = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
        if tab.kind.isLocalShell {
            panel.cwdProvider = { [weak tab] in tab?.currentDirectory }
            panel.followsTerminalCwd = true
        }
        tab.addFilePane(WorkspaceFilePane(remote: false, panel: panel)); panel.start()
    }
    func resetWorkspaceLayout() { tabs.activeTab?.restoreDefaultWorkspace() }
}
