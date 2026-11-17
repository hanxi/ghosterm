// AppModel — 宿主状态：主机列表 + 当前会话（SwiftUI 观测对象）。
//
// 它只做「把 core/terminal 的能力端给 UI」：连接参数的求值、会话的起停。
// 不放字节流路径（那在 SessionController 的会话线程上直连 surface）。
//
// 连接流程（M2 起）：
//   connect → 连接工作线程（TCP+握手阻塞，绝不占主线程）
//           → 需要裁决/凭据时**回调主线程**弹对话框（主机密钥三态 / 口令）
//           → 会话建立后由 SessionController 驱动字节流。
//
// 标签页（M5 起，参照 iGhostVT）：本模型不再持有「唯一一个 surface」，
// 而是持有 TabManager（一组 TerminalTab，各自一组 TerminalPane）。
// 兼容入口 `surface` / `status` / `selected` 转发到**当前标签页**——
// 老测试与无头冒烟（--ssh-smoke）继续可用，语义仍是「现在屏幕上是什么」。
import AppKit
import Combine
import CoreFoundation
import GhostermCore
import GhostermTerminal
import Foundation

@MainActor
public final class AppModel: ObservableObject {
    /// Capabilities implemented by the App-facing Client runtime path.
    public static let extensionHostCapabilities = ["session.rpc", "resource.receive", "file.readScoped", "ui.table", "ui.diff", "ui.task", "ui.views"]

    /// The wire protocol carries semantic versions with three numeric
    /// components. Apple's marketing version commonly omits the patch (for
    /// example, `16.0`), so normalize that form before opening a Session.
    public static func extensionHostProtocolVersion(_ marketingVersion: String?) -> String {
        guard let marketingVersion, !marketingVersion.isEmpty else { return "0.1.0" }
        let components = marketingVersion.split(separator: ".", omittingEmptySubsequences: false)
        guard (2...3).contains(components.count), components.allSatisfy({ component in
            !component.isEmpty && component.utf8.allSatisfy { (48...57).contains($0) } &&
                (component.count == 1 || component.first != "0")
        }) else { return "0.1.0" }
        return components.count == 2 ? marketingVersion + ".0" : marketingVersion
    }

    public enum Target: Hashable {
        case localShell
        case host(String)
        public static func == (lhs: Target, rhs: Target) -> Bool {
            switch (lhs, rhs) {
            case (.localShell, .localShell): return true
            case let (.host(a), .host(b)): return HostEntry.patternKey(a) == HostEntry.patternKey(b)
            default: return false
            }
        }
        public func hash(into hasher: inout Hasher) {
            switch self {
            case .localShell: hasher.combine(0)
            case .host(let pattern): hasher.combine(1); hasher.combine(HostEntry.patternKey(pattern))
            }
        }
    }

    @Published public private(set) var hosts: [HostEntry] = []
    @Published public private(set) var loadError: String?
    /// 终端配置加载失败的原因（nil = 正常）。UI 必须显示它——用户改了
    /// 字体却看到默认大小，比看到一条错误难查得多。
    @Published public private(set) var terminalConfigError: String?
    @Published public private(set) var terminalFontError: String?

    public let terminalApp: TerminalApp?

    /// App-scoped package review and source-decision owner. The ordinary App
    /// build keeps the optional JS backend disabled, so no controller is
    /// offered until a build explicitly includes that runtime.
    public let extensionController: ExtensionController?
    @Published public private(set) var extensionLastLocalProcess: ExtensionController.ProcessState?
    @Published public private(set) var extensionLastLocalTemporary: ExtensionLocalTemporaryState?

    public struct ExtensionRun: Sendable {
        public let packageID: String
        public let packageDigest: String
        public let command: String
        public let arguments: [String]
        public let decodedArguments: String?
        public let executorDigest: String
        public let peerIdentity: String
        public let remoteCommand: String
        public let executorExecution: ExtensionRelayBootstrap.Execution?
        public let executorAttachment: ExtensionRelayBootstrap.Attachment?
        public let localLaunch: ExtensionLocalLaunch?
        public let clientWorkspace: ExtensionController.ClientWorkspace?
        public let methods: [String]
        public let hostMethods: [String]
        public let requiredCapabilities: [String]
        public let session: ExtensionController.SessionState?
        public let runtime: ExtensionController.RuntimeState?
        public let receivedResources: [ExtensionController.ReceivedResource]
        public let codexEvents: [String]
    }

    public struct CodexPendingApproval: Identifiable, Sendable, Equatable {
        public let id: String
        public let method: String
        public let paramsJSON: String
        public let itemID: String?
        public let identity: CodexApprovalIdentity?
        public let permissionReviewID: String?
        public var stableID: String { Data(id.utf8).base64EncodedString() }
    }

    @Published public private(set) var extensionRun: ExtensionRun?
    @Published public private(set) var extensionRunBusy = false
    @Published public private(set) var frontendCandidates: [FrontendCandidate] = []
    @Published public private(set) var frontendDiscoveryIssue: String?
    private var pendingFrontendCandidates: [FrontendCandidate] = []
    private var pendingFrontendIssue: String?
    private var frontendUpdatePending = false
    @Published public private(set) var extensionRunIssue: String?
    @Published public private(set) var codexThreadID: String?
    @Published public var codexMessageDraft = ""
    @Published public private(set) var codexCallBusy = false
    @Published public private(set) var codexTurnActive = false
    @Published public private(set) var codexPendingApprovals: [CodexPendingApproval] = []
    @Published public private(set) var codexTimeline: [CodexTimelineEntry] = []
    @Published public private(set) var codexChangeReview: CodexChangeReviewState?
    private var codexChangeReadGeneration = UUID()
    private var codexChangeReadTask: Task<Void, Never>?
    private var extensionRunGeneration = UUID()
    private var extensionRunInvocation: (packageID: String, digest: String, command: String, arguments: [String], clientWorkspace: ExtensionController.ClientWorkspace?)?
    private var lastCodexEventSequence: UInt64 = 0
    private var pendingExtensionRPCHandles: Set<UInt64> = []
    private var claudeRPC: ClaudeRPCClient?
    @Published public private(set) var extensionTable: PluginTableModel?
    @Published public private(set) var extensionDiff: PluginDiffModel?
    @Published public private(set) var extensionTask: PluginTaskModel?
    @Published public private(set) var extensionRegisteredViews: PluginViewsModel?
    private var extensionViews: [String] = []
    @Published public private(set) var localReservationEntries: [ExtensionLocalJournalEntry] = []
    @Published public private(set) var localReservationBusy = false
    @Published public private(set) var localReservationIssue: String?
    @Published public private(set) var claudeConversation: ClaudeConversation?

    /// 界面外观偏好（跟随系统 / 浅色 / 深色）与终端色板事件（`theme =
    /// light:X,dark:Y` 靠它选边）。**改了要同时喂两个消费者**：AppKit 的
    /// `NSApp.appearance`（我们的界面）与 libghostty 的
    /// `ghostty_app_set_color_scheme`（终端配色）。少喂一处就会出现
    /// 「界面深色、终端浅色」。
    ///
    /// 早先只有启动时喂了 AppKit（见 `GhostermApp` 的 `applicationDidFinishLaunching`），
    /// 运行时改偏好只喂终端——于是用户在设置里切深/浅色**界面不动，要重启才变**
    /// （用户实测）。现在两处都在 `didSet` 里落，改完立刻生效。
    @Published public var appearance: AppearancePreference {
        didSet {
            guard appearance != oldValue else { return }
            UserDefaults.standard.set(appearance.rawValue,
                                      forKey: AppearancePreference.key)
            // 界面：AppKit 的外观一改，所有窗口（含设置窗口）随重画。
            appearance.applyToApplication()
            applyColorSchemeToTerminal()
        }
    }

    /// 把「现在到底是深色还是浅色」告诉 libghostty。
    @MainActor
    public func applyColorSchemeToTerminal() {
        let dark = AppearancePreference.isDarkNow(appearance)
        terminalApp?.setColorScheme(dark: dark)
        objectWillChange.send() // 系统外观变化也会改变已解析的终端主题。
    }

    /// 非聚焦窗格的变暗遮罩（读引擎里**已生效**的配置；nil = 不变暗）。
    ///
    /// 读引擎而不是读配置文件：`theme = X` 这类间接来源下，只有引擎知道最终
    /// 生效的 `background`。每次渲染现读——配置改了（reloadConfig）下一次渲染
    /// 就跟着变，不用另做失效逻辑。规则本身是 `SplitDim.resolve`（纯函数）。
    public var splitDim: SplitDim? {
        guard let app = terminalApp else { return nil }
        return SplitDim.resolve(
            opacity: app.double("unfocused-split-opacity"),
            fill: app.color("unfocused-split-fill").map {
                SplitDim.RGB(r: $0.r, g: $0.g, b: $0.b)
            },
            background: app.color("background").map {
                SplitDim.RGB(r: $0.r, g: $0.g, b: $0.b)
            })
    }

    /// 本窗口的标签页清单（**结构的唯一写入者**，见 TabManager）。
    @Published public private(set) var tabs = TabManager()
    public func toggleTabPlacement() {
        preferences.tabPlacement = preferences.tabPlacement == .sidebar ? .top : .sidebar
    }

    /// 当前标签页的终端面。旧接口：转发到 activeTab.activePane。
    public var surface: TerminalSurface? { activePane?.surface }

    /// 当前标签页的窗格。
    public var activePane: TerminalPane? { tabs.activeTab?.activePane }

    /// 当前标签页的双栏 SFTP 会话（独立 SFTP 标签页）。
    public var remoteFiles: WorkspaceFilePane? { tabs.activeTab?.remoteFiles }

    /// 当前选中的目标（旧接口：由当前标签页反推）。
    public var selected: Target? {
        guard let tab = tabs.activeTab else { return nil }
        switch tab.kind {
        case .localShell: return .localShell
        case .ssh(let t): return .host(t.pattern)
        case .sftp, .localFiles: return nil
        }
    }

    /// 一句话状态（状态条用）。旧接口：由当前标签页的窗格状态算出来。
    public var status: String {
        guard let tab = tabs.activeTab else { return statusMessage }
        if case .sftp = tab.kind { return tab.sftpError ?? "SFTP \(tab.kind.subtitle)" }
        switch tab.status {
        case .connecting: return "连接中…"
        case .ready: return tab.kind.subtitle
        case .failed(let m): return "连接失败：\(m)"
        case .exited(let c): return "已退出（\(c)）"
        case .exitedUnknown: return "已结束（退出码未知）"
        }
    }

    /// 主机密钥确认请求（未知主机 → 弹真三态对话框；nil = 无挂起请求）。
    @Published public private(set) var pendingHostKey: HostKeyPrompt?
    /// 口令输入请求（私钥带 passphrase / 认证失败需要口令；nil = 无挂起）。
    @Published public private(set) var pendingCredential: CredentialPrompt?

    /// 一句话的近况（连接中 / 本地 shell / …）。标签页自己会说状态，
    /// 这个字符串只是状态条上的补充（旧 UI 与冒烟模式读它）。
    private var statusMessage: String = "未连接"
    /// ssh_config 路径（高级设置页回显；只读）。
    public let configPath: String
    /// known_hosts 路径。默认 `~/.ssh/known_hosts`；**可注入**是测试隔离的
    /// 硬要求（e2e 不得污染用户真实文件——decider 接受后 core 会 TOFU 落盘）。
    /// known_hosts 路径（同上；测试注入隔离路径也靠它）。
    public let knownHostsPath: String
    /// 配置写侧（冲突检测 / 备份 / 原子写；reload 后重建）。
    private var configStore: ConfigStore?
    var makeConfigStore: (String) -> ConfigStore = { ConfigStore(path: $0) }

    /// 在途连接的取消票（跨线程：主线程投、工作线程查，故用带锁的类）。
    /// 关闭一个**还在建连接**的标签页时投一张；工作线程在每个会弹框的地方
    /// 查一次，命中就当「用户已经不要这条连接了」——不弹凭据框、不 attach、
    /// 直接释放（见 `ConnectCancellations`）。
    private nonisolated let cancelledConnects = ConnectCancellations()

    /// UI 侧注入的交互源：主线程上弹对话框并拿结果。
    public var interaction: AppInteraction?

    /// 终端配置（字体 / 主题…）的读写口。终端 app 用它的文件启动。
    public let settings: SettingsStore
    var applyPreparedTerminalConfiguration: (TerminalApp.PreparedConfiguration) throws -> Void = { try $0.apply() }
    /// 应用偏好（启动行为 / 默认 shell）。
    ///
    /// **必须 @Published**：设置页的「默认命令行解释器」下拉读
    /// `preferences.defaultShell` 画标题与勾选。`AppPreferences` 的值存在
    /// UserDefaults 里，setter 改完若不发 `objectWillChange`，SwiftUI 就
    /// 不重画——用户切了 shell，下拉还停在旧值上（用户实测）。故
    /// `AppPreferences` 的 setter 写成**变异**的（不是 nonmutating），
    /// 这样 `model.preferences.xxx = y` 会重赋这个 @Published 属性、触发广播。
    @Published public var preferences: AppPreferences

    private var startedInitialSessions = false
    private var authentications: [UUID: (DispatchQueue, SSHAuthenticationOperation)] = [:]
    private var authenticationGenerations: [UUID: UUID] = [:]
    private var connectionOpenings: [UUID: SSHConnectionOpening] = [:]
    var makeSSHConnectionOpening: () -> SSHConnectionOpening = { SSHConnectionOpening() }
    var sshOpeningOperations = SSHOpeningOperations()
    var sshCredentialStorage: SSHCredentialSelector.Storage = .platform
    @Published public var tmuxChooser: TmuxChooser?


