import AppKit
import GhostermAppModel
import SwiftUI

struct WorkspaceExtensionView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tab: TerminalTab
    @ObservedObject var instance: ExtensionInstanceModel
    let isFocused: Bool
    let isVisible: Bool
    let opts: PaneChromeOptions
    var showsActions = false

    private var actions: PaneActionsView {
        PaneActionsView(model: model, tab: tab, paneID: instance.id,
                        revealed: isFocused || showsActions, isAvailable: isVisible)
    }
    var body: some View {
        PaneCard(gap: opts.gap, isFocused: isFocused, isSplit: tab.isSplit,
                 palette: ChromePalette(model: model)) {
            VStack(spacing: 0) {
                if opts.showsTitleBar { title }
                else { WorkspaceHiddenHandle(tab: tab, paneID: instance.id).contextMenu { PaneContextMenu(actions: actions) } }
                ScrollView {
                    ExtensionRunView(model: instance).padding(DS.Padding.m)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Color(nsColor: .textBackgroundColor))
            }
        }
        .accessibilityIdentifier("workspace-extension-\(instance.id)")
    }
    private var title: some View {
        HStack(spacing: 4) {
            HStack(spacing: 4) {
                Image(systemName: "puzzlepiece.extension")
                Text(instance.extensionRun?.command ?? "插件").lineLimit(1).truncationMode(.middle)
                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
            .modifier(WorkspaceTitleDrag(provider: {
                tab.zoomedPaneID = nil
                return workspaceDragProvider(tab: tab, paneID: instance.id)
            }))
            .onTapGesture(count: 2) { model.toggleZoom(tabID: tab.id, paneID: instance.id) }
            actions
        }
        .font(DS.Font.captionEmphasis).padding(.leading, 8).padding(.trailing, 4)
        .frame(height: PaneTitleBar.height)
        .modifier(ChromeSurface(palette: ChromePalette(model: model)))
        .contextMenu { PaneContextMenu(actions: actions) }
    }
}
