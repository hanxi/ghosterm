import GhostermAppModel
import SwiftUI
import UniformTypeIdentifiers

let tabDragType = "com.ghosterm.tab"

struct TabInteraction: ViewModifier {
    @ObservedObject var model: AppModel
    @ObservedObject var tabs: TabManager
    @ObservedObject var tab: TerminalTab

    func body(content: Content) -> some View {
        content
            .contentShape(Rectangle())
            .onTapGesture { tabs.activate(tab) }
            .accessibilityAction { tabs.activate(tab) }
            .contextMenu { TabContextMenu(model: model, tabs: tabs, tab: tab) }
            .onDrag {
                let provider = NSItemProvider()
                let data = Data(tab.id.uuidString.utf8)
                provider.registerDataRepresentation(forTypeIdentifier: tabDragType, visibility: .ownProcess) { completion in
                    completion(data, nil)
                    return nil
                }
                return provider
            }
            .onDrop(of: [UTType(importedAs: tabDragType)], delegate: TabDropDelegate(tabs: tabs, targetID: tab.id))
    }
}

struct TabDropDelegate: DropDelegate {
    let tabs: TabManager
    let targetID: UUID
    func validateDrop(info: DropInfo) -> Bool { info.hasItemsConforming(to: [UTType(importedAs: tabDragType)]) }
    func dropUpdated(info: DropInfo) -> DropProposal? { DropProposal(operation: .move) }
    func performDrop(info: DropInfo) -> Bool {
        guard let provider = info.itemProviders(for: [UTType(importedAs: tabDragType)]).first else { return false }
        provider.loadDataRepresentation(forTypeIdentifier: tabDragType) { data, _ in
            guard let data, let text = String(data: data, encoding: .utf8), let id = UUID(uuidString: text) else { return }
            Task { @MainActor in Self.move(id, onto: targetID, in: tabs) }
        }
        return true
    }
    @MainActor static func move(_ id: UUID, onto targetID: UUID, in tabs: TabManager) {
        guard id != targetID, let tab = tabs.tabs.first(where: { $0.id == id }),
              let index = tabs.tabs.firstIndex(where: { $0.id == targetID }) else { return }
        tabs.move(tab, to: index)
    }
}
