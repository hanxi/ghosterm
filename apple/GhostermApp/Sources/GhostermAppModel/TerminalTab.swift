// TerminalTab / TerminalPane — 标签页、窗格与分屏布局（铁律 #1 的 UI 侧）。
//
// 结构参照 iGhostVT（用户指定的参照实现）：
//   TabManager  ──owns──> [TerminalTab] ──owns──> PaneLayout
//                                                     └─ TerminalPane
//                                                          ├─ TerminalSurface
//                                                          └─ SessionController
//
// 所有权铁律：一个 pane 自己撑住自己的 surface 与 session。
// 谁建会话谁持有——SessionController 的 UAF 教训（transport 只借 gt_ssh_t）。
//
// 三态不折叠（铁律 #6）：pane 状态四档显式（connecting / ready / failed /
// exited），**没有**「连接中 = 空屏」这种折叠——空屏到底是在连还是坏了，
// 必须看得出来（iGhostVT 的 SessionStatusOverlay 同理）。
import Foundation
import Combine
import GhostermCore
import GhostermTerminal

/// 一台远端主机 + 认证主体。
///
/// **两个键分开**：身份是 `TerminalTab.id`（"哪个标签页"），目标才是这个
/// struct（"哪台主机"）。`pattern` 是 ssh_config 里的别名（给人看的名字），
/// `host` 是求值后的真实主机名（给连接用）——两者都要留。未来的「从 SSH
/// 终端跳到同主机的 SFTP」就是按它匹配（见 roadmap 的显式延后项）。
public struct RemoteTarget: Hashable, Sendable {
    public let pattern: String
    public let user: String
    public let host: String
    public let port: UInt16

    public init(pattern: String, user: String, host: String, port: UInt16) {
        self.pattern = pattern
        self.user = user
        self.host = host
        self.port = port
    }
}

/// 一个标签页连接的是什么。决定标题、副标题与重连语义。
///
/// **不携带"身份"**：以前给 `localShell` / `ssh` 挂过 `instance: UUID`，
/// 那是为了配合"已开着就切过去"的去重——而现在终端与 SFTP 都**每次新开**
/// （见 `AppModel.connect` / `openSFTP`），全仓再无按 `kind` 去重的代码，
/// `instance` 就成了纯累赘。身份本来就有 `TerminalTab.id`。故这里回归
/// "是什么 + 在哪"：value type 只描述内容。
public enum TabKind: Equatable {
    /// 本地 shell（同一个窗口可以开多个——每次都是新标签页，不去重）。
    case localShell
    /// 不启动 shell 的本地文件工作区。
    case localFiles

    /// SSH 会话。
    case ssh(RemoteTarget)

    /// SFTP 文件浏览（与终端解耦的第二类标签页：同一 core，不同 UI）。
    case sftp(RemoteTarget)

    /// 远端目标（本地 shell 为 nil）。跳转 / 复建 / 分屏都按它匹配**同一台主机**。
    public var remoteTarget: RemoteTarget? {
        switch self {
        case .localShell, .localFiles: return nil
        case .ssh(let t), .sftp(let t): return t
        }
    }

    /// 侧边栏 / 标签条显示的初始名字（用户可改）。
    public var defaultTitle: String {
        switch self {
        case .localShell: return "本地 shell"
        case .localFiles: return "本地文件"
        case .ssh(let t): return t.pattern
        case .sftp(let t): return "SFTP · \(t.pattern)"
        }
    }

    /// 副标题：`user@host` 风格（iGhostVT 截图里标签 `~ zsh` 的位置）。
    public var subtitle: String {
        switch self {
        case .localShell, .localFiles: return "本机"
        case .ssh(let t): return Self.describe(t)
        case .sftp(let t): return Self.describe(t)
        }
    }

    private static func describe(_ t: RemoteTarget) -> String {
        t.port == 22 ? "\(t.user)@\(t.host)" : "\(t.user)@\(t.host):\(t.port)"
    }

    /// 终端类（可再分屏 / 可喂字节）；SFTP 类只有一个浏览器面。
    public var isTerminal: Bool {
        switch self { case .localShell, .ssh: return true; case .localFiles, .sftp: return false }
    }

