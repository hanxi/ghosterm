// SplitDivider — 分屏分隔线不画出来（拖拽保留）。
//
// 窗格已经有 PaneCard 的圆角边框，NSSplitView 再在两格中间画一条线就是
// 视觉噪音（bug.md：「已经有边框了，再加上分割线好丑」）。但分隔线同时
// 是拖拽调比例的手柄——不能移除，只能**不画**。
//
// macOS 26 实测（真上屏截窗逐像素验证，见 SplitDividerTests 的来历注释）：
// 这条 1pt 线既不是 `drawDivider(in:)` 画的，也不是 NSSplitDividerView 的
// drawRect 画的——两个钩子都动态 override 成空，线纹丝不动。它是
// NSSplitDividerView 挂在自己背衬层上的一条 **CALayer 子层**
// （backgroundColor = 黑 @ 9.8%，宽 1pt，贯穿全高；VSplitView 同理是横条）。
// 所以抹法是清掉那条子层（backgroundColor = nil + isHidden），
// **不动 divider view 本体**——hitTest 与光标矩形都在它身上，拖拽照旧。
//
// 时序：线子层要等第一次 layout 才挂上，故不能只在入树时清一次：
// 入树 async 一次 + 每次 SwiftUI update 再清（幂等；外观切换等场景靠
// SwiftUI 重渲染捎带触发）。实测清掉后强制重新 layout 线也不会回来。
//
// 平台差异诚实标注（铁律 #8）：以上机制是 macOS 26 上实测的形态；更早的
// macOS 走 `drawDivider(in:)` 的老路径时本 hider 不生效（找不到线子层，
// 什么都不做，不报错）——那属于外观差异，不是能力缺失。
import AppKit
import SwiftUI

/// 挂在分屏子项上：沿 superview 链找到外层 NSSplitView，把分隔线抹掉。
struct SplitDividerHider: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let probe = SplitDividerProbe()
        // makeNSView 时还没入树，向上走不到 NSSplitView：延到下一个 runloop。
        DispatchQueue.main.async { probe.hideDividerLines() }
        return probe
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        // 幂等重清：兜住「第一次 layout 晚于 makeNSView」与外观切换重画。
        DispatchQueue.main.async { (nsView as? SplitDividerProbe)?.hideDividerLines() }
    }

    /// 探针视图：不画东西、不挡鼠标，只负责找 split view 并清线。
    final class SplitDividerProbe: NSView {
        /// 探针铺满窗格背景（SwiftUI background 语义），绝不能拦点击。
        override func hitTest(_ point: NSPoint) -> NSView? { nil }

        /// 入树即清一次：线子层是**首次 layout** 时挂上的，入树回调比
        /// makeNSView 的 async 更贴近那个时刻；两条路都留着，幂等。
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            hideDividerLines()
        }

        func hideDividerLines() {
            var cur = superview
            while let v = cur {
                if let sv = v as? NSSplitView {
                    for sub in sv.subviews where Self.isDividerView(sub) {
                        Self.clearThinLines(in: sub)
                    }
                    return
                }
                cur = v.superview
            }
        }

        /// 私有类 NSSplitDividerView；实例可能再被 KVO/动态子类包一层，
        /// 故用 contains 而不是相等。
        static func isDividerView(_ v: NSView) -> Bool {
            NSStringFromClass(type(of: v)).contains("NSSplitDividerView")
        }

        /// 把背衬层里的「细线」子层清掉：任一维 ≤ 1.5pt 即是线
        /// （竖分屏宽 1pt，横分屏高 1pt）。别的子层（hover 高亮之类）不动。
        static func clearThinLines(in view: NSView) {
            guard let subs = view.layer?.sublayers else { return }
            for sl in subs where min(sl.frame.width, sl.frame.height) <= 1.5 {
                sl.backgroundColor = nil
                sl.isHidden = true
            }
        }
    }
}
