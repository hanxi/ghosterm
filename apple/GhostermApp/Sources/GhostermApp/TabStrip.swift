// TabStrip — 顶部标签条。
//
// 取法参照 iGhostVT 的 `Interface/Bars/TabStripBar.swift`：一行可横向滚动的
// 标签胶囊。我们额外画**分屏**的作用域（当前窗格高亮）——iGhostVT 的分屏落
// 在别处，我们这里一并放进标签条，因为它就是「窗格归属」最自然的落点。
//
// 末尾那个 `⋯` 溢出菜单已删：里面每一条都是副本——按标签的那半份就是右键
// 菜单的 `TabContextMenu`，窗口级的（新建标签页 / 关闭窗口 / 设置…）在
// macOS 顶上的全局菜单条里。菜单栏必须齐全这件事由
// `AppMenuCoverageTests` 钉住。
import GhostermAppModel
import SwiftUI

struct TabStrip: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tabs: TabManager
    @Binding var showsSidebar: Bool
    let onShowSettings: () -> Void

    var body: some View {
        GlassBar {
            // 侧栏收起时这条顶栏的最左边就是红绿灯那一带，按钮组画在这里；
            // 侧栏展开时它画在侧栏自己的顶带上（同一个 BandControls，两处的
            // x 一模一样）。两处都画就会出现两个加号——用户实测提过。
            if !showsSidebar {
                BandControls(model: model, showsSidebar: $showsSidebar,
                             onShowSettings: onShowSettings)
            }

            // 侧栏开着 → 顶栏只报**当前**标签（标签在侧栏里列着，这里不必
            // 再排一遍）；侧栏收起 → 顶栏就是标签条，可横向滚动。
            // 这是 iGhostVT 的规则（TabStripBar 的注释原话：sidebar open 时
            // 标签住在侧栏里，这条只显示当前标签的标题）。
            if showsSidebar {
                activeTabCapsule
            } else if tabs.tabs.isEmpty {
                Spacer()
                    .titleBarDoubleClick()
            } else {
                chipStrip
            }
        }
    }

    /// 当前标签的标题胶囊（侧栏打开时的顶栏内容）。
    /// 左对齐而不是居中：程序每次提示符都改标题时（状态行、agent 报进度），
    /// 居中的文字会跟着左右跳（iGhostVT 特意选左对齐，同此理）。
    @ViewBuilder
    private var activeTabCapsule: some View {
        if let tab = tabs.activeTab {
            HStack(spacing: DS.Padding.s) {
                StatusDot(status: tab.status)
                if tab.isLocked {
                    Image(systemName: "lock.fill").font(DS.Font.captionSmall)
                        .foregroundStyle(.secondary)
                }
                Text(tab.title).lineLimit(1)
                Text(tab.subtitle)
                    .font(DS.Font.caption).foregroundStyle(.secondary).lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, DS.Padding.m)
            .padding(.vertical, DS.Padding.xs)
            .background(
                RoundedRectangle(cornerRadius: DS.Radius.control)
                    .fill(Color.secondary.opacity(0.10))
            )
            .frame(maxWidth: .infinity, alignment: .leading)
            .contentShape(Rectangle())
            .help("当前标签页")
            .contextMenu { TabContextMenu(model: model, tabs: tabs, tab: tab) }
            .titleBarDoubleClick()
        } else {
            Spacer()
        }
    }

    /// 可横向滚动的标签条（侧栏收起时顶栏就是它）。
    ///
    /// 取法参照 iGhostVT 的 `TabStripBar`：一条横向 `ScrollView`，外加两件
    /// 让它「真的能滚」的事 ——
    ///   1. `WheelScrollsHorizontally`：普通鼠标的滚轮只有纵向 delta，横向
    ///      `ScrollView` 会忽略它，标签一多右边那些胶囊就够不着（iGhostVT
    ///      原话：the strip's hidden chips were out of reach）；
    ///   2. `.scrollBounceBehavior(.basedOnSize)`：装得下就别橡皮筋，
    ///      跟系统标签栏一样。
    private var chipStrip: some View {
        ScrollViewReader { scroller in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 0) {
                    ForEach(tabs.tabs) { tab in
                        TabChip(tab: tab, isActive: tab.id == tabs.activeTabID)
                            .id(tab.id)
                            .onTapGesture { tabs.activate(tab) }
                            .contextMenu {
                                TabContextMenu(model: model, tabs: tabs, tab: tab)
                            }
                    }
                }
                .padding(.vertical, 2)
                // 滚轮纵向位移 → 横向滚动（见组件说明）。
                .background(WheelScrollsHorizontally())
            }
            .modifier(NoBounceWhenFits())
            .frame(maxWidth: .infinity)
            // 标签条那片也是顶栏：双击它应触发系统的标题栏双击行为
            // （否则会被 ScrollView 的命中区吃掉——用户实测右侧双击没反应）。
            .titleBarDoubleClick()
            // 当前标签换了（键盘 / 侧栏里点）→ 把它滚进可见区（iGhostVT 的
            // reveal 同口径），否则换了标签条里却看不见它。
            .onChange(of: tabs.activeTabID) { id in
                guard let id else { return }
                withAnimation(.easeInOut(duration: 0.2)) { scroller.scrollTo(id) }
            }
            .onAppear {
                if let id = tabs.activeTabID { scroller.scrollTo(id) }
            }
        }
    }

}

