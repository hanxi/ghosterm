// DesignSystem — 一处定义间距 / 圆角 / 字号角色 / 材质。
//
// 取法参照 iGhostVT 的 `Interface/Support/DesignSystem.swift`（用户指定的
// 参照实现，见 apple/README.md）：**界面代码只说 token 名**，不写裸的点值、
// 不拼 ad-hoc 的字体栈。每个刻度刻意小——想加新值时应先挑最近的档。
//
// 「字号角色」这层是这次新加的，理由不是好看：设置页里有一个**界面字号**
// 档位（外观页），要真生效就得让所有界面文字走同一个乘数。macOS 的
// SwiftUI **不认** `dynamicTypeSize`（实测：`.dynamicTypeSize(.xxLarge)`
// 渲染出来仍是 16pt），所以像 Catalyst 上的 iGhostVT 一样，自己乘。
import GhostermAppModel
import SwiftUI

enum DS {
    /// 统一间距（iGhostVT 的 4pt 基准）。
    enum Padding {
        /// 4 — 发丝级间隙：徽标内缩、胶囊内部。
        static let xs: CGFloat = 4
        /// 8 — 紧邻：图标到文字、条内控件之间。
        static let s: CGFloat = 8
        /// 12 — 控件内缩：行内边距、控件簇之间。
        static let m: CGFloat = 12
        /// 16 — 容器内缩：条、卡片、分组。
        static let l: CGFloat = 16
        /// 24 — 喘气：设置页的页边距、空态。
        static let xl: CGFloat = 24
    }

    enum Radius {
        /// 6 — 小点缀：色块、缩略图。
        static let s: CGFloat = 6
        /// 10 — 控件：按钮、行、图标井（我们原先叫 control，保留别名）。
        static let m: CGFloat = 10
        static let control: CGFloat = 10
        /// 14 — 容器：卡片、面板。
        static let l: CGFloat = 14
        static let card: CGFloat = 14
        static let capsule: CGFloat = 999
    }

    /// 文字角色。每个角色 = 一个系统文字样式 + 字重 + 设计（mono 等），
    /// 由 `View.font(_:)` 重载按当前**界面字号**解析成实际磅值。
    /// 基础磅值取平台的系统值（macOS 不随 Dynamic Type 变），乘数来自偏好。
    enum Font {
        /// 小节标题 / 卡片标题。
        case title
        /// 页内大标题（比 `title` 大一号）。
        case titleLarge
        /// 条按钮、独立控件字形。
        case control
        /// 强调的控件：确认按钮、填充按钮。
        case controlEmphasis
        /// 正文。
        case body
        /// 次要正文（比正文小一号）。
        case callout
        /// 列表行、芯片、次要正文。
        case label
        /// `label` 的强调变体。
        case labelEmphasis
        /// 脚注与解释文字（设置页每行下面那段灰字）。
        case detail
        /// 时间戳、计数、副标题。
        case caption
        /// 比 `caption` 再小一号（图标旁的角标数字）。
        case captionSmall
        /// 必须自己撑住的小控件：徽标、关闭字形（`caption` 加粗）。
        case captionEmphasis
        /// 单独占一行的较大符号。
        case symbol
        /// 空态里那个大符号。
        case heroSymbol
        /// 当代码画的一行文件：日志、配置文件。
        case code
        /// 代码行下的元信息。
        case codeCaption

        /// 取磅值的系统文字样式。用 `NSFont.preferredFont`：它给的就是
        /// macOS 自己的界面字号（body=13pt、callout=12pt 等），
        /// 比自己拍数字更贴系统。
        var textStyle: NSFont.TextStyle {
            switch self {
            case .title: return .headline
            case .titleLarge: return .title2
            case .control, .controlEmphasis, .body: return .body
            case .callout: return .callout
            case .label, .labelEmphasis: return .subheadline
            case .detail, .code: return .footnote
            case .caption, .captionEmphasis: return .caption1
            case .captionSmall, .codeCaption: return .caption2
            case .symbol: return .title3
            case .heroSymbol: return .largeTitle
            }
        }

