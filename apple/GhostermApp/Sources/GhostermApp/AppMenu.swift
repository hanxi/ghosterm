// AppMenu — 进程的主菜单栏。
//
// 为什么必须有（不是"顺手加一个"）：
//   1. 没有主菜单的 macOS 应用**接不到任何标准快捷键**——实测
//      `NSApp.mainMenu == nil`。⌘Q 退不出、⌘, 开不了设置，**文本框里的
//      ⌘V 粘贴也用不了**（Edit 菜单的 key equivalent 就是它的实现）；
//   2. 设置页「键盘」页要列快捷键，列出来的每一条都得真的有人接——
//      这个菜单就是那批快捷键的落点（`ShortcutCatalog` 是同一份清单，
//      改一处必须改另一处，注释里互相指着）。
//
// 快捷键的落点：菜单项用 AppKit 的 target/action，优先级高于第一响应者
// 的 keyDown，所以焦点在终端里时 ⌘T/⌘W 仍然由菜单接走；不带 ⌘ 的按键
// 不经过菜单，原样落到终端面（铁律：终端里的程序优先）。
import AppKit
import GhostermAppModel

/// 菜单项的动作落点。持有 AppModel 与窗口，把菜单点击翻译成模型调用。
@MainActor
final class AppCommands: NSObject {
    private weak var model: AppModel?

    init(model: AppModel) { self.model = model }

    /// ⌘T：**沿用当前标签页的类型**新建（本地→本地、ssh→同机、SFTP→同目标）。
    @objc func newTab(_ sender: Any?) { model?.newTabLikeActive() }

    @objc func newConnection(_ sender: Any?) {
        NotificationCenter.default.post(name: .ghostermNewConnection, object: nil)
    }

    @objc func manageHosts(_ sender: Any?) {
        NotificationCenter.default.post(name: .ghostermManageHosts, object: nil)
    }

    @objc func closeTab(_ sender: Any?) { model?.requestCloseActiveTab() }

    @objc func splitRight(_ sender: Any?) {
        model?.splitActiveTab(axis: .horizontal)
    }

    @objc func splitDown(_ sender: Any?) {
        model?.splitActiveTab(axis: .vertical)
    }

    @objc func closePane(_ sender: Any?) { model?.closeActivePane() }

    /// ⇧⌘↩：把当前窗格临时铺满整块终端区（再按一次还原）。
    @objc func toggleZoomPane(_ sender: Any?) { model?.toggleZoomActivePane() }

    /// 关于：开我们自己的关于窗口（不是系统那张标准面板——它列不出随包
    /// 组件的许可，见 `AboutWindow`）。
    @objc func showAbout(_ sender: Any?) { AboutWindow.open() }

    @objc func showSettings(_ sender: Any?) {
        NotificationCenter.default.post(name: .ghostermShowSettings, object: nil)
    }

    @objc func toggleTabPlacement(_ sender: Any?) { model?.toggleTabPlacement() }

    @objc func searchTabs(_ sender: Any?) {
        NotificationCenter.default.post(name: .ghostermSearchTabs, object: nil)
    }

    /// ⌘⇧E：切换当前标签页的内嵌文件面板。
    @objc func toggleFilesPanel(_ sender: Any?) { model?.toggleFilesPanel() }
    @objc func openLocalFiles(_ sender: Any?) { model?.openLocalFiles() }
    @objc func resetWorkspace(_ sender: Any?) { model?.resetWorkspaceLayout() }

    /// 打开全局传输队列窗口。
    @objc func showTransfers(_ sender: Any?) { TransferWindow.open(manager: .shared) }

    /// ⌘1…⌘9：切到第 N 个标签页（与其它终端的惯例一致）。
    @objc func selectTab(_ sender: Any?) {
        guard let item = sender as? NSMenuItem else { return }
        model?.tabs.activate(index: item.tag)
    }
}

enum AppMenu {
    /// 装好主菜单。在窗口与 model 都建好之后调一次。
    @MainActor
    static func install(model: AppModel) {
        let commands = AppCommands(model: model)
        let main = NSMenu()

        main.addItem(appMenuItem(commands: commands))
        main.addItem(fileMenuItem(commands: commands))
        main.addItem(editMenuItem())
        main.addItem(viewMenuItem(commands: commands, tabs: model.tabs))
        main.addItem(windowMenuItem(commands: commands))

        NSApp.mainMenu = main
        // commands 必须活到进程结束（菜单项只存 weak target）。
        objc_setAssociatedObject(NSApp, &AppMenu.commandsKey, commands,
                                 .OBJC_ASSOCIATION_RETAIN)
    }