    public init(configPath: String? = nil, knownHostsPath: String? = nil,
                settings: SettingsStore? = nil, defaults: UserDefaults = .standard,
                pluginResourceDirectory: URL? = nil) {
        self.configPath = configPath ?? AppModel.defaultConfigPath()
        self.knownHostsPath = knownHostsPath ?? AppModel.defaultKnownHostsPath()
        self.settings = settings
            ?? SettingsStore(terminalConfigPath: SettingsStore.defaultTerminalConfigPath())
        self.preferences = AppPreferences(defaults: defaults)
        self.appearance = AppearancePreference.current
        if ExtensionRuntime.supportStatus == 0,
           let resourceDirectory = pluginResourceDirectory ?? Self.pluginResourceDirectory() {
            let profile = try? ExtensionController.HostProfile(
                side: .client, sdkVersion: "1.0.0", protocols: [1],
                runtimes: ["portable-js"], capabilities: Self.extensionHostCapabilities)
            self.extensionController = profile.flatMap {
                try? ExtensionController(profile: $0, resourceDirectory: resourceDirectory,
                    installationDirectory: resourceDirectory.deletingLastPathComponent().appendingPathComponent("PluginPackages", isDirectory: true))
            }
        } else {
            self.extensionController = nil
        }
        // 终端配置**无效**时：留用引擎的**默认配置**（app 还得在），并把错误
        // 显式报出去（启动告警 / 设置页）。**不能**因为一行写错的 `theme`
        // 就把 terminalApp 丢掉——那样整个窗口连一个终端都开不了（实测：
        // 选了一个没随包的主题名，`connect` 直接「libghostty 初始化失败」，
        // 明明只是配色写错了）。`reloadConfig` 失败时旧配置保持有效，
        // 所以这里继续用 app 是安全的、不丢配置。
        //
        // 顺序：先拿共享 app（进程内单例，见 TerminalApp 的文档），再把
        // **用户配置**重载进去。不要让 shared 直接吃 configPath——
        // 单例已存在时那个参数会被忽略，于是「用户配的字体没生效」这种
        // 问题会以最难看懂的形式出现。
        try? TerminalFonts.prepare()
        self.terminalFontError = TerminalFonts.issue
        self.terminalConfigError = self.settings.loadError
        do {
            let app = try TerminalApp.shared()
            let path = self.settings.terminalConfigPath
            self.terminalApp = app
            do {
                let text = try self.settings.rawConfig()
                try self.settings.requireUnchanged(expected: Data(text.utf8))
                let prepared = try app.prepareConfiguration(text: text, origin: URL(fileURLWithPath: path).standardizedFileURL.path)
                try prepared.apply()
            } catch {
                self.terminalConfigError = "配置未应用：\(error)"
            }
        } catch {
            // app 本身建不起来（引擎初始化失败）才是真没救。
            self.terminalApp = nil
            self.terminalConfigError = "\(error)"
        }
        terminalApp?.startEventLoop()
        applyColorSchemeToTerminal()
        reload()

        // 关闭标签页时取消它的在途连接（见 cancelInFlight 说明）。必须在
        // TabManager stop 之前跑。
        tabs.willClose = { [weak self] tab in
            guard let self else { return }; self.cancelInFlight(tab)
        }
    }

    /// 投一张取消票：若这个标签页的连接还在途中，工作线程会在下一个检查点
    /// 跳掉（不 attach、不弹凭据框）。已建好的会话由 TabManager.close →
    /// pane.stop 停掉，不在这里管。
    private func cancelInFlight(_ tab: TerminalTab) {
        for pane in tab.panes where pane.status == .connecting { cancelConnection(pane.id) }
        // 只有真有在途连接的标签页才投票：已建好的会话由 stop 收掉，投了
        // 也没人消费（还会让集合一直涨）。
        for pane in tab.filePanes where pane.remote && pane.panel == nil && pane.error == nil {
            cancelConnection(pane.id)
        }
    }

    nonisolated public static func defaultConfigPath() -> String {
        (NSHomeDirectory() as NSString).appendingPathComponent(".ssh/config")
    }

    nonisolated public static func defaultKnownHostsPath() -> String {
        (NSHomeDirectory() as NSString).appendingPathComponent(".ssh/known_hosts")
    }

    public func reload() {
        for (id, (owner, flow)) in authentications {
            cancelledConnects.add(id); owner.async { flow.cancel() }
        }
        for (id, opening) in connectionOpenings { cancelledConnects.add(id); opening.cancel() }
        do {
            let model = try HostListModel(path: configPath)
            hosts = model.hosts
            wildcards = model.wildcardPatterns
            configModel = model
            configStore = makeConfigStore(configPath)
            loadError = nil
        } catch {
            hosts = []
            wildcards = []
            configModel = nil
            configStore = nil
            loadError = "读取 \(configPath) 失败：\(error)"
        }
    }

    // MARK: - SFTP 文件浏览（M4：与终端解耦，同一 core 不同 UI）

    /// 打开 SFTP 浏览（**新建一个 SFTP 标签页**，独立第二条 SSH 会话——
    /// 与终端会话解耦，互不抢通道）。
    ///
    /// **每次调用都新开**，与 SSH 终端同规则（同一台主机也允许开多个 SFTP
    /// 标签页）。早先按 `kind` 去重（同目标切成已开的那个）已拿掉——与「同一
    /// 台 ssh 每次点都新开」保持一致，也免得「去重」在三个 case 上各说各话。
    public func openSFTP(for pattern: String) {
        let entry = hosts.first { $0.matchesPattern(pattern) }
        let host = entry?.hostname ?? pattern
        let user = entry?.user ?? NSUserName()
        let port = UInt16(clamping: entry?.port ?? 22)
        let identityFiles = expandTilde(entry?.identityFiles ?? [])
        let kind = TabKind.sftp(RemoteTarget(pattern: pattern, user: user,
                                             host: host, port: port, connectionIssues: entry?.connectionIssues ?? []))

        let tab = TerminalTab(kind: kind)
        let local = FilesPanelModel(backend: LocalFileBackend(initialPath: NSHomeDirectory()))
        local.start()
        tab.addFilePane(WorkspaceFilePane(remote: false, panel: local))
        guard let remotePane = tab.remoteFiles else { return }
        tabs.add(tab)
        statusMessage = "SFTP 连接 \(user)@\(host)…"

        let model = self
        let khPath = knownHostsPath
        let tabID = tab.id, paneID = remotePane.id
        Task { @MainActor in
            do {
                let backend = try await model.makeSFTPFileBackend(
                    user: user, host: host, port: port,
                    identityFiles: identityFiles, knownHosts: khPath, cancelID: paneID,
                    connectionIssues: entry?.connectionIssues ?? [], alias: pattern)
                if model.cancelledConnects.take(paneID) { return }
                guard let p = model.tab(withID: tabID)?.filePanes.first(where: { $0.id == paneID }) else { return }
                let remotePanel = FilesPanelModel(backend: backend)
                p.attach(remotePanel); remotePanel.start()
            } catch {
                _ = model.cancelledConnects.take(paneID)
                model.tab(withID: tabID)?.filePanes.first(where: { $0.id == paneID })?.fail("\(error)")
            }
        }
    }

    /// 按 `RemoteTarget` 新开一个 SFTP 标签页（未来「SSH 终端跳到 SFTP」走这里）。
    public func openSFTP(_ target: RemoteTarget) { openSFTP(for: target.pattern) }

    /// 由 ssh_config 别名求值出一个 `RemoteTarget`（别名 → 真实主机名等）。
    public func remoteTarget(for pattern: String) -> RemoteTarget {
        let entry = hosts.first { $0.matchesPattern(pattern) }
        return RemoteTarget(pattern: pattern,
                            user: entry?.user ?? NSUserName(),
                            host: entry?.hostname ?? pattern,
                            port: UInt16(clamping: entry?.port ?? 22), connectionIssues: entry?.connectionIssues ?? [])
    }

    /// 关闭当前 SFTP 浏览（关掉那个标签页）。
    public func closeSFTP() {
        guard let tab = tabs.activeTab, case .sftp = tab.kind else { return }
        tabs.close(tab)
    }

    // MARK: - 内嵌文件面板（终端标签页右侧）

    /// ⌘⇧E：切换当前标签页的内嵌文件面板。
    public func toggleFilesPanel() {
        guard let tab = tabs.activeTab else { return }
        toggleFilesPanel(for: tab)
    }

    /// 切换**指定**标签页的内嵌文件面板（标签右键菜单用——右键的那条不一定
    /// 是当前标签页）。开的时候不静默失败：终端类才有面板，其余直接不出现。
    public func toggleFilesPanel(for tab: TerminalTab) {
        guard tab.canHaveFilesPanel else { return }
        if tab.filesPanelVisible {
            tab.filesPanelVisible = false
            tab.zoomedPaneID = nil
            tab.activeWorkspacePaneID = tab.panes.first?.id
            return
        }
        tabs.activate(tab)   // 从右键菜单开面板时把这条标签页带到前台，否则看不见
        openFilesPanel(for: tab)
    }

    /// 打开（并确保已建）某标签页的文件面板。
    public func openFilesPanel(for tab: TerminalTab) {
        guard tab.canHaveFilesPanel else { return }
        tab.filesPanelVisible = true
        if tab.filesPanel == nil && !tab.filesPanelConnecting {
            startFilesPanel(for: tab)
        }
    }

    private func startFilesPanel(for tab: TerminalTab) {
        // 本地 shell：直接上本地后端，初始目录跟随终端 cwd（内核查询，已交付）。
        if tab.kind.isLocalShell {
            let root = tab.currentDirectory ?? NSHomeDirectory()
            let model = FilesPanelModel(backend: LocalFileBackend(initialPath: root))
            // 本地默认「跟随终端目录」：终端 cd 到哪，面板跟到哪。
            model.cwdProvider = { [weak tab] in tab?.currentDirectory }
            model.followsTerminalCwd = true
            tab.attachFilesPanel(model)
            model.start()
            return
        }
        // SSH：懒开**独立第二条会话**（与终端解耦，互不抢通道）。
        guard case .ssh(let target) = tab.kind else { return }
        let entry = hosts.first { $0.matchesPattern(target.pattern) }
        let identityFiles = expandTilde(entry?.identityFiles ?? [])
        let khPath = knownHostsPath
        let tabID = tab.id
        tab.markFilesPanelConnecting()
        guard let paneID = tab.remoteFiles?.id else { return }
        let model = self
        Task { @MainActor in
            do {
                let backend = try await model.makeSFTPFileBackend(
                    user: target.user, host: target.host, port: target.port,
                    identityFiles: identityFiles, knownHosts: khPath, cancelID: paneID,
                    connectionIssues: target.connectionIssues, alias: target.pattern)
                if model.cancelledConnects.take(paneID) { return }
                guard let t = model.tab(withID: tabID), t.remoteFiles?.id == paneID else { return }
                let panelModel = FilesPanelModel(backend: backend)
                t.attachFilesPanel(panelModel)
                panelModel.start()
            } catch {
                _ = model.cancelledConnects.take(paneID)
                guard let t = model.tab(withID: tabID), t.remoteFiles?.id == paneID else { return }
                t.markFilesPanelFailed("\(error)")
            }
        }
    }

    /// 独立建一条 SFTP 会话 → 文件后端（面板专用；连接在专用线程上跑）。
    /// 失败如实抛出（调用方在面板里显示，不静默降级）。
    public nonisolated func makeSFTPFileBackend(user: String, host: String, port: UInt16,
                                                identityFiles: [String],
                                                knownHosts: String,
                                                cancelID: UUID, connectionIssues: [SSHConnectionIssue] = [], alias: String? = nil) async throws -> SFTPFileBackend {
        let owner = DispatchQueue(label: "dev.ghosterm.files-connect")
        let target = SSHConnectionTarget(alias: alias ?? host, host: host, user: user, port: port,
            knownHosts: knownHosts, identityPaths: identityFiles, connectionIssues: connectionIssues)
        let opening = await beginConnectionOpening(cancelID)
        let operations = await sshOpeningOperations
        do {
            let backend: SFTPFileBackend = try await withTaskCancellationHandler(operation: {
                let ssh = try await authenticate(target, id: cancelID, owner: owner, opening: opening)
                return try await withCheckedThrowingContinuation { continuation in
                    owner.async {
                        do {
                            let remaining = try opening.remainingMilliseconds(maximum: 5000)
                            let sftp = try operations.sftp(ssh, remaining, opening.cancellation)
                            if opening.cancellation.isCancelled {
                                sftp.close()
                                throw GhostermError.status(8, "SFTP 打开已取消")
                            }
                            continuation.resume(returning: SFTPFileBackend(session: ssh, sftp: sftp, user: user, host: host, initialPath: "."))
                        } catch { continuation.resume(throwing: error) }
                    }
                }
            }, onCancel: { opening.cancel() })
            await finishConnectionOpening(cancelID, opening: opening)
            return backend
        } catch {
            await finishConnectionOpening(cancelID, opening: opening)
            throw error
        }
    }

    // MARK: - 分屏 / 标签页动作（UI 的入口，结构改动一律经 TabManager）

    /// 在当前窗格处一分为二：**新窗格自己一条会话**（与现有会话并列），
    /// 连接参数从当前标签页抄（同一个 host 再开一条 shell——
    /// 这是终端的常规用法：同一台机器开两个 shell）。
    public func splitActiveTab(axis: PaneLayout.Axis) {
        guard let tab = tabs.activeTab, let id = tab.activeWorkspacePaneID else { return }
        split(tabID: tab.id, paneID: id, axis: axis)
    }

    /// 窗格按钮传所属 UUID；只在点击时改变焦点，不从失效目标回落到当前窗格。
    public func split(tabID: UUID, paneID: UUID, axis: PaneLayout.Axis) {
        guard let (tab, source) = resolvePane(tabID: tabID, paneID: paneID), !tab.isLocked else { return }
        if let file = source.files {
            guard file.panel != nil else { return }
            tabs.activate(tab)
            tab.duplicateFilePane(file, axis: axis)
            return
        }
        guard let terminalApp, let terminal = source.terminal else { return }
        let startCwd = terminal.currentDirectory
        tabs.activate(tab)
        tab.activeWorkspacePaneID = paneID
        let pane = TerminalPane()
        wireFrontendDiscovery(pane)
        pane.onExit = { [weak self] p in self?.handlePaneExit(p) }
        tab.split(axis: axis, with: pane)
        if tab.kind.isLocalShell {
            startLocal(pane: pane, app: terminalApp, cwd: startCwd)
        } else if let target = tab.kind.remoteTarget {
            let entry = hosts.first { $0.matchesPattern(target.pattern) }
            startSSH(pane: pane, entry: entry, pattern: target.pattern, terminalApp: terminalApp)
        }
    }

    private func resolvePane(tabID: UUID, paneID: UUID) -> (TerminalTab, WorkspacePane)? {
        guard let tab = tabs.tabs.first(where: { $0.id == tabID }),
              tab.visibleWorkspaceLayout?.paneIDs.contains(paneID) == true,
              let pane = tab.workspacePanes.first(where: { $0.id == paneID }) else { return nil }
        return (tab, pane)
    }

    /// 关闭当前标签页（⌘W）。有会话在跑时留给 UI 确认。
    public func requestCloseActiveTab() {
        guard let tab = tabs.activeTab else { return }
        tabs.requestClose(tab)
    }

    /// 标签菜单把该标签当前窗格的身份交给同一命令入口。
    public func split(_ tab: TerminalTab, axis: PaneLayout.Axis) {
        guard let id = tab.activeWorkspacePaneID else { return }
        split(tabID: tab.id, paneID: id, axis: axis)
    }

    public func closePane(in tab: TerminalTab) {
        guard let id = tab.activeWorkspacePaneID else { return }
        closePane(tabID: tab.id, paneID: id)
    }

    public func closePane(tabID: UUID, paneID: UUID) {
        guard let (tab, target) = resolvePane(tabID: tabID, paneID: paneID) else { return }
        if tab.workspacePanes.count == 1 { tabs.requestClose(tab); return }
        if let file = target.files {
            if file.panel == nil && file.error == nil { cancelConnection(file.id) }
            tab.removeFilePane(file.id)
        } else if let terminal = target.terminal {
            if !tab.closePane(terminal) { tabs.requestClose(tab) }
        }
    }

