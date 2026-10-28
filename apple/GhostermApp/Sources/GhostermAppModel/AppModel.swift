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
import GhostermCore
import GhostermTerminal
import Foundation

@MainActor
public final class AppModel: ObservableObject {
    public enum Target: Hashable {
        case localShell
        case host(String)
    }

    @Published public private(set) var hosts: [HostEntry] = []
    @Published public private(set) var loadError: String?
    /// 终端配置加载失败的原因（nil = 正常）。UI 必须显示它——用户改了
    /// 字体却看到默认大小，比看到一条错误难查得多。
    @Published public private(set) var terminalConfigError: String?

    public let terminalApp: TerminalApp?

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

    /// 在途连接的取消票（跨线程：主线程投、工作线程查，故用带锁的类）。
    /// 关闭一个**还在建连接**的标签页时投一张；工作线程在每个会弹框的地方
    /// 查一次，命中就当「用户已经不要这条连接了」——不弹凭据框、不 attach、
    /// 直接释放（见 `ConnectCancellations`）。
    private nonisolated let cancelledConnects = ConnectCancellations()

    /// UI 侧注入的交互源：主线程上弹对话框并拿结果。
    public var interaction: AppInteraction?

    /// 终端配置（字体 / 主题…）的读写口。终端 app 用它的文件启动。
    public let settings: SettingsStore
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

