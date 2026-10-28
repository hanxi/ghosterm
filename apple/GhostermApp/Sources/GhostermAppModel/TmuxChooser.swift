import Combine
import Foundation
import GhostermCore
import GhostermTerminal

/// In-memory connection hint, never a session ID or an automatic restore ticket.
public struct TmuxPaneContext {
    public let target: AppModel.Target
    public let options: TmuxConnection.Options
    public let selectedName: String
}

@MainActor
public final class TmuxChooser: ObservableObject, Identifiable {
    public let id = UUID()
    public let target: AppModel.Target
    public let replacingPaneID: UUID?
    @Published public var executablePath = "" { didSet { if executablePath != oldValue { invalidatePaths() } } }
    @Published public var socketPath = "" { didSet { if socketPath != oldValue { invalidatePaths() } } }
    @Published public var selectedID: String?
    @Published public private(set) var sessions: [TmuxSession] = []
    @Published public private(set) var busy = false
    @Published public private(set) var message = ""
    @Published public private(set) var requiresRefresh = true
    public var selected: TmuxSession? { sessions.first { $0.id == selectedID } }
    public var canOperate: Bool { !busy && !requiresRefresh }
    private let owner = DispatchQueue(label: "dev.ghosterm.tmux-commands")
    private final class IO { var connection: TmuxConnection? }
    private let io = IO()
    private let cancellation = SSHCancellation()
    private weak var model: AppModel?
    private let sshTarget: SSHConnectionTarget?
    private let loginShell: String?
    private var authID: UUID?
    private var options = TmuxConnection.Options()

    init(model: AppModel, target: AppModel.Target, replacing: UUID? = nil,
         options: TmuxConnection.Options = .init()) {
        self.model = model; self.target = target; replacingPaneID = replacing
        if case .host(let pattern) = target { sshTarget = model.sshTarget(for: pattern) } else { sshTarget = nil }
        loginShell = model.preferences.resolvedShell
        executablePath = options.executable ?? ""; socketPath = options.socketPath ?? ""
    }
    public var targetLabel: String {
        if let sshTarget { return "\(sshTarget.alias) · \(sshTarget.user)@\(sshTarget.host):\(sshTarget.port)" }
        return "本机"
    }
    private func invalidatePaths() {
        requiresRefresh = true; selectedID = nil; sessions = []
        message = "路径已改变，请刷新后重新选择。"
    }
    public func cancel() {
        cancellation.cancel()
        if let authID { model?.cancelConnection(authID) }
        let state = io; owner.async { state.connection = nil }
    }
    private func authenticate() async throws -> SSHSession? {
        guard let sshTarget, let model else { return nil }
        let id = UUID(); authID = id; defer { model.finishChooserAuthentication(id); authID = nil }
        return try await model.authenticate(sshTarget, id: id, owner: owner)
    }
    public func refresh() {
        guard !busy, !cancellation.isCancelled else { return }
        busy = true; requiresRefresh = true; selectedID = nil; sessions = []; message = "探测 tmux…"
        options = .init(executable: executablePath.isEmpty ? nil : executablePath,
                        socketPath: socketPath.isEmpty ? nil : socketPath, loginShell: loginShell)
        let options = options
        let state = io, cancel = cancellation, queue = owner
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let ssh = try await self.authenticate()
                queue.async { [weak self] in
                    guard !cancel.isCancelled else { return }
                    let result = Result { () -> TmuxConnection.Snapshot in
                        state.connection = nil
                        let connection = try TmuxConnection(ssh: ssh, options: options, owner: queue, cancellation: cancel)
                        state.connection = connection; return try connection.list()
                    }
                    DispatchQueue.main.async { [weak self] in self?.finishList(result) }
                }
            } catch { self.finishList(.failure(error)) }
        }
    }
    private func finishList(_ result: Result<TmuxConnection.Snapshot, Error>) {
        guard !cancellation.isCancelled else { return }; busy = false; selectedID = nil
        switch result {
        case .success(let snapshot):
            sessions = snapshot.sessions; requiresRefresh = false
            message = snapshot.serverAbsent ? "tmux 服务器未运行；可显式新建后台会话。" : sessions.isEmpty ? "当前没有后台会话。" : "\(sessions.count) 个后台会话。多客户端可能共享窗口选择、输入与尺寸。"
        case .failure(let error):
            sessions = []; requiresRefresh = true
            message = "查不出来：\(connectionErrorMessage(error))。普通终端仍可使用。"
        }
    }
    public func change(_ operation: TmuxConnection.Operation, name: String = "") {
        guard canOperate, operation == .create || selected != nil else { return }
        let selected = selected; selectedID = nil; busy = true
        let state = io, cancel = cancellation
        owner.async { [weak self] in
            guard !cancel.isCancelled, let connection = state.connection else { return }
            let result = connection.change(operation, target: selected, name: name)
            let snapshot: Result<TmuxConnection.Snapshot, Error>? = result.outcome == .confirmed ? Result { try connection.list() } : nil
            DispatchQueue.main.async { [weak self] in
                guard let self, !cancel.isCancelled else { return }
                if let snapshot { self.finishList(snapshot) }
                else {
                    self.busy = false; self.requiresRefresh = true; self.sessions = []
                    self.message = result.outcome == .unknown ? "操作结果未知；请刷新确认，不会自动重发。\(result.error ?? "")" : "操作未完成，请刷新后重新选择。\(result.error ?? "")"
                }
            }
        }
    }
    public func connectSelected() {
        guard canOperate, let selected else { return }; busy = true
        let state = io, cancel = cancellation, queue = owner
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let attachSSH = try await self.authenticate()
                queue.async { [weak self] in
                    guard !cancel.isCancelled, let connection = state.connection else { return }
                    let result = Result { try connection.attach(selected, ssh: attachSSH) }
                    DispatchQueue.main.async { [weak self] in
                        guard let self, !cancel.isCancelled else { return }; self.busy = false
                        do {
                            let transport = try result.get()
                            try self.model?.connectTmux(transport, context: .init(target: self.target, options: self.options, selectedName: selected.name), replacing: self.replacingPaneID)
                            self.model?.dismissTmux()
                        } catch {
                            self.sessions = []; self.selectedID = nil; self.requiresRefresh = true
                            self.message = "未连接：\(connectionErrorMessage(error))。请刷新后重新选择。"
                        }
                    }
                }
            } catch { self.finishList(.failure(error)) }
        }
    }

}