    public func toggleZoom(tabID: UUID, paneID: UUID) {
        guard let (tab, _) = resolvePane(tabID: tabID, paneID: paneID) else { return }
        tabs.activate(tab)
        tab.toggleWorkspaceZoom(paneID)
    }

    /// 菜单 / 快捷键仅负责取得当前身份，行为由窗格命令统一处理。
    public func toggleZoomActivePane() {
        guard let tab = tabs.activeTab, let id = tab.activeWorkspacePaneID else { return }
        toggleZoom(tabID: tab.id, paneID: id)
    }

    public func closeActivePane() {
        guard let tab = tabs.activeTab, let id = tab.activeWorkspacePaneID else { return }
        closePane(tabID: tab.id, paneID: id)
    }

    /// 一个窗格的会话**自己结束**了（本地 / SSH shell `exit`，读到 EOF）。
    ///
    /// 语义（对齐 iTerm2 / Ghostty / tmux，也是用户实测反馈）：分屏里某个
    /// 窗格退出就该**立刻收起这个窗格**、布局塌陷——不在原地留一个「已退出」
    /// 的死窗格占着半屏。**只剩一个窗格时收掉它 = 关标签页**（与
    /// `closeActivePane` 的「关掉最后一个分屏 = 关标签页」同口径），走
    /// `requestClose`：已退出 = 没什么可丢 → 直接关、不弹框（铁律 #6）。
    ///
    /// 只在 `.exited`（会话自己结束）时被调用；`.failed`（建立失败 / 掉线）
    /// **不**走这里——那要在屏幕上报错、保留窗格。
    func handlePaneExit(_ pane: TerminalPane) {
        // 窗格可能属于后台标签页（同主机另一条会话 / 后台分屏）——按**对象**
        // 找，不假定是当前标签页。
        guard let tab = tabs.tabs.first(where: { t in
            t.panes.contains { $0 === pane }
        }) else { return }        // 标签页已经没了（用户先关了）：无需处理
        if tab.workspacePanes.count > 1 {
            tab.closePane(pane)   // 还有别的窗格：收掉这个、布局塌陷
        } else {
            tabs.requestClose(tab) // 最后一个窗格退出 = 关标签页
        }
    }

    // MARK: - 设置（终端配置 + 应用偏好）

    public var terminalFontSelection: TerminalFontSelection {
        TerminalFontSelection(config: terminalConfigText)
    }

    public func selectTerminalFont(family: String?) throws {
        guard !terminalFontSelection.external else {
            throw SettingsStoreError.invalid("字体由附加配置文件管理，请在配置文件页修改")
        }
        guard terminalApp != nil else {
            throw SettingsStoreError.invalid(terminalConfigError ?? "终端引擎不可用")
        }
        if let family { try TerminalFonts.validate(family: family) }
        try replaceTerminalConfig(TerminalFontSelection.replacing(in: terminalConfigText, family: family))
    }

    /// Sparse controls and the full editor share one checked candidate commit.
    public func applyTerminalSettings(_ edits: [(String, String?)]) throws {
        let before = try settings.rawConfig()
        guard !TerminalFontSelection(config: before).external else {
            throw SettingsStoreError.invalid("附加配置参与生效，请在配置文件页修改并核对结果")
        }
        let text = SettingsStore.merging(before, edits: edits)
        try replaceTerminalConfig(text, expectedConfiguration: Data(before.utf8))
    }

    /// 已加载的终端配置文件快照（读取不代表引擎已经应用）。
    public var terminalConfigText: String { settings.loadedText ?? "" }

    /// Read a new disk snapshot without implying that the engine applied it.
    public func readTerminalConfigFromDisk() throws {
        defer { terminalConfigRevision += 1 }
        _ = try settings.reload()
    }

    /// 终端配置文件被写回过一次（每次写回成功 +1）。
    ///
    /// 为什么需要它：`terminalConfigText` / `themeSelection` 读取已加载的文件
    /// 快照，不是 `@Published` 存储属性。显式重新读取也要广播；引擎实际值
    /// 另由强类型 getter 查询。设置页那两个配色下拉的
    /// 标题、勾选与「当前生效的配置」那块都读它们——没有这声广播，SwiftUI
    /// 就不知道要重画，改完一档之后标题会停在旧值上（用户实测：「选了能生效，
    /// 但下拉还显示原来那个」）。写回成功的每一处都必须 `+= 1`。
    @Published public private(set) var terminalConfigRevision = 0

    /// Prepare without changing the engine, commit the exact text snapshot,
    /// then apply the retained parsed configuration without rereading files.
    public func replaceTerminalConfig(_ text: String, expectedConfiguration: Data? = nil) throws {
        let before = try settings.rawConfig()
        try settings.requireUnchanged(expected: expectedConfiguration ?? Data(before.utf8))
        guard let app = terminalApp else {
            throw SettingsStoreError.invalid(terminalConfigError ?? "终端引擎不可用")
        }
        let candidate = try app.prepareConfiguration(text: text,
            origin: URL(fileURLWithPath: settings.terminalConfigPath).standardizedFileURL.path)
        do {
            try settings.saveTerminalConfig(text)
        } catch {
            if case ConfigStoreError.committed = error {
                terminalConfigError = "\(error)；终端未应用这次候选配置"
                terminalConfigRevision += 1
            }
            throw error
        }
        do {
            try applyPreparedTerminalConfiguration(candidate)
        } catch {
            terminalConfigError = "磁盘已保存，终端应用失败：\(error)"
            terminalConfigRevision += 1
            throw SettingsStoreError.committed(String(describing: error))
        }
        terminalConfigError = nil
        terminalConfigRevision += 1
    }

    /// 终端配色的两档选择（设置页「外观」用）。
    ///
    /// 读的是 `theme` 键**原文**（不是 terminalSetting——那个按键的 C 类型
    /// 问引擎，而 `theme` 是个结构化的值，引擎给的是解析后的形态）。
    public var themeSelection: ThemeSelection {
        ThemeSelection.parse(SettingsStore.value(of: "theme",
                                                 in: terminalConfigText))
    }

    /// 保存配色两档（浅色 / 深色）。写回 `theme` 键；都没选就把那行删掉。
    public func applyThemeSelection(_ selection: ThemeSelection) throws {
        try applyTerminalSettings([("theme", selection.configValue)])
    }

    /// 可用的配色名（随包提供的那 600 多套）。
    public var availableThemes: [String] { settings.availableThemes() }

    /// Read a supported scalar from the applied engine, independently of editor text.
    public func terminalSetting(_ key: String) -> String? {
        guard settings.loadedText != nil else { return nil }
        // Query the current conditional configuration with its native C type.
        // Unsupported structured values remain unknown instead of returning raw text.
        switch SettingsStore.cValueKind(of: key) {
        case .float:
            if let v = terminalApp?.float(key) { return "\(v)" }
        case .double:
            if let v = terminalApp?.double(key) { return "\(v)" }
        case .bool:
            if let v = terminalApp?.bool(key) { return v ? "true" : "false" }
        case .text:
            break
        }
        return nil
    }

    // MARK: - ssh_config 编辑（M3：文件即真相的写侧）

    /// 当前配置的解析模型（编辑器用；reload 后重建）。
    @Published public private(set) var configModel: HostListModel?
    /// 通配块（`Host *` 等）：列表不显示，但编辑器可见可改（M1 遗留项）。
    @Published public private(set) var wildcards: [String] = []

    /// 编辑器保存：稀疏补丁 → 冲突检测 → 备份 → 原子写回 → 重新加载。
    /// 任何一步失败都显式抛错（UI 转成提示），绝不静默丢编辑。
    public func applyEdits(alias: String,
                           _ edits: [(String, [String]?)]) throws {
        guard !edits.isEmpty else { return }
        try commitConfigurationEdit { try $0.edit(alias: alias, edits) }
    }

    /// 新建 Host 块（reset=true：文件末尾追加新块）。
    public func addHost(alias: String,
                        _ edits: [(String, [String]?)], expectedConfiguration: Data? = nil) throws {
        try verifyEditorSnapshot(expectedConfiguration)
        try commitConfigurationEdit {
            guard !$0.contains(alias: alias) else { throw GhostermError.failed("别名已存在，不能覆盖已有 Host 块") }
            try $0.edit(alias: alias, edits, reset: true)
        }
    }

    private func commitConfigurationEdit(_ update: (HostListModel) throws -> Void) throws {
        guard let model = configModel, let store = configStore else {
            throw GhostermError.failed("配置未加载")
        }
        // Reparse the real source so Include provenance keeps its file context.
        // The published model remains immutable throughout candidate editing.
        let candidate = try HostListModel(path: configPath)
        guard Data(candidate.rawConfig.utf8) == Data(model.rawConfig.utf8) else {
            throw ConfigStoreError.conflict("配置内容与编辑快照不一致")
        }
        try update(candidate)
        do { try store.save(candidate.serialized) }
        catch {
            if case ConfigStoreError.committed = error { reload() }
            throw error
        }
        configModel = candidate
        hosts = candidate.hosts
        wildcards = candidate.wildcardPatterns
        loadError = nil
    }

    private func verifyEditorSnapshot(_ expected: Data?) throws {
        if let expected, expected != configModel.map({ Data($0.rawConfig.utf8) }) {
            throw ConfigStoreError.conflict("此表单打开后配置已修改，请重新打开表单")
        }
    }

    public func previewHostRename(alias: String, replacement: String, edits: [(String, [String]?)], expectedConfiguration: Data?) throws -> String {
        try verifyEditorSnapshot(expectedConfiguration)
        guard let current = configModel else { throw GhostermError.failed("配置未加载") }
        let before = try current.resolved(alias: alias)
        let candidate = try HostListModel(path: configPath)
        guard Data(candidate.rawConfig.utf8) == Data(current.rawConfig.utf8) else {
            throw ConfigStoreError.conflict("配置内容与改名快照不一致")
        }
        if !edits.isEmpty { try candidate.edit(alias: alias, edits) }
        try candidate.rename(alias: alias, to: replacement)
        let after = try candidate.resolved(alias: replacement)
        func summary(_ host: ResolvedHost, fallback: String) -> String {
            "\(host.user ?? NSUserName())@\(host.hostname ?? fallback):\(host.port)"
        }
        return "\(alias)：\(summary(before, fallback: alias))\n→ \(replacement)：\(summary(after, fallback: replacement))\n改名可能改变继承规则。已有连接继续使用原参数和 UUID。"
    }

    public func saveHost(alias: String, replacement: String, edits: [(String, [String]?)], expectedConfiguration: Data? = nil) throws {
        if edits.isEmpty && Data(alias.utf8) == Data(replacement.utf8) { return }
        try verifyEditorSnapshot(expectedConfiguration)
        guard Data(alias.utf8) != Data(replacement.utf8) else {
            try applyEdits(alias: alias, edits)
            return
        }
        try commitConfigurationEdit {
            if !edits.isEmpty { try $0.edit(alias: alias, edits) }
            try $0.rename(alias: alias, to: replacement)
        }
    }

    /// A failed read must not masquerade as an empty editable block.
    public func directives(of alias: String) throws -> [(keyword: String, values: [String])] {
        guard let configModel else { throw GhostermError.failed("配置未加载") }
        return try configModel.directives(of: alias)
    }

    private let privateKeyImportQueue = DispatchQueue(label: "dev.ghosterm.private-key-import")

