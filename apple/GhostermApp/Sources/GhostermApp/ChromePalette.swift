import AppKit
import GhostermAppModel
import SwiftUI

/// 导航与标题使用同一份配色。终端颜色取引擎已解析的配置，不再解析主题文件。
struct ChromePalette {
    let background: NSColor
    let foreground: NSColor
    let isDark: Bool

    @MainActor init(model: AppModel) {
        func color(_ key: String, fallback: NSColor) -> NSColor {
            guard let rgb = model.terminalApp?.color(key) else { return fallback }
            return NSColor(srgbRed: CGFloat(rgb.r) / 255, green: CGFloat(rgb.g) / 255,
                           blue: CGFloat(rgb.b) / 255, alpha: 1)
        }
        background = color("background", fallback: .windowBackgroundColor)
        foreground = color("foreground", fallback: .labelColor)
        let rgb = background.usingColorSpace(.sRGB) ?? background
        isDark = 0.2126 * rgb.redComponent + 0.7152 * rgb.greenComponent + 0.0722 * rgb.blueComponent < 0.5
    }
    var fill: Color { Color(nsColor: background) }
    var ink: Color { Color(nsColor: foreground) }
    var selection: Color { ink.opacity(isDark ? 0.09 : 0.10) }
    var separator: NSColor { foreground.withAlphaComponent(0.10) }
    var colorScheme: ColorScheme { isDark ? .dark : .light }
}

struct ChromeSurface: ViewModifier {
    let palette: ChromePalette
    func body(content: Content) -> some View {
        content.foregroundStyle(palette.ink)
            .background(palette.fill)
            .environment(\.colorScheme, palette.colorScheme)
    }
}
