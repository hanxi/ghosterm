// PaneChrome — 有间距时恢复细边框，当前分屏由顶部贯穿蓝线标示。
// 描边和焦点线都是不参与布局、命中的覆盖层，标题隐藏时仍然有效。
import GhostermAppModel
import SwiftUI

/// 窗格外观的两项偏好打成一包，沿布局树往下传（避免每层各读一遍 AppStorage）。
struct PaneChromeOptions {
    var gap: Int = PaneGap.defaultValue
    var showsTitleBar: Bool = PaneTitleBar.visibleDefault
}

/// 终端和文件窗格共用边界；贴边时只保留工作区的共享分隔线。
struct PaneCard<Content: View>: View {
    let gap: Int
    let isFocused: Bool
    let isSplit: Bool
    let palette: ChromePalette
    @ViewBuilder var content: () -> Content

    var body: some View {
        let radius = PaneGap.cornerRadius(gap: gap)
        let focused = PaneFocusChrome.showsFocus(isFocused: isFocused, isSplit: isSplit)
        content()
            .overlay {
                if gap > 0 {
                    RoundedRectangle(cornerRadius: radius, style: .continuous)
                        .strokeBorder(focused ? Color.accentColor.opacity(0.45)
                                              : palette.ink.opacity(0.15), lineWidth: 1)
                        .allowsHitTesting(false)
                }
            }
            .overlay(alignment: .top) {
                if focused {
                    Rectangle().fill(Color.accentColor).frame(height: 2)
                        .allowsHitTesting(false)
                }
            }
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