        var weight: SwiftUI.Font.Weight {
            switch self {
            case .title, .controlEmphasis, .labelEmphasis, .captionEmphasis:
                return .semibold
            case .control: return .medium
            case .heroSymbol: return .light
            default: return .regular
            }
        }

        /// 用系统的等宽字体（`code` / `codeCaption`）。
        var design: SwiftUI.Font.Design {
            switch self {
            case .code, .codeCaption: return .monospaced
            default: return .default
            }
        }

        /// 角色在 `scale` 下的实际字体。
        func resolved(scale: CGFloat) -> SwiftUI.Font {
            let base = NSFont.preferredFont(forTextStyle: textStyle).pointSize
            return .system(size: base * scale, weight: weight, design: design)
        }
    }

    /// 顶部 / 底部条上的控件尺寸（点击目标不小于它）。
    static let controlSize: CGFloat = 28
    /// 标签条高度。
    static let tabStripHeight: CGFloat = 38
    /// 侧边栏默认宽度与可拖拽范围。
    static let sidebarWidth: CGFloat = 240
    static let compactSidebarWidth: CGFloat = 88
    static let sidebarMinWidth: CGFloat = 180
    static let sidebarMaxWidth: CGFloat = 360
    /// 设置窗口的内容宽度（所有页都按它排版，窗口高度随页高）。
    static let settingsWidth: CGFloat = 640
}

// MARK: - 界面字号（环境值 + 根修饰器）

private struct InterfaceTextScaleKey: EnvironmentKey {
    static let defaultValue: CGFloat = 1
}

extension EnvironmentValues {
    /// 界面文字乘数（外观页的「界面字号」档位）。1 = 系统基准。
    var interfaceTextScale: CGFloat {
        get { self[InterfaceTextScaleKey.self] }
        set { self[InterfaceTextScaleKey.self] = newValue }
    }
}

extension View {
    /// 按当前偏好给整棵子树装上界面字号。放在每个宿主视图的根上
    /// （主窗口与设置窗口各一次）——**不放**在终端面上：终端字号是
    /// Ghostty 配置的事（`font-size`），两者不是一回事。
    func interfaceTextSize() -> some View {
        modifier(InterfaceTextSizeModifier())
    }

    /// `.font(DS.Font.…)` 解析到的重载：按环境的界面字号取实际磅值。
    func font(_ role: DS.Font) -> some View {
        modifier(DSFontModifier(role: role))
    }
}

private struct InterfaceTextSizeModifier: ViewModifier {
    @AppStorage(InterfaceTextSize.key) private var step = InterfaceTextSize.defaultStep

    func body(content: Content) -> some View {
        content.environment(\.interfaceTextScale, InterfaceTextSize.scale(step: step))
    }
}

private struct DSFontModifier: ViewModifier {
    let role: DS.Font
    @Environment(\.interfaceTextScale) private var scale

    func body(content: Content) -> some View {
        content.font(role.resolved(scale: scale))
    }
}

/// 玻璃容器：macOS 13 用 `.ultraThinMaterial`（`glassEffect` 要 macOS 26）。
/// iGhostVT 的做法也是「一处包起来，别处不提材质名」。
struct GlassBar<Content: View>: View {
    var spacing: CGFloat = DS.Padding.s
    @ViewBuilder var content: Content

    var body: some View {
        HStack(spacing: spacing) { content }
            .padding(.horizontal, DS.Padding.s)
            .frame(height: WindowChrome.barHeight)
            .background(.ultraThinMaterial)
            // 空白处可拖动窗口（标题栏本职）——放在背景层，控件优先。
            .background(WindowDragArea())
    }
}

/// 圆形 / 胶囊玻璃按钮（条上的图标按钮统一用它）。
struct GlassIconButton: View {
    let systemName: String
    let help: String
    var isActive: Bool = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemName)
                .frame(width: DS.controlSize, height: DS.controlSize)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(isActive ? Color.accentColor : Color.primary)
        .background(
            RoundedRectangle(cornerRadius: DS.Radius.control)
                .fill(isActive ? Color.accentColor.opacity(0.15) : Color.clear)
        )
        .help(help)
        .accessibilityLabel(help)
    }
}