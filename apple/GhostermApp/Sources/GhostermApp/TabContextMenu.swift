// TabContextMenu — 标签右键菜单（标签条胶囊 / 侧栏标签行**共用同一份**）。
//
// 为什么要有它：文件面板只能靠 ⌘⇧E 开合（用户实测「目前只有快捷键打开 SFTP
// 侧边栏」），分屏按钮又挤在顶栏里。右键标签是最自然的入口——「这条标签能做什么」
// 就落在「这条标签」上。两处标签入口（顶栏胶囊 / 侧栏行）用同一份菜单，免得
// 修了一处漏另一处。
//
// 菜单项上的快捷键（`.keyboardShortcut`）只是**显示**：实测右键菜单没打开时
// 它不参与按键派发（⌘W 仍由主菜单 `AppMenu` 接走），所以不会多出一份抢键的
// 落点。键位本身是 `AppMenu` 的那一份，`TabContextMenuTests` 拿 `ShortcutCatalog`
// 双向钉住——菜单里改了键而这里没跟上，断言会红。
//
// 一处口径差异（与系统 App 一致，不是 bug）：快捷键作用的是**当前**标签页，
// 菜单项作用的是**被右键的那条**。右键非当前标签时菜单上照样显示 ⌘W —— macOS
// 终端 / 浏览器的标签右键菜单都这么标。
import AppKit
import GhostermAppModel
import SwiftUI

struct TabContextMenu: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tabs: TabManager
    @ObservedObject var tab: TerminalTab

    var body: some View {
        // 内嵌文件面板：只有终端类标签页有（SFTP 标签页是双栏，不在此列）。
        if tab.canHaveFilesPanel {
            Button(tab.filesPanelVisible ? "隐藏文件面板" : "显示文件面板") {
                model.toggleFilesPanel(for: tab)
            }
            .keyboardShortcut("e", modifiers: [.command, .shift])
            Divider()
        }

        // 分屏：终端类、未锁定才给（锁定的意思就是「别动它」，与顶栏按钮同口径）。
        if !tab.workspacePanes.isEmpty, !tab.isLocked {
            Button("向右分屏") { model.split(tab, axis: .horizontal) }
                .keyboardShortcut("d", modifiers: .command)
            Button("向下分屏") { model.split(tab, axis: .vertical) }
                .keyboardShortcut("d", modifiers: [.command, .shift])
        }
        if tab.isSplit {
            // 标题固定不随缩放状态变：主菜单那条也是固定的，而
            // `ShortcutCatalog` 按**标题**与菜单对拍。缩没缩放看窗格标题栏上
            // 那个按钮（它会换成「还原」图标），不靠菜单文案。
            Button("缩放当前分屏") { model.toggleZoomActivePane() }
                .keyboardShortcut(.return, modifiers: [.command, .shift])
            Button("关闭当前分屏") { model.closePane(in: tab) }
        }
        if !tab.workspacePanes.isEmpty, !tab.isLocked || tab.isSplit {
            Divider()
        }

        Button("打开本地文件窗格") { tabs.activate(tab); model.openLocalFiles() }
        Button("恢复默认布局") { tab.restoreDefaultWorkspace() }
        Divider()

        // 整屏文本：从顶栏的 `⋯` 并过来（用户建议「菜单统一成一个更整洁」）。
        // 两条都有**可见后果**（写剪贴板 / 落盘，可能把远端内容带走），
        // 只在明确点了才做。
        if !tab.panes.isEmpty {
            Button("拷贝文本") { copyText() }
            Button("导出文本…") { exportText() }
            Divider()
        }

        Button(tab.isLocked ? "解锁标签页" : "锁定标签页") { tab.isLocked.toggle() }
        Divider()
        if !tab.panes.isEmpty {
            Button("断开并保留会话") { model.detachKeepingSession(tab) }
        }
        Button("关闭标签页") { tabs.requestClose(tab) }
            .keyboardShortcut("w", modifiers: .command)
    }

    /// 拷贝**被右键那条**标签的当前窗格整屏文本（不是「当前标签页」的）。
    private func copyText() {
        guard let text = tab.activePane?.surface?.readText(), !text.isEmpty else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    private func exportText() {
        guard let text = tab.activePane?.surface?.readText(), !text.isEmpty else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "\(tab.title).txt"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        try? text.write(to: url, atomically: true, encoding: .utf8)
    }
}
