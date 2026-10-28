// GhostermApp — macOS 宿主（SwiftUI）。
//
// 顶层只是「窗口 + 侧边栏 + 标签条 + 终端面」的壳：所有能力来自
// GhostermAppModel（主机列表 / 标签页 / 会话）与 GhostermTerminal（surface）。
// 这里不碰 C，也不碰字节流（铁律 #1/#2）。
//
// 文件命名注意：入口 lambda 式的顶层代码会让 Swift 丢掉 `@main` 语义
// （实测：跨到第二个文件后报「'main' attribute cannot be used in a module
// that contains top-level code」）。所以启动走 **NSApplicationDelegate**
// （main.swift 是 `@main struct` 的唯一合法落点），这里只放 delegate 与
// SwiftUI 视图；冒烟模式（--local / --ssh-smoke / --config-check）在
// `app.run()` **之前**分派，理由见 `smokeMode`。
import AppKit
import GhostermAppModel
import GhostermCore
import GhostermTerminal
import SwiftUI

@main
struct GhostermAppMain {
    @MainActor
    static func main() {
        GhostermUI.run()
    }
}

enum GhostermUI {
    /// 无 UI 冒烟模式的判据。必须在 `app.run()` **之前**分派：走 run loop
    /// 就要先处理 `oapp` Apple event，而 AppKit 可能在那条路上弹模态
    /// （`NSPersistentUIRestorer promptToIgnorePersistentState`——本机有崩溃
    /// 记录时实测会起 `-[NSAlert runModal]`），无人值守的闸门会被永久卡住。
    /// 三个模式自己 `exit()`，永远不需要窗口。
    @MainActor
    static func smokeMode(_ args: [String]) -> Bool {
        guard args.count >= 2 else { return false }
        switch args[1] {
        case "--local":
            LocalSmoke.run(executable: args.count >= 3 ? args[2] : nil)
        case "--ssh-smoke" where args.count >= 4:
            SSHSmoke.run(configPath: args[2], pattern: args[3])
        case "--config-check" where args.count >= 3:
            ConfigSmoke.run(configPath: args[2])
        default:
            return false
        }
        return true
    }

    @MainActor
    static func run() {
        let app = NSApplication.shared
        if smokeMode(CommandLine.arguments) { return }
        app.setActivationPolicy(.regular)
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var model: AppModel?
    private var windowController: TabWindowController?
    /// 观察系统外观变化（跟随系统时终端配色要跟着换）。
    private var appearanceObserver: NSKeyValueObservation?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let model = AppModel()
        model.interaction = AlertInteraction(model: model)
        // 主菜单必须在窗口之前装好：没有它，⌘Q / ⌘, / ⌘V 这些标准快捷键
        // 一个都接不到（设置页里的文本框连粘贴都用不了）。
        AppMenu.install(model: model)
        // S4：把面板/传输的冲突询问接到 UI（弹「覆盖/跳过/重命名 + 应用到全部」）。
        installTransferConflictPrompt()
        let controller = TabWindowController(model: model, requestClose: { NSApp.terminate(nil) })
        let window = controller.window!
        window.center()
        controller.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.windowController = controller
        self.model = model

        // 外观偏好（浅色 / 深色 / 跟随系统）落到 AppKit；跟随系统时还要
        // 观察 `effectiveAppearance`——系统在跑的时候被切了，终端也得跟着换。
        model.appearance.applyToApplication()
        appearanceObserver = NSApp.observe(\.effectiveAppearance,
                                           options: [.new]) { [weak model] _, _ in
            MainActor.assumeIsolated { model?.applyColorSchemeToTerminal() }
        }

        // App 级焦点：告诉 libghostty 本 App 是不是活动 App（见 ghostty_host.h）。
        // 启动时 App 不一定已经 active，故这里显式按当前状态报一次；后续由
        // applicationDidBecomeActive / DidResignActive 两处维持。
        model.terminalApp?.setFocused(NSApp.isActive)

        // 终端配置 **无效** 时明确报一次（不然「改了字体没生效」无从查起）。
        if let err = model.terminalConfigError {
            let alert = NSAlert()
            alert.messageText = "终端配置无效"
            alert.informativeText = err + "\n\n（已改用引擎默认配置；"
                + "修好 \(model.settings.terminalConfigPath) 后重启即可。）"
            alert.alertStyle = .warning
            alert.addButton(withTitle: "好")
            alert.runModal()
        }

        // 启动行为：默认开一个本地 shell（可在设置里关掉）。
        model.startInitialSessions()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    /// 普通终端随应用结束；显式 tmux 会话只断开客户端。
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model, !model.shouldTerminate() else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = "退出 Ghosterm？"
        alert.informativeText = "退出会结束普通终端会话，并中断文件传输和正在建立的连接。显式 tmux 后台会话会继续运行。"
        alert.alertStyle = .warning
        alert.addButton(withTitle: "退出")
        alert.addButton(withTitle: "取消")
        return alert.runModal() == .alertFirstButtonReturn ? .terminateNow : .terminateCancel
    }

    /// 切回本 App：报「活动」。
    ///
    /// 注意：surface 级的「失焦/得焦」由各窗格的 TerminalHostView 自己观察
    /// 窗口 key 状态上报（见 TerminalSurfaceView），这里报的是 **App 级**焦点
    /// （语义不同，见 ghostty_host.h）。
    func applicationDidBecomeActive(_ notification: Notification) {
        model?.terminalApp?.setFocused(true)
    }

    /// 切走本 App：报「不活动」。
    func applicationDidResignActive(_ notification: Notification) {
        model?.terminalApp?.setFocused(false)
    }

    /// 退出前停掉所有会话（顺序：先停会话再释放 surface——读循环还在跑）。
    func applicationWillTerminate(_ notification: Notification) {
        model?.tabs.removeAll()
    }
}


/// S4：把全局传输队列的冲突询问接到一个模态对话框。
@MainActor
func installTransferConflictPrompt() {
    TransferManager.shared.conflictResolver = { name, direction in
        let alert = NSAlert()
        alert.messageText = "落点已存在：\(name)"
        alert.informativeText = direction == .download
            ? "本地已有同名文件。" : "远端已有同名文件。"
        alert.addButton(withTitle: "覆盖")
        alert.addButton(withTitle: "跳过")
        alert.addButton(withTitle: "保留两份")
        let applyBox = NSButton(checkboxWithTitle: "应用到全部", target: nil, action: nil)
        alert.accessoryView = applyBox
        let resp = alert.runModal()
        let choice: TransferManager.Conflict
        switch resp {
        case .alertFirstButtonReturn: choice = .overwrite
        case .alertSecondButtonReturn: choice = .skip
        default: choice = .rename
        }
        if applyBox.state == .on { TransferManager.shared.applyToAll = choice }
        return choice
    }
}