/// 一个标签胶囊：状态点 + 标题 + （分屏时的窗格数）+ 关闭。
/// 新建标签页的 `+`：本地 shell 或直接选一台主机（省掉「先开一个再切」）。
/// 列表里的主机来自 ~/.ssh/config（与侧边栏同一份真相）。
///
/// 同一个组件给两处用（侧栏顶带 / 顶栏），但**同时只画一处**——
/// 谁画由侧栏开合决定，见 TabStrip.body 与 Sidebar.header。
struct NewTabMenu: View {
    @ObservedObject var model: AppModel

    var body: some View {
        Menu {
            Button("选择连接…") {
                NotificationCenter.default.post(name: .ghostermNewConnection, object: nil)
            }
            Divider()
            Button("本地 shell") { model.connect(to: .localShell) }
            Button("本机后台会话…") { model.showTmux(for: .localShell) }
            Button("本地文件") { model.openLocalFiles(newTab: true) }
            if !model.hosts.isEmpty {
                Divider()
                ForEach(model.hosts) { host in
                    Button("\(host.pattern)  ·  \(host.summary)") {
                        model.connect(to: .host(host.pattern))
                    }
                }
            }
            if !model.wildcards.isEmpty {
                Divider()
                Text("通配块不参与连接").font(DS.Font.caption)
            }
        } label: {
            Image(systemName: "plus")
                .frame(width: DS.controlSize, height: DS.controlSize)
                .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .frame(width: DS.controlSize)
        .help("新建标签页")
        .accessibilityLabel("新建标签页")
    }
}

private struct TabChip: View {
    @ObservedObject var tab: TerminalTab
    let isActive: Bool
    @State private var hovering = false

    var body: some View {
        HStack(spacing: DS.Padding.xs) {
            TabCloseButton(tab: tab, visible: hovering)
            Text(tab.title).lineLimit(1)
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
                Text("\(tab.panes.count)")
                    .font(DS.Font.captionSmall)
                    .padding(.horizontal, 4)
                    .background(Capsule().fill(Color.secondary.opacity(0.2)))
                    .help("\(tab.panes.count) 个分屏窗格")
            }
        }
        .padding(.horizontal, DS.Padding.s)
        .padding(.vertical, DS.Padding.xs)
        .frame(minWidth: 120, idealWidth: 180, maxWidth: 200)
        .background(
            RoundedRectangle(cornerRadius: DS.Radius.control)
                .fill(isActive ? Color.primary.opacity(0.12)
                               : Color.clear)
        )
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
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
