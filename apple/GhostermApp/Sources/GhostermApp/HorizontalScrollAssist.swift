// HorizontalScrollAssist — 让横向滚动区在 macOS 上「普通鼠标也滚得动」。
//
// 取法参照 iGhostVT 的 `TabStripBar.ChipScroller` 及其注释。它在 Mac 上踩过的
// 坑原话是：横向 `ScrollView` 会忽略只有纵向 delta 的鼠标滚轮，于是标签一多，
// 右边那些藏着不露的胶囊用普通鼠标就**够不着**（"the strip's hidden chips
// were out of reach"）。触控板本就能横滑，照旧交给 `ScrollView` 自己。
//
// 为什么拦在**事件分发层**而不是子视图的 `scrollWheel`（第一版就是那么写错的）：
// SwiftUI 的 `NSHostingView` 把 `hitTest` 收成它自己（实测：抓到的命中视图是
// `NSHostingView<RootView>`），放在滚动内容里的任何子视图都收不到滚轮事件——
// 自己写 `scrollWheel` 等于白写。所以在 `NSApplication` 分发之前挂一个
// **本地监视器**（local monitor），只对「落在本滚动区上、以纵向为主」的滚轮
// 事件下手；其余（触控板横滑、别处的滚动）原样放行。
import AppKit
import SwiftUI

/// 把鼠标滚轮的**纵向**滚动，转成所在横向 `ScrollView` 的横向位移。
///
/// 放在横向滚动内容里（`.background(...)`）：它沿 superview 链找到 SwiftUI 建的
/// `NSScrollView`，挂一个 `scrollWheel` 本地监视器，把滚轮的纵向 delta 加到横向
/// offset 上——就是 AppKit 标签栏的手感。横向手势（触控板两指横滑、shift+滚轮）
/// 原样放行，不抢系统的活。
///
/// 为什么是监视器而不是 `scrollWheel` 覆写：SwiftUI 的 hosting view 会吃掉
/// `hitTest`，滚动内容里的子视图根本收不到事件（见文件头）。
public struct WheelScrollsHorizontally: NSViewRepresentable {
    public init() {}

    public func makeNSView(context: Context) -> WheelView { WheelView() }
    public func updateNSView(_ nsView: WheelView, context: Context) {}

    public final class WheelView: NSView {
        private weak var scrollView: NSScrollView?
        private var monitor: Any?

        public override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let monitor { NSEvent.removeMonitor(monitor); self.monitor = nil }
            scrollView = nil
            guard window != nil else { return }
            var ancestor: NSView? = superview
            while let view = ancestor, !(view is NSScrollView) { ancestor = view.superview }
            guard let scrollView = ancestor as? NSScrollView else { return }
            self.scrollView = scrollView
            // 事件会先过这里再进 NSApplication 的正常分发。
            monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) {
                [weak self] event in
                guard let self else { return event }
                // 消费掉 → 返回 nil；否则原样放行。
                return self.handle(event) ? nil : event
            }
        }

        deinit { if let monitor { NSEvent.removeMonitor(monitor) } }

        /// 监视器入口：把滚轮换算到本窗口坐标，再交给 `wheelScroll`。
        ///
        /// **真机事件走 `event.locationInWindow`**——那是 AppKit 已经算好的窗口
        /// 坐标，最稳。这里踩过一次坑：为了「好测」改成 `event.cgEvent.location`
        /// 再用 `convertPoint(fromScreen:)`，但 `CGEvent.location` 是 **Quartz 屏
        /// 坐标（原点在主屏左上、y 向下）**，而 `fromScreen:` 收的是 **AppKit 屏
        /// 坐标（原点左下、y 向上）**——y 一翻，点就跑到标签条外面去了，真机上
        /// 直接滚不动（用户实测）。所以真机不绕这一圈。
        ///
        /// 合成事件没有 window（`locationInWindow` 不可信），才退到 cgEvent，
        /// 并且**先把 Quartz 坐标翻成 AppKit 坐标**再换算——单测据此走同一条路。
        public func handle(_ event: NSEvent) -> Bool {
            guard let scrollView, let window = scrollView.window else { return false }
            let inWindow: NSPoint
            if event.window === window {
                inWindow = event.locationInWindow
            } else if let cg = event.cgEvent {
                inWindow = window.convertPoint(fromScreen: Self.appKitScreenPoint(cg.location))
            } else {
                inWindow = window.convertPoint(fromScreen: NSEvent.mouseLocation)
            }
            return wheelScroll(deltaX: event.scrollingDeltaX, deltaY: event.scrollingDeltaY,
                               atWindowPoint: inWindow)
        }

        /// Quartz 屏坐标（`CGEvent.location`，原点在主屏左上、y 向下）→
        /// AppKit 屏坐标（`NSScreen`，原点在主屏左下、y 向上）。
        /// 关系就是绕主屏高度翻一次 y：`appKitY = 主屏高 - quartzY`。
        static func appKitScreenPoint(_ p: CGPoint) -> NSPoint {
            let primaryHeight = NSScreen.screens.first?.frame.height ?? 0
            return NSPoint(x: p.x, y: primaryHeight - p.y)
        }

        /// 监视器是否已挂上（单测断言用）。
        public var isMonitoring: Bool { monitor != nil }

        /// 核心：把**纵向为主**的滚轮在窗口坐标点 `point` 处转成横向位移。
        /// 返回 `true` 表示已消费（调用方返回 nil，不再往下发）。
        ///
        /// 拆成独立方法是为了能被直接断言（见 `TabStripScrollTests`）：`NSEvent`
        /// 没法在单测里造出「带 window」的真事件，但这条判定逻辑本身是确定的。
        public func wheelScroll(deltaX: CGFloat, deltaY: CGFloat,
                                atWindowPoint point: NSPoint) -> Bool {
            guard let scrollView, scrollView.window != nil else { return false }
            // 指针得落在这条滚动区上。
            let local = scrollView.convert(point, from: nil)
            guard scrollView.bounds.contains(local) else { return false }
            // 只认「纵向为主」的滚轮；横向手势交给 ScrollView 自己。
            guard abs(deltaY) > 0.01, abs(deltaX) <= abs(deltaY) else { return false }
            let clip = scrollView.contentView
            let contentWidth = scrollView.documentView?.frame.width ?? clip.bounds.width
            let maxX = max(0, contentWidth - clip.bounds.width)
            guard maxX > 0 else { return false }
            // 滚轮向下（deltaY < 0）→ 往右滚：与 AppKit 标签栏一致。
            let x = min(maxX, max(0, clip.bounds.origin.x - deltaY))
            clip.scroll(to: NSPoint(x: x, y: clip.bounds.origin.y))
            scrollView.reflectScrolledClipView(clip)
            return true
        }
    }
}

/// 装得下的横向滚动区不要橡皮筋（跟系统标签栏一致）。
///
/// `scrollBounceBehavior(_:axes:)` 是 macOS 13.3 起才有；更早的系统直接原样返回
/// （顶栏装得下时的手指回弹在旧系统上也无伤大雅）。
struct NoBounceWhenFits: ViewModifier {
    @ViewBuilder
    func body(content: Content) -> some View {
        if #available(macOS 13.3, *) {
            content.scrollBounceBehavior(.basedOnSize, axes: .horizontal)
        } else {
            content
        }
    }
}