    /// 图标（侧边栏 / 标签条）。
    public var symbolName: String {
        switch self {
        case .localShell: return "terminal"
        case .ssh: return "network"
        case .sftp, .localFiles: return "folder"
        }
    }

    /// 本地 shell 标签页（分屏时再起一个本地 PTY，不走 SSH）。
    public var isLocalShell: Bool { self == .localShell }
}

/// 一个窗格的状态。**四档显式**，不折叠：
///   connecting —— 正在建立（工作线程在跑），屏幕该显示「连接中…」；
///   ready      —— 会话在线，屏幕是终端内容；
///   failed     —— 建立失败（错误串必须非空），屏幕显示卡片；
///   exited     —— 会话自己结束了（远端 shell 退出），屏幕保留最后画面。
public enum PaneStatus: Equatable {
    case connecting
    case ready
    case failed(String)
    case exited(code: Int32)
    case exitedUnknown

    public var label: String {
        switch self {
        case .connecting: return "连接中…"
        case .ready: return ""
        case .failed(let m): return "失败：\(m)"
        case .exited(let c): return "已退出（\(c)）"
        case .exitedUnknown: return "已结束（退出码未知）"
        }
    }

    public var isFailed: Bool { if case .failed = self { return true }; return false }
    public var isConnecting: Bool { self == .connecting }
    public var isReady: Bool { self == .ready }
    public var isExited: Bool { if case .exited = self { return true }; return self == .exitedUnknown }
}

/// 上一条命令的观测（来自 OSC 133 的 `output_start` / `command_end`）。
///
/// **三态不折叠（铁律 #6）**：`exitCode == nil` 表示 **shell 没报退出码**——
/// 既不是成功也不是失败。`failed` 只在「报了且非 0」时为 true。
public struct LastCommand: Equatable, Sendable {
    /// shell 报的命令行（**不可信文本**，显示前要清理）；没报就是空串。
    public var commandLine: String = ""
    /// shell 报的退出码；nil = 没报。
    public var exitCode: Int32?
    /// 命令耗时；nil = 没有起点（shell 中途才开始报 / 只收到结束）。
    public var duration: TimeInterval?

    /// 内部：算耗时用的单调时刻起点。
    var startedAt: TimeInterval?

    /// 有东西可显示吗（视图据此决定画不画那一行）。
    public var hasAnything: Bool { !commandLine.isEmpty || exitCode != nil }

    /// 明确失败（报了退出码且非 0）。**没报退出码时是 false**，不是 true。
    public var failed: Bool { if let c = exitCode { return c != 0 }; return false }

    /// 这条命令**还在跑**吗（窗格标题要显示进程名，靠的就是它）。
    ///
    /// 看的是 `startedAt`（outputStart 置、commandEnd 清），**不是**
    /// `exitCode == nil`：后者在「shell 没报退出码」时也成立，于是一条
    /// 早就跑完的命令会被当成还在跑，标题永远卡在那个程序名上——正是
    /// 铁律 #6 要防的那种折叠。
    public var isRunning: Bool { startedAt != nil }

    /// 侧栏那行的文案。命令行按规范清理过不可见格式字符。
    public var sidebarLine: String {
        var parts: [String] = []
        let cmd = ProgramStatusBadge.clean(commandLine)
        parts.append(cmd.isEmpty ? "上一条命令" : cmd)
        if let d = duration { parts.append(String(format: "%.2fs", d)) }
        if let c = exitCode { parts.append("退出码 \(c)") }
        return parts.joined(separator: " · ")
    }

    /// 推进这个状态机。**纯函数**：`now` 由调用方给（单调时刻），
    /// 于是状态转移与耗时都能离线测，且接线与测试走同一条路径。
    static func advanced(_ s: LastCommand, event: SemanticPromptEvent,
                         now: TimeInterval) -> LastCommand {
        var s = s
        switch event.kind {
        case .outputStart:
            s.commandLine = event.command
            s.exitCode = nil        // 新命令刚开始：退出码还没报
            s.duration = nil
            s.startedAt = now
        case .commandEnd:
            s.exitCode = event.hasExitCode ? event.exitCode : nil
            if let start = s.startedAt { s.duration = max(0, now - start) }
            s.startedAt = nil
        default:
            break                   // input_start / prompt_start 等不动它
        }
        return s
    }
}

