// 产品窗口在自动化测试里的落点。
//
// 「关于」「设置」这些窗口在测试里是**被测对象**：要真的建、真的布局、真的
// 能抓帧，但没有任何理由出现在用户桌面上。测试进程设置
// GHOSTERM_AUTOMATION_OFFSCREEN=1 后，这里把窗口放到所有屏幕之外；生产路径
// 不设置该变量，行为完全不变（窗口照常居中、照常抢焦点）。
import AppKit

@MainActor
enum WindowPresentation {
    static var offscreenForAutomation: Bool {
        ProcessInfo.processInfo.environment["GHOSTERM_AUTOMATION_OFFSCREEN"] == "1"
    }

    /// 只摆位置，不显示（给 `showWindow` 这类会自己上屏的路径用）。
    static func prepare(_ window: NSWindow?) {
        guard offscreenForAutomation, let window else { return }
        window.setFrameOrigin(NSPoint(x: -30_000, y: -30_000))
    }

    /// 显示并置前；自动化测试里离屏。
    static func present(_ window: NSWindow) {
        prepare(window)
        window.makeKeyAndOrderFront(nil)
    }
}
