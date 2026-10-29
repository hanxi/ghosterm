// RootView — 窗口的全部界面。
//
// 取法参照 iGhostVT 的 `Interface/Main/RootView.swift`：
//   侧边栏 ┃ 终端列（顶部标签条 + 所有标签页的终端面堆在 ZStack 里）
//
// **后台标签页保持挂载**：每条的 surface 都还在视图树里，只切可见性与
// 命中测试。这不是为了动画好看——libghostty 的 surface 一释放，那条会话的
// 网格与连接就没了；切回来是重连而不是切换。iGhostVT 同样如此
// （Documents/ARCHITECTURE.md: "All of a window's surfaces stay mounted in
// one ZStack (hidden, not removed)"）。
import AppKit
import GhostermAppModel
import GhostermTerminal
import SwiftUI

/// 窗口内容视图的**唯一出厂口**：生产与测试共用，两边的安全区语义才不会
/// 悄悄分叉（测试直接 `NSHostingView(rootView:)` 过，就量不到生产的真值）。
///
/// `safeAreaRegions = []` 是关键的一笔：窗口开着 `.fullSizeContentView`，
/// 顶部模式由标签条预留窗口按钮区，侧边模式只在侧栏预留，工作区直达顶边。
/// SwiftUI 不该再替我们避让一次。不关掉的话，分屏时 SwiftUI 给
/// `NSSplitView` 每个子项套的那层独立宿主视图会把安全区**又算一遍**，
/// 于是分屏比单窗格多顶出 5pt（配合子项上的 `.ignoresSafeArea` 才归零）。
@MainActor
func makeRootHostingView(model: AppModel) -> NSHostingView<RootView> {
    let host = NSHostingView(rootView: RootView(model: model))
    // 13.3 才有这个开关；更早的系统上分屏会比单窗格高 5pt，纯外观差，
    // 不影响任何能力，故不报错。
    if #available(macOS 13.3, *) { host.safeAreaRegions = [] }
    return host
}

struct RootView: View {
    @ObservedObject var model: AppModel
    @ObservedObject private var tabs: TabManager
    private var showsSidebar: Bool { model.preferences.tabPlacement == .sidebar }
    private var tmuxBinding: Binding<TmuxChooser?> {
        Binding(get: { panel == nil ? model.tmuxChooser : nil },
                set: { if $0 == nil { model.dismissTmux() } })
    }
    @State private var editError: String?
    @State private var panel: WindowPanel?
    @State private var pendingConnection: ConnectionRequest?
    init(model: AppModel) {
        self.model = model
        self.tabs = model.tabs
    }

