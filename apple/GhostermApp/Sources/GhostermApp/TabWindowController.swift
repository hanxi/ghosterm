import AppKit
import GhostermAppModel

/// 一个窗口承载所有标签。导航变化不替换 contentView，也不参与会话生命周期。
@MainActor
final class TabWindowController: NSWindowController, NSWindowDelegate {
    private let requestClose: () -> Void

    init(model: AppModel, window: NSWindow? = nil, requestClose: @escaping () -> Void) {
        self.requestClose = requestClose
        let window = window ?? NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1040, height: 680),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.tabbingMode = .disallowed
        window.contentView = makeRootHostingView(model: model)
        super.init(window: window)
        window.delegate = self
        WindowChrome.apply(to: window)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func showWindow(_ sender: Any?) {
        super.showWindow(sender)
        if let window { WindowChrome.redress(window) }
    }

    func windowDidResize(_ notification: Notification) {
        if let window { WindowChrome.redress(window) }
    }

    func windowDidExitFullScreen(_ notification: Notification) {
        if let window { WindowChrome.redress(window) }
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        // 关闭整个工作窗口与退出共用任务 / 传输确认，单个标签仍走 TabManager。
        requestClose()
        return false
    }
}
