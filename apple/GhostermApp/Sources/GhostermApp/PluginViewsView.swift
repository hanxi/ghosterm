import AppKit
import GhostermAppModel
import SwiftUI

/// Generic standard components, keyed by Native registration rather than a
/// command name. Removed handles unmount their polling tasks and focus targets.
struct PluginViewsView: View {
    @ObservedObject var model: PluginViewsModel
    @FocusState private var focusedHandle: String?
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            if let issue = model.issue { Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-views-issue") }
            if model.items.isEmpty && model.available { Text("当前没有打开的插件视图").accessibilityIdentifier("plugin-views-empty") }
            ForEach(model.items) { item in
                PluginRegisteredView(item: item, manager: model, focusedHandle: $focusedHandle)
            }
        }.accessibilityIdentifier("plugin-views")
            .onChange(of: model.items.map(\.id)) { ids in
                if let focusedHandle, !ids.contains(focusedHandle) { self.focusedHandle = ids.first }
            }
    }
}

private struct PluginRegisteredView: View {
    @ObservedObject var item: PluginViewItem
    @ObservedObject var manager: PluginViewsModel
    var focusedHandle: FocusState<String?>.Binding
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(item.registration.viewID).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    .accessibilityAddTraits(.isHeader)
                Spacer()
                Button(item.closing ? "正在关闭视图…" : "关闭视图") { Task { await manager.closeView(handle: item.id) } }
                    .disabled(!manager.available || item.closing)
                    .focused(focusedHandle, equals: item.id)
                    .accessibilityIdentifier("plugin-view-close")
                    .accessibilityLabel(item.closing ? "正在关闭视图 \(item.registration.viewID)" : "关闭视图 \(item.registration.viewID)")
                    .accessibilityHint("只关闭此视图，不取消执行端任务")
            }
            if let issue = item.closeIssue { Text(issue).foregroundStyle(.red).accessibilityIdentifier("plugin-view-close-issue") }
            switch item.content {
            case .table(let model): PluginTableView(model: model)
            case .diff(let model): PluginDiffView(model: model)
            case .task(let model): PluginTaskView(model: model)
            }
        }.padding(12).overlay(RoundedRectangle(cornerRadius: 6).stroke(.secondary.opacity(0.3)))
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("plugin-registered-view-" + item.id)
    }
}

extension View {
    func announcePluginStatus(_ message: String?) -> some View {
        onChange(of: message) { message in
            guard let message, !message.isEmpty, let app = NSApp, app.isActive else { return }
            NSAccessibility.post(element: app, notification: .announcementRequested,
                userInfo: [.announcement: message, .priority: NSAccessibilityPriorityLevel.medium.rawValue])
        }
    }
}
