// SessionController — M1 竖切的粘合剂。
//
// 它把 transport（SSH 或本地 PTY，来自 GhostermCore）与 terminal
// （libghostty surface，来自 GhostermTerminal）接起来，并**只做接线**：
//
//   session 线程：transport.read → surface.feed       （字节流直连）
//   终端输入：    surface.onInput → transport.write
//   尺寸变化：    surface.onResize → transport.resize
//
// 铁律 #1/#2：surface 不知道背后是 SSH 还是本地 shell（两个分支只在
// attach 时选一次）；字节流不进事件队列、不过 UI 线程——read 循环跑在
// 专用会话线程上，读到的字节直接交给 libghostty 的网格。
import Foundation
import GhostermCore
import GhostermTerminal

/// 会话状态。`unknown` 与 `exited` 是**不同**的东西（三态不得折叠）。
public enum SessionState: Equatable {
    case idle
    case running
    case exited(code: Int32)
    case exitedUnknown
    case failed(String)
}

/// 一条会话。持有 transport 与 surface，并驱动 read 循环。
public final class SessionController {
    private let stateLock = NSLock()
    private var _state: SessionState = .idle

    /// 会话状态。可在任意线程读取（值本身加锁）；**变化通知**总在主线程发出。
    public var state: SessionState {
        stateLock.lock(); defer { stateLock.unlock() }
        return _state
    }
    /// 状态变化（低频）→ 投给 UI。终端字节流**不**走这里。
    public var onStateChange: ((SessionState) -> Void)?

    private func setState(_ s: SessionState) {
        stateLock.lock()
        let changed = (_state != s)
        _state = s
        stateLock.unlock()
        guard changed else { return }
        // 通知走主线程（UI 约定），但**状态值本身**是同步写好的——
        // 否则测试/后台线程在阻塞等待时会永远读不到终态。
        if Thread.isMainThread {
            onStateChange?(s)
        } else {
            DispatchQueue.main.async { [weak self] in self?.onStateChange?(s) }
        }
    }

    let transport: Transport
    let surface: TerminalSurface

    /// 前台作业判定（关闭确认用）。直读 transport 的内核查询——**即时**、
    /// 不缓存，故没有「报的是旧值」的问题。SSH / ConPTY 恒 `.unknown`。
    public var childState: Transport.ChildState { transport.childState }

    /// 子进程的工作目录（本地 PTY 才有；SSH 返回 nil）。内核查询，随取随新。
    public var childCwd: String? { transport.childCwd }
    private let queue = DispatchQueue(label: "dev.ghosterm.session")
    private var stopped = false
    private var _running = false
    private var running: Bool {
        get { stateLock.lock(); defer { stateLock.unlock() }; return _running }
        set { stateLock.lock(); _running = newValue; stateLock.unlock() }
    }

    /// core 的契约是 read/write/resize 由**同一会话线程**独占调用（libssh2 的
    /// session 非线程安全）。但 libghostty 会在它自己的 termio 线程上回调
    /// onInput / onResize，所以我们**不能**在那条线程上直接写 transport——
    /// 那样既破坏单线程契约，又会在读循环持锁阻塞时死锁（实测：resize 回调
    /// 发出后永远不返回，libghostty 的 termio 线程被卡住）。
    ///
    /// 做法：回调只把操作压进待办队列，由会话线程在下一次读循环里排空执行。
    private enum Pending {
        case write([UInt8])
        case resize(cols: UInt16, rows: UInt16)
    }
    private let pendingLock = NSLock()
    private var pending: [Pending] = []

    private func enqueue(_ op: Pending) {
        guard !isStopped else { return }
        pendingLock.lock()
        pending.append(op)
        pendingLock.unlock()
    }

    private func drainPending() throws {
        pendingLock.lock()
        let ops = pending
        pending.removeAll(keepingCapacity: true)
        pendingLock.unlock()
        for op in ops {
            guard running else { return }
            switch op {
            case .write(let bytes):
                try transport.write(bytes)
            case .resize(let cols, let rows):
                try transport.resize(cols: cols, rows: rows)
            }
        }
    }

