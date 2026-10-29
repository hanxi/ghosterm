import GhostermAppModel
import SwiftUI

/// 侧栏只展示已经打开的标签；SSH 配置通过新建连接与管理面板按需访问。
struct Sidebar: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tabs: TabManager

    var body: some View {
        VStack(spacing: 0) {
            Color.clear.frame(height: WindowChrome.barHeight)
                .background(WindowDragArea())
            ScrollViewReader { scroller in
                List {
                    ForEach(tabs.tabs) { tab in
                        TabRow(tab: tab, isActive: tab.id == tabs.activeTabID)
                            .id(tab.id)
                            .modifier(TabInteraction(model: model, tabs: tabs, tab: tab))
                            .listRowInsets(EdgeInsets(top: 3, leading: 6, bottom: 3, trailing: 6))
                    }
                    NewConnectionButton(showsTitle: true)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .frame(height: 32)
                        .listRowInsets(EdgeInsets(top: 4, leading: 14, bottom: 4, trailing: 14))
                }
                .listStyle(.sidebar)
                .onChange(of: tabs.activeTabID) { id in if let id { scroller.scrollTo(id) } }
            }
        }
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

/// 标签页行：状态点 + 标题 + 副标题 + 锁定标记 + 关闭。
struct TabRow: View {
    @ObservedObject var tab: TerminalTab
    let isActive: Bool
    @State private var hovering = false

    var body: some View {
        expandedBody
        .background(HoverTracking(hovering: $hovering, identifier: "tab-\(tab.id)"))
        .help("\(tab.title) · \(tab.subtitle)\n\(tab.status.label)")
        .accessibilityElement(children: .contain)
        .accessibilityLabel("\(tab.title)，\(tab.subtitle)，\(tab.status.label)")
    }

    private var expandedBody: some View {
        // 当前标签页要**看得出是它**：这行是「窗口现在在哪」的唯一标识
        // （iGhostVT 的 SidebarRow 同口径——中性 slab，不是强调色；
        // 强调色 slab 在终端底色上看起来像 bug 而不是选中）。
        HStack(spacing: DS.Padding.s) {
            Image(systemName: tab.kind.symbolName)
                .font(DS.Font.callout)
                .foregroundStyle(isActive ? .primary : .secondary)
                .frame(width: 30, height: 30)
                .background(RoundedRectangle(cornerRadius: 7).fill(Color.primary.opacity(0.06)))
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: DS.Padding.xs) {
                    Text(tab.title).lineLimit(1)
                        .fontWeight(isActive ? .semibold : .regular)
                    if tab.isLocked {
                        Image(systemName: "lock.fill")
                            .font(DS.Font.captionSmall).foregroundStyle(.secondary)
                    }
                }
                Text(tab.subtitle)
                    .font(DS.Font.caption).foregroundStyle(.tertiary).lineLimit(1)
                // blocked 的 message 是「为什么停下」——侧栏有地方，就显示出来
                // （文本已在模型层清理过不可见字符）。
                if let rec = tab.programStatusSummary, rec.state == .blocked {
                    Text(ProgramStatusBadge.clean(rec.message).isEmpty
                         ? "等待你处理" : ProgramStatusBadge.clean(rec.message))
                        .font(DS.Font.caption)
                        .foregroundStyle(.orange)
                        .lineLimit(1)
                }
                // 上一条命令（OSC 133）：命令行 / 耗时 / 退出码。
                // 失败的那条用红字——「刚才那条挂了」要看得见。
                if let lc = tab.lastCommandSummary {
                    Text(lc.sidebarLine)
                        .font(DS.Font.caption)
                        .foregroundStyle(lc.failed ? Color.red : Color.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .help(lc.sidebarLine)
                }
            }
            Spacer(minLength: 0)
            if let badge = ProgramStatusBadge(record: tab.programStatusSummary,
                                              terminalName: tab.title) {
                ProgramStatusBadgeView(badge: badge)
            }
            StatusDot(status: tab.status)
            TabCloseButton(tab: tab, visible: hovering)
        }
        .padding(.horizontal, DS.Padding.xs)
        .padding(.vertical, DS.Padding.xs)
        .background(
            RoundedRectangle(cornerRadius: DS.Radius.control)
                .fill(isActive ? Color.primary.opacity(0.12) : Color.clear)
        )
        .contentShape(Rectangle())

        .accessibilityAddTraits(isActive ? [.isSelected] : [])
    }
}

/// 状态点：连着是绿、连接中是黄、失败是红、退出是灰。
/// 三态不折叠（铁律 #6）——颜色只是提示，文字在标签条上。
struct StatusDot: View {
    let status: PaneStatus

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 8, height: 8)
            .help(status.label.isEmpty ? "已就绪" : status.label)
    }

    private var color: Color {
        switch status {
        case .ready: return .green
        case .connecting: return .yellow
        case .failed: return .red
        case .exited, .exitedUnknown: return .secondary
        }
    }
}
