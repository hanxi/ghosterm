import Cghosterm
import Foundation

/// Trusted native handshake/consent worker. A lease is available only after
/// both READY messages. JSON requests stay on this owner; App views follow later.
public final class ExtensionSession {
    public enum Role: UInt32 { case host, executor }
    public enum PeerKind: UInt32 { case local, ssh }
    public enum Decision: UInt32 { case unknown, allow, deny }
    public enum Access: UInt32 { case unknown, allowed, denied, revoked, unsupported }
    public enum Phase: UInt32 {
        case sending, waitHello, waitOffer, consent, waitDecision, waitAccept
        case waitReady, waitAck, active, ending, ended, closed
    }
    public enum End: UInt32 { case none, denied, revoked, expired, incompatible, `protocol`, lost, peer, local, `internal` }
    public enum JSONShape: UInt32 { case any, object, array, string, boolean, number, null }
    public enum RPCStatus: UInt32 { case ok, wait, invalid, denied, unsupported, closed }
    public enum RPCState: UInt32 { case queued, pending, succeeded, failed, cancelled, unknown }
    public struct Method {
        public let name: String, capability: String, receiver: Role, inputShape: JSONShape, outputShape: JSONShape
        public init(name: String, capability: String, receiver: Role, inputShape: JSONShape = .any, outputShape: JSONShape = .any) {
            self.name = name; self.capability = capability; self.receiver = receiver; self.inputShape = inputShape; self.outputShape = outputShape
        }
    }
    public struct RPCAdmission { public let status: RPCStatus, handle: UInt64? }
    public struct RPCResult {
        public let handle: UInt64, requestID: String, state: RPCState, cancelRequested: Bool, deadlineElapsed: Bool, confirmed: Bool, outcome: Data
    }
    public struct RPCRequest { public let ticket: UInt64, requestID: String, method: String, input: Data }
    public struct Budgets {
        public let fragmentBytes: UInt32, messageBytes: UInt32, resourceBytes: UInt32
        public init(fragmentBytes: UInt32 = 768, messageBytes: UInt32 = 65536, resourceBytes: UInt32 = 262144) {
            self.fragmentBytes = fragmentBytes; self.messageBytes = messageBytes; self.resourceBytes = resourceBytes
        }
    }
    public struct Config {
        public let pluginID: String, pluginVersion: String, clientDigest: String, executorDigest: String
        public let hostVersion: String, sdkVersion: String, required: [String], available: [String]
        public let peerKind: PeerKind, peerIdentity: String, budgets: Budgets
        public let handshakeMilliseconds: UInt32, consentMilliseconds: UInt32
        public let methods: [Method]?
        public init(pluginID: String, pluginVersion: String, clientDigest: String, executorDigest: String,
                    hostVersion: String, sdkVersion: String = "1.0.0", required: [String], available: [String],
                    peerKind: PeerKind, peerIdentity: String, budgets: Budgets = Budgets(),
                    handshakeMilliseconds: UInt32 = 5000, consentMilliseconds: UInt32 = 60000, methods: [Method]? = nil) {
            self.pluginID = pluginID; self.pluginVersion = pluginVersion; self.clientDigest = clientDigest
            self.executorDigest = executorDigest; self.hostVersion = hostVersion; self.sdkVersion = sdkVersion
            self.required = required; self.available = available; self.peerKind = peerKind; self.peerIdentity = peerIdentity
            self.budgets = budgets; self.handshakeMilliseconds = handshakeMilliseconds; self.consentMilliseconds = consentMilliseconds
            self.methods = methods
        }
    }
    public struct Info {
        public let phase: Phase, end: End, decision: Decision, error: Int32, cleanupError: Int32
        public let connection: String, instance: String, consentToken: String, peerHostVersion: String
        public let epoch: UInt64, consentDeadline: UInt64, received: UInt64, sent: UInt64
        public let budgets: Budgets, peerKind: PeerKind, peerIdentity: String
    }
    public struct Lease { fileprivate let raw: gt_ext_lease_t }
    private let owner: DispatchQueue, stopLock = NSLock()
    private var raw: OpaquePointer?
    private func pointer() throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { throw GhostermError.failed("插件会话已释放") }; return raw
    }
    /// Consumes the channel only on success. Caller supplies a verified native
    /// package/endpoint snapshot; matching peer metadata is not package hashing.
    public init(channel: ExtensionChannel, role: Role, config: Config) throws {
        owner = channel.sessionOwner
        var c = gt_ext_session_config_t()
        try sessionPut(config.pluginID, into: &c.plugin_id); try sessionPut(config.pluginVersion, into: &c.plugin_version)
        try sessionPut(config.clientDigest, into: &c.client_digest); try sessionPut(config.executorDigest, into: &c.executor_digest)
        try sessionPut(config.hostVersion, into: &c.host_version); try sessionPut(config.sdkVersion, into: &c.sdk_version)
        try sessionCaps(config.required, into: &c.required); try sessionCaps(config.available, into: &c.available)
        c.required_count = config.required.count; c.available_count = config.available.count
        c.peer.kind = gt_ext_peer_kind_t(rawValue: config.peerKind.rawValue); try sessionPut(config.peerIdentity, into: &c.peer.identity)
        c.budgets = gt_ext_budgets_t(fragment_bytes: config.budgets.fragmentBytes, message_bytes: config.budgets.messageBytes,
                                    resource_bytes: config.budgets.resourceBytes)
        c.handshake_ms = config.handshakeMilliseconds; c.consent_ms = config.consentMilliseconds
        var methods: [gt_ext_rpc_method_t]?
        if let registry = config.methods {
            guard registry.count <= 16 else { throw GhostermError.failed("请求方法数量超限") }
            methods = try registry.map { entry in
                var m = gt_ext_rpc_method_t(); try sessionPut(entry.name, into: &m.method); try sessionPut(entry.capability, into: &m.capability)
                m.receiver = gt_ext_role_t(rawValue: entry.receiver.rawValue); m.input_shape = gt_ext_json_shape_t(rawValue: entry.inputShape.rawValue)
                m.output_shape = gt_ext_json_shape_t(rawValue: entry.outputShape.rawValue); return m
            }
            c.features = UInt32(GT_EXT_FEATURE_REQUESTS)
        }
        raw = try channel.adoptSession(role: gt_ext_role_t(rawValue: role.rawValue), config: &c, methods: methods)
    }
    public func step(milliseconds: Int32 = 0) throws -> Phase {
        guard milliseconds >= 0 else { throw GhostermError.failed("会话等待预算无效") }
        return Phase(rawValue: gt_ext_session_step(try pointer(), milliseconds).rawValue) ?? .ended
    }
    public func info() throws -> Info {
        var i = gt_ext_session_info_t()
        guard gt_ext_session_info(try pointer(), &i) else { throw GhostermError.failed("无法读取插件会话状态") }
        return Info(phase: Phase(rawValue: i.phase.rawValue) ?? .ended, end: End(rawValue: i.end.rawValue) ?? .internal,
                    decision: Decision(rawValue: i.decision.rawValue) ?? .unknown, error: Int32(i.error.rawValue), cleanupError: Int32(i.cleanup_error.rawValue),
                    connection: sessionString(i.connection), instance: sessionString(i.instance), consentToken: sessionString(i.consent_token),
                    peerHostVersion: sessionString(i.peer_host_version), epoch: i.epoch, consentDeadline: i.consent_deadline,
                    received: i.received, sent: i.sent,
                    budgets: Budgets(fragmentBytes: i.budgets.fragment_bytes, messageBytes: i.budgets.message_bytes, resourceBytes: i.budgets.resource_bytes),
                    peerKind: PeerKind(rawValue: i.peer.kind.rawValue) ?? .local, peerIdentity: sessionString(i.peer.identity))
    }
    public func decide(ticket: String, decision: Decision) throws -> Int32 {
        let p = try pointer(); guard !ticket.utf8.contains(0) else { throw GhostermError.failed("批准票据含 NUL") }
        return ticket.withCString { Int32(gt_ext_session_decide(p, $0, gt_ext_decision_t(rawValue: decision.rawValue)).rawValue) }
    }
    public func lease() throws -> Lease? {
        var l = gt_ext_lease_t(); return gt_ext_session_lease(try pointer(), &l) ? Lease(raw: l) : nil
    }
    public func access(_ lease: Lease, capability: String) throws -> Access {
        let p = try pointer(); guard !capability.utf8.contains(0) else { throw GhostermError.failed("能力名称含 NUL") }
        var l = lease.raw
        return capability.withCString { Access(rawValue: gt_ext_session_access(p, &l, $0).rawValue) ?? .denied }
    }
    public func revoke() throws { gt_ext_session_revoke(try pointer()) }
    public func request(_ lease: Lease, method: String, input: Data, timeoutMilliseconds: UInt32 = 30000) throws -> RPCAdmission {
        let p = try pointer(); guard !method.utf8.contains(0) else { throw GhostermError.failed("请求方法含 NUL") }
        var l = lease.raw, handle: UInt64 = 0
        let status = method.withCString { name in input.withUnsafeBytes { gt_ext_session_request(p, &l, name, $0.baseAddress, $0.count, timeoutMilliseconds, &handle) } }
        let value = RPCStatus(rawValue: status.rawValue) ?? .invalid
        return RPCAdmission(status: value, handle: value == .ok ? handle : nil)
    }
    public func cancelRequest(_ lease: Lease, handle: UInt64) throws -> RPCStatus {
        var l = lease.raw; return RPCStatus(rawValue: gt_ext_session_cancel_request(try pointer(), &l, handle).rawValue) ?? .invalid
    }
    public func requestResult(_ lease: Lease, handle: UInt64) throws -> (RPCStatus, RPCResult?) {
        var l = lease.raw, value = gt_ext_rpc_result_t()
        let status = RPCStatus(rawValue: gt_ext_session_request_result(try pointer(), &l, handle, &value).rawValue) ?? .invalid
        guard status == .ok else { return (status, nil) }
        guard value.outcome_size <= 3072 else { stop(); throw GhostermError.failed("请求结果超限") }
        let data = withUnsafeBytes(of: value.outcome) { Data($0.prefix(Int(value.outcome_size))) }
        return (status, RPCResult(handle: value.handle, requestID: sessionString(value.request_id), state: RPCState(rawValue: value.state.rawValue) ?? .unknown,
            cancelRequested: value.cancel_requested, deadlineElapsed: value.deadline_elapsed, confirmed: value.confirmed, outcome: data))
    }
    public func forgetRequest(_ lease: Lease, handle: UInt64) throws -> RPCStatus {
        var l = lease.raw; return RPCStatus(rawValue: gt_ext_session_forget_request(try pointer(), &l, handle).rawValue) ?? .invalid
    }
    public func takeRequest(_ lease: Lease) throws -> (RPCStatus, RPCRequest?) {
        var l = lease.raw, value = gt_ext_rpc_request_t()
        let status = RPCStatus(rawValue: gt_ext_session_take_request(try pointer(), &l, &value).rawValue) ?? .invalid
        guard status == .ok else { return (status, nil) }
        guard value.input_size <= 3072 else { stop(); throw GhostermError.failed("请求内容超限") }
        let data = withUnsafeBytes(of: value.input) { Data($0.prefix(Int(value.input_size))) }
        return (status, RPCRequest(ticket: value.ticket, requestID: sessionString(value.request_id), method: sessionString(value.method), input: data))
    }
    public func checkRequest(_ lease: Lease, ticket: UInt64) throws -> RPCStatus {
        var l = lease.raw; return RPCStatus(rawValue: gt_ext_session_check_request(try pointer(), &l, ticket).rawValue) ?? .invalid
    }
    public func respond(_ lease: Lease, ticket: UInt64, outcome: Data) throws -> RPCStatus {
        let p = try pointer(); var l = lease.raw
        return outcome.withUnsafeBytes { RPCStatus(rawValue: gt_ext_session_respond(p, &l, ticket, $0.baseAddress, $0.count).rawValue) ?? .invalid }
    }
    public func stop() { stopLock.lock(); defer { stopLock.unlock() }; if let raw { gt_ext_session_stop(raw) } }
    @discardableResult public func closeStep(milliseconds: Int32 = 0) throws -> Bool {
        let p = try pointer(); guard milliseconds >= 0 else { throw GhostermError.failed("会话关闭预算无效") }
        let result = gt_ext_session_close(p, milliseconds)
        if result == GT_EXT_CLOSE_ERROR { throw GhostermError.failed("会话释放失败，仍保留所有权") }
        guard result == GT_EXT_RELEASED else { return false }
        stopLock.lock(); defer { stopLock.unlock() }
        guard gt_ext_session_destroy(&raw) == GT_OK else { throw GhostermError.failed("会话仍有未释放资源") }; return true
    }
    public static var pendingCloseCount: Int { ExtensionSessionCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { ExtensionSessionCleanup.retry(owner) }
    deinit { if let raw { ExtensionSessionCleanup.enqueue(raw, owner: owner) } }
}
private func sessionPut<T>(_ text: String, into value: inout T) throws {
    let bytes = Array(text.utf8)
    guard !bytes.contains(0), bytes.count < MemoryLayout<T>.size else { throw GhostermError.failed("插件会话文本无效或超限") }
    withUnsafeMutableBytes(of: &value) { $0.initializeMemory(as: UInt8.self, repeating: 0); $0.copyBytes(from: bytes) }
}
private func sessionCaps<T>(_ texts: [String], into value: inout T) throws {
    guard texts.count <= 16 else { throw GhostermError.failed("插件会话能力数量超限") }
    let bytes = try texts.map { text -> [UInt8] in
        let b = Array(text.utf8); guard !b.contains(0), b.count <= 128 else { throw GhostermError.failed("插件能力名称无效") }; return b
    }
    withUnsafeMutableBytes(of: &value) { buffer in
        buffer.initializeMemory(as: UInt8.self, repeating: 0)
        for (i, text) in bytes.enumerated() { buffer[(i * 129)..<(i * 129 + text.count)].copyBytes(from: text) }
    }
}
private func sessionString<T>(_ value: T) -> String { withUnsafeBytes(of: value) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) } }
private final class ExtensionSessionCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: ExtensionSessionCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue
    private var raw: OpaquePointer?
    init(_ raw: OpaquePointer, owner: DispatchQueue) { self.raw = raw; self.owner = owner }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(_ raw: OpaquePointer, owner: DispatchQueue) {
        let item = ExtensionSessionCleanup(raw, owner: owner); lock.lock(); entries[item.id] = item; lock.unlock()
        owner.async { item.run() }
    }
    static func retry(_ owner: DispatchQueue) {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        lock.lock(); let values = entries.values.filter { $0.owner === owner }; lock.unlock(); values.forEach { $0.run() }
    }
    private func run() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { return }; gt_ext_session_stop(raw)
        for _ in 0..<32 {
            let result = gt_ext_session_close(raw, 0)
            if result == GT_EXT_CLOSE_ERROR { return }
            if result == GT_EXT_RELEASED {
                guard gt_ext_session_destroy(&self.raw) == GT_OK else { return }
                Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock(); return
            }
        }
    }
}