    /// Independent, explicit import action; cancelling a form does not undo it.
    public func reimportPrivateKey(_ reference: String) async throws {
        let path = expandTilde([reference])[0]
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            privateKeyImportQueue.async {
                do { try NativePrivateKeyStore.reimport(path); continuation.resume() }
                catch { continuation.resume(throwing: error) }
            }
        }
    }

    // MARK: - 连接

    /// 打开一个新标签页连到 target（**不再**拆掉当前标签页——
    /// 这是标签页模型与旧「一窗一面」的语义分界）。
    ///
    /// **每次都开一个新标签页**（不去重）：本地 shell 与 SSH 都一样。
    ///
    /// 去重只留给 SFTP（`openSFTP` 里按目标复用一条浏览会话是合理的）。
    /// 终端会话被吞掉就是用户实测的「同一个 ssh 无法创建多个 tab」——
    /// 同一台机器上开几个 shell 是常规用法。
    public func connect(to target: Target, cwd: String? = nil) {
        guard let terminalApp = terminalApp else {
            statusMessage = "libghostty 初始化失败"
            return
        }
        let kind: TabKind
        switch target {
        case .localShell:
            // 本地 shell **不做去重**：每次点都该开一个新的（终端本来就是这样
            // ——要在一个窗口里开几个 shell 是常规用法）。
            kind = .localShell
        case .host(let pattern):
            let entry = hosts.first { $0.matchesPattern(pattern) }
            // `.ssh`：**每次点击都开一个新标签页**（同一台主机也允许开
            // 多个）——去重只留给 SFTP（同一目标复用一条浏览会话是合理的），
            // 终端会话不该被吞掉（用户实测）。
            kind = .ssh(RemoteTarget(pattern: pattern,
                                     user: entry?.user ?? NSUserName(),
                                     host: entry?.hostname ?? pattern,
                                     port: UInt16(clamping: entry?.port ?? 22), connectionIssues: entry?.connectionIssues ?? []))
        }
        // 两个分支都**不带身份**、且这里**没有去重**：一律新建标签页
        // （同一台主机 / 本地 shell 都允许开多个，见方法头注释）。

        // 标签页与窗格先建（状态 .connecting：屏幕显示「连接中…」），
        // 连接在工作线程建好后再 attach——绝不空窗假装已连。
        let pane = TerminalPane()
        wireFrontendDiscovery(pane)
        // 单窗格标签页退出 = 关标签页（与分屏同一策略；最后一个窗格不留画面）。
        pane.onExit = { [weak self] p in self?.handlePaneExit(p) }
        let tab = TerminalTab(kind: kind, pane: pane)
        tabs.add(tab)

        switch target {
        case .localShell:
            startLocal(pane: pane, app: terminalApp, cwd: cwd)
            statusMessage = "本地 shell"
        case .host(let pattern):
            let entry = hosts.first { $0.matchesPattern(pattern) }
            startSSH(pane: pane, entry: entry, pattern: pattern, terminalApp: terminalApp)
        }
    }

    /// 新建标签页，**沿用当前标签页的类型**（⌘T / 溢出菜单「新建标签页」）：
    ///   本地 shell → 本地 shell；
    ///   SSH        → 同一台主机再开一条（别名回当前 ssh_config 求值）；
    ///   SFTP       → 同目标再开一条；
    ///   没有标签页（空态）→ 本地 shell。
    ///
    /// 与 iGhostVT 的 ⌘T 同口径（它的 `.activeTab` 分支：远端开远端、本地开
    /// 本地）。已开着同名目标**不去重**——终端开多个 shell 是常规用法。
    public func newTabLikeActive() {
        if let tab = tabs.activeTab, tab.panes.isEmpty {
            if tab.remoteFiles != nil, let target = tab.kind.remoteTarget { openSFTP(target) }
            else { openLocalFiles(newTab: true) }
            return
        }
        guard let kind = tabs.activeTab?.kind else {
            connect(to: .localShell)   // 空态：本地 shell
            return
        }
        switch kind {
        case .localShell:
            // 本地：顺带继承当前目录（同一个 shell 常在同一处继续开新的）。
            connect(to: .localShell, cwd: tabs.activeTab?.activePane?.currentDirectory)
        case .ssh(let t): connect(to: .host(t.pattern))
        case .sftp(let t): openSFTP(for: t.pattern)
        case .localFiles: openLocalFiles(newTab: true)
        }
    }

    private func startSSH(pane: TerminalPane, entry: HostEntry?, pattern: String,
                          terminalApp: TerminalApp) {
        let target = sshTarget(for: pattern), id = pane.id
        let owner = DispatchQueue(label: "dev.ghosterm.terminal-connect")
        statusMessage = "连接 \(target.user)@\(target.host)…"
        pane.onStop = { [weak self] in self?.cancelConnection(id) }
        let opening = beginConnectionOpening(id), operations = sshOpeningOperations
        Task { @MainActor [weak self, weak pane] in
            guard let self else { return }
            defer { self.finishConnectionOpening(id, opening: opening) }
            do {
                let transport: Transport = try await withTaskCancellationHandler(operation: {
                    let ssh = try await self.authenticate(target, id: id, owner: owner, opening: opening)
                    return try await withCheckedThrowingContinuation { continuation in
                        owner.async {
                            do {
                                let remaining = try opening.remainingMilliseconds()
                                let transport = try operations.shell(ssh, remaining, opening.cancellation)
                                transport.releaseOn(owner); continuation.resume(returning: transport)
                            } catch { continuation.resume(throwing: error) }
                        }
                    }
                }, onCancel: { opening.cancel() })
                guard self.connectionOpenings[id] === opening else { return }
                let cancelled = self.cancelledConnects.take(id)
                guard let pane, self.tabs.tabs.contains(where: { $0.panes.contains(where: { $0.id == id }) }) else { return }
                if cancelled || opening.cancellation.isCancelled { pane.markFailed("SSH 连接已取消"); return }
                pane.onStop = nil
                let surface = try TerminalSurface(app: terminalApp)
                let session = SessionController(transport: transport, surface: surface)
                pane.attach(surface: surface, session: session); pane.start()
            } catch {
                guard self.connectionOpenings[id] === opening else { return }
                _ = self.cancelledConnects.take(id)
                guard let pane, self.tabs.tabs.contains(where: { $0.panes.contains(where: { $0.id == id }) }) else { return }
                pane.markFailed(opening.cancellation.isCancelled ? "SSH 连接已取消" : connectionErrorMessage(error))
            }
        }
    }

    private func startLocal(pane: TerminalPane, app: TerminalApp, cwd: String?) {
        do {
            let surface = try TerminalSurface(app: app)
            let session = try SessionController.localShell(surface: surface,
                executable: preferences.resolvedShell, cwd: cwd ?? NSHomeDirectory(), kind: .shell)
            pane.attach(surface: surface, session: session); pane.start()
        } catch { pane.markFailed(connectionErrorMessage(error)) }
    }

    /// 普通启动只按偏好新开 shell；不读取旧记录、探测 tmux 或启动服务。
    public func startInitialSessions() {
        guard !startedInitialSessions else { return }; startedInitialSessions = true
        if tabs.tabs.isEmpty && preferences.shellOnLaunch {
            connect(to: .localShell)
            tabs.activeTab?.isLocked = preferences.lockTabsOnLaunch
        }
    }

    private func tab(withID id: UUID) -> TerminalTab? {
        tabs.tabs.first { $0.id == id }
    }

    /// 取一个标签页里的窗格。`paneID` 为 nil 时取主窗格（v0 的旧口径）。
    private func pane(withID tabID: UUID, paneID: UUID? = nil) -> TerminalPane? {
        guard let tab = tab(withID: tabID) else { return nil }
        guard let paneID else { return tab.activePane }
        return tab.panes.first { $0.id == paneID }
    }

    // MARK: - 主机密钥对话框（真三态：未知 ≠ 拒绝）

    /// 测试辅助：伪造「终端配置无效」。
    ///
    /// 真实路径只在 init 里按 libghostty 的解析结果设一次，测试没法让它出错；
    /// 配置错误条只在这个值非空时才画，不给钩子就
    /// 没法钉住「配置坏了说得出口」这条。
    public func setTerminalConfigErrorForTesting(_ message: String?) {
        terminalConfigError = message
    }

    /// 测试辅助：停掉全部标签页并清空（断言收尾用）。
    public func disconnectForTesting() {
        for tab in tabs.tabs { cancelInFlight(tab) }
        tabs.removeAll()
        statusMessage = "未连接"
    }

    /// 普通终端的前台作业与传输仍需关闭确认。
    public func shouldTerminate() -> Bool {
        !TransferManager.shared.hasActive && !tabs.tabs.contains { tab in
            if case .sftp = tab.kind { return tab.closeNeedsConfirmation }
            return tab.panes.contains { $0.closeNeedsConfirmation }
        }
    }

    /// One authentication implementation for ordinary terminals, files and
    /// optional management/attach. Prompts stay on main; DNS/libssh2 on owner.
    func authenticate(_ target: SSHConnectionTarget, id: UUID, owner: DispatchQueue,
                      opening: SSHConnectionOpening? = nil) async throws -> SSHSession {
        if cancelledConnects.contains(id) { throw ConnectCancelled() }
        if target.connectionIssues.contains(where: { $0.keyword == "proxyjump" }) {
            let targets = try configuredSSHRoute(for: [target.alias])
            guard targets.count > 1 else { throw GhostermError.failed("SSH 跳板配置已改变，请重新连接") }
            guard let finalTarget = targets.last, finalTarget.host == target.host,
                  finalTarget.user == target.user, finalTarget.port == target.port,
                  finalTarget.knownHosts == target.knownHosts, finalTarget.identityPaths == target.identityPaths else {
                throw GhostermError.failed("SSH 目标配置已改变，请重新连接")
            }
            return try await authenticateRoute(targets, alias: target.alias, id: id, owner: owner, opening: opening)
        }
        let selector = SSHCredentialSelector(target: target, storage: sshCredentialStorage, ask: { [weak self] kind in
            guard let self, !self.cancelledConnects.contains(id) else { return nil }
            let prompt = CredentialPrompt(host: target.host, port: target.port, user: target.user, kind: kind)
            self.pendingCredential = prompt; defer { self.pendingCredential = nil }
            return self.interaction?.askCredential(prompt)
        }, hostkey: { [weak self] type, fingerprint in
            guard let self, !self.cancelledConnects.contains(id) else { return .unknown }
            let prompt = HostKeyPrompt(host: target.host, port: target.port, keyType: type, fingerprint: fingerprint)
            self.pendingHostKey = prompt; defer { self.pendingHostKey = nil }
            return self.interaction?.confirmHostKey(prompt) ?? .unknown
        }, notice: { [weak self] message in self?.statusMessage = message })
        return try await withCheckedThrowingContinuation { continuation in
            var request: UInt64 = 0
            let generation = UUID()
            let flow = SSHAuthenticationFlow(owner: owner,
                options: SSHOptions(host: target.host, port: target.port, username: target.user, knownHostsPath: target.knownHosts),
                opening: opening, configurationIssues: target.connectionIssues, prompts: selector.prompts, nextID: { guard request < UInt64.max else { return nil }; request += 1; return request },
                completion: { [weak self] result in
                    if case .success(let value) = result { value.connection.releaseOn(owner) }
                    DispatchQueue.main.async { self?.finishAuthentication(id, generation: generation) }
                    continuation.resume(with: result.map { $0.connection }.mapError { $0 as Error })
                })
            registerAuthentication(id, owner: owner, flow: flow, generation: generation)
            owner.async { flow.start() }
        }
    }
    private func authenticateRoute(_ targets: [SSHConnectionTarget], alias: String, id: UUID, owner: DispatchQueue,
                                   opening: SSHConnectionOpening? = nil) async throws -> SSHSession {
        let valid = { [weak self] in
            guard let self, !self.cancelledConnects.contains(id), self.loadError == nil else { return false }
            return (try? self.configuredSSHRoute(for: [alias])) == targets
        }
        let selectors = targets.map { target in
            SSHCredentialSelector(target: target, storage: sshCredentialStorage, ask: { [weak self] kind in
                guard let self, valid() else { return nil }
                let prompt = CredentialPrompt(host: target.host, port: target.port, user: target.user, kind: kind)
                self.pendingCredential = prompt; defer { self.pendingCredential = nil }
                let answer = self.interaction?.askCredential(prompt)
                return valid() ? answer : nil
            }, hostkey: { [weak self] type, fingerprint in
                guard let self, valid() else { return .unknown }
                let prompt = HostKeyPrompt(host: target.host, port: target.port, keyType: type, fingerprint: fingerprint)
                self.pendingHostKey = prompt; defer { self.pendingHostKey = nil }
                let decision = self.interaction?.confirmHostKey(prompt) ?? .unknown
                return valid() ? decision : .unknown
            }, notice: { [weak self] message in self?.statusMessage = message })
        }
        let prompts = selectors.map { selector -> SSHAuthenticationPrompts in
            let original = selector.prompts
            return SSHAuthenticationPrompts(credential: { request, attempt, done in
                guard valid() else { done(.failure(ConnectCancelled())); return }
                original.credential(request, attempt) { result in done(valid() ? result : .failure(ConnectCancelled())) }
            }, hostkey: original.hostkey, cancelled: original.cancelled, methods: original.methods,
                accepted: { request, attempt in if valid() { original.accepted(request, attempt) } })
        }
        let connection: SSHSession = try await withCheckedThrowingContinuation { continuation in
            var request: UInt64 = 0
            let generation = UUID()
            let flow = SSHRouteAuthenticationFlow(owner: owner, options: targets.map {
                SSHOptions(host: $0.host, port: $0.port, username: $0.user, knownHostsPath: $0.knownHosts)
            }, prompts: prompts, opening: opening, nextID: { guard request < UInt64.max else { return nil }; request += 1; return request }, completion: { [weak self] result in
                if case .success(let value) = result { value.connection.releaseOn(owner) }
                DispatchQueue.main.async { self?.finishAuthentication(id, generation: generation) }
                continuation.resume(with: result.map { $0.connection }.mapError { $0 as Error })
            })
            registerAuthentication(id, owner: owner, flow: flow, generation: generation); owner.async { flow.start() }
        }
        guard valid() else { owner.async { connection.close() }; throw ConnectCancelled() }
        return connection
    }
    func registerAuthentication(_ id: UUID, owner: DispatchQueue, flow: SSHAuthenticationOperation, generation: UUID) {
        if let (previousOwner, previous) = authentications[id] { previousOwner.async { previous.cancel() } }
        authenticationGenerations[id] = generation
        authentications[id] = (owner, flow)
    }
    func finishAuthentication(_ id: UUID, generation: UUID) {
        guard authenticationGenerations[id] == generation else { return }
        authenticationGenerations.removeValue(forKey: id); authentications.removeValue(forKey: id)
    }
    func beginConnectionOpening(_ id: UUID) -> SSHConnectionOpening {
        connectionOpenings[id]?.cancel()
        let opening = makeSSHConnectionOpening()
        connectionOpenings[id] = opening
        if cancelledConnects.contains(id) { opening.cancel() }
        return opening
    }
    func finishConnectionOpening(_ id: UUID, opening: SSHConnectionOpening) {
        if connectionOpenings[id] === opening { connectionOpenings.removeValue(forKey: id) }
    }

    func cancelConnection(_ id: UUID) {
        cancelledConnects.add(id)
        connectionOpenings[id]?.cancel()
        if let (owner, flow) = authentications[id] { owner.async { flow.cancel() } }
    }
    func finishChooserAuthentication(_ id: UUID) { _ = cancelledConnects.take(id) }
    func sshTarget(for pattern: String) -> SSHConnectionTarget {
        let entry = hosts.first { $0.matchesPattern(pattern) }
        return SSHConnectionTarget(alias: pattern, host: entry?.hostname ?? pattern,
            user: entry?.user ?? NSUserName(), port: UInt16(clamping: entry?.port ?? 22),
            knownHosts: knownHostsPath, identityPaths: expandTilde(entry?.identityFiles ?? []), connectionIssues: entry?.connectionIssues ?? [])
    }

    func configuredSSHRoute(for aliases: [String]) throws -> [SSHConnectionTarget] {
        guard loadError == nil, let configModel else { throw GhostermError.failed("SSH 配置尚未载入") }
        return try configModel.sshRoute(aliases: aliases, knownHosts: knownHostsPath)
    }

    /// Opens the optional plugin channel through the same resolved ssh_config
    /// / ProxyJump route as terminal and SFTP connections. DNS is resolved only
    /// for the first hop; inner hostnames are forwarded through direct-tcpip.
    public func openPluginSSHChannel(alias: String, bootstrap: String) async throws -> ExtensionController.ChannelState {
        guard let extensionController else { throw GhostermError.failed("此构建未启用插件运行时") }
        let hops = try await resolvePluginSSHRoute(alias: alias)
        return try await extensionController.openSSHChannel(hops: hops, bootstrap: bootstrap)
    }

    /// Resolves the product ssh_config route and the first hop's address for
    /// use by the Native plugin channel; later hops stay name-based.
    public func resolvePluginSSHRoute(alias: String) async throws -> [ExtensionController.SSHHop] {
        let targets = try configuredSSHRoute(for: [alias])
        guard let first = targets.first else { throw GhostermError.failed("SSH 配置没有生成连接路线") }
        let address = try await resolvePluginAddress(first.host)
        return targets.enumerated().map { index, target in
            ExtensionController.SSHHop(host: target.host, port: target.port, username: target.user,
                knownHostsPath: target.knownHosts,
                addressFamily: index == 0 ? address.family : nil,
                addressBytes: index == 0 ? address.bytes : nil,
                scope: index == 0 ? address.scope : 0,
                viaHost: index == 0 ? nil : target.host)
        }
    }

    /// Opens the configured plugin route and answers each Native SSH
    /// challenge through the App's existing host-key and Keychain credential
    /// flows. Caller owns subsequent Session / consent / Runtime work.
    public func authenticatePluginSSHChannel(alias: String, bootstrap: String) async throws -> ExtensionController.ChannelState {
        guard let extensionController else { throw GhostermError.failed("此构建未启用插件运行时") }
        let targets = try configuredSSHRoute(for: [alias])
        guard !targets.isEmpty else { throw GhostermError.failed("SSH 配置没有生成连接路线") }
        var state = try await openPluginSSHChannel(alias: alias, bootstrap: bootstrap)
        let selectors = targets.map { target in
            SSHCredentialSelector(target: target, storage: sshCredentialStorage, ask: { [weak self] kind in
                guard let self else { return nil }
                let prompt = CredentialPrompt(host: target.host, port: target.port, user: target.user, kind: kind)
                return self.interaction?.askCredential(prompt)
            }, hostkey: { _, _ in .unknown }, notice: { [weak self] message in
                self?.statusMessage = message
            })
        }
        var attempts = Array(repeating: UInt32(0), count: targets.count)
        var credentialRequests = Array(repeating: UInt64(0), count: targets.count)
        var lastHandledChallenge: UInt64?
        let deadline = ProcessInfo.processInfo.systemUptime + 60
        do {
            while state.phase == ExtensionChannel.Phase.connecting.rawValue {
                try Task.checkCancellation()
                guard ProcessInfo.processInfo.systemUptime < deadline else {
                    throw GhostermError.failed("插件 SSH 认证超过 60 秒")
                }
                guard try configuredSSHRoute(for: [alias]) == targets else {
                    throw GhostermError.failed("SSH 配置在插件认证期间发生变化")
                }
                let index = Int(state.hop)
                if state.authentication.indices.contains(index), state.challenge > 0,
                   state.challenge != lastHandledChallenge {
                    let auth = state.authentication[index]
                    if auth.phase == SSHAuthenticator.Phase.hostkey.rawValue {
                        let prompt = HostKeyPrompt(host: targets[index].host, port: targets[index].port,
                                                   keyType: auth.keyType, fingerprint: auth.fingerprint)
                        let decision = interaction?.confirmHostKey(prompt) ?? .unknown
                        guard decision != .unknown else {
                            throw GhostermError.failed("未确认插件 SSH 主机密钥，连接已取消")
                        }
                        let result = try await extensionController.decideSSHHost(hop: state.hop,
                            challenge: state.challenge, decision: decision.rawValue)
                        guard result == 0 else { throw GhostermError.failed("无法保存插件 SSH 主机信任记录") }
                        lastHandledChallenge = state.challenge
                    } else if auth.phase == SSHAuthenticator.Phase.credentials.rawValue {
                        attempts[index] &+= 1
                        let prompts = selectors[index].prompts
                        prompts.methods(state.challenge, auth.methods, auth.error)
                        let credential = try await withCheckedThrowingContinuation { continuation in
                            prompts.credential(state.challenge, attempts[index]) { result in
                                continuation.resume(with: result)
                            }
                        } as GhostermCore.SSHCredential
                        let submitted: Int32
                        switch credential {
                        case .password(let value):
                            submitted = try await extensionController.submitSSHCredential(hop: state.hop,
                                challenge: state.challenge, credential: .password(value))
                        case .privateKey(let pem, let passphrase):
                            submitted = try await extensionController.submitSSHCredential(hop: state.hop,
                                challenge: state.challenge, credential: .privateKey(pem: pem, passphrase: passphrase))
                        }
                        guard submitted == 0 else { throw GhostermError.failed("插件 SSH 凭据已被拒绝") }
                        credentialRequests[index] = state.challenge
                        lastHandledChallenge = state.challenge
                    } else if auth.phase == SSHAuthenticator.Phase.failed.rawValue {
                        throw GhostermError.status(auth.error, "插件 SSH 第 \(index + 1) 跳认证失败")
                    }
                }
                state = try await extensionController.stepSSHChannel(milliseconds: 10)
                if state.phase == ExtensionChannel.Phase.connecting.rawValue {
                    try await Task.sleep(nanoseconds: 10_000_000)
                }
            }
            guard state.phase == ExtensionChannel.Phase.ready.rawValue else {
                throw GhostermError.status(state.error, "插件 SSH 通道未能就绪")
            }
            for index in selectors.indices where credentialRequests[index] > 0 {
                selectors[index].prompts.accepted(credentialRequests[index], attempts[index])
            }
            return state
        } catch {
            try? await extensionController.stopSSHChannel()
            try? await extensionController.releaseFinished()
            throw error
        }
    }

    public func frontendCandidateIsCurrent(_ candidate: FrontendCandidate) -> Bool {
        !candidate.expired && tabs.tabs.contains { tab in
            tab.panes.contains { $0.id == candidate.sourceID && $0.isRunning && $0.discoveryGeneration == candidate.sourceGeneration }
        }
    }

    public func dismissFrontendCandidate(_ id: UUID) {
        frontendCandidates.removeAll { $0.id == id }
    }

    private func wireFrontendDiscovery(_ pane: TerminalPane) {
        pane.onDiscovery = { [weak self] pane, offers, skipped, issue in
            guard let self, self.extensionController != nil,
                  let tab = self.tabs.tabs.first(where: { $0.panes.contains(where: { $0.id == pane.id }) }) else { return }
            let source = tab.kind.remoteTarget.map { "外层 SSH：\($0.pattern)（执行端未核实）" } ?? "本地终端（执行端未核实）"
            if let issue { self.pendingFrontendIssue = issue }
            if skipped > 0 { self.pendingFrontendIssue = "此终端已跳过超限或损坏的候选提示（\(skipped)）；仍可手工附着。" }
            for offer in offers {
                self.pendingFrontendCandidates.removeAll { $0.sourceID == pane.id && $0.offer.instance == offer.instance }
                self.pendingFrontendCandidates.append(FrontendCandidate(id: UUID(), sourceID: pane.id,
                    sourceGeneration: pane.discoveryGeneration, sourceDescription: source, offer: offer,
                    receivedAt: ProcessInfo.processInfo.systemUptime))
                if self.pendingFrontendCandidates.count > 8 { self.pendingFrontendCandidates.removeFirst() }
            }
            guard !self.frontendUpdatePending else { return }
            self.frontendUpdatePending = true
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(100)) { [weak self] in
                guard let self else { return }
                self.frontendUpdatePending = false
                var candidates = self.frontendCandidates.filter(self.frontendCandidateIsCurrent)
                for candidate in self.pendingFrontendCandidates where self.frontendCandidateIsCurrent(candidate) {
                    candidates.removeAll { $0.sourceID == candidate.sourceID && $0.offer.instance == candidate.offer.instance }
                    candidates.append(candidate)
                    if candidates.count > 8 { candidates.removeFirst() }
                }
                self.pendingFrontendCandidates.removeAll()
                self.frontendCandidates = candidates
                if let issue = self.pendingFrontendIssue { self.frontendDiscoveryIssue = issue; self.pendingFrontendIssue = nil }
            }
        }
    }

    /// Metadata only, through a caller-selected native route. Never adopts a
    /// Session or infers a host from the observed declaration.
    public func inspectForegroundPlugin(sshAlias: String, executable: String, directory: String,
                                         instance: String, localLaunch: ExtensionLocalLaunch? = nil) async throws -> PluginForegroundDeclaration {
        guard let extensionController, !extensionRunBusy, extensionRun == nil else { throw GhostermError.failed("已有插件运行或授权流程") }
        extensionRunBusy = true; extensionRunIssue = nil
        defer { extensionRunBusy = false }
        var opened = false
        do {
            if let localLaunch {
                let argv = try PluginForegroundDeclaration.inspectionArguments(executable: executable, directory: directory, instance: instance, local: true)
                _ = try await extensionController.openLocalChannel(executable: executable, arguments: argv, launch: localLaunch)
            } else {
                let command = try PluginForegroundDeclaration.inspectionBootstrap(executable: executable, directory: directory, instance: instance)
                _ = try await authenticatePluginSSHChannel(alias: sshAlias, bootstrap: command)
            }
            opened = true
            let value = try await extensionController.readForegroundDeclaration(instance: instance)
            try await extensionController.stopSSHChannel()
            try await extensionController.releaseFinished()
            return value
        } catch {
            if opened || localLaunch != nil {
                if opened { try? await extensionController.stopSSHChannel() }
                do { try await extensionController.releaseFinished() }
                catch { extensionRunIssue = "前台检查资源仍待释放：\(error.localizedDescription)" }
                if localLaunch != nil {
                    extensionLastLocalProcess = try? await extensionController.localProcessState()
                    extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
                }
            }
            throw error
        }
    }

    /// Reviews the exact approved Client command, starts its remote Executor,
    /// completes SSH authentication and Native Session negotiation, then
    /// leaves the UI at the explicit Native consent decision.
    public func startExtensionClient(packageID: String, packageDigest: String, command: String,
                                     arguments: [String], sshAlias: String, executable: String,
                                     executorDirectory: String, executorDigest: String,
                                     methods: [String], hostMethods: [String],
                                     scopedRoots: [ExtensionRelayBootstrap.ScopedRoot] = [],
                                     processPrograms: [ExtensionRelayBootstrap.ProcessProgram] = [],
                                     processEnvironmentNames: [String] = [],
                                     executorExecution: ExtensionRelayBootstrap.Execution? = nil,
                                     executorAttachment: ExtensionRelayBootstrap.Attachment? = nil,
                                     clientWorkspace: ExtensionController.ClientWorkspace? = nil,
                                     localLaunch: ExtensionLocalLaunch? = nil) async {
        guard let extensionController else { extensionRunIssue = "此构建未启用插件运行时"; return }
        guard !extensionRunBusy, extensionRun == nil else { extensionRunIssue = "已有插件运行或授权流程"; return }
        extensionRunBusy = true; extensionRunIssue = nil; extensionLastLocalProcess = nil; extensionLastLocalTemporary = nil
        do {
            let selected = try await extensionController.resolve(command: command, selectedID: packageID,
                                                                  selectedDigest: packageDigest)
            guard selected.status == 0, selected.packageID == packageID,
                  selected.packageDigest == packageDigest, selected.command?.path == command else {
                throw GhostermError.failed("只能启动本进程已批准摘要中的精确 Client 命令")
            }
            let tableContribution = selected.command?.views.contains("table") == true
            if tableContribution && !["session.rpc", "ui.table"].allSatisfy(selected.requiredCapabilities.contains) {
                throw GhostermError.failed("表格贡献必须声明 session.rpc 与 ui.table")
            }
            let diffContribution = selected.command?.views.contains("diff") == true
            if diffContribution && !["session.rpc", "ui.diff"].allSatisfy(selected.requiredCapabilities.contains) {
                throw GhostermError.failed("差异贡献必须声明 session.rpc 与 ui.diff")
            }
            let taskContribution = selected.command?.views.contains("task") == true
            if taskContribution && !["session.rpc", "ui.task"].allSatisfy(selected.requiredCapabilities.contains) {
                throw GhostermError.failed("任务贡献必须声明 session.rpc 与 ui.task")
            }
            let registeredViews = selected.requiredCapabilities.contains("ui.views")
            if registeredViews && !(tableContribution || diffContribution || taskContribution) {
                throw GhostermError.failed("视图注册服务需要所选命令声明标准视图")
            }
            var sessionMethods = !registeredViews && tableContribution ? Array(Set(methods + PluginTableModel.methods)).sorted() : methods
            if !registeredViews && diffContribution { sessionMethods = Array(Set(sessionMethods + PluginDiffModel.methods)).sorted() }
            if !registeredViews && taskContribution { sessionMethods = Array(Set(sessionMethods + PluginTaskModel.methods)).sorted() }
            var clientServices = tableContribution ? ["ui.table"] : []
            if diffContribution { clientServices.append("ui.diff") }
            if taskContribution { clientServices.append("ui.task") }
            if registeredViews { clientServices.append("ui.views") }
            guard (sessionMethods.isEmpty && hostMethods.isEmpty) || selected.requiredCapabilities.contains("session.rpc") else {
                throw GhostermError.failed("此 Client 包未声明 session.rpc，不能协商 RPC 方法")
            }
            try clientWorkspace?.validate(requiredCapabilities: selected.requiredCapabilities)
            if ["ai codex", "ai claude"].contains(command), let executorAttachment, !executorAttachment.remoteProcesses {
                throw GhostermError.failed("此命令需要明确允许沿用前台实例已有的远端业务进程范围")
            }
            let decodedArguments = try await extensionController.decodeArguments(packageID: packageID,
                packageDigest: packageDigest, command: command, arguments: arguments)
            let request = ExtensionRelayBootstrap.Request(executable: executable,
                executorDirectory: executorDirectory, executorDigest: executorDigest,
                clientDigest: packageDigest, methods: sessionMethods, hostMethods: hostMethods,
                clientCapabilities: clientServices,
                scopedRoots: scopedRoots, processPrograms: processPrograms,
                processEnvironmentNames: processEnvironmentNames, execution: executorExecution, attachment: executorAttachment,
                command: command, arguments: arguments)
            let argv = try localLaunch.map { _ in try ExtensionRelayBootstrap.buildLocal(request) }
            let bootstrap = try argv.map { String(decoding: try JSONEncoder().encode($0), as: UTF8.self) } ?? ExtensionRelayBootstrap.build(request)
            if let localLaunch, let executorExecution, localLaunch.temporaryDirectory != executorExecution.temporaryDirectory {
                throw GhostermError.failed("本机宿主与 Executor 必须使用同一显式临时目录")
            }
            let peerIdentity = localLaunch.map { "local:" + $0.executableSHA256 } ?? sshAlias
            let additionalCapabilities = ((scopedRoots.isEmpty && clientWorkspace == nil && executorAttachment?.scopedFiles != true) || !selected.requiredCapabilities.contains("resource.receive")
                ? [] : ["file.readScoped"]) +
                (processPrograms.isEmpty && executorAttachment?.remoteProcesses != true ? [] : ["process.spawn", "process.pipes"])
            if let localLaunch, let argv {
                _ = try await extensionController.openLocalChannel(executable: executable, arguments: argv, launch: localLaunch, environmentNames: processEnvironmentNames)
            } else {
                _ = try await authenticatePluginSSHChannel(alias: sshAlias, bootstrap: bootstrap)
            }
            let rpcMethods = sessionMethods.map {
                ExtensionController.RPCMethod(name: $0, capability: "session.rpc",
                    receiver: GhostermCore.ExtensionSession.Role.executor.rawValue)
            } + hostMethods.map {
                ExtensionController.RPCMethod(name: $0, capability: "session.rpc",
                    receiver: GhostermCore.ExtensionSession.Role.host.rawValue)
            }
            let version = Self.extensionHostProtocolVersion(
                Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String)
            let session: ExtensionController.SessionState
            if localLaunch != nil {
                session = try await extensionController.adoptLocalSession(pluginID: packageID, packageDigest: packageDigest, executorDigest: executorDigest, hostVersion: version, peerIdentity: peerIdentity, methods: rpcMethods, additionalRequiredCapabilities: additionalCapabilities)
            } else {
                session = try await extensionController.adoptSSHSession(pluginID: packageID, packageDigest: packageDigest, executorDigest: executorDigest, hostVersion: version, peerIdentity: peerIdentity, methods: rpcMethods, additionalRequiredCapabilities: additionalCapabilities)
            }
            extensionRunInvocation = (packageID, packageDigest, command, arguments, clientWorkspace)
            extensionRegisteredViews?.invalidate()
            extensionTable?.invalidate(); extensionTable = nil
            extensionDiff?.invalidate(); extensionDiff = nil
            extensionTask?.invalidate(); extensionTask = nil
            extensionRegisteredViews = nil
            extensionViews = selected.command?.views ?? []
            claudeRPC?.invalidate(); claudeRPC = nil
            claudeConversation?.invalidate(); claudeConversation = nil
            extensionRunGeneration = UUID()
            codexCallBusy = false
            lastCodexEventSequence = 0
            codexThreadID = nil
            codexMessageDraft = ""
            codexTurnActive = false
            codexPendingApprovals = []
            codexTimeline = []
            cancelCodexChangeReview()
            extensionRun = ExtensionRun(packageID: packageID, packageDigest: packageDigest,
                command: command, arguments: arguments, decodedArguments: decodedArguments,
                executorDigest: executorDigest,
                peerIdentity: peerIdentity, remoteCommand: bootstrap, executorExecution: executorExecution, executorAttachment: executorAttachment, localLaunch: localLaunch, clientWorkspace: clientWorkspace, methods: sessionMethods, hostMethods: hostMethods,
                requiredCapabilities: Array(Set(selected.requiredCapabilities + additionalCapabilities)).sorted(),
                session: session, runtime: nil, receivedResources: [], codexEvents: [])
            let deadline = ProcessInfo.processInfo.systemUptime + 60
            var current = session
            while current.phase != GhostermCore.ExtensionSession.Phase.consent.rawValue &&
                  current.phase != GhostermCore.ExtensionSession.Phase.active.rawValue &&
                  current.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue &&
                  current.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                try Task.checkCancellation()
                guard ProcessInfo.processInfo.systemUptime < deadline else {
                    throw GhostermError.failed("插件 Session 握手超过 60 秒")
                }
                current = try await extensionController.stepSession(milliseconds: 10)
                updateExtensionRun(session: current)
                try await Task.sleep(nanoseconds: 20_000_000)
            }
            guard current.phase == GhostermCore.ExtensionSession.Phase.consent.rawValue else {
                throw GhostermError.failed("插件 Session 未进入待授权状态（phase \(current.phase), end \(current.end), error \(current.error), cleanup \(current.cleanupError)）")
            }
        } catch {
            extensionRunIssue = "插件启动失败：\(error)"
            try? await extensionController.stopSession()
            try? await extensionController.stopSSHChannel()
            try? await extensionController.releaseFinished()
            extensionLastLocalProcess = try? await extensionController.localProcessState()
            extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
            extensionRun = nil; extensionRunInvocation = nil
        }
        extensionRunBusy = false
    }

    public func decideExtensionConsent(allow: Bool) async {
        guard let extensionController, let run = extensionRun,
              let session = run.session,
              session.phase == GhostermCore.ExtensionSession.Phase.consent.rawValue,
              !session.consentToken.isEmpty, !extensionRunBusy else { return }
        extensionRunBusy = true; extensionRunIssue = nil
        do {
            var current = try await extensionController.decideSession(consentToken: session.consentToken, allow: allow)
            updateExtensionRun(session: current)
            let deadline = ProcessInfo.processInfo.systemUptime + 60
            if allow {
                while current.phase != GhostermCore.ExtensionSession.Phase.active.rawValue &&
                      current.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue &&
                      current.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                    try Task.checkCancellation()
                    guard ProcessInfo.processInfo.systemUptime < deadline else {
                        throw GhostermError.failed("插件 Session 授权握手超过 60 秒")
                    }
                    current = try await extensionController.stepSession(milliseconds: 10)
                    updateExtensionRun(session: current)
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
                guard current.phase == GhostermCore.ExtensionSession.Phase.active.rawValue,
                      let invocation = extensionRunInvocation else {
                    throw GhostermError.failed("插件 Session 未进入 ACTIVE")
                }
                let runtime = try await extensionController.activateClient(pluginID: invocation.packageID,
                    packageDigest: invocation.digest, command: invocation.command,
                    arguments: invocation.arguments, clientWorkspace: invocation.clientWorkspace)
                updateExtensionRun(runtime: runtime)
                Task { await self.advanceExtensionRuntime() }
            } else {
                while current.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue &&
                      current.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                    current = try await extensionController.stepSession(milliseconds: 10)
                    updateExtensionRun(session: current)
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
            }
        } catch { extensionRunIssue = "插件授权失败：\(error)" }
        extensionRunBusy = false
    }

    public func stopExtensionRun() async {
        guard let extensionController, extensionRun != nil, !extensionRunBusy else { return }
        extensionRegisteredViews?.invalidate()
        extensionTable?.invalidate()
        extensionDiff?.invalidate()
        extensionTask?.invalidate()
        claudeRPC?.invalidate(); claudeRPC = nil
        claudeConversation?.invalidate(); claudeConversation = nil
        extensionRunGeneration = UUID()
        codexCallBusy = false
        cancelCodexChangeReview()
        extensionRunBusy = true; extensionRunIssue = nil
        let runtimeOwnsSession = extensionRun?.runtime != nil
        do {
            var temporaryOnly = false
            if extensionRun?.localLaunch != nil, let temporary = try await extensionController.localTemporaryState() {
                temporaryOnly = temporary.phase != .active
            }
            if !temporaryOnly, extensionRun?.runtime != nil {
                try await extensionController.stopRuntime()
                var stopped = false
                for _ in 0..<200 {
                    let state = try await extensionController.stepRuntime()
                    updateExtensionRun(runtime: state)
                    if !Self.runtimeIsRunning(state.phase) { stopped = true; break }
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
                guard stopped else { throw GhostermError.failed("插件 Runtime 未在停止期限内结束，保留 owner 供重试") }
            }
            if !temporaryOnly, !runtimeOwnsSession, let session = extensionRun?.session,
               session.phase != GhostermCore.ExtensionSession.Phase.ended.rawValue,
               session.phase != GhostermCore.ExtensionSession.Phase.closed.rawValue {
                try await extensionController.stopSession()
                var ended = false
                for _ in 0..<200 {
                    let state = try await extensionController.stepSession(milliseconds: 10)
                    updateExtensionRun(session: state)
                    if state.phase == GhostermCore.ExtensionSession.Phase.ended.rawValue ||
                       state.phase == GhostermCore.ExtensionSession.Phase.closed.rawValue { ended = true; break }
                    try await Task.sleep(nanoseconds: 20_000_000)
                }
                guard ended else { throw GhostermError.failed("插件 Session 未在停止期限内结束，保留 owner 供重试") }
            }
            try await extensionController.releaseFinished()
            extensionLastLocalProcess = try? await extensionController.localProcessState()
            extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
            extensionRun = nil; extensionRunInvocation = nil
            extensionTable = nil; extensionDiff = nil; extensionTask = nil; extensionViews = []; extensionRegisteredViews = nil
            codexThreadID = nil; codexMessageDraft = ""
            codexTurnActive = false
            codexPendingApprovals = []
            codexTimeline = []
            pendingExtensionRPCHandles.removeAll()
        } catch {
            extensionLastLocalProcess = try? await extensionController.localProcessState()
            extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
            extensionRunIssue = "释放插件资源失败：\(error)"
        }
        extensionRunBusy = false
    }

    public func retryLocalExtensionCleanup() async {
        guard let extensionController, extensionRun == nil, !extensionRunBusy,
              extensionLastLocalProcess != nil || extensionLastLocalTemporary != nil else { return }
        extensionRunBusy = true
        try? await extensionController.stopRuntime()
        try? await extensionController.stopSession()
        try? await extensionController.stopSSHChannel()
        do { try await extensionController.releaseFinished(); extensionRunIssue = nil }
        catch { extensionRunIssue = "释放本机插件资源失败，仍保留 owner：\(error)" }
        extensionLastLocalProcess = try? await extensionController.localProcessState()
        extensionLastLocalTemporary = try? await extensionController.localTemporaryState()
        extensionRunBusy = false
    }

    public func inspectLocalReservations() async {
        guard let extensionController, !localReservationBusy else { return }
        localReservationBusy = true; defer { localReservationBusy = false }
        do { localReservationEntries = try await extensionController.inspectLocalReservations(); localReservationIssue = nil }
        catch { localReservationIssue = "本机暂存记录查不出来：\(error)" }
    }

    public func releaseMissingLocalReservation(_ entry: ExtensionLocalJournalEntry, confirmed: Bool) async {
        guard let extensionController, !localReservationBusy else { return }
        localReservationBusy = true; defer { localReservationBusy = false }
        do {
            try await extensionController.releaseMissingLocalReservation(entry, confirmed: confirmed)
            localReservationEntries = try await extensionController.inspectLocalReservations(); localReservationIssue = nil
        } catch { localReservationIssue = "未能释放预约，请重新检查：\(error)" }
    }

    public func startCodexThread() async {
        guard let run = extensionRun, run.command == "ai codex", !extensionRunBusy, !codexCallBusy,
              run.methods.contains("codex.thread.start") else { return }
        let generation = extensionRunGeneration
        codexCallBusy = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Started: Decodable { let threadID: String }
            let value: Started = try await callCodexExecutor("codex.thread.start", input: [:])
            codexThreadID = value.threadID
        } catch { if generation == extensionRunGeneration { extensionRunIssue = "无法启动 Codex 对话：\(error)" } }
    }

    public func sendCodexMessage() async {
        guard let run = extensionRun, run.command == "ai codex", !extensionRunBusy, codexThreadID != nil,
              !codexTurnActive,
              !codexCallBusy, run.methods.contains("codex.turn.send") else { return }
        let text = codexMessageDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        let generation = extensionRunGeneration
        codexCallBusy = true
        codexTurnActive = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Sent: Decodable { let turnID: String }
            let _: Sent = try await callCodexExecutor("codex.turn.send", input: ["text": text])
            codexMessageDraft = ""
        } catch { if generation == extensionRunGeneration { extensionRunIssue = "Codex 消息发送失败：\(error)" } }
    }

    public func interruptCodexTurn() async {
        guard let run = extensionRun, run.command == "ai codex", !extensionRunBusy, codexTurnActive,
              !codexCallBusy, run.methods.contains("codex.turn.interrupt") else { return }
        let generation = extensionRunGeneration
        codexCallBusy = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Interrupted: Decodable { let accepted: Bool }
            let result: Interrupted = try await callCodexExecutor("codex.turn.interrupt", input: [:])
            guard result.accepted else { throw GhostermError.failed("Executor 未接受中断请求") }
        } catch { if generation == extensionRunGeneration { extensionRunIssue = "Codex 中断失败：\(error)" } }
    }

    public func codexApprovalSnapshot(_ approval: CodexPendingApproval) -> String? {
        if approval.method == "item/permissions/requestApproval" { return approval.permissionReviewID }
        return approval.identity?.reviewedSnapshot(codexChangeReview, isCurrent: currentCodexChange)
    }

    public func respondCodexApproval(id: String, allow: Bool, snapshotID: String? = nil) async {
        guard let run = extensionRun, run.command == "ai codex", !extensionRunBusy, !codexCallBusy,
              run.runtime?.phase == ExtensionRuntime.Phase.active.rawValue,
              let approval = codexPendingApprovals.first(where: { $0.id.utf8.elementsEqual(id.utf8) }),
              run.methods.contains("codex.approval.respond") else { return }
        let fileApproval = approval.method == "item/fileChange/requestApproval"
        let permissionApproval = approval.method == "item/permissions/requestApproval"
        if permissionApproval && (snapshotID == nil || snapshotID != approval.permissionReviewID) {
            extensionRunIssue = "权限请求已更新，请重新核对当前完整权限范围。"
            return
        }
        if allow && fileApproval && (snapshotID == nil || snapshotID != codexApprovalSnapshot(approval)) {
            extensionRunIssue = "请先读取并核对当前完整补丁，再批准此版本。"
            return
        }
        let generation = extensionRunGeneration
        codexCallBusy = true
        defer { if generation == extensionRunGeneration { codexCallBusy = false } }
        do {
            struct Responded: Decodable { let accepted: Bool }
            let decisionJSON = String(data: try JSONEncoder().encode(allow ? "accept" : "decline"), encoding: .utf8)!
            let result: Responded = try await callCodexExecutor("codex.approval.respond",
                input: ["requestID": id, "decisionJSON": decisionJSON,
                        "snapshotID": permissionApproval || (allow && fileApproval) ? snapshotID! : ""])
            guard result.accepted else { throw GhostermError.failed("Executor 未接受审批决定") }
            codexPendingApprovals.removeAll { $0.id.utf8.elementsEqual(id.utf8) }
        } catch {
            if generation == extensionRunGeneration {
                if allow && fileApproval { cancelCodexChangeReview() }
                extensionRunIssue = "Codex 审批响应失败：\(error)" +
                    (allow && fileApproval ? "；请重新读取完整补丁后核对。" : "")
            }
        }
    }

    private struct CodexRPCEnvelope<Value: Decodable>: Decodable {
        let kind: String
        let value: Value?
    }

    private func callCodexExecutor<Value: Decodable>(_ method: String, input: [String: Any]) async throws -> Value {
        let bytes = try JSONSerialization.data(withJSONObject: input, options: [.sortedKeys])
        let outcome = try await callExtensionExecutor(method, inputBytes: bytes, command: "ai codex")
        let envelope = try JSONDecoder().decode(CodexRPCEnvelope<Value>.self, from: outcome)
        guard envelope.kind == "success", let value = envelope.value else {
            throw GhostermError.failed("Codex RPC 返回了无效结果 envelope")
        }
        return value
    }

    /// Complete vendor records for the Native Claude conversation adapter.
    /// A replacement run never reuses the previous adapter or child sequence.
    public func requestClaudeAction(method: String, parameters: Data) async throws -> Data {
        let generation = extensionRunGeneration
        try checkExtensionRunGeneration(generation)
        guard !extensionRunBusy, extensionRun?.command == "ai claude",
              extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue,
              ClaudeRPCClient.methods.allSatisfy({ extensionRun?.methods.contains($0) == true }) else {
            throw GhostermError.failed("Claude 需要本次已授权的 ACTIVE Runtime 和完整 RPC allowlist")
        }
        if claudeRPC == nil {
            claudeRPC = ClaudeRPCClient { [weak self] name, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callExtensionExecutor(name, inputBytes: input, command: "ai claude")
            }
        }
        return try await claudeRPC!.request(method: method, parameters: parameters)
    }

    /// Current-Runtime observation only; never creates a replacement adapter.
    public func observeClaudeOutcome() async throws -> ClaudeRPCClient.Observation {
        let generation = extensionRunGeneration
        try checkExtensionRunGeneration(generation)
        guard !extensionRunBusy, extensionRun?.command == "ai claude",
              extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue,
              ClaudeRPCClient.methods.allSatisfy({ extensionRun?.methods.contains($0) == true }),
              let claudeRPC else { throw GhostermError.failed("原 Claude Runtime 已不可观察") }
        let result = try await claudeRPC.observe()
        try checkExtensionRunGeneration(generation)
        return result
    }

    /// Standard Native views retain declared failures and uncertainty as facts.
    func callPluginViewOutcome(_ method: String, inputBytes: Data, command: String) async throws -> Data {
        try await callExtensionExecutor(method, inputBytes: inputBytes, command: command, returnTerminalOutcome: true)
    }

    private func callExtensionExecutor(_ method: String, inputBytes: Data, command: String,
                                       returnTerminalOutcome: Bool = false) async throws -> Data {
        let generation = extensionRunGeneration
        try checkExtensionRunGeneration(generation)
        guard let extensionController, let run = extensionRun,
              !extensionRunBusy, run.command == command, run.methods.contains(method) else {
            throw GhostermError.failed("插件 RPC 未在本次 Session allowlist 中")
        }
        let admission = try await extensionController.requestExecutorRPC(method: method, input: inputBytes)
        guard admission.status == .ok, let handle = admission.handle else {
            if returnTerminalOutcome {
                let codes: [ExtensionSession.RPCStatus: String] = [.wait: "BUSY", .invalid: "INVALID_INPUT", .denied: "DENIED", .unsupported: "UNSUPPORTED", .closed: "CANCELLED"]
                if let code = codes[admission.status] {
                    return try JSONSerialization.data(withJSONObject: ["kind": "failed", "error": ["code": code, "message": code, "effect": "none"]])
                }
            }
            throw GhostermError.failed("插件 RPC 未能进入发送队列（status \(admission.status.rawValue)）")
        }
        var completed = false
        defer { if !completed && generation == extensionRunGeneration { pendingExtensionRPCHandles.insert(handle) } }
        try checkExtensionRunGeneration(generation)
        let deadline = ProcessInfo.processInfo.systemUptime + 30
        do {
            while ProcessInfo.processInfo.systemUptime < deadline {
                try checkExtensionRunGeneration(generation)
                let (status, result) = try await extensionController.executorRPCResult(handle: handle)
                try checkExtensionRunGeneration(generation)
                if status == .ok, let result,
                   result.state != .queued, result.state != .pending {
                    let forget = try await extensionController.forgetExecutorRPC(handle: handle)
                    try checkExtensionRunGeneration(generation)
                    guard forget == .ok else { throw GhostermError.failed("插件 RPC 结果未能释放") }
                    completed = true
                    if returnTerminalOutcome {
                        guard result.state != .succeeded || result.confirmed else {
                            return Data(#"{"kind":"unknown","reason":"UNCONFIRMED_SUCCESS"}"#.utf8)
                        }
                        return result.outcome
                    }
                    guard result.state == .succeeded, result.confirmed else {
                        throw GhostermError.failed("插件 RPC 结果为 \(result.state)，执行效果未知或失败")
                    }
                    return result.outcome
                }
                try Task.checkCancellation()
                try await Task.sleep(nanoseconds: 20_000_000)
            }
            try checkExtensionRunGeneration(generation)
            _ = try await extensionController.cancelExecutorRPC(handle: handle)
            throw GhostermError.failed("插件 RPC 等待超过 30 秒，结果可能未知")
        } catch {
            // Do not retry or discard an unresolved handle: Core retains its
            // unknown outcome until the caller explicitly observes it.
            throw error
        }
    }

    private func checkExtensionRunGeneration(_ generation: UUID) throws {
        try Task.checkCancellation()
        guard generation == extensionRunGeneration else { throw CancellationError() }
    }

    public var canReadCodexChanges: Bool {
        !extensionRunBusy && extensionRun?.command == "ai codex" &&
        extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue &&
        extensionRun?.methods.contains("codex.change.read") == true
    }

    public func loadCodexChanges(_ reference: CodexChangeReference) {
        guard canReadCodexChanges, currentCodexChange(reference) else { return }
        beginCodexChangeRead(reference: reference, read: { [weak self] offset in
            guard let self else { throw CancellationError() }
            return try await self.callCodexExecutor("codex.change.read",
                input: ["snapshotID": reference.snapshotID, "offset": offset])
        }, isCurrent: { [weak self] in self?.currentCodexChange(reference) == true && self?.canReadCodexChanges == true })
    }

    private func currentCodexChange(_ reference: CodexChangeReference) -> Bool {
        codexThreadID?.utf8.elementsEqual(reference.threadID.utf8) == true &&
        codexTimeline.contains { $0.changeReference == reference }
    }

    public func cancelCodexChangeReview() {
        codexChangeReadGeneration = UUID()
        codexChangeReadTask?.cancel()
        codexChangeReadTask = nil
        codexChangeReview = nil
    }

    /// The injected read boundary also exercises asynchronous invalidation in tests.
    @discardableResult
    func beginCodexChangeRead(reference: CodexChangeReference,
                              read: @escaping @MainActor (Int) async throws -> CodexChangeChunk,
                              isCurrent: @escaping @MainActor () -> Bool) -> Task<Void, Never> {
        cancelCodexChangeReview()
        let token = codexChangeReadGeneration
        codexChangeReview = CodexChangeReviewState(reference: reference, receivedBytes: 0, document: nil, issue: nil)
        let task = Task { [weak self] in
            guard let self else { return }
            defer { if self.codexChangeReadGeneration == token { self.codexChangeReadTask = nil } }
            var assembler = CodexChangeAssembler(reference: reference)
            let deadline = ProcessInfo.processInfo.systemUptime + 120
            do {
                for _ in 0..<2048 {
                    try Task.checkCancellation()
                    guard self.codexChangeReadGeneration == token else { return }
                    guard isCurrent() else { self.cancelCodexChangeReview(); return }
                    guard ProcessInfo.processInfo.systemUptime < deadline else { throw CodexChangeReadError.incomplete }
                    let chunk = try await read(assembler.nextOffset)
                    try Task.checkCancellation()
                    guard ProcessInfo.processInfo.systemUptime < deadline else { throw CodexChangeReadError.incomplete }
                    guard self.codexChangeReadGeneration == token else { return }
                    guard isCurrent() else { self.cancelCodexChangeReview(); return }
                    try assembler.append(chunk)
                    self.codexChangeReview = CodexChangeReviewState(reference: reference,
                        receivedBytes: assembler.receivedBytes, document: nil, issue: nil)
                    if assembler.done {
                        let complete = assembler
                        let document = try await Task.detached(priority: .userInitiated) { try complete.document() }.value
                        try Task.checkCancellation()
                        guard ProcessInfo.processInfo.systemUptime < deadline else { throw CodexChangeReadError.incomplete }
                        guard self.codexChangeReadGeneration == token else { return }
                        guard isCurrent() else { self.cancelCodexChangeReview(); return }
                        self.codexChangeReview = CodexChangeReviewState(reference: reference,
                            receivedBytes: assembler.receivedBytes, document: document, issue: nil)
                        return
                    }
                }
                throw CodexChangeReadError.incomplete
            } catch {
                guard self.codexChangeReadGeneration == token else { return }
                guard !Task.isCancelled, isCurrent() else { self.cancelCodexChangeReview(); return }
                self.codexChangeReview = CodexChangeReviewState(reference: reference,
                    receivedBytes: assembler.receivedBytes, document: nil, issue: error.localizedDescription)
            }
        }
        codexChangeReadTask = task
        return task
    }

    private func advanceExtensionRuntime() async {
        guard let extensionController else { return }
        let generation = extensionRunGeneration
        do {
            while true {
                try Task.checkCancellation()
                let state = try await extensionController.stepRuntime()
                try checkExtensionRunGeneration(generation)
                updateExtensionRun(runtime: state)
                if !Self.runtimeIsRunning(state.phase) { return }
                try await reapPendingExtensionRPCs()
                try checkExtensionRunGeneration(generation)
                if state.phase == GhostermCore.ExtensionRuntime.Phase.active.rawValue {
                    try await pumpCodexHostEvent()
                }
                if extensionRun?.requiredCapabilities.contains("resource.receive") == true,
                   let resource = try await extensionController.takeSessionResource(maxBytes: 1048576) {
                    try checkExtensionRunGeneration(generation)
                    appendExtensionResource(resource)
                }
                try await Task.sleep(nanoseconds: 50_000_000)
            }
        } catch {
            if Task.isCancelled || generation != extensionRunGeneration { return }
            extensionRegisteredViews?.invalidate()
            extensionTable?.invalidate()
            extensionDiff?.invalidate()
            extensionTask?.invalidate()
            claudeRPC?.invalidate()
            claudeConversation?.invalidate()
            cancelCodexChangeReview()
            codexPendingApprovals = []
            extensionRunIssue = "插件 Runtime 运行失败：\(error)"
        }
    }

    public func publishClientResource(relativePath: String, root: String = "client", instanceID: String? = nil) async {
        guard !extensionRunBusy, let controller = extensionController, extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue else { return }
        let generation = extensionRunGeneration
        do {
            let state = try await controller.publishClientResource(instanceID: instanceID ?? extensionRun?.runtime?.instanceID ?? "", root: root, relativePath: relativePath)
            guard generation == extensionRunGeneration else { return }
            updateExtensionRun(runtime: state)
        } catch {
            if generation == extensionRunGeneration { extensionRunIssue = "资源发布失败：\(error)" }
        }
    }
    public func cancelClientPublication(id: String) async {
        guard let controller = extensionController, extensionRun?.runtime?.publication?.id == id else { return }
        let generation = extensionRunGeneration
        do { try await controller.cancelClientPublication(id: id) }
        catch { if generation == extensionRunGeneration { extensionRunIssue = "无法取消资源发布：\(error)" } }
    }

    private func updateExtensionRun(session: ExtensionController.SessionState) {
        guard let run = extensionRun else { return }
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest,
            peerIdentity: run.peerIdentity, remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace,
            methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities,
            session: session, runtime: run.runtime, receivedResources: run.receivedResources,
            codexEvents: run.codexEvents)
    }

    private func updateExtensionRun(runtime: ExtensionController.RuntimeState) {
        guard let run = extensionRun else { return }
        if !Self.runtimeIsRunning(runtime.phase) {
            extensionRegisteredViews?.invalidate()
            extensionTable?.invalidate()
            extensionDiff?.invalidate()
            extensionTask?.invalidate()
            claudeRPC?.invalidate()
            claudeConversation?.invalidate()
            codexPendingApprovals = []
            cancelCodexChangeReview()
        }
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest,
            peerIdentity: run.peerIdentity, remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace,
            methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities,
            session: run.session, runtime: runtime, receivedResources: run.receivedResources,
            codexEvents: run.codexEvents)
        if let issue = runtime.viewIssue { extensionRunIssue = issue }
        if run.requiredCapabilities.contains("ui.views"), runtime.phase == ExtensionRuntime.Phase.active.rawValue {
            do {
                guard let data = runtime.views, let instance = runtime.viewSessionInstance, let epoch = runtime.viewSessionEpoch else {
                    throw PluginViewValue.Failure.invalidResponse
                }
                if extensionRegisteredViews == nil {
                    let generation = extensionRunGeneration, runtimeID = runtime.instanceID
                    extensionRegisteredViews = try PluginViewsModel(instance: instance, epoch: epoch,
                        kinds: Set(extensionViews.compactMap(PluginViewRegistration.Kind.init(rawValue:))),
                        capabilities: Set(run.requiredCapabilities), methods: Set(run.methods),
                        rpc: { [weak self] method, input in
                            guard let self else { throw CancellationError() }
                            try self.checkExtensionRunGeneration(generation)
                            return try await self.callPluginViewOutcome(method, inputBytes: input, command: run.command)
                        }, isCurrent: { [weak self] in
                            guard let self else { return false }
                            return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                                self.extensionRun?.runtime?.instanceID == runtimeID &&
                                self.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
                        }, close: { [weak self] handle in
                            guard let self, let controller = self.extensionController else { throw CancellationError() }
                            try self.checkExtensionRunGeneration(generation)
                            let state = try await controller.closeRuntimeView(instanceID: runtimeID, handle: handle)
                            try self.checkExtensionRunGeneration(generation)
                            guard let data = state.views else { throw PluginViewValue.Failure.invalidResponse }
                            return data
                        })
                }
                try extensionRegisteredViews?.apply(data)
            } catch {
                extensionRegisteredViews?.invalidate()
                extensionRunIssue = "插件视图注册不可用，可停止并释放：\(error)"
            }
        }
        if !run.requiredCapabilities.contains("ui.views"), extensionViews.contains("table"), run.requiredCapabilities.contains("ui.table"),
           runtime.phase == ExtensionRuntime.Phase.active.rawValue, extensionTable == nil,
           PluginTableModel.methods.allSatisfy(run.methods.contains) {
            let generation = extensionRunGeneration
            extensionTable = PluginTableModel(rpc: { [weak self] method, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callExtensionExecutor(method, inputBytes: input, command: run.command)
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
            })
        }
        if !run.requiredCapabilities.contains("ui.views"), extensionViews.contains("diff"), run.requiredCapabilities.contains("ui.diff"),
           runtime.phase == ExtensionRuntime.Phase.active.rawValue, extensionDiff == nil,
           PluginDiffModel.methods.allSatisfy(run.methods.contains) {
            let generation = extensionRunGeneration
            extensionDiff = PluginDiffModel(rpc: { [weak self] method, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callExtensionExecutor(method, inputBytes: input, command: run.command)
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
            })
        }
        if !run.requiredCapabilities.contains("ui.views"), extensionViews.contains("task"), run.requiredCapabilities.contains("ui.task"),
           runtime.phase == ExtensionRuntime.Phase.active.rawValue, extensionTask == nil,
           PluginTaskModel.methods.allSatisfy(run.methods.contains) {
            let generation = extensionRunGeneration
            extensionTask = PluginTaskModel(rpc: { [weak self] method, input in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.callPluginViewOutcome(method, inputBytes: input, command: run.command)
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
            })
        }
        if run.command == "ai claude", runtime.phase == ExtensionRuntime.Phase.active.rawValue,
           ClaudeRPCClient.methods.allSatisfy(run.methods.contains),
           claudeConversation == nil {
            let generation = extensionRunGeneration
            claudeConversation = ClaudeConversation(request: { [weak self] method, parameters in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.requestClaudeAction(method: method, parameters: parameters)
            }, observe: { [weak self] in
                guard let self else { throw CancellationError() }
                try self.checkExtensionRunGeneration(generation)
                return try await self.observeClaudeOutcome()
            }, isCurrent: { [weak self] in
                guard let self else { return false }
                return generation == self.extensionRunGeneration && !self.extensionRunBusy &&
                    self.extensionRun?.runtime?.phase == ExtensionRuntime.Phase.active.rawValue
            })
        }
    }

    private func appendExtensionResource(_ resource: ExtensionController.ReceivedResource) {
        guard let run = extensionRun else { return }
        let total = run.receivedResources.reduce(UInt64(0)) { $0 + $1.size }
        guard run.receivedResources.count < 16, resource.size <= 1048576,
              total <= 8388608, resource.size <= 8388608 - total else {
            extensionRunIssue = "插件资源超过本次界面的数量或总大小上限"
            return
        }
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest, peerIdentity: run.peerIdentity,
            remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace, methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities,
            session: run.session, runtime: run.runtime,
            receivedResources: run.receivedResources + [resource], codexEvents: run.codexEvents)
    }

    private struct CodexHostEvent: Decodable {
        let sequence: UInt64
        let kind: String
        let notification: Notification?
        struct Notification: Decodable { let method: String }
    }

    private func pumpCodexHostEvent() async throws {
        guard let extensionController, let run = extensionRun, run.command == "ai codex" else { return }
        let generation = extensionRunGeneration
        let poll = try await extensionController.pollHostRPCRequest()
        try checkExtensionRunGeneration(generation)
        guard let request = poll.request else {
            guard poll.status == UInt32(GhostermCore.ExtensionSession.RPCStatus.wait.rawValue) else {
                throw GhostermError.failed("读取 Codex Executor Host RPC 失败（status \(poll.status)）")
            }
            return
        }
        guard request.method == "codex.event", run.hostMethods.contains("codex.event") else {
            let failure = Data(#"{"kind":"failed","error":{"code":"DENIED","message":"Host method is not registered","effect":"none"}}"#.utf8)
            guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: failure) == 0 else {
                throw GhostermError.failed("无法拒绝未注册的插件 Host RPC")
            }
            return
        }
        let encoded: String
        let event: CodexHostEvent
        do {
            struct EventInput: Decodable { let event: String }
            encoded = try JSONDecoder().decode(EventInput.self, from: request.input).event
            event = try JSONDecoder().decode(CodexHostEvent.self, from: Data(encoded.utf8))
        } catch {
            let failure = Data(#"{"kind":"failed","error":{"code":"DENIED","message":"Malformed Codex event","effect":"none"}}"#.utf8)
            guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: failure) == 0 else {
                throw GhostermError.failed("无法拒绝格式错误的 Codex 事件")
            }
            throw GhostermError.failed("Codex Executor 发送了格式错误的事件")
        }
        let rawEvent = (try? JSONSerialization.jsonObject(with: Data(encoded.utf8))) as? [String: Any]
        var approvalToAdd: CodexPendingApproval?
        var approvalToRemove: String?
        if event.kind == "approval-request" {
            guard let approval = Self.decodeCodexApproval(rawEvent?["request"]),
                  !codexPendingApprovals.contains(where: { $0.id.utf8.elementsEqual(approval.id.utf8) }),
                  codexPendingApprovals.count < 32 else {
                try await rejectCodexEvent(extensionController, ticket: request.ticket,
                                           message: "Invalid or excessive Codex approval request")
                throw GhostermError.failed("Codex Executor approval 请求无效")
            }
            approvalToAdd = approval
        } else if event.kind == "approval-resolved" {
            approvalToRemove = Self.codexRequestKey(rawEvent?["requestID"])
        }
        guard let history = CodexEventHistory.appending(encoded, kind: event.kind, sequence: event.sequence,
                                                       after: lastCodexEventSequence, to: run.codexEvents) else {
            let failure = Data(#"{"kind":"failed","error":{"code":"DENIED","message":"Invalid or excessive Codex event","effect":"none"}}"#.utf8)
            guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: failure) == 0 else {
                throw GhostermError.failed("无法拒绝无效的 Codex 事件")
            }
            throw GhostermError.failed("Codex Executor 事件序列无效或超过 Host 限额")
        }
        let success = Data(#"{"kind":"success","value":{"accepted":true}}"#.utf8)
        guard try await extensionController.respondHostRPC(ticket: request.ticket, outcome: success) == 0 else {
            throw GhostermError.failed("无法确认 Codex Executor 事件")
        }
        try checkExtensionRunGeneration(generation)
        lastCodexEventSequence = event.sequence
        applyCodexApprovalEvent(kind: event.kind, notificationMethod: event.notification?.method,
            added: approvalToAdd, removedID: approvalToRemove)
        extensionRun = ExtensionRun(packageID: run.packageID, packageDigest: run.packageDigest,
            command: run.command, arguments: run.arguments, decodedArguments: run.decodedArguments,
            executorDigest: run.executorDigest, peerIdentity: run.peerIdentity,
            remoteCommand: run.remoteCommand, executorExecution: run.executorExecution, executorAttachment: run.executorAttachment, localLaunch: run.localLaunch, clientWorkspace: run.clientWorkspace, methods: run.methods, hostMethods: run.hostMethods,
            requiredCapabilities: run.requiredCapabilities, session: run.session, runtime: run.runtime,
            receivedResources: run.receivedResources, codexEvents: history)
        if event.kind == "notification" {
            if event.notification?.method == "turn/started" { codexTurnActive = true }
            if event.notification?.method == "turn/completed" { codexTurnActive = false }
            if let notification = rawEvent?["notification"] as? [String: Any],
               let method = notification["method"] as? String,
               let params = notification["params"], JSONSerialization.isValidJSONObject(params),
               let data = try? JSONSerialization.data(withJSONObject: params, options: [.sortedKeys]) {
                codexTimeline = CodexTimelineReducer.apply(codexTimeline, sequence: event.sequence,
                    method: method, params: data)
            }
        } else if event.kind != "approval-request" && event.kind != "approval-resolved" {
            codexTimeline = CodexTimelineReducer.apply(codexTimeline, sequence: event.sequence,
                method: event.kind, params: Data(encoded.utf8))
        }
        if let review = codexChangeReview,
           !codexTimeline.contains(where: { $0.changeReference == review.reference }) {
            cancelCodexChangeReview()
        }
    }

    /// Applies only after the incoming Host RPC has been validated and acknowledged.
    func applyCodexApprovalEvent(kind: String, notificationMethod: String? = nil,
                                added: CodexPendingApproval? = nil, removedID: String? = nil) {
        if let added { codexPendingApprovals.append(added) }
        if let removedID { codexPendingApprovals.removeAll { $0.id.utf8.elementsEqual(removedID.utf8) } }
        if (kind == "notification" && notificationMethod == "turn/completed") ||
           ["protocol-error", "closed", "outcome-unknown"].contains(kind) {
            codexPendingApprovals = []
        }
        if ["protocol-error", "closed", "outcome-unknown"].contains(kind) { cancelCodexChangeReview() }
    }

    private func rejectCodexEvent(_ controller: ExtensionController, ticket: UInt64,
                                  message: String) async throws {
        let encodedMessage = String(data: try JSONEncoder().encode(message), encoding: .utf8)!
        let outcome = Data("{\"kind\":\"failed\",\"error\":{\"code\":\"DENIED\",\"message\":\(encodedMessage),\"effect\":\"none\"}}".utf8)
        guard try await controller.respondHostRPC(ticket: ticket, outcome: outcome) == 0 else {
            throw GhostermError.failed("无法拒绝无效的 Codex 事件")
        }
    }

    static func decodeCodexApproval(_ value: Any?) -> CodexPendingApproval? {
        guard let object = value as? [String: Any],
              let id = codexRequestKey(object["id"]),
              let method = object["method"] as? String,
              ["item/commandExecution/requestApproval", "item/fileChange/requestApproval", "item/permissions/requestApproval"].contains(method),
              let params = object["params"], JSONSerialization.isValidJSONObject(params),
              let data = try? JSONSerialization.data(withJSONObject: params, options: [.sortedKeys]),
              let paramsJSON = String(data: data, encoding: .utf8) else { return nil }
        let itemID = (object["params"] as? [String: Any])?["itemId"] as? String
        var identity: CodexApprovalIdentity?
        var permissionReviewID: String?
        if method == "item/fileChange/requestApproval" || method == "item/permissions/requestApproval" {
            let fields = object["params"] as? [String: Any]
            guard let thread = fields?["threadId"] as? String, !thread.isEmpty, thread.utf8.count <= 128,
                  let turn = fields?["turnId"] as? String, !turn.isEmpty, turn.utf8.count <= 128,
                  let itemID, !itemID.isEmpty, itemID.utf8.count <= 128 else { return nil }
            if method == "item/fileChange/requestApproval" {
                identity = CodexApprovalIdentity(threadID: thread, turnID: turn, itemID: itemID)
            } else {
                guard let fields, let profile = fields["permissions"] as? [String: Any],
                      Set(profile.keys) == ["network", "fileSystem"],
                      ["network", "fileSystem"].allSatisfy({ profile[$0] is NSNull || profile[$0] is [String: Any] }),
                      fields["cwd"] is String,
                      let token = object["permissionReviewID"] as? String,
                      token.hasPrefix("permissions."), (1...20).contains(token.dropFirst(12).count),
                      token.dropFirst(12).first != "0",
                      token.dropFirst(12).allSatisfy({ $0 >= "0" && $0 <= "9" }) else { return nil }
                permissionReviewID = token
            }
        }
        return CodexPendingApproval(id: id, method: method, paramsJSON: paramsJSON, itemID: itemID,
                                    identity: identity, permissionReviewID: permissionReviewID)
    }

    static func codexRequestKey(_ value: Any?) -> String? {
        if let value = value as? String, !value.isEmpty, value.utf8.count <= 128 { return "s:\(value)" }
        if let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
           number.doubleValue.isFinite, number.doubleValue.rounded(.towardZero) == number.doubleValue,
           abs(number.doubleValue) <= 9_007_199_254_740_991 {
            return "n:\(number.stringValue)"
        }
        return nil
    }

    private func reapPendingExtensionRPCs() async throws {
        guard let extensionController else { return }
        let generation = extensionRunGeneration
        for handle in pendingExtensionRPCHandles.sorted() {
            try checkExtensionRunGeneration(generation)
            let (status, result) = try await extensionController.executorRPCResult(handle: handle)
            try checkExtensionRunGeneration(generation)
            guard status == .ok, let result,
                  result.state != .queued, result.state != .pending else { continue }
            let forgotten = try await extensionController.forgetExecutorRPC(handle: handle)
            try checkExtensionRunGeneration(generation)
            guard forgotten == .ok else { throw GhostermError.failed("无法释放已结束的 Codex RPC 结果") }
            pendingExtensionRPCHandles.remove(handle)
        }
    }

    private static func pluginResourceDirectory() -> URL? {
        guard let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
            return nil
        }
        let directory = base.appendingPathComponent("Ghosterm/PluginResources", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                    attributes: [.posixPermissions: 0o700])
            return directory
        } catch { return nil }
    }

    private static func runtimeIsRunning(_ phase: UInt32) -> Bool {
        phase == GhostermCore.ExtensionRuntime.Phase.created.rawValue ||
        phase == GhostermCore.ExtensionRuntime.Phase.starting.rawValue ||
        phase == GhostermCore.ExtensionRuntime.Phase.active.rawValue
    }

    private func resolvePluginAddress(_ host: String) async throws -> SSHAddress {
        let owner = DispatchQueue(label: "org.ghosterm.plugin-dns")
        return try await withCheckedThrowingContinuation { continuation in
            owner.async {
                _ = SSHAddressResolver.resolve(host: host, owner: owner) { result in
                    switch result {
                    case .success(let address): continuation.resume(returning: address)
                    case .failure(let error):
                        continuation.resume(throwing: GhostermError.failed("无法解析插件 SSH 首跳地址：\(error)"))
                    }
                }
            }
        }
    }

}

