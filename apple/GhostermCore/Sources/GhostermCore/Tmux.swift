import Foundation
import Cghosterm

private func tmuxCancelled(_ context: UnsafeMutableRawPointer?) -> Bool {
    guard let context else { return false }
    return Unmanaged<SSHCancellation>.fromOpaque(context).takeUnretainedValue().isCancelled
}

public struct TmuxSession: Identifiable, Equatable {
    fileprivate var native: gt_tmux_session_t
    public var id: String { "\(native.server_pid)/\(native.server_started)/\(native.id)/\(native.created)/\(socketPath)" }
    public var name: String { var value = native.name; return withUnsafeBytes(of: &value) { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) } }
    public var socketPath: String { var value = native.socket_path; return withUnsafeBytes(of: &value) { String(cString: $0.baseAddress!.assumingMemoryBound(to: CChar.self)) } }
    public var windows: UInt32 { native.windows }
    public var attachedClients: UInt32 { native.attached }
    public var retentionWarning: String {
        if native.destroy_unattached < 0 || native.exit_unattached < 0 { return "保留策略查不清；断开后任务可能结束。" }
        if native.destroy_unattached != 0 || native.exit_unattached != 0 { return "tmux 设置了 destroy-unattached 或 exit-unattached，最后一个客户端断开时任务可能结束。" }
        return "当前 tmux 设置允许无人连接时继续运行；hooks、登出清理、重启仍可能结束任务。"
    }
    public static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.id == rhs.id && lhs.name == rhs.name && lhs.native.destroy_unattached == rhs.native.destroy_unattached && lhs.native.exit_unattached == rhs.native.exit_unattached
    }
}

/// A short-lived live-list connection. All methods run on its serial IO owner.
/// No on-disk catalog; a fresh list is required whenever the chooser reopens.
public final class TmuxConnection {
    public struct Options {
        public var executable: String?, socketPath: String?, loginShell: String?
        public var timeoutMilliseconds: UInt32
        public init(executable: String? = nil, socketPath: String? = nil, loginShell: String? = nil, timeoutMilliseconds: UInt32 = 5000) {
            self.executable = executable; self.socketPath = socketPath; self.loginShell = loginShell; self.timeoutMilliseconds = timeoutMilliseconds
        }
    }
    public struct Snapshot { public let sessions: [TmuxSession]; public let serverAbsent: Bool }
    public enum Operation { case create, rename, terminate }
    public enum Outcome { case confirmed, rejected, unknown }
    public struct Change { public let outcome: Outcome; public let error: String? }
    private let handle: OpaquePointer
    private let ssh: SSHSession?
    private let owner: DispatchQueue
    private let cancellation: SSHCancellation
    public init(ssh: SSHSession? = nil, options: Options = .init(), owner: DispatchQueue, cancellation: SSHCancellation) throws {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        for value in [options.executable, options.socketPath, options.loginShell].compactMap({ $0 }) {
            guard !value.utf8.contains(0) else { throw GhostermError.failed("tmux 路径包含 NUL") }
        }
        self.owner = owner; self.ssh = ssh; self.cancellation = cancellation
        var opts = gt_tmux_opts_t(); opts.timeout_ms = options.timeoutMilliseconds
        opts.cancelled = tmuxCancelled; opts.context = Unmanaged.passUnretained(cancellation).toOpaque()
        var result: OpaquePointer?
        options.executable.withCStringOrNil { exe in options.socketPath.withCStringOrNil { socket in options.loginShell.withCStringOrNil { shell in
            opts.executable = exe; opts.socket_path = socket; opts.login_shell = shell
            result = gt_tmux_open(ssh?.handle, &opts)
        } } }
        guard let result else { throw GhostermError.failed(Ghosterm.lastError) }; handle = result
    }
    deinit {
        gt_tmux_close(handle)
        if let ssh { owner.async { withExtendedLifetime(ssh) {} } }
    }
    public func list() throws -> Snapshot {
        dispatchPrecondition(condition: .onQueue(owner))
        let rows = UnsafeMutablePointer<gt_tmux_session_t>.allocate(capacity: Int(GT_TMUX_SESSION_MAX)); defer { rows.deallocate() }
        var count = 0, server = GT_TMUX_SERVER_RUNNING
        let code = gt_tmux_list(handle, rows, Int(GT_TMUX_SESSION_MAX), &count, &server)
        guard code == 0 else { throw GhostermError.status(Int32(code), Ghosterm.lastError) }
        return Snapshot(sessions: (0..<count).map { TmuxSession(native: rows[$0]) }, serverAbsent: server == GT_TMUX_SERVER_ABSENT)
    }
    public func change(_ operation: Operation, target: TmuxSession? = nil, name: String = "") -> Change {
        dispatchPrecondition(condition: .onQueue(owner))
        guard !name.utf8.contains(0) else { return Change(outcome: .rejected, error: "名称包含 NUL") }
        let op = operation == .create ? GT_TMUX_CREATE : operation == .rename ? GT_TMUX_RENAME : GT_TMUX_TERMINATE
        var native = target?.native ?? gt_tmux_session_t(), result = GT_TMUX_UNKNOWN
        let code = name.withCString { value in
            withUnsafePointer(to: &native) { gt_tmux_change(handle, op, target == nil ? nil : $0, value, &result) }
        }
        return Change(outcome: result == GT_TMUX_CONFIRMED ? .confirmed : result == GT_TMUX_REJECTED ? .rejected : .unknown,
                      error: code == 0 ? nil : Ghosterm.lastError)
    }
    public func attach(_ session: TmuxSession, ssh attachSSH: SSHSession? = nil, cols: UInt16 = 80, rows: UInt16 = 24) throws -> Transport {
        dispatchPrecondition(condition: .onQueue(owner))
        var selected = session.native, size = gt_shell_opts_t(); size.cols = cols; size.rows = rows
        let raw = "xterm-256color".withCString { term -> OpaquePointer? in
            size.term = term; return gt_tmux_attach(handle, &selected, attachSSH?.handle, &size)
        }
        guard let raw else { throw GhostermError.failed(Ghosterm.lastError) }
        let result = Transport(handle: raw, sshOwner: attachSSH); result.releaseOn(owner); return result
    }
}