    /// 本地 PTY 会话（`gt_transport_open_pty`）。
    ///
    /// `kind` 无默认值，跟着 `Transport.openPTY` 一起要调用点表态：起交互
    /// shell（`.shell`，macOS 上才有登录语义、PATH 才全）还是跑一条命令。
    public static func localShell(
        surface: TerminalSurface,
        executable: String? = nil,
        argv: [String] = [],
        cwd: String? = nil,
        kind: Transport.PTYCommandKind
    ) throws -> SessionController {
        let t = try Transport.openPTY(
            executable: executable, argv: argv, cwd: cwd,
            term: "xterm-256color", kind: kind,
            cols: surface.gridSize.cols, rows: surface.gridSize.rows
        )
        return SessionController(transport: t, surface: surface)
    }

    /// 持有构建 transport 的 SSH 会话。
    ///
    /// **不是可有可无**：`gt_transport_open_ssh_shell` 只**借用** gt_ssh_t
    /// （C 侧不拥有）；libssh2 session 的生命周期由这里的 SSHSession 对象
    /// 撑住。谁建会话谁持有——否则会话建立后 SSHSession 释放 →
    /// gt_ssh_close → libssh2_session_free，而 transport 还在用同一 session
    /// 读 shell 通道（实测：读循环里 use-after-free，直接 SIGSEGV）。
    private let sshSession: SSHSession?

    /// SSH 交互式 shell 会话（`gt_transport_open_ssh_shell`）。
    public static func sshShell(session: SSHSession, surface: TerminalSurface) throws
        -> SessionController {
        let t = try Transport.openSSHShell(
            session, term: "xterm-256color",
            cols: surface.gridSize.cols, rows: surface.gridSize.rows
        )
        return SessionController(transport: t, surface: surface, sshSession: session)
    }

    public init(transport: Transport, surface: TerminalSurface,
                sshSession: SSHSession? = nil) {
        self.transport = transport
        self.surface = surface
        self.sshSession = sshSession
        transport.releaseOn(queue)
        wireSurfaceToTransport(surface)
    }

    deinit { stop() }

    // MARK: - surface → transport

    private func wireSurfaceToTransport(_ surface: TerminalSurface) {
        surface.onInput = { [weak self] data in
            self?.enqueue(.write([UInt8](data)))
        }
        surface.onResize = { [weak self] cols, rows in
            self?.enqueue(.resize(cols: cols, rows: rows))
        }
    }

    // MARK: - transport → surface

    /// 起会话线程。read 循环在专用线程上，字节直连 surface（不过 UI 线程）。
    public func start() {
        stateLock.lock()
        guard !_running, !stopped else { stateLock.unlock(); return }
        _running = true
        stateLock.unlock()
        setState(.running)
        queue.async { [weak self] in self?.readLoop() }
    }

    private func readLoop() {
        var exitCode: Int32 = 0
        while running {
            // 先排空输入/尺寸待办：这些是 UI/termio 线程转交过来的，
            // 在这里执行才满足「transport 由会话线程独占」的契约。
            let result: Transport.ReadResult
            do {
                try drainPending()
                result = try transport.read(timeoutMilliseconds: 20)
            } catch {
                finish(.failed(Ghosterm.lastError))
                return
            }
            switch result {
            case .data(let bytes):
                surface.feed(Data(bytes))
            case .timeout:
                continue
            case .eof:
                let exited = transport.exited(exitCode: &exitCode)
                finish(exited ? .exited(code: exitCode) : .exitedUnknown)
                return
            }
        }
    }

    private func finish(_ s: SessionState) {
        running = false
        setState(s)
    }

    private var isStopped: Bool {
        stateLock.lock(); defer { stateLock.unlock() }; return stopped
    }
    /// Cancel synchronously; the IO invocation retains self until its bounded
    /// read/write returns. Transport cleanup then runs on the same IO owner.
    public func stop() {
        stateLock.lock(); stopped = true; _running = false; stateLock.unlock()
        pendingLock.lock(); pending.removeAll(); pendingLock.unlock()
    }

    /// 等待会话结束（测试用；返回最终状态）。
    @discardableResult
    public func waitUntilExit(timeout: TimeInterval = 10) -> SessionState {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            switch state {
            case .exited, .exitedUnknown, .failed: return state
            default: Thread.sleep(forTimeInterval: 0.02)
            }
        }
        return state
    }
}
