// ThemePreview — 从配色文件里读出「一眼看得出它是哪套配色」的那几个颜色。
//
// 为什么单独一层：配色选择器要给每一项画**色块 + 调色板条**（照抄 iGhostVT
// 的 ThemeListView）。这些颜色就在配色文件里（Ghostty 的配置语法），
// 但它是**数据**不是我们的配置——解析成一个只读的小结构，UI 只读不算。
//
// 语法（见 vendor/ghostty/src/config/Config.zig 的 theme 一节）：
//   background = #1e1e2e        前景 / 背景各一行
//   palette = 0=#45475a         调色板，下标 0~7 是基本八色
// 解析失败 / 没有这些键时**显式返回 nil**（UI 画空块，而不是拿默认色假装）。
import Foundation

public struct ThemePreview: Equatable {
    public let background: String?
    public let foreground: String?
    /// 基本八色（0~7）；缺失的位置是 nil（画成前景色）。
    public let palette: [String?]

    /// 解析一份配色文件的原文。永远成功——读不到的字段就是 nil。
    public init(text: String) {
        var bg: String?
        var fg: String?
        var colors = [String?](repeating: nil, count: 8)
        for rawLine in text.split(separator: "\n", omittingEmptySubsequences: false) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let parts = line.split(separator: "=", maxSplits: 2).map {
                $0.trimmingCharacters(in: .whitespaces)
            }
            guard parts.count >= 2 else { continue }
            switch parts[0].lowercased() {
            case "background":
                bg = parts.count == 2 ? parts[1] : nil
            case "foreground":
                fg = parts.count == 2 ? parts[1] : nil
            case "palette":
                // `palette = 0=#45475a`（两段）或 `palette = 0=#45475a,1=…`
                let body = line.split(separator: "=", maxSplits: 1)
                    .dropFirst().first.map(String.init) ?? ""
                for entry in body.split(separator: ",") {
                    let kv = entry.split(separator: "=", maxSplits: 1)
                    guard kv.count == 2,
                          let idx = Int(kv[0].trimmingCharacters(in: .whitespaces)),
                          colors.indices.contains(idx) else { continue }
                    colors[idx] = kv[1].trimmingCharacters(in: .whitespaces)
                }
            default:
                break
            }
        }
        background = bg
        foreground = fg
        palette = colors
    }

    /// SwiftUI 里用的 RGBA 分量（0~1）。认不出的颜色串返回 nil。
    public static func rgba(_ hex: String?) -> (r: Double, g: Double, b: Double, a: Double)? {
        guard let hex else { return nil }
        var s = hex.trimmingCharacters(in: .whitespaces)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6 || s.count == 8, let v = UInt32(s, radix: 16) else { return nil }
        if s.count == 6 {
            return (Double((v >> 16) & 0xff) / 255,
                    Double((v >> 8) & 0xff) / 255,
                    Double(v & 0xff) / 255, 1)
        }
        return (Double((v >> 24) & 0xff) / 255,
                Double((v >> 16) & 0xff) / 255,
                Double((v >> 8) & 0xff) / 255,
                Double(v & 0xff) / 255)
    }
}