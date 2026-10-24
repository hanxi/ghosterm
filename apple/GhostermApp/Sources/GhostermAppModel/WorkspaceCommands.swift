import Foundation

@MainActor public extension AppModel {
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