    var body: some View {
        // 工作区始终留在同一棵树中，布局切换只增减导航与可用空间。
        HStack(spacing: 0) {
            if showsSidebar {
                Sidebar(model: model, tabs: tabs)
                    .frame(width: DS.sidebarWidth)
                Rectangle().fill(Color(nsColor: ChromePalette(model: model).separator)).frame(width: 1)
            }
            VStack(spacing: 0) {
                if model.preferences.tabPlacement == .top {
                    TabStrip(model: model, tabs: tabs)
                    Rectangle().fill(Color(nsColor: ChromePalette(model: model).separator)).frame(height: 1)
                }
                if let error = model.terminalConfigError {
                    TerminalConfigurationError(message: error) { SettingsWindow.open(model: model) }
                }
                panes
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .modifier(ChromeSurface(palette: ChromePalette(model: model)))
        .frame(minWidth: 720, minHeight: 420)
        // 顶栏要铺到窗口最顶上（红绿灯浮在它上面），所以**忽略上安全区**——
        // 否则 SwiftUI 会把内容压到系统标题栏以下，看起来就是「红绿灯
        // 在一条灰带上、顶栏在它下面」。
        .ignoresSafeArea(.container, edges: .top)
        // 关闭标签页：胶囊上的 × 发通知过来（视图不直接改 tabs）。
        .onReceive(NotificationCenter.default.publisher(
            for: .ghostermRequestCloseTab)) { note in
            guard let id = note.object as? UUID,
                  let tab = tabs.tabs.first(where: { $0.id == id }) else { return }
            tabs.requestClose(tab)
        }
        .sheet(item: tmuxBinding) { chooser in
            TmuxSheet(chooser: chooser, close: { model.dismissTmux() })
                .onDisappear { chooser.cancel() }
        }
        .sheet(item: $panel, onDismiss: openPendingConnection) { presented in
            switch presented {
            case .chooser:
                ConnectionChooser(model: model, choose: { request in
                    pendingConnection = request
                    panel = nil
                }, manageHosts: { panel = .hosts })
            case .hosts:
                HostManagerView(model: model)
            case .tabs:
                TabSearchView(tabs: tabs, select: { id in
                    guard let tab = tabs.tabs.first(where: { $0.id == id }) else { return }
                    tabs.activate(tab)
                    panel = nil
                })
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .ghostermNewConnection)) { _ in
            guard model.tmuxChooser == nil else { return }
            panel = .chooser
        }
        .onReceive(NotificationCenter.default.publisher(for: .ghostermManageHosts)) { _ in
            guard model.tmuxChooser == nil else { return }
            panel = .hosts
        }
        .onReceive(NotificationCenter.default.publisher(for: .ghostermSearchTabs)) { _ in
            guard model.tmuxChooser == nil else { return }
            panel = .tabs
        }
        // 设置：**独立窗口**（⌘, 或主菜单），不是 sheet——理由见
        // SettingsWindow 的说明。菜单与界面都只发意图，窗口的开关归这里。
        .onReceive(NotificationCenter.default.publisher(
            for: .ghostermShowSettings)) { _ in
            SettingsWindow.open(model: model)
        }
        // 界面字号：装在整棵树的根上（终端面不吃它——终端字号是 Ghostty
        // 配置里的事，见 AppearancePane 的说明）。
        .interfaceTextSize()
        // 关闭标签页的确认。**只在真有东西会被打断时才出现**（空闲提示符 /
        // 已退出 / 失败 / 连接中都不问——判据在 TabManager.requestClose）。
        .alert("关闭这个标签页？",
               isPresented: Binding(
                get: { tabs.closeRequest != nil },
                set: { if !$0 && tabs.closeRequest != nil { tabs.cancelClose() } })) {
            Button("关闭", role: .destructive) { tabs.confirmClose() }
            Button("取消", role: .cancel) { tabs.cancelClose() }
        } message: {
            if let tab = tabs.closeRequest {
                Text("关闭「\(tab.title)」（\(tab.subtitle)）会结束其中的终端连接，并停止尚在建立的文件连接。已入队的文件传输仍由传输队列继续管理。")
            }
        }
        .alert("操作失败", isPresented: Binding(
            get: { editError != nil }, set: { if !$0 { editError = nil } })) {
            Button("好", role: .cancel) {}
        } message: {
            Text(editError ?? "")
        }
    }

    private func openPendingConnection() {
        guard let request = pendingConnection else { return }
        pendingConnection = nil
        do { try request.open(in: model) }
        catch { editError = error.localizedDescription }
    }

    /// 所有标签页的面都挂在这里，只显示当前那个。
    private var panes: some View {
        ZStack {
            if tabs.tabs.isEmpty {
                EmptyStateView { model.connect(to: .localShell) }
            }
            ForEach(tabs.tabs) { tab in
                TabContentView(model: model, tab: tab,
                               isActive: tab.id == tabs.activeTabID)
                    .opacity(tab.id == tabs.activeTabID ? 1 : 0)
                    .allowsHitTesting(tab.id == tabs.activeTabID)
                    .zIndex(tab.id == tabs.activeTabID ? 1 : 0)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private enum WindowPanel: String, Identifiable {
    case chooser, hosts, tabs
    var id: Self { self }
}

/// 一个标签页的内容：终端（可能多窗格）或 SFTP 浏览器。
private struct TabContentView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tab: TerminalTab
    let isActive: Bool

    // 窗格外观的两项偏好。读成 @AppStorage 而不是一次性快照：设置页一改，
    // 这里立刻重画（用户改完要能当场看见效果，不该要求重开窗口）。
    @AppStorage(PaneGap.key) private var paneGap = PaneGap.defaultValue
    @AppStorage(PaneTitleBar.visibleKey)
    private var titleBarVisible = PaneTitleBar.visibleDefault

    var body: some View {
        WorkspaceView(model: model, tab: tab, isActive: isActive,
                      opts: PaneChromeOptions(gap: PaneGap.resolved(paneGap),
                                              showsTitleBar: titleBarVisible))
    }

}

/// 一个窗格：卡片外壳 + 标题栏 + 终端面 + 状态覆盖层 + 锁定遮罩。
struct PaneView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tab: TerminalTab
    @ObservedObject var pane: TerminalPane
    let isFocused: Bool
    /// 这个窗格所在标签页是不是当前显示的那一个。后台标签页的 surface 还在
    /// 树上（只 opacity(0)），要报成「不可见」让引擎停画——省 CPU/GPU。
    let isVisible: Bool
    /// 非聚焦窗格的变暗遮罩（nil = 不变暗）。
    let dim: SplitDim?
    let opts: PaneChromeOptions

    var showsActions = false
    var preview: (PaneLayout.Axis?) -> Void = { _ in }
    private var actions: PaneActionsView {
        PaneActionsView(model: model, tab: tab, paneID: pane.id, revealed: isFocused || showsActions, isAvailable: isVisible, preview: preview)
    }

    var body: some View {
        PaneCard(gap: opts.gap) {
            VStack(spacing: 0) {
                if !opts.showsTitleBar {
                    WorkspaceHiddenHandle(tab: tab, paneID: pane.id)
                        .contextMenu { PaneContextMenu(actions: actions) }
                }
                if opts.showsTitleBar {
                    PaneTitleBarView(
                        pane: pane, fallbackTitle: tab.title,
                        palette: ChromePalette(model: model),
                        isFocused: isFocused,
                        actions: actions, isSplit: tab.isSplit,
                        dragProvider: {
                            tab.zoomedPaneID = nil
                            return workspaceDragProvider(tab: tab, paneID: pane.id)
                        })
                }
                if let context = pane.tmuxContext {
                    Text("tmux · 连接时选择：\(context.selectedName)")
                        .font(.caption).foregroundStyle(.secondary).lineLimit(1)
                        .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 8)
                        .padding(.vertical, 3).background(Color(nsColor: .controlBackgroundColor))
                }
                terminalArea
            }
        }
    }

    private var terminalArea: some View {
        ZStack {
            if let surface = pane.surface {
                TerminalSurfaceView(surface: surface, isFocused: isFocused,
                                    isVisible: isVisible)
                    .id(ObjectIdentifier(surface))
                    .allowsHitTesting(!tab.isLocked)
            } else {
                ChromePalette(model: model).fill
            }

            // 非聚焦窗格盖一层**背景色**遮罩（上游 ghostty 的
            // unfocused-split-opacity 口径）。放在终端面之上、状态覆盖层之下：
            // 我们的状态提示不该被一起变暗。
            if tab.isSplit, !isFocused, let dim {
                Rectangle()
                    .fill(Color(red: dim.red, green: dim.green, blue: dim.blue))
                    .opacity(dim.alpha)
                    .allowsHitTesting(false)
            }

            // 覆盖层只在**非 ready** 时出现：ready 时屏幕就是终端内容，
            // 盖东西上去反而碍事（iGhostVT 的 SessionStatusOverlay 同口径）。
            if let context = pane.tmuxContext, pane.status.isExited || pane.status.isFailed {
                StatusCard(title: "tmux 连接已断开", detail: "后端状态未知。连接时选择：\(context.selectedName)", isError: pane.status.isFailed,
                           action: { model.showTmux(for: context.target, replacing: pane) }, actionTitle: "重新选择会话")
            } else { switch pane.status {
            case .ready:
                EmptyView()
            case .connecting:
                StatusPill(text: "连接中…")
            case .failed(let message):
                StatusCard(title: "连接失败", detail: message, isError: true,
                           action: nil)
            case .exited(let code):
                StatusPill(text: "已退出（\(code)）")
            case .exitedUnknown:
                StatusPill(text: "已结束（退出码未知）")
            }

            }

            if tab.isLocked {
                LockOverlay { tab.isLocked = false }
            }
            // 当前窗格由标题前的短标记与非聚焦内容淡化共同区分。
        }
    }
}

/// 锁定遮罩：挡住误触输入。点一下解锁（与 iGhostVT 的「锁定标签页」同用途）。
private struct LockOverlay: View {
    let onUnlock: () -> Void

    var body: some View {
        ZStack {
            Color.black.opacity(0.12)
            VStack(spacing: DS.Padding.s) {
                Image(systemName: "lock.fill").font(DS.Font.titleLarge)
                Text("已锁定").font(DS.Font.callout)
                Button("解锁", action: onUnlock)
            }
            .padding(DS.Padding.l)
            .background(RoundedRectangle(cornerRadius: DS.Radius.card)
                .fill(.regularMaterial))
        }
        .contentShape(Rectangle())
    }
}

/// 连接中 / 已退出的胶囊提示。
private struct StatusPill: View {
    let text: String

    var body: some View {
        VStack {
            Spacer()
            Text(text)
                .font(DS.Font.callout)
                .padding(.horizontal, DS.Padding.m)
                .padding(.vertical, DS.Padding.xs)
                .background(Capsule().fill(.regularMaterial))
            Spacer().frame(height: DS.Padding.l)
        }
        .allowsHitTesting(false)
    }
}

/// 失败 / 空态卡片（带可选动作按钮）。
private struct StatusCard: View {
    let title: String
    let detail: String
    let isError: Bool
    let action: (() -> Void)?
    var actionTitle = "关闭标签页"

    var body: some View {
        VStack(spacing: DS.Padding.m) {
            Image(systemName: isError ? "exclamationmark.triangle" : "terminal")
                .font(DS.Font.heroSymbol)
                .foregroundStyle(isError ? Color.orange : Color.secondary)
            Text(title).font(DS.Font.title)
            if !detail.isEmpty {
                Text(detail)
                    .font(DS.Font.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .textSelection(.enabled)
                    .frame(maxWidth: 420)
            }
            if let action {
                Button(actionTitle, action: action)
            }
        }
        .padding(DS.Padding.l)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// 一个标签页都没有时的空态（iGhostVT 的 EmptyTabsView 位）。
private struct EmptyStateView: View {
    let onNewTab: () -> Void

    var body: some View {
        VStack(spacing: DS.Padding.m) {
            Image(systemName: "terminal").font(DS.Font.heroSymbol)
                .foregroundStyle(.secondary)
            Text("没有打开的会话").foregroundStyle(.secondary)
            HStack(spacing: DS.Padding.s) {
                Button("新建本地 shell", action: onNewTab)
                    .keyboardShortcut("t", modifiers: .command)
                Text("或点击左上角 ＋ 选择连接")
                    .font(DS.Font.callout).foregroundStyle(.tertiary)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
