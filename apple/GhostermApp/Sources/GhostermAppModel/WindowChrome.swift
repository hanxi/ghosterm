// WindowChrome — 把标题栏变成我们的顶栏（对齐 iGhostVT 的观感）。
//
// 要做到的效果（用户给的参照是 iGhostVT）：窗口**看不到系统标题栏**，
// 顶栏就是那条 38pt 高的自绘条，红绿灯直接嵌在它的垂直中心上，下面紧接
// 内容（侧边栏与终端从同一条线开始）。这正是 iGhostVT 的做法
// （`Application/CatalystWindowChrome.swift`），只是它是 Catalyst、用
// ObjC runtime 绕路；我们是真 AppKit，可以直说。
//
// 三件事必须一起做，缺一个都会露馅：
//   1. `.fullSizeContentView` + `titlebarAppearsTransparent` + 隐藏标题字：
//      内容铺满整窗（否则顶栏被压在系统标题栏下面，多出一条空带）；
//   2. `titlebarSeparatorStyle = .none`：去掉标题栏那条分隔线；
//   3. 把三个红绿灯**移进我们的顶栏**：系统默认位置是按系统标题栏高度
//      （约 28pt）算的，我们的条是 38pt——不移就会看着偏上、与内容对不齐。
//
// 为什么不用 `titlebarHeight` 之类的私有 API 调整标题栏高度：那是 AppKit
// 私有属性（`titlebarHeight` 只有 Catalyst 的 UINSWindow 才有对应物），
// 在真 AppKit 上只能靠移按钮 + 让内容铺满来实现同样的观感。
import AppKit
import SwiftUI

public enum WindowChrome {
    /// 顶栏高度。红绿灯按它的垂直中心摆，侧边栏与终端也从这条线以下开始。
    public static let barHeight: CGFloat = 38

    /// 把窗口收拾成「顶栏即标题栏」的样子。必须在窗口有内容、且已
    /// `makeKeyAndOrderFront` 之后调用一次（红绿灯的 superview 那时才存在）。
    /// 幂等：可以在窗口每次 resize / 重新 tile 之后再调一次（见 `redress`）。
    public static func apply(to window: NSWindow) {
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        // 连标题字也不留：AppKit 在**分屏 / 重新 tile 标题栏**时会把标题
        // 重新画出来（用户实测「分屏时左上角冒出 Ghosterm」），只靠
        // `titleVisibility = .hidden` 挡不住——清空标题才是「没东西可画」。
        window.title = ""
        window.styleMask.insert(.fullSizeContentView)
        if #available(macOS 11.0, *) {
            window.titlebarSeparatorStyle = .none
        }
        // 系统标题栏仍然存在（红绿灯住在它里面），只是不画字也不画背景。
        // 我们的顶栏画在内容区最上面，视觉上盖住它。
        window.isMovableByWindowBackground = false
        positionTrafficLights(in: window)
    }

    /// 把红绿灯挪到我们的顶栏垂直中心。
    ///
    /// 系统的默认位置按系统标题栏（约 28pt）算，我们的条是 38pt——
    /// 直接沿用会显得偏上。`superview.isFlipped` 要判：标题栏视图在
    /// 不同 macOS 版本里翻转属性不一致（实测本机不翻转，坐标原点在左下）。
    public static func positionTrafficLights(in window: NSWindow) {
        let topInset = barHeight / 2 - trafficLightHeight / 2
        for kind in [NSWindow.ButtonType.closeButton,
                     .miniaturizeButton, .zoomButton] {
            guard let button = window.standardWindowButton(kind),
                  let superview = button.superview else { continue }
            var frame = button.frame
            frame.origin.y = superview.isFlipped
                ? topInset
                : superview.bounds.height - topInset - frame.height
            button.setFrameOrigin(frame.origin)
        }
    }

    /// AppKit 每次 resize / 分屏都会重新 tile 标题栏：把红绿灯摆回系统位置、
    /// 也常把标题字与标题栏背景又画出来（iGhostVT 的
    /// `CatalystWindowChrome` 专门观察 `NSWindowDidResizeNotification` 复装，
    /// 原话：“AppKit tiles the title bar again on every resize”）。故这里
    /// 也监听 resize，把我们的装束重新套一遍。
    public static func redress(_ window: NSWindow) { apply(to: window) }

    /// 双击顶栏 = 系统的「双击标题栏」行为。
    ///
    /// 按系统设置走（系统设置 → 桌面与程序坞 → 「双击窗口的标题栏以」），
    /// 而不是我们自己拍一个：用户把「双击标题栏」设成最小化时，我们只认
    /// 缩放就成了「不听话」。键是 `AppleActionOnDoubleClick`
    /// （Maximize / Minimize / None），缺省=Maximize（Apple 的默认）。
    public static func performDoubleClickAction(on window: NSWindow?) {
        guard let window else { return }
        switch UserDefaults.standard.string(forKey: "AppleActionOnDoubleClick") {
        case "Minimize":
            window.performMiniaturize(nil)
        case "None":
            break // 用户显式关掉了这个行为：什么都不做，别自作主张
        default: // "Maximize" / 未设置
            window.performZoom(nil)
        }
    }

    /// 红绿灯按钮的直径（系统值是 14，取 14 让垂直居中算得干净）。
    private static let trafficLightHeight: CGFloat = 14

    /// 红绿灯右侧需要留出的宽度（三个 14pt 按钮 + 间距 + 左边距）。
    /// 顶栏左侧内容据此避让——不然侧边栏切换按钮会压在红绿灯上。
    public static let trafficLightsWidth: CGFloat = 78
}

/// 拖这条顶栏能移动窗口（标题栏的本职）。放在顶栏的**背景**里：
/// 控件盖在它上面，按到控件时控件赢；只有按在空白处才走到拖动。
///
/// iGhostVT 在 Catalyst 上得绕 UIKit 事件再转给 AppKit
/// （`dispatchTouchAsWindowMovement`）；我们本来就是 AppKit，直接
/// `performDrag(with:)` 即可。
public struct WindowDragArea: NSViewRepresentable {
    public init() {}
    public func makeNSView(context: Context) -> NSView { DragView() }
    public func updateNSView(_ nsView: NSView, context: Context) {}

    private final class DragView: NSView {
        override func mouseDown(with event: NSEvent) {
            // 双击走系统的「标题栏双击」行为（缩放 / 最小化，按系统设置）。
            if event.clickCount == 2 {
                WindowChrome.performDoubleClickAction(on: window)
                return
            }
            window?.performDrag(with: event)
        }
    }
}

/// 让顶栏上**盖住背景的那些控件**也认「双击标题栏」：它们有自己的
/// `contentShape`，会把事件吃掉，后面那层 `WindowDragArea` 收不到
/// （用户实测：右侧那片双击没反应——那里正是「当前标签」胶囊与标签条）。
public struct TitleBarDoubleClick: ViewModifier {
    public init() {}
    public func body(content: Content) -> some View {
        content.onTapGesture(count: 2) {
            WindowChrome.performDoubleClickAction(on: NSApp.keyWindow)
        }
    }
}

public extension View {
    /// 见 `TitleBarDoubleClick`。
    func titleBarDoubleClick() -> some View { modifier(TitleBarDoubleClick()) }
}