    public init(configPath: String? = nil, knownHostsPath: String? = nil,
                settings: SettingsStore? = nil) {
        self.configPath = configPath ?? AppModel.defaultConfigPath()
        self.knownHostsPath = knownHostsPath ?? AppModel.defaultKnownHostsPath()
        self.settings = settings
            ?? SettingsStore(terminalConfigPath: SettingsStore.defaultTerminalConfigPath())
        self.preferences = AppPreferences()
        self.appearance = AppearancePreference.current
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
        self.terminalConfigError = nil
        do {
            let app = try TerminalApp.shared()
            let path = self.settings.terminalConfigPath
            if FileManager.default.fileExists(atPath: path),
               let err = app.reloadConfig(path: path) {
                // 配置无效：报错，但**不丢 app**（继续用引擎默认配置）。
                self.terminalApp = app
                self.terminalConfigError = "配置无效：\(err)"
            } else {
                self.terminalApp = app
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
        for pane in tab.panes where pane.status == .connecting { cancelledConnects.add(pane.id) }
        // 只有真有在途连接的标签页才投票：已建好的会话由 stop 收掉，投了
        // 也没人消费（还会让集合一直涨）。
        for pane in tab.filePanes where pane.remote && pane.panel == nil && pane.error == nil {
            cancelledConnects.add(pane.id)
        }
    }

    nonisolated public static func defaultConfigPath() -> String {
        (NSHomeDirectory() as NSString).appendingPathComponent(".ssh/config")
    }

    nonisolated public static func defaultKnownHostsPath() -> String {
        (NSHomeDirectory() as NSString).appendingPathComponent(".ssh/known_hosts")
    }

    public func reload() {
        do {
            let model = try HostListModel(path: configPath)
            hosts = model.hosts
            wildcards = model.wildcardPatterns
            configModel = model
            configStore = ConfigStore(path: configPath)
            loadError = nil
        } catch {
            hosts = []
            configModel = nil
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
        let entry = hosts.first { $0.pattern == pattern }
        let host = entry?.hostname ?? pattern
        let user = entry?.user ?? NSUserName()
        let port = UInt16(entry?.port ?? 22)
        let identityFiles = expandTilde(entry?.identityFiles ?? [])
        let kind = TabKind.sftp(RemoteTarget(pattern: pattern, user: user,
                                             host: host, port: port))

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
                    identityFiles: identityFiles, knownHosts: khPath, cancelID: paneID)
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
        let entry = hosts.first { $0.pattern == pattern }
        return RemoteTarget(pattern: pattern,
                            user: entry?.user ?? NSUserName(),
                            host: entry?.hostname ?? pattern,
                            port: UInt16(entry?.port ?? 22))
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
        let entry = hosts.first { $0.pattern == target.pattern }
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
                    identityFiles: identityFiles, knownHosts: khPath, cancelID: paneID)
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
                                                cancelID: UUID) async throws -> SFTPFileBackend {
        let ssh: SSHSession = try await withCheckedThrowingContinuation { cont in
            let model = self
            let thread = Thread {
                do {
                    let s = try model.connectAuthenticated(
                        host: host, port: port, user: user, knownHosts: knownHosts,
                        identityFiles: identityFiles, cancelID: cancelID
                    ) { [weak model] h, p, kt, fp in
                        guard let model, !model.cancelledConnects.contains(cancelID) else {
                            return .unknown
                        }
                        return model.decideHostKey(host: h, port: p, keyType: kt,
                                                  fingerprint: fp, cancelID: cancelID)
                    }
                    cont.resume(returning: s)
                } catch {
                    cont.resume(throwing: error)
                }
            }
            thread.name = "ghosterm-files-connect"
            thread.start()
        }
        let sftp = try ssh.sftp()
        return SFTPFileBackend(session: ssh, sftp: sftp, user: user, host: host,
                               initialPath: ".")
    }

    // MARK: - 分屏 / 标签页动作（UI 的入口，结构改动一律经 TabManager）

    /// 在当前窗格处一分为二：**新窗格自己一条会话**（与现有会话并列），
    /// 连接参数从当前标签页抄（同一个 host 再开一条 shell——
    /// 这是终端的常规用法：同一台机器开两个 shell）。
    public func splitActiveTab(axis: PaneLayout.Axis) {
        guard let tab = tabs.activeTab, !tab.isLocked else { return }
        if let file = tab.activeWorkspacePane?.files {
            tab.duplicateFilePane(file, axis: axis); return
        }
        guard let terminalApp = terminalApp, tab.activeWorkspacePane?.terminal != nil else { return }
        let startCwd = tab.activePane?.currentDirectory
        let pane = TerminalPane()
        // 两条路径同一语义：窗格自己结束（shell 退出）= 收起这格、布局塌陷。
        pane.onExit = { [weak self] p in self?.handlePaneExit(p) }
        tab.split(axis: axis, with: pane)

        if tab.kind.isLocalShell {
            startLocal(pane: pane, app: terminalApp, cwd: startCwd)
        } else if let target = tab.kind.remoteTarget {
            let entry = hosts.first { $0.pattern == target.pattern }
            startSSH(pane: pane, entry: entry, pattern: target.pattern, terminalApp: terminalApp)
        }
    }

    /// 关闭当前标签页（⌘W）。有会话在跑时留给 UI 确认。
    public func requestCloseActiveTab() {
        guard let tab = tabs.activeTab else { return }
        tabs.requestClose(tab)
    }

    /// 对**指定**标签页分屏（标签右键菜单用）：先切到它，再在它的当前窗格
    /// 一分为二。分屏的作用域是「当前窗格」，故不可能悄悄作用在别的标签页上。
    public func split(_ tab: TerminalTab, axis: PaneLayout.Axis) {
        tabs.activate(tab)
        splitActiveTab(axis: axis)
    }

    /// 关闭**指定**标签页的当前窗格（右键菜单）；只剩一个窗格时改为关标签页。
    public func closePane(in tab: TerminalTab) {
        if let file = tab.activeWorkspacePane?.files {
            if tab.workspacePanes.count == 1 { tabs.requestClose(tab); return }
            if file.panel == nil && file.error == nil { cancelledConnects.add(file.id) }
            tab.removeFilePane(file.id)
            return
        }
        guard let pane = tab.activePane else { tabs.requestClose(tab); return }
        if !tab.closePane(pane) { tabs.requestClose(tab) }
    }

    /// 缩放/还原当前窗格（⇧⌘↩）。没分屏时什么都不做——单格本来就铺满。
    public func toggleZoomActivePane() {
        guard let tab = tabs.activeTab, let id = tab.activeWorkspacePaneID else { return }
        tab.toggleWorkspaceZoom(id)
    }

    /// 关闭当前窗格；只剩一个窗格时改为关标签页（与 iGhostVT 同语义：
    /// 「关掉最后一个分屏」= 关掉这个标签页）。
    public func closeActivePane() {
        guard let tab = tabs.activeTab else { return }
        if let file = tab.activeWorkspacePane?.files {
            if tab.workspacePanes.count == 1 { tabs.requestClose(tab); return }
            if file.panel == nil && file.error == nil { cancelledConnects.add(file.id) }
            tab.removeFilePane(file.id)
            return
        }
        guard let pane = tab.activePane else { tabs.requestClose(tab); return }
        if !tab.closePane(pane) { tabs.requestClose(tab) }
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

    /// 保存终端设置并**应用到已有终端面**。
    ///
    /// 三步都不许静默：写文件失败抛错；libghostty 不接受这份配置也抛错
    /// （用户看到的就是「字体没变」——得告诉他为什么）。
    public func applyTerminalSettings(_ edits: [(String, String?)]) throws {
        let text = SettingsStore.merging(settings.rawConfig(), edits: edits)
        try settings.saveTerminalConfig(text)

        guard let app = terminalApp else {
            throw SettingsStoreError.invalid(
                terminalConfigError ?? "libghostty 未初始化")
        }
        // libghostty 的 app 级更新会遍历所有 surface 下发（App.zig
        // updateConfig），所以这一步之后已有终端面自动是新配置。
        if let err = app.reloadConfig(path: settings.terminalConfigPath) {
            throw SettingsStoreError.invalid(err)
        }
        terminalConfigRevision += 1
    }

    /// 当前生效的终端配置文本（设置页回显用）。
    public var terminalConfigText: String { settings.rawConfig() }

    /// 终端配置文件被写回过一次（每次写回成功 +1）。
    ///
    /// 为什么需要它：`terminalConfigText` / `themeSelection` 都是**从文件现读**
    /// 的（文件即真相），不是 `@Published` 存储属性。设置页那两个配色下拉的
    /// 标题、勾选与「当前生效的配置」那块都读它们——没有这声广播，SwiftUI
    /// 就不知道要重画，改完一档之后标题会停在旧值上（用户实测：「选了能生效，
    /// 但下拉还显示原来那个」）。写回成功的每一处都必须 `+= 1`。
    @Published public private(set) var terminalConfigRevision = 0

    /// 整份替换终端配置（「配置文件」页的编辑框）。
    ///
    /// 与 `applyTerminalSettings` 的区别：那个是**稀疏补丁**（改几个键），
    /// 这个是**整份原文**——用户在编辑框里写的就是他要的全文，逐字写回。
    /// 两步仍然都不许静默：写文件失败抛错；引擎不接受这份配置也抛错，
    /// 并把**旧配置**留在原地（`saveTerminalConfig` 是先写文件，所以这里
    /// 要先把文件恢复回去，不能让一个坏配置落了盘）。
    public func replaceTerminalConfig(_ text: String) throws {
        let before = settings.rawConfig()
        try settings.saveTerminalConfig(text)
        guard let app = terminalApp else {
            // 连 app 都没有：说明本来就没在跑配置，写文件即完成。
            terminalConfigRevision += 1
            return
        }
        if let err = app.reloadConfig(path: settings.terminalConfigPath) {
            // 引擎不收：把文件恢复成上一份（**不留半份**——它现在是个
            // 引擎读不懂的配置，留着会让下次启动直接进「配置无效」）。
            try? settings.saveTerminalConfig(before)
            throw SettingsStoreError.invalid(err)
        }
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

    /// 读一个终端配置项的当前值（有文件读文件；否则问 libghostty 的默认值）。
    public func terminalSetting(_ key: String) -> String? {
        let text = settings.rawConfig()
        if let v = SettingsStore.value(of: key, in: text) { return v }
        // 文件里没写 → 回落到 libghostty 的默认值（按键的 C 类型问：
        // f32 与 f64 是不同的入口，用错类型只会拿到 0/false）。
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
        guard let model = configModel, let store = configStore else {
            throw GhostermError.failed("配置未加载")
        }
        try model.edit(alias: alias, edits)
        try store.save(model.serialized)
        reload()
    }

    /// 新建 Host 块（reset=true：文件末尾追加新块）。
    public func addHost(alias: String,
                        _ edits: [(String, [String]?)]) throws {
        guard let model = configModel, let store = configStore else {
            throw GhostermError.failed("配置未加载")
        }
        try model.edit(alias: alias, edits, reset: true)
        try store.save(model.serialized)
        reload()
    }

    /// 一块的全部指令（编辑器初值）。
    public func directives(of alias: String) -> [(keyword: String, values: [String])] {
        configModel?.directives(of: alias) ?? []
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
            let entry = hosts.first { $0.pattern == pattern }
            // `.ssh`：**每次点击都开一个新标签页**（同一台主机也允许开
            // 多个）——去重只留给 SFTP（同一目标复用一条浏览会话是合理的），
            // 终端会话不该被吞掉（用户实测）。
            kind = .ssh(RemoteTarget(pattern: pattern,
                                     user: entry?.user ?? NSUserName(),
                                     host: entry?.hostname ?? pattern,
                                     port: UInt16(entry?.port ?? 22)))
        }
        // 两个分支都**不带身份**、且这里**没有去重**：一律新建标签页
        // （同一台主机 / 本地 shell 都允许开多个，见方法头注释）。

        // 标签页与窗格先建（状态 .connecting：屏幕显示「连接中…」），
        // 连接在工作线程建好后再 attach——绝不空窗假装已连。
        let pane = TerminalPane()
        // 单窗格标签页退出 = 关标签页（与分屏同一策略；最后一个窗格不留画面）。
        pane.onExit = { [weak self] p in self?.handlePaneExit(p) }
        let tab = TerminalTab(kind: kind, pane: pane)
        tabs.add(tab)

        switch target {
        case .localShell:
            startLocal(pane: pane, app: terminalApp, cwd: cwd)
            statusMessage = "本地 shell"
        case .host(let pattern):
            let entry = hosts.first { $0.pattern == pattern }
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
        let host = entry?.hostname ?? pattern, user = entry?.user ?? NSUserName()
        let port = UInt16(entry?.port ?? 22)
        let paths = expandTilde(entry?.identityFiles ?? [])
        let knownHosts = knownHostsPath, id = pane.id
        statusMessage = "连接 \(user)@\(host)…"
        // 与 SFTP 共用认证；连接只在工作线程进行，每条终端独占 SSHSession。
        pane.onStop = { [weak self] in self?.cancelledConnects.add(id) }
        let thread = Thread { [weak self, weak pane] in
            guard let self else { return }
            let result: Result<(SSHSession, Transport), Error> = Result {
                let ssh = try self.connectAuthenticated(host: host, port: port, user: user,
                    knownHosts: knownHosts, identityFiles: paths, cancelID: id) { [weak self] h, p, kt, fp in
                        self?.decideHostKey(host: h, port: p, keyType: kt, fingerprint: fp, cancelID: id) ?? .unknown
                    }
                try self.throwIfCancelled(id)
                let transport = try Transport.openSSHShell(ssh, term: "xterm-256color", cols: 80, rows: 24)
                return (ssh, transport)
            }
            DispatchQueue.main.async { [weak self, weak pane] in
                guard let self else { return }
                let cancelled = self.cancelledConnects.take(id)
                guard let pane, !cancelled, self.tabs.tabs.contains(where: { $0.panes.contains(where: { $0.id == id }) }) else { return }
                pane.onStop = nil
                do {
                    let (ssh, transport) = try result.get()
                    let surface = try TerminalSurface(app: terminalApp)
                    let session = SessionController(transport: transport, surface: surface, sshSession: ssh)
                    pane.attach(surface: surface, session: session); pane.start()
                } catch { pane.markFailed(connectionErrorMessage(error)) }
            }
        }
        thread.name = "ghosterm-terminal-connect"; thread.start()
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

    /// 挂起一次确认请求并在主线程等结果。decider 在工作线程被调用，
    /// 这里同步桥到主线程的 interaction（AppKit alert 是模态的）。
    private nonisolated func decideHostKey(host: String, port: UInt16,
                                           keyType: String,
                                           fingerprint: String, cancelID: UUID) -> HostKeyDecision {
        var decision = HostKeyDecision.unknown
        let semaphore = DispatchSemaphore(value: 0)
        DispatchQueue.main.async { [weak self] in
            guard let self = self, !self.cancelledConnects.contains(cancelID) else { semaphore.signal(); return }
            let prompt = HostKeyPrompt(host: host, port: port, keyType: keyType,
                                       fingerprint: fingerprint)
            self.pendingHostKey = prompt
            defer {
                self.pendingHostKey = nil
                semaphore.signal()
            }
            guard let interaction = self.interaction else { return }
            decision = interaction.confirmHostKey(prompt)
        }
        semaphore.wait()
        return decision
    }

    /// UI 调用：用户按了「信任并连接」（TOFU——核心会落盘 known_hosts）。
    public func resolveHostKey(_ decision: HostKeyDecision) {
        // decideHostKey 用模态 interaction 拿结果，这条路径留给
        // 非模态 UI：结果已在 pendingHostKey 挂起时记录，这里只清状态。
        pendingHostKey = nil
    }

    /// 测试辅助：伪造「终端配置无效」。
    ///
    /// 真实路径只在 init 里按 libghostty 的解析结果设一次，测试没法让它出错；
    /// 而顶带那个 ⚠ 按钮（`BandControls`）只在这个值非空时才画，不给钩子就
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

    // MARK: - 认证（凭据桥）

    /// 建一条**已认证**的 SSH 会话（工作线程）。
    ///
    /// 顺序照 OpenSSH，也照用户对 `ssh` 命令行的预期：
    ///   1. **先握手一次** —— 拿到服务器允许的认证方式（`gt_ssh_auth_methods`）；
    ///   2. **逐把钥匙真试**（同一条会话上重试，libssh2 支持）：钥匙每次
    ///      从 `~/.ssh` 读文件（**不缓存进 Keychain**，见下），只有**真加密**
    ///      的钥匙才用缓存口令或问一次——没口令的钥匙绝不该被问口令；
    ///   3. 钥匙都不行：服务器收密码 → 问**登录密码**（不是私钥口令）；
    ///   4. 都不行：如实报「试过什么 + 服务器允许什么」，不猜、不静默。
    ///
    /// 为什么必须按这个顺序（用户实测踩过的坑）：
    ///   · 先前是「设凭据 → connect（内含握手+认证）」一步走：方法表在认证前
    ///     恒为空，于是「服务器只收密码」永远判不出来，一律猜成「问私钥口令」
    ///     ——用户看到的就是「一把没口令的钥匙却让我输口令」；
    ///   · 先前只要文件读得出来就先弹口令框（连没加密的钥匙也弹）。
    private nonisolated func connectAuthenticated(
        host: String, port: UInt16, user: String, knownHosts: String,
        identityFiles: [String], cancelID: UUID, decider: @escaping HostKeyDecider
    ) throws -> SSHSession {
        // 用户在握手/认证途中把这个标签页关掉了 → 就地取消，不在已关掉的
        // 标签页上继续弹框（凭据 / 主机密钥）。
        try throwIfCancelled(cancelID)
        let ssh = try SSHSession(SSHOptions(
            host: host, port: port, username: user, knownHostsPath: knownHosts
        ))
        // 主机密钥：三态裁决。UNKNOWN ≠ REJECT——弹真对话框。
        // （已知且一致由核心直接放行，decider 不会被调用。）
        ssh.setHostKeyDecider(decider)

        // 握手：主机密钥 + 服务器认证方式。只做一次，钥匙都在这一条会话上试。
        try ssh.handshake()
        let methods = ssh.authMethods

        // ---- 1. 逐把钥匙 ----
        let keyPaths = identityFiles.isEmpty ? Self.defaultKeyPaths() : identityFiles
        var triedKeys: [String] = []
        var lastAuthError: Error?
        for path in keyPaths {
            // 钥匙**每次都从文件读**：开着的会话本来就要公钥去认证，而
            // 「把已解锁的 PEM 缓存进 Keychain」正是会让 macOS 弹
            // 「想要使用钥匙串」的那一步（一个 generic-password 项被反复读，
            // 用户莫名其妙被问一次）——用户实测报的就是这个。私钥仍不落盘、
            // 不进日志（铁律 #5），只是不再多存一份副本。
            guard let pem = readPrivateKeyFile(path) else {
                continue // 文件不存在/读不了：跳过，别把它算进「试过」
            }
            triedKeys.append(path)

            // 口令只对**真加密**的钥匙问（或复用 Keychain 里存过的）。
            var passphrase: String? = nil
            switch Ghosterm.keyPassphraseRequirement(pem) {
            case .needed:
                try throwIfCancelled(cancelID)
                passphrase = try passphraseFor(keyPath: path, host: host,
                                               port: port, user: user, cancelID: cancelID)
                if passphrase == nil {
                    // 用户取消了口令框：这把钥匙留着不动，试下一把。
                    continue
                }
            case .notNeeded, .unknown:
                // 没加密（或认不出来的格式）：不问口令，直接拿去试。
                // 认不出来的文件 libssh2 本来也用不了，问口令解不开它。
                passphrase = nil
            }

            do {
                try ssh.authenticate(privateKeyPEM: pem, passphrase: passphrase)
                try ssh.authenticate()
                return ssh
            } catch {
                lastAuthError = error
                // 这把钥匙没被接受：把整条 host 的口令缓存撤掉（多半是错的，
                // 留着会让下一次连接拿错口令去试别的钥匙）。
                try? KeychainStore.removePassphrase(forHost: host, port: Int(port))
            }
        }

        // ---- 2. Keychain 里存过的登录密码（用户授权过一次就不该反复问）----
        if let saved = try? KeychainStore.password(forHost: host, port: Int(port)),
           !saved.isEmpty {
            do {
                try ssh.authenticate(password: saved)
                try ssh.authenticate()
                return ssh
            } catch {
                // 密码过期了：清掉，走下面的常规流程。
                lastAuthError = error
                try? KeychainStore.removePassword(forHost: host, port: Int(port))
            }
        }

        // ---- 3. 按**握手上拿到的**方法表决定问什么 ----
        // 这一条是先前永远走不到的死路（方法表那时还没握手）：明明服务器
        // 只收密码，却被问成「私钥口令」。
        let wantsPassword = methods.contains(.password)
            || methods.contains(.keyboardInteractive)
        if wantsPassword {
            try throwIfCancelled(cancelID)
            guard let answer = askCredential(host: host, port: port, user: user,
                                             kind: .password(reason: passwordReason(
                                                tried: triedKeys)), cancelID: cancelID) else {
                throw GhostermError.failed(
                    "服务器要求密码认证，但未提供密码（服务器允许：\(methods)）")
            }
            guard let password = answer.password, !password.isEmpty else {
                throw GhostermError.failed("未输入密码")
            }
            try ssh.authenticate(password: password)
            try ssh.authenticate()
            // 存 Keychain：同一 host:port 下次**不用再问**（铁律 #5 的
            // 平台安全存储；密钥材料不进日志、不进文件）。
            try? KeychainStore.setPassword(password, forHost: host, port: Int(port))
            return ssh
        }

        // ---- 4. 都不行：如实说清（不猜、不静默降级）----
        if triedKeys.isEmpty {
            throw GhostermError.failed(
                "没有可用的私钥文件（试过：\(keyPaths.joined(separator: "、"))）"
                + "；服务器允许：\(methods)")
        }
        throw lastAuthError ?? GhostermError.failed(
            "已试过私钥都不被服务器接受（\(triedKeys.joined(separator: "、"))）"
            + "；服务器允许：\(methods)")
    }

    /// 取消票命中就抛——把这次连接就地结束，不再弹框、不建会话。
    private nonisolated func throwIfCancelled(_ id: UUID) throws {
        if cancelledConnects.contains(id) { throw ConnectCancelled() }
    }

    /// 密码摘要里的一句话：说清「私钥试过但没被接受」，别让用户以为是
    /// 自己密码打错了。
    private nonisolated func passwordReason(tried keyPaths: [String]) -> String {
        if keyPaths.isEmpty {
            return "服务器要求密码认证。"
        }
        let names = keyPaths.map { ($0 as NSString).lastPathComponent }
            .joined(separator: "、")
        return "已试过私钥（\(names)）但服务器不接受，改用密码认证。"
    }

    /// 主线程问口令；无 interaction（无 UI）返回 nil。
    private nonisolated func askCredential(host: String, port: UInt16, user: String,
                                           kind: CredentialPrompt.Kind, cancelID: UUID)
        -> CredentialAnswer? {
        var answer: CredentialAnswer?
        let semaphore = DispatchSemaphore(value: 0)
        DispatchQueue.main.async { [weak self] in
            guard let self = self, !self.cancelledConnects.contains(cancelID) else { semaphore.signal(); return }
            let prompt = CredentialPrompt(host: host, port: port, user: user,
                                          kind: kind)
            self.pendingCredential = prompt
            defer {
                self.pendingCredential = nil
                semaphore.signal()
            }
            guard let interaction = self.interaction else { return }
            answer = interaction.askCredential(prompt)
        }
        semaphore.wait()
        return answer
    }

    /// IdentityFile 缺省时的默认链（与 OpenSSH 用户习惯一致）。
    private nonisolated static func defaultKeyPaths() -> [String] {
        ["id_ed25519", "id_ecdsa", "id_rsa"].map { name in
            (NSHomeDirectory() as NSString).appendingPathComponent(".ssh/\(name)")
        }
    }

    private nonisolated func readPrivateKeyFile(_ path: String) -> String? {
        try? String(contentsOfFile: path, encoding: .utf8)
    }

    /// Keychain 里的口令，没有则问一次并存回。
    ///
    /// **只在调用方确认这把钥匙真的加密之后**才会被调（见 connectAuthenticated）：
    /// 没口令的钥匙不该被问口令——那是用户实测里最刺眼的一条。
    private nonisolated func passphraseFor(keyPath: String, host: String,
                                           port: UInt16, user: String, cancelID: UUID) throws -> String? {
        if let cached = try? KeychainStore.passphrase(forHost: host, port: Int(port)) {
            return cached
        }
        guard let answer = askCredential(host: host, port: port, user: user,
                                         kind: .passphrase, cancelID: cancelID) else { return nil }
        if let pass = answer.passphrase {
            try? KeychainStore.setPassphrase(pass, forHost: host, port: Int(port))
        }
        return answer.passphrase
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
