import AppKit
import GhostermAppModel
import SwiftUI

struct PaneAction: Identifiable {
    let id: String
    let symbol: String
    let title: String
    var axis: PaneLayout.Axis?
    var enabled = true
    let perform: () -> Void
    var shortcut: KeyboardShortcut? {
        switch id {
        case "split-right": return KeyboardShortcut("d", modifiers: .command)
        case "split-down": return KeyboardShortcut("d", modifiers: [.command, .shift])
        case "zoom": return KeyboardShortcut(.return, modifiers: [.command, .shift])
        default: return nil
        }
    }
    var help: String {
        let keys = id == "split-right" ? "⌘D" : id == "split-down" ? "⇧⌘D" : id == "zoom" ? "⇧⌘↩" : ""
        return keys.isEmpty ? title : title + " " + keys
    }
}

/// 终端与文件窗格共用动作描述；按钮、右键菜单都只发送所属 UUID。
struct PaneActionsView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tab: TerminalTab
    let paneID: UUID
    let revealed: Bool
    var isAvailable = true
    var preview: (PaneLayout.Axis?) -> Void = { _ in }

    var items: [PaneAction] {
        let file = tab.workspacePanes.first { $0.id == paneID }?.files
        let canSplit = isAvailable && !tab.isLocked && (file == nil || file?.panel != nil)
        var result: [PaneAction] = []
        if file?.remote == true {
            result.append(PaneAction(id: "local-files", symbol: "folder.badge.plus", title: "打开本地文件窗格", enabled: isAvailable) {
                guard model.tabs.tabs.contains(where: { $0.id == tab.id }),
                      tab.visibleWorkspaceLayout?.paneIDs.contains(paneID) == true else { return }
                model.tabs.activate(tab); tab.activeWorkspacePaneID = paneID; model.openLocalFiles()
            })
        }
        result.append(PaneAction(id: "split-right", symbol: "square.split.2x1", title: "向右分屏", axis: .horizontal, enabled: canSplit) {
            model.split(tabID: tab.id, paneID: paneID, axis: .horizontal)
        })
        result.append(PaneAction(id: "split-down", symbol: "square.split.1x2", title: "向下分屏", axis: .vertical, enabled: canSplit) {
            model.split(tabID: tab.id, paneID: paneID, axis: .vertical)
        })
        if tab.isSplit {
            let zoomed = tab.zoomedPaneID == paneID
            result.append(PaneAction(id: "zoom", symbol: zoomed ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right",
                                     title: zoomed ? "还原分屏" : "缩放当前分屏", enabled: isAvailable) {
                model.toggleZoom(tabID: tab.id, paneID: paneID)
            })
        }
        if file != nil || tab.isSplit {
            result.append(PaneAction(id: "close", symbol: "xmark", title: file != nil ? "关闭文件窗格" : "关闭这个窗格", enabled: isAvailable) {
                model.closePane(tabID: tab.id, paneID: paneID)
            })
        }
        return result
    }

    var body: some View {
        PaneActionsRepresentable(items: items, ink: ChromePalette(model: model).foreground, paneID: paneID, revealed: revealed, preview: preview)
            .frame(width: CGFloat(items.count * 21 - 1), height: 20)
    }
}

struct PaneContextMenu: View {
    let actions: PaneActionsView
    var body: some View {
        ForEach(actions.items) { item in
            Button(item.title) { actions.preview(nil); item.perform() }
                .disabled(!item.enabled).keyboardShortcut(item.shortcut)
        }
    }
}

private struct PaneActionsRepresentable: NSViewRepresentable {
    let items: [PaneAction]
    let ink: NSColor
    let paneID: UUID
    let revealed: Bool
    let preview: (PaneLayout.Axis?) -> Void
    func makeNSView(context: Context) -> PaneActionsHost { PaneActionsHost() }
    func updateNSView(_ view: PaneActionsHost, context: Context) {
        view.update(items: items, paneID: paneID, revealed: revealed, preview: preview)
        view.buttons.forEach { $0.chromeInk = ink }
    }
}

