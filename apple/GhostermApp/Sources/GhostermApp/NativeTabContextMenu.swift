import AppKit
import GhostermAppModel

/// Rebuild the native tab's menu from its current backing tab whenever it opens.
@MainActor
final class NativeTabContextMenu: NSObject, NSMenuDelegate {
    private weak var owner: NativeTitlebarWindow?
    private weak var view: NSView?
    private weak var model: AppModel?
    private var actions: [NativeMenuAction] = []

    init(owner: NativeTitlebarWindow, view: NSView, model: AppModel) {
        self.owner = owner; self.view = view; self.model = model
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems(); actions.removeAll()
        guard let owner, let view, let model,
              let id = owner.tabID(for: view),
              let tab = model.tabs.tabs.first(where: { $0.id == id }) else {
            let item = NSMenuItem(title: "标签页已不可用", action: nil, keyEquivalent: "")
            item.isEnabled = false; menu.addItem(item); return
        }
        func add(_ title: String, key: String = "", modifiers: NSEvent.ModifierFlags = [], _ action: @escaping () -> Void) {
            let target = NativeMenuAction(action)
            actions.append(target)
            let item = NSMenuItem(title: title, action: #selector(NativeMenuAction.invoke(_:)), keyEquivalent: key)
            item.keyEquivalentModifierMask = modifiers; item.target = target; menu.addItem(item)
        }
        if tab.canHaveFilesPanel {
            add(tab.filesPanelVisible ? "隐藏文件面板" : "显示文件面板", key: "e", modifiers: [.command, .shift]) { model.toggleFilesPanel(for: tab) }
            menu.addItem(.separator())
        }
        if !tab.workspacePanes.isEmpty && !tab.isLocked {
            add("向右分屏", key: "d", modifiers: .command) { model.split(tab, axis: .horizontal) }
            add("向下分屏", key: "d", modifiers: [.command, .shift]) { model.split(tab, axis: .vertical) }
        }
        if tab.isSplit {
            add("缩放当前分屏", key: "\r", modifiers: [.command, .shift]) { model.tabs.activate(tab); model.toggleZoomActivePane() }
            add("关闭当前分屏") { model.closePane(in: tab) }
        }
        if (!tab.workspacePanes.isEmpty && !tab.isLocked) || tab.isSplit { menu.addItem(.separator()) }
        if tab.kind.isTerminal {
            add("后台会话…") { model.showTmux(for: tab.kind.remoteTarget.map { .host($0.pattern) } ?? .localShell) }
        }
        add("打开本地文件窗格") { model.tabs.activate(tab); model.openLocalFiles() }
        add("恢复默认布局") { tab.restoreDefaultWorkspace() }
        menu.addItem(.separator())
        if !tab.panes.isEmpty {
            add("拷贝文本") {
                guard let text = tab.activePane?.surface?.readText(), !text.isEmpty else { return }
                NSPasteboard.general.clearContents(); NSPasteboard.general.setString(text, forType: .string)
            }
            add("导出文本…") {
                guard let text = tab.activePane?.surface?.readText(), !text.isEmpty else { return }
                let panel = NSSavePanel(); panel.nameFieldStringValue = "\(tab.title).txt"
                guard panel.runModal() == .OK, let url = panel.url else { return }
                try? text.write(to: url, atomically: true, encoding: .utf8)
            }
            menu.addItem(.separator())
        }
        add(tab.isLocked ? "解锁标签页" : "锁定标签页") { tab.isLocked.toggle() }
        menu.addItem(.separator())
        add("关闭标签页", key: "w", modifiers: .command) { model.tabs.requestClose(tab) }
    }
}

@MainActor
private final class NativeMenuAction: NSObject {
    let action: () -> Void
    init(_ action: @escaping () -> Void) { self.action = action }
    @objc func invoke(_ sender: Any?) { action() }
}
