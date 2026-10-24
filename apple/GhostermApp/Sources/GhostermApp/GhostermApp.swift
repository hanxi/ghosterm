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
// SwiftUI 视图；冒烟模式（--local / --ssh-smoke）也在 delegate 里分派。
import AppKit
import GhostermAppModel
import GhostermCore
import GhostermTerminal
import SwiftUI

@main
struct GhostermAppMain {
    static func main() {
        GhostermUI.run()
    }
}

enum GhostermUI {
    static func run() {
        let app = NSApplication.shared
        app.setActivationPolicy(.regular)
        let delegate = AppDelegate()
        app.delegate = delegate
        app.run()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow?
    private var model: AppModel?
    /// 观察系统外观变化（跟随系统时终端配色要跟着换）。
    private var appearanceObserver: NSKeyValueObservation?

    func applicationDidFinishLaunching(_ notification: Notification) {
        // 冒烟模式（无窗口 / 无 UI；闸门与调试用）。
        let args = CommandLine.arguments
        if args.count >= 2, args[1] == "--local" {
            LocalSmoke.run(executable: args.count >= 3 ? args[2] : nil)
            return
        }
        if args.count >= 4, args[1] == "--ssh-smoke" {
            SSHSmoke.run(configPath: args[2], pattern: args[3])
            return
        }
        if args.count >= 3, args[1] == "--config-check" {
            ConfigSmoke.run(configPath: args[2])
            return
        }

        if args.count == 4, args[1] == "--session-lifecycle-probe", ["create", "restore"].contains(args[3]) {
            SessionLifecycleProbe.run(configuration: args[2], restore: args[3] == "restore")
            return
        }

        let model = AppModel()
        model.interaction = AlertInteraction(model: model)
        // 主菜单必须在窗口之前装好：没有它，⌘Q / ⌘, / ⌘V 这些标准快捷键
        // 一个都接不到（设置页里的文本框连粘贴都用不了）。
        AppMenu.install(model: model)
        // S4：把面板/传输的冲突询问接到 UI（弹「覆盖/跳过/重命名 + 应用到全部」）。
        installTransferConflictPrompt()
        // `.fullSizeContentView` 必须在**创建时**就在 styleMask 里：事后
        // insert 进去窗口不会重新布局，标题栏照样占一条（实测：用户看到的
        // 就是「红绿灯在一条灰带上、标签条在它下面」）。
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1040, height: 680),
            styleMask: [.titled, .closable, .resizable, .miniaturizable,
                        .fullSizeContentView],
            backing: .buffered, defer: false
        )
        // 标题留空：AppKit 会把 CFBundleName（"Ghosterm"）当默认标题，而
        // 「标题栏即顶栏」这件事并不总是成立——实测**分屏**会触发 AppKit
        // 重新 tile 标题栏，把标题字画出来（用户看到的「Ghosterm」就在那）。
        // 我们不显示任何标题，空串是唯一对「连 AppKit 都别想画」都成立的值。
        window.title = ""
        window.contentView = makeRootHostingView(model: model)
        window.center()
        window.makeKeyAndOrderFront(nil)
        // 顶栏即标题栏：红绿灯挪进我们的条里，内容铺满整窗（见 WindowChrome）。
        WindowChrome.apply(to: window)
        NSApp.activate(ignoringOtherApps: true)
        // AppKit 分屏 / 改尺寸会重新 tile 标题栏（红绿灯归位、标题字复现）。
        // 挂上 delegate，在每次 resize 后把装束重新套一遍（见 WindowChrome.redress）。
        window.delegate = self
        self.window = window
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

    /// 已建立的终端脱离 GUI 后保留；仅为传输、连接准备或非持久化
    /// 会话确认退出。显式关闭标签页仍走独立终止操作。
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model, !model.shouldTerminate() else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = "退出 Ghosterm？"
        alert.informativeText = "退出会中断文件传输与正在建立的连接。已建立的终端会话会保留，可在下次启动时恢复。"
        alert.alertStyle = .warning
        alert.addButton(withTitle: "退出")
        alert.addButton(withTitle: "取消")
        return alert.runModal() == .alertFirstButtonReturn ? .terminateNow : .terminateCancel
    }

    /// 分屏 / 改尺寸后重装顶栏（红绿灯与标题字都会被 AppKit 重新摆一遍）。
    func windowDidResize(_ notification: Notification) {
        guard let window = notification.object as? NSWindow else { return }
        WindowChrome.redress(window)
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