/// 一个窗格：一套 surface + session，以及它的状态。
@MainActor
public final class TerminalPane: ObservableObject, Identifiable {
    public let id = UUID()
    /// 建立完成后才有值；连接中为 nil（**不是**用空 surface 假装已连）。
    @Published public private(set) var surface: TerminalSurface?
    @Published public private(set) var status: PaneStatus = .connecting

    /// 程序状态（OSC 7501）。terminal 只把报告搬过来，记录管理在这层
    /// （见 ProgramStatusStore 顶部注释）。
    @Published public private(set) var programStatus = ProgramStatusStore()

    /// 上一条命令（OSC 133 的 output_start / command_end）。
    @Published public private(set) var lastCommand = LastCommand()

    /// surface 报来的工作目录（OSC 7）。远端用它（本地另有更可靠的内核查询）。
    /// 没有 shell 集成时为 nil——**不猜**。
    @Published public private(set) var reportedDirectory: String?

    /// 会话**自己结束**（本地 / SSH shell `exit`，读到 EOF）时的回调。
    ///
    /// **只在 `.exited` 时发，不在建立失败（`.failed`）时发**：失败要在屏幕上
    /// 保留错误卡片，不能用这条把窗格抹掉（铁律 #6「不静默降级」）。分屏里
    /// 收到它就该**收掉这个窗格**（布局塌陷）——由持有者决定（pane 不认识自己
    /// 的 tab，结构改动一律经持有者，见 `AppModel.handlePaneExit`）。
    public var onExit: ((TerminalPane) -> Void)?

    /// 谁建会话谁持有（UAF 教训）：pane 撑住 SessionController，
    /// SessionController 撑住 transport 与 SSHSession。
    @Published public var tmuxContext: TmuxPaneContext?
    private var session: SessionController?
    var onStop: (() -> Void)?

    public init() {}

    /// 装配一条已建立的会话（surface 必须在主线程创建）。
    public func attach(surface: TerminalSurface, session: SessionController) {
        self.surface = surface
        self.session = session
        status = .ready
        observeState()
        observeProgramStatus()
        observeSemanticPrompt()
        observePwd()
    }

    public func start() { session?.start() }

    /// 会话是否还在（`stop()` 之后为 false）。
    ///
    /// 存在的理由：这是**能被外部观测**的会话归属——「关掉标签页之后
    /// 会话还在后台跑」是一种不报错的泄漏（多一条没人管的连接与读循环），
    /// 没有这个读数就只能靠内存工具才看得见。UI 也可用它做灰显判断。
    public var isRunning: Bool { session != nil }

    /// 停会话（先置停止标志，再等读循环真的退出——见 SessionController.stop）。
    public func stop() {
        onStop?()
        session?.stop()
        session = nil
    }

    /// 会话状态变化（低频；SessionController 已 hop 回主线程）。
    func observeState() {
        session?.onStateChange = { [weak self] state in
            guard let self = self else { return }
            switch state {
            case .running: self.status = .ready
            case .idle: self.status = .connecting
            case .exited(let code):
                self.status = .exited(code: code)
                // 程序退出：working/blocked 不再成立（done/error 留到用户看过）。
                self.programStatus.removeEphemeral()
                // 通知持有者：分屏里该收掉这个窗格（最后一个 → 关标签页）。
                self.onExit?(self)
            case .exitedUnknown:
                self.status = .exitedUnknown
                self.programStatus.removeEphemeral()
                self.onExit?(self)
            case .failed(let message):
                self.status = .failed(message)
                self.programStatus.removeEphemeral()
            }
        }
    }

    /// 接上程序状态（OSC 7501）。
    ///
    /// 回调来自 libghostty 的 **termio 线程**（与 onInput 同一线程），
    /// 故必须 hop 回主线程再改 @Published 状态（同 SessionController 的写法）。
    ///
    /// 提示符开始那一半由 `observeSemanticPrompt`（OSC 133）负责；
    /// 程序退出那一半在 `observeState` 里。
    func observeProgramStatus() {
        surface?.onProgramStatus = { [weak self] report in
            DispatchQueue.main.async { [weak self] in
                self?.applyProgramStatus(report)
            }
        }
    }

