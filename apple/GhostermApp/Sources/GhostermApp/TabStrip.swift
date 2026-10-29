import GhostermAppModel
import SwiftUI

/// 顶部和侧边读取同一个 TabManager，窗口没有第二份标签列表或选中状态。
struct TabStrip: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tabs: TabManager

    var body: some View {
        NavigationBar {
            Color.clear.frame(width: WindowChrome.trafficLightsWidth, height: 1)
            ScrollViewReader { scroller in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 4) {
                        ForEach(tabs.tabs) { tab in
                            TabChip(tab: tab, isActive: tab.id == tabs.activeTabID, selection: ChromePalette(model: model).selection)
                                .id(tab.id)
                                .modifier(TabInteraction(model: model, tabs: tabs, tab: tab))
                        }
                    }
                    .padding(.vertical, 2)
                    .background(WheelScrollsHorizontally())
                }
                .modifier(NoBounceWhenFits())
                .frame(maxWidth: .infinity)
                .onChange(of: tabs.activeTabID) { id in if let id { scroller.scrollTo(id) } }
                .onAppear { if let id = tabs.activeTabID { scroller.scrollTo(id) } }
            }
            NewConnectionButton(tint: ChromePalette(model: model).foreground).frame(width: DS.controlSize, height: DS.controlSize)
        }
        .modifier(ChromeSurface(palette: ChromePalette(model: model)))
    }
}

struct TabChip: View {
    @ObservedObject var tab: TerminalTab
    let isActive: Bool
    var selection: Color = Color.primary.opacity(0.12)
    @State private var hovering = false

    var body: some View {
        HStack(spacing: DS.Padding.xs) {
            StatusDot(status: tab.status)
            Text(tab.title).lineLimit(1).truncationMode(.middle)
                .frame(maxWidth: .infinity)
            if let badge = ProgramStatusBadge(record: tab.programStatusSummary,
                                              terminalName: tab.title) {
                ProgramStatusBadgeView(badge: badge)
            }
            // 上一条命令失败（OSC 133 报了非 0 退出码）：一个小红点。
            // 「没报退出码」不画——三态不折叠（未知 ≠ 失败）。
            if let lc = tab.lastCommandSummary, lc.failed {
                Circle()
                    .fill(Color.red)
                    .frame(width: 6, height: 6)
                    .help("上一条命令失败（退出码 \(lc.exitCode ?? 0)）")
                    .accessibilityLabel("上一条命令失败，退出码 \(lc.exitCode ?? 0)")
            }
            if tab.isLocked {
                Image(systemName: "lock.fill").font(DS.Font.captionSmall)
                    .foregroundStyle(.secondary)
            }
            if tab.isSplit {
                Text("\(tab.workspacePanes.count)")
                    .font(DS.Font.captionSmall)
                    .padding(.horizontal, 4)
                    .background(Capsule().fill(Color.secondary.opacity(0.2)))
                    .help("\(tab.workspacePanes.count) 个分屏窗格")
            }
            TabCloseButton(tab: tab, visible: hovering)
        }
        .padding(.horizontal, DS.Padding.s)
        .padding(.vertical, DS.Padding.xs)
        .frame(minWidth: 120, idealWidth: 180, maxWidth: 200)
        .background(
            Capsule()
                .fill(isActive ? selection
                               : Color.clear)
        )
        .contentShape(Rectangle())
        .background(HoverTracking(hovering: $hovering, identifier: "tab-\(tab.id)"))
        .help("\(tab.title) · \(tab.subtitle)\n\(tab.status.label)")
        .accessibilityLabel("\(tab.title)，\(tab.subtitle)，\(tab.status.label)")
        .accessibilityAddTraits(isActive ? [.isSelected] : [])
    }
}

struct TabCloseButton: View {
    @ObservedObject var tab: TerminalTab
    let visible: Bool

    var body: some View {
        Button {
            NotificationCenter.default.post(name: .ghostermRequestCloseTab, object: tab.id)
        } label: {
            Image(systemName: "xmark")
                .font(DS.Font.captionSmall)
                .frame(width: 16, height: 16)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(.secondary)
        .opacity(visible ? 1 : 0)
        .allowsHitTesting(visible)
        .help("关闭标签页")
        .accessibilityLabel("关闭标签页")
        .accessibilityHidden(!visible)
    }
}

extension Notification.Name {
    /// 标签胶囊上的关闭按钮 → 交给 TabManager 发确认请求
    /// （视图不直接改 tabs：TabManager 是结构的唯一写入者）。
    static let ghostermRequestCloseTab = Notification.Name("Ghosterm.requestCloseTab")
}
