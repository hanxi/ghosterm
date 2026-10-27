// Sidebar — 左侧栏：打开的标签页 + 可连接的 Host 列表。
//
// 取法参照 iGhostVT 的 `Interface/Sidebar/SidebarView.swift`：标签页列表在
// 上（当前项高亮、行尾有关闭按钮）。我们多一节
// 「连接」（来自 ~/.ssh/config 的 Host）——这是 Ghosterm 的差异点，
// iGhostVT 没有配置文件这一层。
//
// 底部那条 ⚙︎「设置」footer 已删：macOS 顶上就有全局菜单条，设置在
// ⌘, / Ghosterm ▸ 设置…，窗口里再挂一份是重复。footer 上唯一不重复的
// 「终端配置无效」挪到了顶带（见 BandControls），而且收起侧栏也还看得见。
import GhostermAppModel
import SwiftUI

struct Sidebar: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tabs: TabManager
    @Binding var editingHost: HostAlias?
    @Binding var addingHost: Bool
    /// 顶带上的切换按钮要能收起侧栏（按钮组两处共用，见 BandControls）。
    @Binding var showsSidebar: Bool
    let onShowSettings: () -> Void
    @State private var forgetRequest: SessionRecord?

    var body: some View {
        VStack(spacing: 0) {
            header
            List {
                tabsSection
                if !model.savedSessions.isEmpty { savedSessionsSection }
                hostsSection
                if !model.wildcards.isEmpty { wildcardsSection }
            }
            .listStyle(.sidebar)
        }
        .alert("遗忘会话记录？", isPresented: Binding(
            get: { forgetRequest != nil }, set: { if !$0 { forgetRequest = nil } })) {
            Button("遗忘记录（不终止后端）", role: .destructive) {
                if let record = forgetRequest { model.forgetSavedSession(record) }
                forgetRequest = nil
            }
            Button("取消", role: .cancel) { forgetRequest = nil }
        } message: {
            Text("只删除「\(forgetRequest?.displayTitle ?? "")」的本机记录，不会停止本地或远端后台会话。若仍在运行，遗忘后可能无法从应用恢复；要停止它，请先使用「终止会话」。")
        }
    }

    /// 侧栏自己的顶带（与顶栏同高）：按钮组贴着红绿灯**左对齐**。
    ///
    /// 与顶栏共用 `BandControls`，侧栏开合时按钮的 x 不变——理由写在那边。
    /// 标题字**不画**：窗口的标题栏就是这条顶带，AppKit 的标题
    /// （CFBundleName = "Ghosterm"）会盖在左上角，用户实测看到过。
    private var header: some View {
        HStack(spacing: DS.Padding.s) {
            BandControls(model: model, showsSidebar: $showsSidebar,
                         onShowSettings: onShowSettings)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, DS.Padding.s)
        .frame(height: WindowChrome.barHeight)
        // 这条顶带也是标题栏的一部分：空白处能拖窗口。
        .background(WindowDragArea())
    }

    // MARK: - 标签页

    private var tabsSection: some View {
        Section {
            ForEach(tabs.tabs) { tab in
                TabRow(tab: tab, isActive: tab.id == tabs.activeTabID)
                    .contentShape(Rectangle())
                    .onTapGesture { tabs.activate(tab) }
                    // 与顶栏胶囊/胶囊条**共用同一份**菜单（见 TabContextMenu）：
                    // 文件面板、分屏、锁定、关闭都在这里，两处不会各说各话。
                    .contextMenu {
                        TabContextMenu(model: model, tabs: tabs, tab: tab)
                    }
            }
            // 「新建标签页」挪到了这一节的顶带上（见 header）——清单末尾那一行
            // 在标签多了之后会被挤到滚动区外。
        } header: {
            Text("标签页")
        }
    }

    private var savedSessionsSection: some View {
        Section {
            ForEach(model.savedSessions) { record in
                HStack(spacing: DS.Padding.xs) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(record.displayTitle).lineLimit(1)
                        Text(record.stateLabel).font(DS.Font.caption).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .contentShape(Rectangle())
                    .onTapGesture { if record.canRestore { model.restoreSavedSession(record) } }
                    Menu { savedSessionActions(record) } label: {
                        Image(systemName: "ellipsis")
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                    .help("管理「\(record.displayTitle)」：检查、终止或遗忘记录")
                    .accessibilityLabel("管理会话：\(record.displayTitle)")
                }
                .contextMenu { savedSessionActions(record) }
            }
        } header: {
            HStack {
                Text("保留的会话")
                Spacer()
                Button { model.refreshSavedSessions() } label: { Image(systemName: "arrow.clockwise").font(DS.Font.caption) }
                    .buttonStyle(.plain).help("重新读取保留的会话")
            }
        }
    }

    @ViewBuilder
    private func savedSessionActions(_ record: SessionRecord) -> some View {
        Button("恢复会话") { model.restoreSavedSession(record) }
            .disabled(!record.canRestore || model.sessionIsManaging(record))
        Button("检查状态") { model.inspectSavedSession(record) }
            .disabled(!record.canRestore || model.sessionIsManaging(record))
        Button("终止会话", role: .destructive) { model.terminateSavedSession(record) }
            .disabled(!record.canRestore || model.sessionIsManaging(record))
        Divider()
        Button("遗忘记录（不终止后端）", role: .destructive) { forgetRequest = record }
            .disabled(model.sessionIsManaging(record))
    }

    // MARK: - 可连接的 Host

    private var hostsSection: some View {
        Section {
            if model.hosts.isEmpty {
                Text(model.loadError ?? "没有配置主机")
                    .foregroundStyle(.secondary)
                    .font(DS.Font.callout)
            }
            ForEach(model.hosts) { host in
                HostRow(entry: host)
                    .contentShape(Rectangle())
                    .onTapGesture {
                        model.connect(to: .host(host.pattern))
                    }
                    .contextMenu {
                        Button("连接") { model.connect(to: .host(host.pattern)) }
                        Button("用 SFTP 浏览文件…") {
                            model.openSFTP(for: host.pattern)
                        }
                        Divider()
                        Button("编辑…") { editingHost = HostAlias(host.pattern) }
                    }
            }
        } header: {
            HStack {
                Text("连接")
                Spacer()
                Button {
                    model.reload()
                } label: {
                    Image(systemName: "arrow.clockwise").font(DS.Font.caption)
                }
                .buttonStyle(.plain)
                .help("重新读取 ssh_config")
                Button {
                    addingHost = true
                } label: {
                    Image(systemName: "plus").font(DS.Font.caption)
                }
                .buttonStyle(.plain)
                .help("新建 Host 块")
            }
        }
    }

    private var wildcardsSection: some View {
        Section("通配块（不参与连接）") {
            ForEach(model.wildcards, id: \.self) { pattern in
                HStack {
                    Image(systemName: "asterisk").foregroundStyle(.tertiary)
                    Text(pattern).foregroundStyle(.secondary)
                    Spacer()
                }
                .contentShape(Rectangle())
                .onTapGesture { editingHost = HostAlias(pattern) }
                .contextMenu {
                    Button("编辑…") { editingHost = HostAlias(pattern) }
                }
            }
        }
    }
}

/// 标签页行：状态点 + 标题 + 副标题 + 锁定标记 + 关闭。
private struct TabRow: View {
    @ObservedObject var tab: TerminalTab
    let isActive: Bool

    var body: some View {
        // 当前标签页要**看得出是它**：这行是「窗口现在在哪」的唯一标识
        // （iGhostVT 的 SidebarRow 同口径——中性 slab，不是强调色；
        // 强调色 slab 在终端底色上看起来像 bug 而不是选中）。
        HStack(spacing: DS.Padding.s) {
            StatusDot(status: tab.status)
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
            Image(systemName: tab.kind.symbolName)
                .font(DS.Font.caption).foregroundStyle(.tertiary)
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

private struct HostRow: View {
    let entry: HostEntry

    var body: some View {
        HStack(spacing: DS.Padding.s) {
            Image(systemName: "network").foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text(entry.pattern).lineLimit(1)
                Text(entry.summary)
                    .font(DS.Font.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 0)
        }
        .padding(.vertical, 2)
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
        case .connecting, .recovering: return .yellow
        case .failed: return .red
        case .exited, .exitedUnknown: return .secondary
        }
    }
}