    /// 测试辅助：直接设「上一条命令」（避免为了造状态去驱动整条 OSC 133）。
    func applyLastCommandForTesting(_ value: LastCommand) {
        lastCommand = value
    }

    /// 应用一条程序状态报告。抽成函数是为了接线与测试**走同一条路径**。
    func applyProgramStatus(_ report: ProgramStatusReport) {
        programStatus.apply(report)
    }

    /// 接上 shell 集成（OSC 133）。
    ///
    /// 规范要求：**提示符（重新）开始时**清掉 working/blocked 记录——
    /// 用户面前又出现了命令行，说明上一条命令在「显示层面」结束了
    /// （`done`/`error` 保留到用户看过）。同 onProgramStatus，回调在
    /// termio 线程，必须 hop 回主线程。
    func observeSemanticPrompt() {
        surface?.onSemanticPrompt = { [weak self] event in
            DispatchQueue.main.async { [weak self] in
                self?.handleSemanticPrompt(event)
            }
        }
    }

    /// 处理一条 shell 集成事件。抽成函数是为了接线与测试**走同一条路径**
    /// （不然测的是另写一遍的规则）。
    func handleSemanticPrompt(_ event: SemanticPromptEvent) {
        // 1) 「提示符（重新）开始」= 上一条命令在显示层面结束 → 清 working/blocked。
        if event.kind == .promptStart {
            programStatus.removeEphemeral()
        }
        // 2) 推进「上一条命令」（命令行 / 耗时 / 退出码）。
        lastCommand = LastCommand.advanced(lastCommand, event: event,
                                           now: ProcessInfo.processInfo.systemUptime)
    }

    /// 接上工作目录（OSC 7）。回调在 libghostty 线程，故 hop 回主线程
    /// （同 onProgramStatus 的写法）。
    func observePwd() {
        surface?.onPwd = { [weak self] pwd in
            DispatchQueue.main.async { [weak self] in self?.reportedDirectory = pwd }
        }
    }

    /// 测试辅助：只接一个 surface（不建会话），用于断言 OSC 7 这类**表面事件**。
    func attachForTesting(surface: TerminalSurface) {
        self.surface = surface
        observePwd()
    }

    /// 当前目录优先使用本地 PTY 的内核观测，随后回落到
    /// OSC 7；没有证据时保持 nil，供新建、分屏与文件面板使用。
    public var currentDirectory: String? {
        if tmuxContext == nil, let local = session?.childCwd { return local }
        return reportedDirectory
    }

    /// 这格正在跑的程序名；nil = 停在提示符，或这个 shell 没装集成。
    ///
    /// 唯一来源是 OSC 133 报的命令行——整条链路（Transport.ChildState 只有
    /// unknown/exited/idle/job）里**没有**前台进程名这个东西。拿不到就老实
    /// 返回 nil 让标题回落到目录，不去猜。
    public var runningProgram: String? {
        guard lastCommand.isRunning else { return nil }
        return PaneTitleRule.programName(commandLine: lastCommand.commandLine)
    }

    /// 窗格标题栏上那两段文字：进程 → 目录 → `fallback`（标签页标题）。
    public func displayTitle(fallback: String) -> PaneTitleText {
        PaneTitleRule.resolve(program: runningProgram,
                              directory: currentDirectory,
                              tabTitle: fallback)
    }

    /// 建立失败的收尾：**保留** pane（屏幕显示失败卡片），不是把 tab
    /// 悄悄删掉——用户得看得见失败与原因（铁律 #6）。
    public func markFailed(_ message: String) {
        session?.stop(); onStop = nil
        surface = nil
        session = nil
        status = .failed(message)
    }

    public func markConnecting() { status = .connecting }

    /// 锁定态下挡住的输入（UI 层用它决定是否把按键交给 surface）。
    /// 这里只做数据面：`TerminalTab.isLocked` 是真相，UI 读它。