/// 始终预留同一宽度；未显现时不接鼠标，但仍可通过原生键盘 / AX 聚焦按钮。
final class PaneActionsHost: NSView {
    private(set) var buttons: [PaneActionButton] = []
    private var revealed = false
    private var preview: (PaneLayout.Axis?) -> Void = { _ in }
    override var mouseDownCanMoveWindow: Bool { false }
    var controlsVisible: Bool { revealed || buttons.contains { $0.keyboardFocused } }

    func update(items: [PaneAction], paneID: UUID, revealed: Bool, preview: @escaping (PaneLayout.Axis?) -> Void) {
        self.revealed = revealed; self.preview = preview
        if buttons.map({ $0.descriptor?.id }) != items.map({ Optional($0.id) }) {
            buttons.forEach { $0.removeFromSuperview() }
            buttons = items.map { _ in PaneActionButton() }
            buttons.forEach(addSubview)
        }
        for (button, item) in zip(buttons, items) {
            button.descriptor = item
            button.identifier = NSUserInterfaceItemIdentifier("pane-\(paneID)-\(item.id)")
            button.image = NSImage(systemSymbolName: item.symbol, accessibilityDescription: item.title)
            button.toolTip = item.help
            button.setAccessibilityLabel(item.title)
            if button.isEnabled && !item.enabled && (button.hovered || button.keyboardFocused) { preview(nil) }
            button.isEnabled = item.enabled
            button.activityChanged = { [weak self] in self?.refreshActivity() }
            button.invoke = { [weak self] in self?.preview(nil); item.perform() }
        }
        refreshVisibility()
        needsLayout = true
    }
    override func layout() {
        super.layout()
        for (index, button) in buttons.enumerated() {
            button.frame = NSRect(x: index * 21, y: 0, width: 20, height: 20)
        }
    }
    override func hitTest(_ point: NSPoint) -> NSView? {
        controlsVisible ? super.hitTest(point) : nil
    }
    private func refreshVisibility() {
        buttons.forEach { $0.alphaValue = controlsVisible ? 1 : 0 }
    }
    private func refreshActivity() {
        refreshVisibility()
        let active = buttons.first { $0.hovered && $0.isEnabled } ?? buttons.first { $0.keyboardFocused && $0.isEnabled }
        preview(active?.descriptor?.axis)
    }
}

final class PaneActionButton: NSButton {
    var chromeInk: NSColor = .labelColor { didSet { updateAppearance() } }
    var descriptor: PaneAction?
    var invoke: (() -> Void)?
    var activityChanged: (() -> Void)?
    private(set) var hovered = false
    private(set) var keyboardFocused = false
    override var mouseDownCanMoveWindow: Bool { false }
    override var acceptsFirstResponder: Bool { isEnabled }
    override init(frame: NSRect) {
        super.init(frame: frame)
        isBordered = false; imagePosition = .imageOnly; contentTintColor = .secondaryLabelColor
        symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 11, weight: .regular)
        wantsLayer = true; layer?.cornerRadius = 4
        target = self; action = #selector(performAction)
        setButtonType(.momentaryChange)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func performAction() { invoke?() }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    override func mouseEntered(with event: NSEvent) { hovered = true; updateAppearance(); activityChanged?() }
    override func mouseExited(with event: NSEvent) { hovered = false; updateAppearance(); activityChanged?() }
    override func viewDidChangeEffectiveAppearance() { super.viewDidChangeEffectiveAppearance(); updateAppearance() }
    private func updateAppearance() {
        effectiveAppearance.performAsCurrentDrawingAppearance {
            layer?.backgroundColor = (hovered || keyboardFocused ? NSColor.controlAccentColor.withAlphaComponent(0.16) : .clear).cgColor
        }
        contentTintColor = chromeInk.withAlphaComponent(hovered || keyboardFocused ? 1 : 0.65)
    }
    override func becomeFirstResponder() -> Bool {
        let accepted = super.becomeFirstResponder()
        if accepted { keyboardFocused = true; updateAppearance(); activityChanged?() }
        return accepted
    }
    override func resignFirstResponder() -> Bool {
        let accepted = super.resignFirstResponder()
        if accepted { keyboardFocused = false; updateAppearance(); activityChanged?() }
        return accepted
    }
}
