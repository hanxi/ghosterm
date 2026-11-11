// 自动化测试的后台窗口策略。
//
// 为什么需要老窗口：AppKit 只有在**真窗口**里才走完整 layout/display，
// `cacheDisplay`（像素断言）也要求视图有 backing store；无窗口的视图抓出来
// 是空白。但这些窗口没有任何理由出现在用户桌面上。
//
// 做法：禁止应用激活；显示前透明、离屏、后台排序。排序守卫覆盖后续重排和 sheet。
// 视图仍可布局和缓存绘制；这不是前台窗口/系统键盘交互验收。
// 配合每个 GUI 测试类启动时的 `setActivationPolicy(.prohibited)`，测试进程也
// 不进 Dock、不抢焦点。
import AppKit
import Darwin
import ObjectiveC.runtime

// 不标 @MainActor：部分测试类本身是 nonisolated，窗口 API 在这些用例里
// 本来就直接调用（见各测试的 window.orderFrontRegardless 原始写法）。
enum TestWindows {
    // Test-process-only guard also covers panels/sheets and later AppKit reorderings.
    private static let installOrderingGuard: Void = {
        let replacements: [(Selector, Selector)] = [
            (#selector(NSWindow.order(_:relativeTo:)), #selector(NSWindow.ghostermBackgroundOrder(_:relativeTo:))),
            (#selector(NSWindow.orderFrontRegardless), #selector(NSWindow.ghostermBackgroundOrderFrontRegardless)),
            (#selector(NSWindow.makeKeyAndOrderFront(_:)), #selector(NSWindow.ghostermBackgroundMakeKeyAndOrderFront(_:))),
        ]
        for (originalSelector, guardedSelector) in replacements {
            let original = class_getInstanceMethod(NSWindow.self, originalSelector)!
            let guarded = class_getInstanceMethod(NSWindow.self, guardedSelector)!
            method_exchangeImplementations(original, guarded)
        }
    }()

    /// 让产品窗口（关于 / 设置）也走离屏展示。幂等，测试类 setUp 里调用。
    static func enterAutomationMode() {
        _ = installOrderingGuard
        setenv("GHOSTERM_AUTOMATION_OFFSCREEN", "1", 1)
        NSApplication.shared.setActivationPolicy(.prohibited)
    }

    /// 远离所有屏幕（AppKit 不会把 `setFrameOrigin` 的窗口拽回屏内）。
    static let offscreenOrigin = NSPoint(x: -30_000, y: -30_000)

    /// 上屏但不可见；不抢 key window / 焦点。
    static func presentOffscreen(_ window: NSWindow) {
        enterAutomationMode()
        window.alphaValue = 0
        window.setFrameOrigin(offscreenOrigin)
        window.order(.below, relativeTo: 0)
    }

    /// 上屏但不可见，并成为 key window（first responder / 焦点路由测试用）。
    static func presentOffscreenAsKey(_ window: NSWindow) {
        enterAutomationMode()
        window.alphaValue = 0
        window.setFrameOrigin(offscreenOrigin)
        window.order(.below, relativeTo: 0)
        window.makeKey()
    }
}

private extension NSWindow {
    @objc func ghostermBackgroundOrder(_ place: NSWindow.OrderingMode, relativeTo otherWindow: Int) {
        // Runs before every ordering operation, including indirectly created sheets.
        // Preserve order-out; visible ordering stays transparent and behind other apps.
        if place == .out { ghostermBackgroundOrder(place, relativeTo: otherWindow); return }
        alphaValue = 0
        ghostermBackgroundOrder(.below, relativeTo: 0)
    }

    @objc func ghostermBackgroundOrderFrontRegardless() {
        alphaValue = 0
        ghostermBackgroundOrder(.below, relativeTo: 0)
    }

    @objc func ghostermBackgroundMakeKeyAndOrderFront(_ sender: Any?) {
        alphaValue = 0
        ghostermBackgroundOrder(.below, relativeTo: 0)
        makeKey()
    }
}
