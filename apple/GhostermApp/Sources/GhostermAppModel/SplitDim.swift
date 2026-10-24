// SplitDim — 「非聚焦窗格变暗」的遮罩参数（对齐上游 Ghostty 的
// `unfocused-split-opacity` / `unfocused-split-fill`）。
//
// 为什么单独一层：上游 macOS 的 `SurfaceView.swift` 也是这么做的——非聚焦的
// 分屏上盖一层**背景色**的矩形，而不聚焦的那个就不盖。用背景色（而不是黑）
// 是为了「褪向背景」：浅色主题下压黑会很难看。
//
// 规则抽成**纯函数**：既能离线断言（矩阵穷举），也保证视图层只做「有没有
// 遮罩」这一个判断，不重复实现语义。
import Foundation

/// 非聚焦窗格的变暗遮罩。`nil` = 不变暗。
public struct SplitDim: Equatable {
    /// 遮罩不透明度（0~1）。
    public let alpha: Double
    /// 遮罩颜色（0~1 分量）。
    public let red: Double
    public let green: Double
    public let blue: Double

    public struct RGB: Equatable {
        public let r: UInt8, g: UInt8, b: UInt8
        public init(r: UInt8, g: UInt8, b: UInt8) {
            self.r = r; self.g = g; self.b = b
        }
    }

    /// 从引擎里读到的三个值算出遮罩参数：
    ///
    /// - `opacity` —— `unfocused-split-opacity`：非聚焦分屏**保留**多少
    ///   不透明度（上游默认 0.7，引擎把它夹进 [0.15, 1]）；遮罩 alpha = 1 - 它。
    /// - `fill` —— `unfocused-split-fill`：遮罩颜色；**未设时用背景色**。
    /// - `background` —— `background`：上面的回落目标。
    ///
    /// 返回 `nil` 的两种情况（都不猜）：遮罩颜色一个都读不到；或 opacity ≥ 1
    /// （用户显式关掉了这个特性，上游口径）。
    public static func resolve(opacity: Double?,
                               fill: RGB?,
                               background: RGB?) -> SplitDim? {
        // 引擎的夹取口径与上游 Config.zig 一致（@min(1, @max(0.15, x))）。
        let kept = min(1, max(0.15, opacity ?? 0.7))
        guard kept < 1 else { return nil }
        guard let c = fill ?? background else { return nil }
        return SplitDim(alpha: 1 - kept,
                        red: Double(c.r) / 255,
                        green: Double(c.g) / 255,
                        blue: Double(c.b) / 255)
    }
}
