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
            // 保留 SwiftUI 的拖放参与者以维持 List / ScrollView 的原生点击
            // 路由，但标签主体不再提供拖动数据。真正的重排入口在
            // TabDragHandle，普通点击因此不会创建拖动会话。
            .onDrag { NSItemProvider() }
            .onDrop(of: [UTType(importedAs: tabDragType)], delegate: TabDropDelegate(tabs: tabs, targetID: tab.id))
    }
}

/// 标签重排的显式拖动把手。
///
/// SwiftUI 的 `.onDrag` 会在部分 macOS 版本的第一次点击后立刻启动拖动，
/// 与标签选择争抢同一个事件。把手用 AppKit 的 4pt 起拖阈值自行启动
/// `NSDraggingSession`，标签主体因此不会再拥有拖动手势。
struct TabDragHandle: View {
    @ObservedObject var tabs: TabManager
    @ObservedObject var tab: TerminalTab

    var body: some View {
        ZStack {
            Image(systemName: "line.3.horizontal")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(.secondary.opacity(0.65))
                .frame(width: 18, height: 24)
            TabDragSourceView(tabID: tab.id, tabs: tabs)
        }
        .frame(width: 18, height: 24)
        .help("拖动以重新排序标签页")
        .accessibilityLabel("拖动以重新排序标签页")
    }
}

private struct TabDragSourceView: NSViewRepresentable {
    let tabID: UUID
    let tabs: TabManager

    func makeNSView(context: Context) -> DragView { DragView(tabID: tabID, tabs: tabs) }
    func updateNSView(_ nsView: DragView, context: Context) {
        nsView.tabID = tabID
        nsView.tabs = tabs
    }

    final class DragView: NSView, NSDraggingSource {
        var tabID: UUID
        var tabs: TabManager
        private var downPoint: NSPoint?
        private var didStartDrag = false

        init(tabID: UUID, tabs: TabManager) {
            self.tabID = tabID
            self.tabs = tabs
            super.init(frame: .zero)
        }

        required init?(coder: NSCoder) { fatalError("init(coder:) 未实现") }

        override func mouseDown(with event: NSEvent) {
            downPoint = convert(event.locationInWindow, from: nil)
            didStartDrag = false
        }

        override func mouseDragged(with event: NSEvent) {
            guard !didStartDrag, let downPoint else { return }
            let point = convert(event.locationInWindow, from: nil)
            guard hypot(point.x - downPoint.x, point.y - downPoint.y) >= 4 else { return }
            guard let tab = tabs.tabs.first(where: { $0.id == tabID }) else { return }
            didStartDrag = true

            let pasteboardItem = NSPasteboardItem()
            pasteboardItem.setString(tab.id.uuidString,
                                     forType: NSPasteboard.PasteboardType(tabDragType))
            let item = NSDraggingItem(pasteboardWriter: pasteboardItem)
            item.setDraggingFrame(bounds,
                                  contents: NSImage(systemSymbolName: "rectangle.on.rectangle",
                                                    accessibilityDescription: "标签页"))
            beginDraggingSession(with: [item], event: event, source: self)
        }

        override func mouseUp(with event: NSEvent) {
            if !didStartDrag, let tab = tabs.tabs.first(where: { $0.id == tabID }) {
                tabs.activate(tab)
            }
            downPoint = nil
            didStartDrag = false
        }

        func draggingSession(_ session: NSDraggingSession,
                             sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
            .move
        }

        func draggingSession(_ session: NSDraggingSession,
                             endedAt screenPoint: NSPoint,
                             operation: NSDragOperation) {
            downPoint = nil
            didStartDrag = false
        }
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