    /// 前台作业判定（关闭确认用）。建立前/失败后没有 transport，按
    /// `.unknown`（**不折叠**：查不出来就不当空闲）。
    public var childState: Transport.ChildState {
        // The local attach client's foreground group says nothing about tmux's
        // inner jobs. Only its stream ending is observable without a protocol.
        if tmuxContext != nil { return status.isExited ? .exited : .unknown }
        return session?.childState ?? .unknown
    }

    /// 关闭这个窗格会不会打断东西。**纯函数**：接线与测试走同一条路径
    /// （不建真会话也能断言每一档，见 TabManagerTests）。
    public static func closeNeedsConfirmation(status: PaneStatus,
                                              childState: Transport.ChildState) -> Bool {
        switch status {
        case .exited, .exitedUnknown, .failed, .connecting: return false  // 没什么可丢
        case .ready: return childState != .idle           // 有作业 / 查不出 → 问
        }
    }

    /// 关这个窗格要不要先问（读实时状态）。
    public var closeNeedsConfirmation: Bool {
        // 已结束：没什么可丢。记录可能还没刷到 .ended（状态先到、记录后到），
        // 所以先看本窗格自己的状态，否则退出后的自动收起会被弹框挡住。
        if tmuxContext != nil || status.isExited { return false }
        return Self.closeNeedsConfirmation(status: status, childState: childState)
    }
}

/// 分屏布局：叶子是窗格，内部节点是「一排 / 一列」子布局。
///
/// 用**递归**而不是「两个窗格」的扁平结构：三窗格以上的嵌套分屏是终端的
/// 常规用法（iGhostVT 的 panes 也是递归的），扁平结构早晚要重写。
public indirect enum PaneLayout {
    case leaf(TerminalPane)
    case split(axis: Axis, children: [PaneLayout])

    public enum Axis: Equatable { case horizontal, vertical }

    /// 渲染身份：**按结构定位**（自己 + 子树里每个叶子的 id），不用数组下标。
    ///
    /// 用途见 `RootView` 的 `PaneLayoutView`：分屏容器拿它当 `.id`，结构一变就
    /// 强制重建，避免 `HSplitView`/`VSplitView` 背后的 `NSSplitView` 沿用旧的
    /// 分隔位置。实测：三窗格关掉中间那个后，两个剩下的窗格停在上一次布局给
    /// 的**不相等**宽度（163.5 vs 735.5，而不是各半 450）——子视图集合变了，
    /// NSSplitView 只增删、不重算比例。身份里含每个叶子的 id，故**只拖分隔线
    /// 不改它**（子节点 id 没变），用户手动调好的比例不会被重建打断。
    public var viewID: String {
        switch self {
        case .leaf(let p): return "L\(p.id.uuidString)"
        case .split(let axis, let children):
            let a = axis == .horizontal ? "H" : "V"
            return "S\(a)[\(children.map(\.viewID).joined(separator: ","))]"
        }
    }
}

@MainActor
public final class TerminalTab: ObservableObject, Identifiable {
    public let id = UUID()
    public let kind: TabKind

    /// 终端类标签页的窗格布局（SFTP 类为 nil）。
    @Published public var workspaceLayout: WorkspaceLayout?
    private var fileObservers: [AnyCancellable] = []
    @Published public var workspacePanes: [WorkspacePane] = [] {
        didSet {
            fileObservers = workspacePanes.compactMap(\.files).map { file in
                file.objectWillChange.sink { [weak self] _ in self?.objectWillChange.send() }
            }
        }
    }
    @Published public var layoutCustomized = false
    /// 旧终端查询是只读投影，不是另一份布局状态。
    public var layout: PaneLayout? { workspaceLayout.flatMap(terminalProjection) }
    /// 分屏时「当前窗格」——⌘W 关它、输入焦点在它。
    @Published public var activeWorkspacePaneID: UUID?
    public var activePaneID: UUID? {
        get { activeWorkspacePaneID }
        set { activeWorkspacePaneID = newValue }
    }
    /// 被缩放（⇧⌘↩）铺满整块终端区的窗格；nil = 没缩放。
    ///
    /// **只是遮挡，不是卸载**：libghostty 的 surface 一释放会话就没了，
    /// 并且把隐藏的兄弟窗格缩到 1×1 会给它们发一轮 SIGWINCH 把回卷内容
    /// 揉烂。视图层的做法是让整棵布局照原尺寸渲染，只在上面盖一层全尺寸的
    /// 被缩放窗格——兄弟们的几何一个像素都不变。
    @Published public var zoomedPaneID: UUID?
    /// SFTP 类标签页的双栏会话（左本地右远程；与终端面互斥）。
    public var localFiles: WorkspaceFilePane? { filePanes.first { !$0.remote } }
    public var remoteFiles: WorkspaceFilePane? { filePanes.first { $0.remote } }
    public var filePanes: [WorkspaceFilePane] { workspacePanes.compactMap(\.files) }
    /// SFTP 建立失败的原因（**不折叠**：nil = 还在连或已就绪，看 browser）。
    public var sftpError: String? { remoteFiles?.error }

