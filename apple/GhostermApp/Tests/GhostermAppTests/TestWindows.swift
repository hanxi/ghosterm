// 自动化测试的后台窗口策略。
//
// 为什么需要老窗口：AppKit 只有在**真窗口**里才走完整 layout/display，
// `cacheDisplay`（像素断言）也要求视图有 backing store；无窗口的视图抓出来
// 是空白。但这些窗口没有任何理由出现在用户桌面上。
//
// 做法：把窗口移到所有屏幕之外再上屏——视图照常布局/绘制，用户什么也看不到。
// 配合每个 GUI 测试类启动时的 `setActivationPolicy(.accessory)`，测试进程也
// 不进 Dock、不抢焦点。
import AppKit
import Darwin

// 不标 @MainActor：部分测试类本身是 nonisolated，窗口 API 在这些用例里
// 本来就直接调用（见各测试的 window.orderFrontRegardless 原始写法）。
enum TestWindows {
    /// 让产品窗口（关于 / 设置）也走离屏展示。幂等，测试类 setUp 里调用。
    static func enterAutomationMode() {
        setenv("GHOSTERM_AUTOMATION_OFFSCREEN", "1", 1)
    }

    /// 远离所有屏幕（AppKit 不会把 `setFrameOrigin` 的窗口拽回屏内）。
    static let offscreenOrigin = NSPoint(x: -30_000, y: -30_000)

    /// 上屏但不可见；不抢 key window / 焦点。
    static func presentOffscreen(_ window: NSWindow) {
        window.setFrameOrigin(offscreenOrigin)
        window.orderFrontRegardless()
    }

    /// 上屏但不可见，并成为 key window（first responder / 焦点路由测试用）。
    static func presentOffscreenAsKey(_ window: NSWindow) {
        window.setFrameOrigin(offscreenOrigin)
        window.makeKeyAndOrderFront(nil)
    }
}
