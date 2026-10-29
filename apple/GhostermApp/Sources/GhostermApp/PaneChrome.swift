// PaneChrome — 无描边窗格与融入终端的标题栏。
// 间距为零时贴边，有留白时保留圆角；焦点只在标题前画短线。
import GhostermAppModel
import SwiftUI

/// 窗格外观的两项偏好打成一包，沿布局树往下传（避免每层各读一遍 AppStorage）。
struct PaneChromeOptions {
    var gap: Int = PaneGap.defaultValue
    var showsTitleBar: Bool = PaneTitleBar.visibleDefault
}

/// 窗格外壳仅负责裁切与间距，不叠加外框。
struct PaneCard<Content: View>: View {
    let gap: Int
    @ViewBuilder var content: () -> Content

    var body: some View {
        let radius = PaneGap.cornerRadius(gap: gap)
        content()
            .clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
            .padding(CGFloat(gap) / 2)
    }
}

/// 窗格标题栏：状态点 · 进程/目录 · 程序角标 · 右侧操作按钮。
struct PaneTitleBarView: View {
    @ObservedObject var pane: TerminalPane
    /// 进程与目录都拿不到时回落到它（标签页标题）。
    let fallbackTitle: String
    let palette: ChromePalette
    let isFocused: Bool
    let actions: PaneActionsView
    let isSplit: Bool
    var dragProvider: (() -> NSItemProvider)? = nil

    var body: some View {
        let title = pane.displayTitle(fallback: fallbackTitle)
        HStack(spacing: DS.Padding.xs) {
            PaneFocusMarker(isFocused: isFocused, isSplit: isSplit)
            StatusDot(status: pane.status)
            Text(title.name)
                .font(DS.Font.captionEmphasis)
                .lineLimit(1)
                .truncationMode(.middle)
                .foregroundStyle(isFocused ? Color.primary : Color.secondary)
                .modifier(WorkspaceTitleDrag(provider: dragProvider))
            if let detail = title.detail {
                Text(detail)
                    .font(DS.Font.caption)
                    .lineLimit(1)
                    .truncationMode(.head)      // 目录太长时砍前面，留下末级目录
                    .foregroundStyle(.secondary)
            }
            if let badge = ProgramStatusBadge(record: pane.programStatus.summary,
                                              terminalName: title.name) {
                ProgramStatusBadgeView(badge: badge)
            }
            Color.clear.frame(maxWidth: .infinity, minHeight: 20)
                .contentShape(Rectangle())
                .modifier(WorkspaceTitleDrag(provider: dragProvider))
            actions
        }
        .padding(.leading, DS.Padding.s)
        .padding(.trailing, DS.Padding.xs)
        .frame(height: PaneTitleBar.height)
        .modifier(ChromeSurface(palette: palette))
        .contextMenu { PaneContextMenu(actions: actions) }
    }

}

/// 固定占位，切焦点不推动标题和按钮；单窗格也不改变命中位置。
struct PaneFocusMarker: View {
    let isFocused: Bool
    let isSplit: Bool
    var body: some View {
        Capsule().fill(Color.accentColor)
            .frame(width: 16, height: 2)
            .opacity(PaneFocusChrome.showsMarker(isFocused: isFocused, isSplit: isSplit) ? 1 : 0)
            .accessibilityHidden(true)
    }
}
