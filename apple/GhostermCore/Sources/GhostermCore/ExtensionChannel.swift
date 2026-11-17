import Cghosterm
import Foundation

/// Native transport only. All IO belongs to the supplied non-main serial queue;
/// raw protocol bytes must never be posted as UI events. Plugin consent and
/// framed Session traffic are a later layer, independent of this provider.
public final class ExtensionChannel {
    public enum Phase: UInt32 { case connecting, ready, failed, stopped, closed }
    public enum Stage: UInt32 { case none, authentication, forward, exec, stream }
    public enum IOState: UInt32 { case data, wait, eof, closed, error }
    public struct IO { public let state: IOState, count: Int, error: Int32 }
    public struct ProcessConfiguration {
        public let executable: String, arguments: [String], environment: [String], directoryFD: Int32
        public let executionAuthorized: Bool, lifetimeMilliseconds: UInt32, stopGraceMilliseconds: UInt32
        public let stdoutLimit: UInt64, stderrLimit: UInt64
        public init(executable: String, arguments: [String], directoryFD: Int32, environment: [String] = [],
                    executionAuthorized: Bool, lifetimeMilliseconds: UInt32 = 0, stopGraceMilliseconds: UInt32 = 0,
                    stdoutLimit: UInt64 = 0, stderrLimit: UInt64 = 0) {
            self.executable = executable; self.arguments = arguments; self.directoryFD = directoryFD
            self.environment = environment; self.executionAuthorized = executionAuthorized
            self.lifetimeMilliseconds = lifetimeMilliseconds; self.stopGraceMilliseconds = stopGraceMilliseconds
            self.stdoutLimit = stdoutLimit; self.stderrLimit = stderrLimit
        }
    }
    public enum ProcessReason: UInt32 { case none, deadline, stdoutLimit, stderrLimit }
    public struct ProcessObservation {
        public let error: Int32, cleanupError: Int32, groupSignalError: Int32, reason: ProcessReason
        public let childExited: Bool, reaped: Bool, groupFenced: Bool, stdoutEOF: Bool, stderrEOF: Bool, stopping: Bool
        public let exitCode: Int32?, termSignal: Int32?, stdoutBytes: UInt64, stderrBytes: UInt64
        public let diagnosticTruncated: Bool, diagnostic: Data
        internal init(_ i: gt_ext_transport_process_info_t) throws {
            guard i.diagnostic_size <= 4096, let reason = ProcessReason(rawValue: i.reason.rawValue) else {
                throw GhostermError.failed("本机进程观察无效")
            }
            error = Int32(i.error.rawValue); cleanupError = Int32(i.cleanup_error.rawValue)
            groupSignalError = i.group_signal_error; self.reason = reason
            childExited = i.child_exited; reaped = i.reaped; groupFenced = i.group_fenced
            stdoutEOF = i.stdout_eof; stderrEOF = i.stderr_eof; stopping = i.stopping
            exitCode = i.child_exited && i.term_signal == 0 ? i.exit_code : nil
            termSignal = i.child_exited && i.term_signal != 0 ? i.term_signal : nil
            stdoutBytes = i.stdout_bytes; stderrBytes = i.stderr_bytes
            diagnosticTruncated = i.diagnostic_truncated
            diagnostic = withUnsafeBytes(of: i.diagnostic) { Data($0.prefix(Int(i.diagnostic_size))) }
        }
    }
    public struct Hop {
        public let options: SSHOptions, address: SSHAddress?, viaHost: String?, timeoutMilliseconds: UInt32
        public init(options: SSHOptions, address: SSHAddress? = nil, viaHost: String? = nil,
                    timeoutMilliseconds: UInt32 = 60000) {
            self.options = options; self.address = address; self.viaHost = viaHost
            self.timeoutMilliseconds = timeoutMilliseconds
        }
    }
    public struct Info {
        public let phase: Phase, stage: Stage, failureStage: Stage, error: Int32, cleanupError: Int32
        public let hop: UInt32, challenge: UInt64, setupExpired: Bool
        public let authentication: [SSHAuthenticator.Info], decisions: [HostKeyDecision]
        public let accepted: UInt64, sent: UInt64, received: UInt64, stderrBytes: UInt64, buffered: UInt32
        public let remoteEOF: Bool, exitStatus: Int32?, diagnosticTruncated: Bool, diagnostic: Data
    }
    private let owner: DispatchQueue
    private let stopLock = NSLock()
    private var raw: OpaquePointer?
    internal private(set) var multiplexOwner: ExtensionMultiplexer?
    private var finalProcessObservation: ProcessObservation?
    private var hops: [Hop] = []
    private func checkOwner() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
    }
    private func pointer() throws -> OpaquePointer {
        checkOwner(); guard let raw else { throw GhostermError.failed("插件通道已释放") }; return raw
    }
    // Transactional transfer: exclude every cross-thread stop while C changes
    // ownership. On failure raw is unchanged; on success this wrapper is inert.
    var sessionOwner: DispatchQueue { checkOwner(); return owner }
    func adoptMultiplexer(config: inout gt_ext_mux_config_t) throws -> OpaquePointer {
        _ = try pointer(); stopLock.lock(); defer { stopLock.unlock() }
        var value: OpaquePointer?
        let status = gt_ext_mux_open(&raw, &config, &value)
        guard status == GT_OK, let value else { throw GhostermError.status(Int32(status.rawValue), "无法接管多实例物理通道") }
        return value
    }
    internal init(adopting raw: OpaquePointer, owner: DispatchQueue, multiplex: ExtensionMultiplexer) {
        self.owner = owner; self.raw = raw; self.multiplexOwner = multiplex; checkOwner()
    }
    func adoptSession(role: gt_ext_role_t, config: inout gt_ext_session_config_t, methods: [gt_ext_rpc_method_t]? = nil, store: OpaquePointer? = nil, scopeBytes: UInt64 = 0) throws -> OpaquePointer {
        _ = try pointer(); stopLock.lock(); defer { stopLock.unlock() }
        var session: OpaquePointer?
        let result: gt_status_t
        if let store {
            result = (methods ?? []).withUnsafeBufferPointer { gt_ext_session_open_resources(&raw, role, &config, $0.baseAddress, $0.count, store, scopeBytes, &session) }
        } else if let methods {
            result = methods.withUnsafeBufferPointer { gt_ext_session_open_rpc(&raw, role, &config, $0.baseAddress, $0.count, &session) }
        } else { result = gt_ext_session_open(&raw, role, &config, &session) }
        guard result == GT_OK, let session else { throw GhostermError.status(Int32(result.rawValue), "无法接管插件会话") }
        return session
    }
    /// Exclusive pipe/socket descriptors are consumed on success only.
    public init(readFD: Int32, writeFD: Int32, owner: DispatchQueue) throws {
        self.owner = owner; checkOwner()
        let result = gt_ext_channel_open_fd(readFD, writeFD, &raw)
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "无法接管插件通道") }
    }
    /// Trusted Native launch only. The explicit directory fd stays with caller;
    /// no shell/PATH/PTY, inherited environment or implicit plugin consent.
    public init(process: ProcessConfiguration, owner: DispatchQueue) throws {
        self.owner = owner; checkOwner()
        guard process.executable.hasPrefix("/"), (1...64).contains(process.arguments.count),
              process.environment.count <= 32 else { throw GhostermError.failed("本机进程参数无效") }
        var buffers: [(UnsafeMutablePointer<CChar>, Int)] = []
        defer { for (p, size) in buffers { p.initialize(repeating: 0, count: size); p.deallocate() } }
        func copy(_ text: String) throws -> UnsafePointer<CChar>? {
            guard !text.utf8.contains(0), text.utf8.count <= 4096 else { throw GhostermError.failed("本机进程参数无效或超限") }
            let bytes = Array(text.utf8CString), p = UnsafeMutablePointer<CChar>.allocate(capacity: bytes.count)
            p.initialize(from: bytes, count: bytes.count); buffers.append((p, bytes.count)); return UnsafePointer(p)
        }
        let executable = try copy(process.executable)
        let arguments = try process.arguments.map(copy), environment = try process.environment.map(copy)
        var config = gt_ext_process_channel_config_t()
        config.executable = executable; config.cwd_fd = process.directoryFD
        config.argument_count = arguments.count; config.environment_count = environment.count
        config.execution_authorized = process.executionAuthorized
        config.lifetime_ms = process.lifetimeMilliseconds; config.stop_grace_ms = process.stopGraceMilliseconds
        config.stdout_limit = process.stdoutLimit; config.stderr_limit = process.stderrLimit
        let status = arguments.withUnsafeBufferPointer { argv in
            environment.withUnsafeBufferPointer { env in
                config.arguments = argv.baseAddress; config.environment = env.baseAddress
                return gt_ext_channel_open_process(&config, &raw)
            }
        }
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "无法启动本机插件通道") }
    }
    /// Snapshot only. The final observation remains available after closeStep
    /// destroys the Core owner; adoption requires querying the new owner.
    public func processObservation() throws -> ProcessObservation {
        checkOwner()
        if raw == nil, let finalProcessObservation { return finalProcessObservation }
        var i = gt_ext_transport_process_info_t()
        let status = gt_ext_channel_process_info(try pointer(), &i)
        guard status == GT_OK else { throw GhostermError.status(Int32(status.rawValue), "无法观察本机插件通道") }
        return try ProcessObservation(i)
    }
    /// Credentials are requested after each host has been verified. No material
    /// is copied from an existing terminal/SFTP owner; no DNS occurs here.
    public init(hops: [Hop], bootstrap: String, setupMilliseconds: UInt32 = 30000,
                admissionMilliseconds: UInt32 = 5000, stderrLimit: UInt32 = 262144,
                owner: DispatchQueue) throws {
        self.owner = owner; self.hops = hops; checkOwner()
        guard (1...8).contains(hops.count), !bootstrap.utf8.contains(0),
              hops.allSatisfy({ !($0.viaHost?.utf8.contains(0) ?? false) }),
              hops[0].address?.bytes.count == 16, hops[0].viaHost == nil,
              hops.dropFirst().allSatisfy({ $0.address == nil && !($0.viaHost?.isEmpty ?? true) }) else {
            throw GhostermError.failed("插件跳板路线参数无效")
        }
        var materials: [OpaquePointer?] = []; var configs: [gt_ext_ssh_hop_t] = []
        defer { materials.forEach { gt_ssh_close($0) } }
        for hop in hops {
            materials.append(try SSHAuthenticator.unlockedSSH(options: hop.options, credential: nil))
            var c = gt_ext_ssh_hop_t(); c.authentication.timeout_ms = hop.timeoutMilliseconds
            c.authentication.defer_credentials = true
            if let address = hop.address {
                c.authentication.family = address.family; c.authentication.scope = address.scope
                withUnsafeMutableBytes(of: &c.authentication.address) { $0.copyBytes(from: address.bytes) }
            }
            configs.append(c)
        }
        var options = gt_ext_ssh_options_t(setup_ms: setupMilliseconds, admission_ms: admissionMilliseconds,
                                           stderr_limit: stderrLimit)
        func withRoutes(_ index: Int, _ body: () -> gt_status_t) -> gt_status_t {
            guard index < hops.count else { return body() }
            if let via = hops[index].viaHost {
                return via.withCString { configs[index].via_host = $0; return withRoutes(index + 1, body) }
            }
            return withRoutes(index + 1, body)
        }
        let result = withRoutes(0) {
            bootstrap.withCString { command in
                gt_ext_channel_open_ssh(&materials, &configs, configs.count, command, &options, &raw)
            }
        }
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "无法启动插件 SSH 路线") }
    }
    public func step(milliseconds: Int32 = 0) throws -> Phase {
        guard milliseconds >= 0 else { throw GhostermError.failed("插件通道等待预算无效") }
        return Phase(rawValue: gt_ext_channel_step(try pointer(), milliseconds).rawValue) ?? .failed
    }
    public func info() throws -> Info {
        var i = gt_ext_channel_info_t()
        guard gt_ext_channel_info(try pointer(), &i) else { throw GhostermError.failed("插件通道状态不可读") }
        return try Self.decodeInfo(i)
    }
    internal static func decodeInfo(_ i: gt_ext_channel_info_t) throws -> Info {
        guard i.hops <= 8, i.diagnostic_size <= 4096 else {
            throw GhostermError.failed("插件通道状态无效")
        }
        let auth = withUnsafeBytes(of: i.authentication) { buffer in
            buffer.bindMemory(to: gt_ssh_auth_info_t.self).prefix(Int(i.hops)).map { a in
                SSHAuthenticator.Info(phase: SSHAuthenticator.Phase(rawValue: a.phase.rawValue) ?? .failed,
                    host: SSHAuthenticator.Host(rawValue: a.host.rawValue) ?? .unchecked,
                    error: Int32(a.error.rawValue), deadline: a.deadline, methods: a.auth_methods,
                    keyType: extensionString(a.key_type), fingerprint: extensionString(a.fingerprint))
            }
        }
        let decisions = withUnsafeBytes(of: i.decisions) { buffer in
            buffer.bindMemory(to: gt_hostkey_decision_t.self).prefix(Int(i.hops)).map {
                HostKeyDecision(rawValue: $0.rawValue) ?? .unknown
            }
        }
        let diagnostic = withUnsafeBytes(of: i.diagnostic) { Data($0.prefix(Int(i.diagnostic_size))) }
        return Info(phase: Phase(rawValue: i.phase.rawValue) ?? .failed,
                    stage: Stage(rawValue: i.stage.rawValue) ?? .none,
                    failureStage: Stage(rawValue: i.failure_stage.rawValue) ?? .none, error: Int32(i.error.rawValue), cleanupError: Int32(i.cleanup_error.rawValue),
                    hop: i.hop, challenge: i.challenge, setupExpired: i.setup_expired,
                    authentication: auth, decisions: decisions, accepted: i.accepted, sent: i.sent,
                    received: i.received, stderrBytes: i.stderr_bytes, buffered: i.buffered,
                    remoteEOF: i.remote_eof, exitStatus: i.exit_status_known ? i.exit_status : nil,
                    diagnosticTruncated: i.diagnostic_truncated, diagnostic: diagnostic)
    }
    public func decide(hop: UInt32, challenge: UInt64, decision: HostKeyDecision) throws -> Int32 {
        Int32(gt_ext_channel_decide(try pointer(), hop, challenge,
                                  gt_hostkey_decision_t(rawValue: decision.rawValue)).rawValue)
    }
    public func supply(hop: UInt32, challenge: UInt64, credential: SSHCredential) throws -> Int32 {
        let p = try pointer(); guard Int(hop) < hops.count else { throw GhostermError.failed("插件跳板下标无效") }
        var material: OpaquePointer? = try SSHAuthenticator.unlockedSSH(options: hops[Int(hop)].options, credential: credential)
        defer { gt_ssh_close(material) }
        return Int32(gt_ext_channel_supply(p, hop, challenge, &material).rawValue)
    }
    public func write(_ data: Data) throws -> IO {
        let p = try pointer()
        let value = data.withUnsafeBytes { gt_ext_channel_write(p, $0.baseAddress, $0.count) }
        return IO(state: IOState(rawValue: value.state.rawValue) ?? .error, count: value.count,
                  error: Int32(value.error.rawValue))
    }
    public func read(capacity: Int = 16384, milliseconds: Int32 = 0) throws -> (IO, Data) {
        let p = try pointer(); guard capacity > 0, capacity <= 16384, milliseconds >= 0 else {
            throw GhostermError.failed("插件通道读取参数无效")
        }
        var data = Data(count: capacity)
        let value = data.withUnsafeMutableBytes { gt_ext_channel_read(p, $0.baseAddress, $0.count, milliseconds) }
        data.count = value.count
        return (IO(state: IOState(rawValue: value.state.rawValue) ?? .error, count: value.count,
                   error: Int32(value.error.rawValue)), data)
    }
    /// Thread safe; resources remain charged until the IO owner closes them.
    public func stop() { stopLock.lock(); defer { stopLock.unlock() }; if let raw { gt_ext_channel_stop(raw) } }
    @discardableResult public func closeStep(milliseconds: Int32 = 0) throws -> Bool {
        let p = try pointer(); guard milliseconds >= 0 else { throw GhostermError.failed("关闭预算无效") }
        let result = gt_ext_channel_close(p, milliseconds)
        if result == GT_EXT_CLOSE_ERROR { throw GhostermError.failed("插件通道释放失败，仍保留所有权") }
        guard result == GT_EXT_RELEASED else { _ = try multiplexOwner?.step(); return false }
        var observation = gt_ext_transport_process_info_t()
        if gt_ext_channel_process_info(p, &observation) == GT_OK { finalProcessObservation = try ProcessObservation(observation) }
        stopLock.lock(); defer { stopLock.unlock() }
        guard gt_ext_channel_destroy(&raw) == GT_OK else { throw GhostermError.failed("插件通道尚未释放") }
        return true
    }
    /// Failed deinit cleanup stays charged in a bounded native owner pool.
    /// The native controller must expose/retry this state, not claim release.
    public static var pendingCloseCount: Int { ExtensionChannelCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { ExtensionChannelCleanup.retry(owner) }
    deinit { if let raw { ExtensionChannelCleanup.enqueue(raw, owner: owner, multiplex: multiplexOwner) } }
}
private func extensionString<T>(_ value: T) -> String {
    withUnsafeBytes(of: value) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
}
private final class ExtensionChannelCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: ExtensionChannelCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue
    private var raw: OpaquePointer?
    private let multiplex: ExtensionMultiplexer?
    init(_ raw: OpaquePointer, owner: DispatchQueue, multiplex: ExtensionMultiplexer?) { self.raw = raw; self.owner = owner; self.multiplex = multiplex }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(_ raw: OpaquePointer, owner: DispatchQueue, multiplex: ExtensionMultiplexer?) {
        let item = ExtensionChannelCleanup(raw, owner: owner, multiplex: multiplex)
        lock.lock(); entries[item.id] = item; lock.unlock()
        owner.async { item.run() }
    }
    static func retry(_ owner: DispatchQueue) {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        lock.lock(); let values = entries.values.filter { $0.owner === owner }; lock.unlock()
        values.forEach { $0.run() }
    }
    private func run() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { return }
        gt_ext_channel_stop(raw)
        // Finite local work; DRAINING/error retains the entry for explicit retry.
        for _ in 0..<32 {
            let result = gt_ext_channel_close(raw, 0)
            if result == GT_EXT_DRAINING { _ = try? multiplex?.step() }
            if result == GT_EXT_CLOSE_ERROR { return }
            if result == GT_EXT_RELEASED {
                guard gt_ext_channel_destroy(&self.raw) == GT_OK else { return }
                Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock(); return
            }
        }
    }
}