    // ---- 内嵌文件面板（终端标签页右侧；本地 shell / SSH 都有后端）----

    /// 文件面板的模型（nil = 未建；本地 shell 立即建，SSH 懒开第二会话）。
    public var filesPanel: FilesPanelModel? { (kind.isLocalShell ? localFiles : remoteFiles)?.panel }
    /// 面板可见性（默认收起，按标签记忆）。
    @Published public var filesPanelVisible: Bool = false
    /// 面板建立失败的原因（**不折叠**：nil = 还在连或已就绪）。
    public var filesPanelError: String? { remoteFiles?.error }
    /// 面板正在连接（SSH 懒开会话期间）。
    public var filesPanelConnecting: Bool { remoteFiles.map { $0.panel == nil && $0.error == nil } ?? false }

    @Published public var title: String
    /// 锁定：挡住误触输入（iGhostVT 的「锁定标签页」）。锁的是**输入**，
    /// 不是会话——会话继续跑。
    @Published public var isLocked: Bool = false

    /// SFTP 类标签页：无终端面，浏览器**建立后才**装配（连接中为 nil）。
    /// 之所以不做「先放个占位 browser」：占位得假装能列目录，那是把
    /// 「连不上」折叠成「空目录」（铁律 #6）。
    public init(kind: TabKind) {
        self.kind = kind
        self.workspaceLayout = nil
        self.title = kind.defaultTitle
        if case .sftp = kind {
            let file = WorkspaceFilePane(remote: true)
            self.workspacePanes = [.files(file)]
            self.workspaceLayout = .leaf(file.id)
            self.activeWorkspacePaneID = file.id
            self.filesPanelVisible = true
        }
    }

    /// 终端标签页（一个初始窗格）。
    public init(kind: TabKind, pane: TerminalPane) {
        self.kind = kind
        self.workspaceLayout = .leaf(pane.id)
        self.workspacePanes = [.terminal(pane)]
        self.activeWorkspacePaneID = pane.id
        self.title = kind.defaultTitle
    }

    /// 全部窗格（按布局顺序）。关闭 / 停止都遍历它。
    public var panes: [TerminalPane] {
        (workspaceLayout?.paneIDs ?? []).compactMap { id in workspacePanes.first { $0.id == id }?.terminal }
    }
    public var activeWorkspacePane: WorkspacePane? { workspacePanes.first { $0.id == activeWorkspacePaneID } }
    public var visibleWorkspaceLayout: WorkspaceLayout? {
        guard !filesPanelVisible, !panes.isEmpty else { return workspaceLayout }
        return workspaceLayout?.filtering(Set(panes.map(\.id)))
    }
    private func terminalProjection(_ node: WorkspaceLayout) -> PaneLayout? {
        switch node {
        case .leaf(let id): return workspacePanes.first { $0.id == id }?.terminal.map { .leaf($0) }
        case .split(_, let axis, _, let children):
            let projected = children.compactMap(terminalProjection)
            if projected.isEmpty { return nil }
            if projected.count == 1 { return projected[0] }
            return .split(axis: axis, children: projected)
        }
    }

    /// 当前窗格的工作目录（⌘T 继承目录用）。同 `TerminalPane.currentDirectory`。
    public var currentDirectory: String? { activePane?.currentDirectory }