    private static var commandsKey: UInt8 = 0

    // MARK: - 各菜单

    private static func appMenuItem(commands: AppCommands) -> NSMenuItem {
        let item = NSMenuItem()
        let menu = NSMenu(title: "Ghosterm")
        let about = NSMenuItem(title: "关于 Ghosterm",
                               action: #selector(AppCommands.showAbout(_:)),
                               keyEquivalent: "")
        about.target = commands
        menu.addItem(about)
        menu.addItem(.separator())
        let settings = NSMenuItem(title: "设置…",
                                  action: #selector(AppCommands.showSettings(_:)),
                                  keyEquivalent: ",")
        settings.target = commands
        menu.addItem(settings)
        menu.addItem(.separator())
        menu.addItem(withTitle: "隐藏 Ghosterm",
                     action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        menu.addItem(withTitle: "隐藏其它",
                     action: #selector(NSApplication.hideOtherApplications(_:)),
                     keyEquivalent: "h").keyEquivalentModifierMask = [.command, .option]
        menu.addItem(.separator())
        menu.addItem(withTitle: "退出 Ghosterm",
                     action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = menu
        return item
    }

    private static func fileMenuItem(commands: AppCommands) -> NSMenuItem {
        let item = NSMenuItem()
        let menu = NSMenu(title: "文件")

        let newTab = NSMenuItem(title: "新建标签页",
                                action: #selector(AppCommands.newTab(_:)),
                                keyEquivalent: "t")
        newTab.target = commands

        let closeTab = NSMenuItem(title: "关闭标签页",
                                  action: #selector(AppCommands.closeTab(_:)),
                                  keyEquivalent: "w")
        closeTab.target = commands

        // ⇧⌘W 关窗口（系统的 performClose 走 confirm-close 那一套）。
        let closeWindow = NSMenuItem(title: "关闭窗口",
                                     action: #selector(NSWindow.performClose(_:)),
                                     keyEquivalent: "w")
        closeWindow.keyEquivalentModifierMask = [.command, .shift]

        menu.addItem(newTab)
        let connection = NSMenuItem(title: "新建连接…", action: #selector(AppCommands.newConnection(_:)), keyEquivalent: "")
        connection.target = commands
        menu.addItem(connection)
        let hosts = NSMenuItem(title: "管理 SSH 配置…", action: #selector(AppCommands.manageHosts(_:)), keyEquivalent: "")
        hosts.target = commands
        menu.addItem(hosts)
        menu.addItem(.separator())
        menu.addItem(closeTab)
        menu.addItem(closeWindow)
        item.submenu = menu
        return item
    }

    /// 编辑菜单：**标准项一个不能少**——设置页里的文本框（口令、自定义
    /// 路径、配置原文）靠它才有 ⌘A / ⌘C / ⌘V / ⌘Z。
    private static func editMenuItem() -> NSMenuItem {
        let item = NSMenuItem()
        let menu = NSMenu(title: "编辑")
        menu.addItem(withTitle: "撤销", action: Selector(("undo:")), keyEquivalent: "z")
        let redo = menu.addItem(withTitle: "重做", action: Selector(("redo:")),
                                keyEquivalent: "z")
        redo.keyEquivalentModifierMask = [.command, .shift]
        menu.addItem(.separator())
        menu.addItem(withTitle: "剪切", action: #selector(NSText.cut(_:)),
                     keyEquivalent: "x")
        menu.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)),
                     keyEquivalent: "c")
        menu.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)),
                     keyEquivalent: "v")
        menu.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)),
                     keyEquivalent: "a")
        item.submenu = menu
        return item
    }

    private static func viewMenuItem(commands: AppCommands, tabs: TabManager) -> NSMenuItem {
        let item = NSMenuItem()
        let menu = NSMenu(title: "显示")

        let right = NSMenuItem(title: "向右分屏",
                               action: #selector(AppCommands.splitRight(_:)),
                               keyEquivalent: "d")
        right.target = commands
        let down = NSMenuItem(title: "向下分屏",
                              action: #selector(AppCommands.splitDown(_:)),
                              keyEquivalent: "d")
        down.keyEquivalentModifierMask = [.command, .shift]
        // 没有 target 的菜单项会沿响应者链找人接——AppCommands 不在链上，
        // 于是 ⇧⌘D 与点这一项都**什么都不发生**（漏了这一行，别的项都有）。
        down.target = commands
        // 标题刻意不是「缩放」：窗口菜单里已经有一个系统的「缩放」
        // （performZoom，最大化窗口），两个同名项会让人按错。
        let zoomPane = NSMenuItem(title: "缩放当前分屏",
                                  action: #selector(AppCommands.toggleZoomPane(_:)),
                                  keyEquivalent: "\r")
        zoomPane.keyEquivalentModifierMask = [.command, .shift]
        zoomPane.target = commands
        let closePane = NSMenuItem(title: "关闭当前分屏",
                                   action: #selector(AppCommands.closePane(_:)),
                                   keyEquivalent: "")
        closePane.target = commands

        let sidebar = NSMenuItem(title: "切换标签位置",
                                 action: #selector(AppCommands.toggleTabPlacement(_:)),
                                 keyEquivalent: "s")
        // ⌥⌘S 而不是 ⌘S：⌘S 是各应用「存储」的通用键位，抢它会让用户
        // 在自己的 shell 里按 ⌘S 时莫名其妙收起侧边栏（设置页列的是 ⌥⌘S）。
        sidebar.keyEquivalentModifierMask = [.command, .option]
        sidebar.target = commands

        let filesPanel = NSMenuItem(title: "文件面板",
                                    action: #selector(AppCommands.toggleFilesPanel(_:)),
                                    keyEquivalent: "e")
        filesPanel.keyEquivalentModifierMask = [.command, .shift]
        filesPanel.target = commands

        menu.addItem(right)
        menu.addItem(down)
        menu.addItem(zoomPane)
        menu.addItem(closePane)
        menu.addItem(.separator())
        menu.addItem(filesPanel)
        let localFiles = NSMenuItem(title: "打开本地文件窗格", action: #selector(AppCommands.openLocalFiles(_:)), keyEquivalent: "")
        localFiles.target = commands
        menu.addItem(localFiles)
        let reset = NSMenuItem(title: "恢复默认布局", action: #selector(AppCommands.resetWorkspace(_:)), keyEquivalent: "")
        reset.target = commands
        menu.addItem(reset)
        menu.addItem(sidebar)
        menu.addItem(.separator())
        for i in 0..<9 {
            let switchTab = NSMenuItem(title: "标签页 \(i + 1)",
                                       action: #selector(AppCommands.selectTab(_:)),
                                       keyEquivalent: "\(i + 1)")
            switchTab.tag = i
            switchTab.target = commands
            menu.addItem(switchTab)
        }
        item.submenu = menu
        return item
    }

    private static func windowMenuItem(commands: AppCommands) -> NSMenuItem {
        let item = NSMenuItem()
        let menu = NSMenu(title: "窗口")
        let search = NSMenuItem(title: "搜索标签页…", action: #selector(AppCommands.searchTabs(_:)), keyEquivalent: "A")
        search.keyEquivalentModifierMask = [.command, .shift]
        search.target = commands
        menu.addItem(search)
        menu.addItem(.separator())
        let transfers = NSMenuItem(title: "传输",
                                   action: #selector(AppCommands.showTransfers(_:)),
                                   keyEquivalent: "")
        transfers.target = commands
        menu.addItem(transfers)
        menu.addItem(.separator())
        menu.addItem(withTitle: "最小化",
                     action: #selector(NSWindow.performMiniaturize(_:)),
                     keyEquivalent: "m")
        menu.addItem(withTitle: "缩放",
                     action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        item.submenu = menu
        NSApp.windowsMenu = menu
        return item
    }
}

extension Notification.Name {
    static let ghostermNewConnection = Notification.Name("Ghosterm.newConnection")
    static let ghostermManageHosts = Notification.Name("Ghosterm.manageHosts")
    /// 菜单「设置…」（⌘,）→ 由 RootView 打开设置面板。
    /// 与 `ghostermRequestCloseTab` 同一套路：菜单不发号施令，只把意图
    /// 广播给视图，结构的改动仍由视图/模型自己做。
    static let ghostermShowSettings = Notification.Name("Ghosterm.showSettings")
    static let ghostermSearchTabs = Notification.Name("Ghosterm.searchTabs")
}