// MARK: - UI 交互协议与请求模型

/// 一次主机密钥确认请求（三态 UI 的数据面）。
public struct HostKeyPrompt: Identifiable, Equatable {
    public let id = UUID()
    public let host: String
    public let port: UInt16
    public let keyType: String
    public let fingerprint: String
}

/// 一次凭据请求。
///
/// `kind` 说清**为什么问**——用户最容易被这一步激怒的就是「我明明输了登录
/// 密码，为什么还问私钥口令」：
///   - `.passphrase`：私钥带口令，解锁用（不是登录密码）；
///   - `.password`：服务器只收密码（或私钥没被接受），要的是**登录密码**。
public struct CredentialPrompt: Identifiable, Equatable {
    public enum Kind: Equatable {
        case passphrase
        case password(reason: String)
    }
    public let id = UUID()
    public let host: String
    public let port: UInt16
    public let user: String
    public let kind: Kind

    /// 对话框标题与说明（UI 直接用，避免每处各写一份文案）。
    public var title: String {
        switch kind {
        case .passphrase: return "需要私钥口令"
        case .password: return "需要登录密码"
        }
    }

    public var detail: String {
        switch kind {
        case .passphrase:
            // 注意：NSAlert 不解析 markdown——星号会原样显示在用户面前。
            return "连接 \(user)@\(host):\(port) 需要解锁私钥。"
                + "这是私钥的口令，不是登录密码。"
        case .password(let reason):
            return "连接 \(user)@\(host):\(port)：\(reason)"
                + "请输入登录密码（会存在 Keychain，下次不用再输）。"
        }
    }
}