extension AppModel {
    public func showTmux(for target: Target, replacing pane: TerminalPane? = nil) {
        if let pane, pane.isRunning && !pane.status.isExited && !pane.status.isFailed { return }
        dismissTmux()
        let chooser = TmuxChooser(model: self, target: target, replacing: pane?.id,
                                  options: pane?.tmuxContext?.options ?? .init())
        tmuxChooser = chooser; chooser.refresh()
    }
    public func dismissTmux() { tmuxChooser?.cancel(); tmuxChooser = nil }
    func connectTmux(_ transport: Transport, context: TmuxPaneContext, replacing id: UUID?) throws {
        guard let app = terminalApp else { throw GhostermError.failed("libghostty 初始化失败") }
        let pane: TerminalPane
        if let id {
            guard let existing = tabs.tabs.flatMap(\.panes).first(where: { $0.id == id }),
                  existing.tmuxContext != nil, existing.status.isExited || existing.status.isFailed else {
                throw GhostermError.failed("原格子已关闭或已建立连接；请从新列表打开新格子")
            }
            pane = existing
        } else { pane = TerminalPane() }
        let surface = try TerminalSurface(app: app)
        let session = SessionController(transport: transport, surface: surface)
        pane.stop(); pane.tmuxContext = context; pane.onExit = nil
        pane.attach(surface: surface, session: session); pane.start()
        if id == nil {
            let kind: TabKind
            switch context.target {
            case .localShell: kind = .localShell
            case .host(let pattern):
                let target = sshTarget(for: pattern)
                kind = .ssh(.init(pattern: pattern, user: target.user, host: target.host, port: target.port))
            }
            let tab = TerminalTab(kind: kind, pane: pane); tab.title = "tmux · \(context.selectedName)"; tabs.add(tab)
        }
    }
}