    /// 连接还在途中吗（工作线程在跑，还没有结果）——关它需要投取消票。
    /// 终端类看有没有窗格在「连接中」；SFTP 类看「还没有 browser 也没报错」。
    public var hasInFlightConnection: Bool {
        if filePanes.contains(where: { $0.remote && $0.panel == nil && $0.error == nil }) { return true }
        return panes.contains { $0.status.isConnecting }
    }

    /// 关这个标签页要不要先问——即「有没有东西会被打断」。
    ///
    /// 铁律 #6：只有**能证实**「没什么可丢」才不问；查不出来（`.unknown`）
    /// 一律当「可能有事」→ 问。口径对齐 iGhostVT 的 `hasRunningProgram`
    /// （空闲提示符 = 直接关；有作业 / 未知 = 问）。
    ///
    /// 终端类：**任一**窗格要问就问（只要有一个窗格在跑，整页就有东西会丢）；
    /// 全部窗格都空闲/已退出/失败 → 直接关。
    /// SFTP 类：有在途传输 → 问；没连上（已失败）→ 直接关；还在连 → 问。
    public var closeNeedsConfirmation: Bool {
        if filePanes.contains(where: {
            if let b = $0.panel?.backend as? TransferBackend { return TransferManager.shared.hasActive(for: b) }
            return $0.remote && $0.error == nil
        }) { return true }
        if panes.isEmpty { return false }          // 无窗格：没什么可丢
        // 任一窗格在跑（或查不出）→ 整页有东西会丢。
        return panes.contains { $0.closeNeedsConfirmation }
    }

    /// 当前窗格（焦点所在；分屏关闭后自动收敛）。
    public var activePane: TerminalPane? {
        if let id = activePaneID, let p = panes.first(where: { $0.id == id }) { return p }
        return panes.first
    }

    /// tab 一句话状态（标签条 / 侧边栏角标）。取**最差**的那个：
    /// 一个窗格失败就该看得出来，不能被另一个 ready 的窗格盖住。
    public var status: PaneStatus {
        let files: [PaneStatus] = filePanes.map {
            if let e = $0.error { return .failed(e) }
            return $0.panel == nil ? .connecting : .ready
        }
        return Self.worst(panes.map(\.status) + files)
    }

    /// 这个标签页的「上一条命令」（标签条 / 侧边栏用）。
    ///
    /// 取最该注意的一个窗格：有**失败**的优先，否则取最后一个有内容的。
    public var lastCommandSummary: LastCommand? {
        let all = panes.map(\.lastCommand).filter(\.hasAnything)
        return all.first(where: \.failed) ?? all.last
    }

    /// 这个标签页该显示的程序状态摘要（标签条 / 侧边栏角标用）。
    ///
    /// 取所有窗格里「最该注意」的一条（blocked > error > working > done），
    /// 由 `ProgramStatusStore.summary` 决定；多个窗格时取优先级最高的那个。
    public var programStatusSummary: ProgramStatusStore.Record? {
        let summaries = panes.compactMap(\.programStatus.summary)
        func rank(_ s: ProgramStatusReport.State) -> Int {
            switch s {
            case .blocked: return 4
            case .error: return 3
            case .working: return 2
            case .done: return 1
            case .idle: return 0
            case .clear: return -1
            }
        }
        return summaries.max { rank($0.state) < rank($1.state) }
    }

    /// 一组窗格状态里的「最差」那个。优先级显式排定，**不靠遍历顺序**：
    /// 失败 > 连接中 > 已退出 > 就绪。抽成纯函数是为了它能被直接断言
    /// （不然验这条规则得造真 surface + 真会话，测试反而更脆）。
    public static func worst(_ statuses: [PaneStatus]) -> PaneStatus {
        if let failed = statuses.first(where: { $0.isFailed }) { return failed }
        if statuses.contains(where: { $0.isConnecting }) { return .connecting }
        if statuses.contains(.exitedUnknown) { return .exitedUnknown }
        if let exited = statuses.first(where: {
            if case .exited = $0 { return true }; return false }) { return exited }
        return .ready
    }