/// 凭据请求的用户答复。
///
/// `pem` 非空 = 用户选了某把私钥文件；空 = 用户给的是口令/密码（哪种由
/// 请求的 `CredentialPrompt.Kind` 决定）。
public struct CredentialAnswer {
    public let pem: String
    public let passphrase: String?
    public let password: String?
    public init(pem: String, passphrase: String? = nil, password: String? = nil) {
        self.pem = pem
        self.passphrase = passphrase
        self.password = password
    }
}

/// App 层注入的交互源：宿主 UI（AppKit alert）实现并给出模态结果。
/// 无 UI 环境（闸门冒烟）不注入——连接会显式失败，不静默装成功。
public protocol AppInteraction: AnyObject {
    /// 主机密钥确认：未知 ≠ 拒绝；返回用户裁决。
    func confirmHostKey(_ prompt: HostKeyPrompt) -> HostKeyDecision
    /// 凭据询问：返回 nil = 用户取消。
    func askCredential(_ prompt: CredentialPrompt) -> CredentialAnswer?
}

/// ~ 展开为家目录（IdentityFile 语义）。
func expandTilde(_ paths: [String]) -> [String] {
    let home = NSHomeDirectory()
    return paths.map { p in
        if p == "~" { return home }
        // **分隔符不能丢**：`home + p.dropFirst(2)` 把 `~/` 连斜杠一起吃掉，
        // `~/.ssh/id_rsa` 就变成 `<home>.ssh/id_rsa`——默认钥匙链一把都
        // 读不到，用户看到的是「未找到可用私钥（试过：<home>.ssh/…）」，
        // 而 ssh 命令行用同一把钥匙明明连得上（实测踩过）。
        if p.hasPrefix("~/") { return home + p.dropFirst(1) }
        return p
    }
}


/// 在途连接的取消票。主线程投、工作线程查，故自带锁。
///
/// 为什么需要：关掉一个**还在建连接**的标签页时，工作线程可能正卡在
/// 「弹主机密钥 / 问私钥口令 / 问登录密码」上。不投这张票，用户就会看到
/// 「标签页都没了，还弹一个口令框」。工作线程在每个会弹框的地方查一次，
/// 命中就当作取消——不弹框、直接让这次连接以失败收场（标签页已不在，
/// 失败也不会落到任何界面上）。
final class ConnectCancellations: @unchecked Sendable {
    private let lock = NSLock()
    private var ids: Set<UUID> = []

    func add(_ id: UUID) { lock.lock(); ids.insert(id); lock.unlock() }

    /// 命中即消费（一次性）。
    func take(_ id: UUID) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return ids.remove(id) != nil
    }

    func contains(_ id: UUID) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return ids.contains(id)
    }
}


/// 在途连接被用户取消（关闭了还在连接的标签页）。用它把连接就地结束，
/// 不弹凭据框、不建会话；调用方按「已取消」静默收尾。
struct ConnectCancelled: Error {}
