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
    private var hops: [Hop] = []
    private func checkOwner() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
    }
    private func pointer() throws -> OpaquePointer {
        checkOwner(); guard let raw else { throw GhostermError.failed("插件通道已释放") }; return raw
    }
    /// Exclusive pipe/socket descriptors are consumed on success only.
    public init(readFD: Int32, writeFD: Int32, owner: DispatchQueue) throws {
        self.owner = owner; checkOwner()
        let result = gt_ext_channel_open_fd(readFD, writeFD, &raw)
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "无法接管插件通道") }
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
        guard gt_ext_channel_info(try pointer(), &i), i.hops <= 8, i.diagnostic_size <= 4096 else {
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
        guard result == GT_EXT_RELEASED else { return false }
        stopLock.lock(); defer { stopLock.unlock() }
        guard gt_ext_channel_destroy(&raw) == GT_OK else { throw GhostermError.failed("插件通道尚未释放") }
        return true
    }
    /// Failed deinit cleanup stays charged in a bounded native owner pool.
    /// The native controller must expose/retry this state, not claim release.
    public static var pendingCloseCount: Int { ExtensionChannelCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { ExtensionChannelCleanup.retry(owner) }
    deinit { if let raw { ExtensionChannelCleanup.enqueue(raw, owner: owner) } }
}
private func extensionString<T>(_ value: T) -> String {
    withUnsafeBytes(of: value) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
}
private final class ExtensionChannelCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: ExtensionChannelCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue
    private var raw: OpaquePointer?
    init(_ raw: OpaquePointer, owner: DispatchQueue) { self.raw = raw; self.owner = owner }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(_ raw: OpaquePointer, owner: DispatchQueue) {
        let item = ExtensionChannelCleanup(raw, owner: owner)
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
            if result == GT_EXT_CLOSE_ERROR { return }
            if result == GT_EXT_RELEASED {
                guard gt_ext_channel_destroy(&self.raw) == GT_OK else { return }
                Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock(); return
            }
        }
    }
}