    /// SFTP 建立收尾（成功 / 失败都走这里，失败留原因不静默）。
    public func attachFileWorkspace(local: FilesPanelModel, remote: FilesPanelModel) {
        let l = WorkspaceFilePane(remote: false, panel: local)
        let r = WorkspaceFilePane(remote: true, panel: remote)
        workspacePanes = [.files(l), .files(r)]
        workspaceLayout = .group([.leaf(l.id), .leaf(r.id)], axis: .horizontal)
        filesPanelVisible = true; activeWorkspacePaneID = r.id
        local.start()
        remote.start()
    }

    public func markSFTPFailed(_ message: String) {
        remoteFiles?.fail(message)
    }

    // MARK: - 文件面板

    /// 文件面板建立收尾（成功）。
    public func attachFilesPanel(_ panel: FilesPanelModel) {
        if let existing = kind.isLocalShell ? localFiles : remoteFiles { existing.attach(panel) }
        else { addFilePane(WorkspaceFilePane(remote: panel.backend is TransferBackend, panel: panel)) }
    }

    public func markFilesPanelConnecting() {
        if remoteFiles == nil { addFilePane(WorkspaceFilePane(remote: true)) }
        remoteFiles?.beginConnection()
    }

    public func markFilesPanelFailed(_ message: String) {
        remoteFiles?.fail(message)
    }

    /// 有没有可用的文件面板后端（本地 shell / SSH 才开）。
    public var canHaveFilesPanel: Bool { !panes.isEmpty }

    public var subtitle: String { kind.subtitle }
    public var isSplit: Bool { (visibleWorkspaceLayout?.paneIDs.count ?? 0) > 1 }

    // MARK: - 分屏

    /// 在当前窗格处一分为二（新窗格由调用方建好并连上）。
    ///
    /// 语义：把 activePane 的叶子替换成一个 split 节点，原来的窗格与新窗格
    /// 并排。这样在**已分屏的子区域**里再分屏也能正确嵌套（递归替换）。
    public func split(axis: PaneLayout.Axis, with newPane: TerminalPane) {
        guard let node = workspaceLayout, let targetID = activePane?.id else { return }
        workspacePanes.append(.terminal(newPane))
        workspaceLayout = node.replacing(targetID, with: .group([.leaf(targetID), .leaf(newPane.id)], axis: axis))
        layoutCustomized = true
        activePaneID = newPane.id
        zoomedPaneID = nil
    }

    /// 关闭一个窗格（最后一个窗格不允许关——那是关标签页）。
    /// 返回是否真的关掉了。
    @discardableResult
    public func closePane(_ pane: TerminalPane) -> Bool {
        guard let node = workspaceLayout, workspacePanes.count > 1 else { return false }
        pane.stop()
        workspaceLayout = node.removing(pane.id, equalize: true)
        workspacePanes.removeAll { $0.id == pane.id }
        if activePaneID == pane.id { activePaneID = workspaceLayout?.paneIDs.first }
        // 关掉的正是被缩放那格，或者关完只剩一格：退出缩放。否则
        // zoomedPaneID 会指向一个已经不在布局里的窗格，视图盖出一片空白。
        if zoomedPaneID == pane.id || workspacePanes.count < 2 { zoomedPaneID = nil }
        return true
    }

    /// 被缩放的窗格对象（已不在布局里则为 nil）。
    public var zoomedPane: TerminalPane? {
        guard let id = zoomedPaneID else { return nil }
        return panes.first { $0.id == id }
    }

    /// 切换缩放（⇧⌘↩）。没分屏时**不留状态**——单格「铺满」本来就是
    /// 它现在的样子，留个 true 只会让下次分屏莫名其妙带着缩放。
    public func toggleZoom(_ pane: TerminalPane) {
        guard isSplit, panes.contains(where: { $0.id == pane.id }) else {
            zoomedPaneID = nil
            return
        }
        if zoomedPaneID == pane.id {
            zoomedPaneID = nil
        } else {
            zoomedPaneID = pane.id
            activePaneID = pane.id      // 铺满了却不是焦点，说不通
        }
    }

    /// 停止本 tab 的全部窗格（关闭标签页时调用；顺序：先停会话再置 nil）。
    public func stop() {
        for pane in panes { pane.stop() }
        workspacePanes.removeAll { $0.files != nil }
        workspaceLayout = workspaceLayout?.filtering(Set(panes.map(\.id)))
    }
}
