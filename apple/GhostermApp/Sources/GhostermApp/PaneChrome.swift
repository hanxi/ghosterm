// PaneChrome — 窗格的卡片外壳与标题栏。
//
// 分屏之前，一块终端区里「哪一格是哪台机器、哪一格在跑东西」全靠记。
// 这里给每格加一圈卡片 + 一条标题栏（状态点 · 进程 · 目录 · 操作按钮），
// 把那些只存在于脑子里的信息画出来。
//
// 两种形态，由**间距**这一个偏好决定（见 PaneAppearance）：
//   间距 > 0：卡片有圆角、有一圈边框，格与格之间留白；
//   间距 = 0：退化成贴边——不画圆角、不画闲置边框（相邻两格的边会叠成
//            两像素的粗线），只留聚焦描边（NSSplitView 的分隔线已抹掉，
//            见 SplitDivider）。
// 这样「把偏好都调到最保守的一端」能原样回到加这套卡片之前的样子。
import GhostermAppModel
import SwiftUI

/// 窗格外观的两项偏好打成一包，沿布局树往下传（避免每层各读一遍 AppStorage）。
struct PaneChromeOptions {
    var gap: Int = PaneGap.standard
    var showsTitleBar: Bool = PaneTitleBar.visibleDefault
}

/// 窗格的卡片外壳：圆角 + 边框 + 外间距。
struct PaneCard<Content: View>: View {
    let gap: Int
    /// 这格是不是当前窗格（画强调色描边）。
    let isFocused: Bool
    /// 这个标签页分屏了没有。**没分屏就不画聚焦描边**——只有一格时
    /// 「哪格是当前」不是个问题，描边只是噪音（分屏前的老行为）。
    let isSplit: Bool
    @ViewBuilder var content: () -> Content

    var body: some View {
        let radius = PaneGap.cornerRadius(gap: gap)
        content()
            .clipShape(RoundedRectangle(cornerRadius: radius, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .strokeBorder(borderColor, lineWidth: 1)
                    .allowsHitTesting(false)
            }
            // 每格自己吃一半间距 → 格与格之间合起来正好是一个 gap，
            // 容器外再补一半就与窗口边也对齐（见 RootView.terminalContent）。
            .padding(CGFloat(gap) / 2)
    }

    private var borderColor: Color {
        if isFocused, isSplit { return Color.accentColor.opacity(0.8) }
        // 贴边形态下闲置边框会与邻格的叠成两像素，索性不画。
        if gap == 0 { return .clear }
        return Color(nsColor: .separatorColor)
    }
}

/// 窗格标题栏：状态点 · 进程/目录 · 程序角标 · 右侧操作按钮。
struct PaneTitleBarView: View {
    @ObservedObject var pane: TerminalPane
    /// 进程与目录都拿不到时回落到它（标签页标题）。
    let fallbackTitle: String
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
        .background {
            // 「哪格是当前」主要靠这条带：聚焦那格点强调色。
            // 判据是纯函数，视图与像素断言走同一条路（见 PaneFocusChrome）。
            Color(nsColor: .windowBackgroundColor)
                .overlay(Color.accentColor.opacity(
                    PaneFocusChrome.tintOpacity(isFocused: isFocused, isSplit: isSplit)))
        }
        .overlay(alignment: .bottom) { Divider() }
        .contextMenu { PaneContextMenu(actions: actions) }
    }

}
