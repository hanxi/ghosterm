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
    private let resourceStore: ExtensionResourceStore?
    private var raw: OpaquePointer?
    private func pointer() throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard let raw else { throw GhostermError.failed("插件会话已释放") }; return raw
    }
    /// Consumes the channel only on success. Caller supplies a verified native
    /// package/endpoint snapshot; matching peer metadata is not package hashing.
    public init(channel: ExtensionChannel, role: Role, config: Config, resourceStore: ExtensionResourceStore? = nil, resourceScopeBytes: UInt64 = 1048576) throws {
        owner = channel.sessionOwner; self.resourceStore = resourceStore
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
        let store = try resourceStore?.pointer(on: owner)
        if store != nil { c.features |= UInt32(GT_EXT_FEATURE_RESOURCES) }
        raw = try channel.adoptSession(role: gt_ext_role_t(rawValue: role.rawValue), config: &c, methods: methods, store: store, scopeBytes: store == nil ? 0 : resourceScopeBytes)
    }
    public func step(milliseconds: Int32 = 0) throws -> Phase {
        guard milliseconds >= 0 else { throw GhostermError.failed("会话等待预算无效") }
        let phase = Phase(rawValue: gt_ext_session_step(try pointer(), milliseconds).rawValue) ?? .ended
        ExtensionFileSource.reapDroppedSources(on: owner); return phase
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
    public struct ResourceAdmission { public let status: ExtensionResourceStore.Status, handle: UInt64? }
    public enum TransferState: UInt32 { case queued, sending, pending, succeeded, failed, cancelled, unknown }
    public struct TransferResult {
        public let handle: UInt64, accepted: UInt64, state: TransferState, confirmed: Bool, cancelRequested: Bool
        public let resourceID: String, code: String, generation: UInt64
    }
    public func sendResource(_ lease: Lease, descriptor: ExtensionResourceStore.Descriptor) throws -> ResourceAdmission {
        var l = lease.raw, d = try descriptor.native(), h: UInt64 = 0
        let status = ExtensionResourceStore.Status(gt_ext_session_send_resource(try pointer(), &l, &d, &h))
        return ResourceAdmission(status: status, handle: status == .ok ? h : nil)
    }
    public func writeResource(_ lease: Lease, handle: UInt64, bytes: Data) throws -> (ExtensionResourceStore.Status, Int) {
        let p = try pointer(); var l = lease.raw, n = 0
        let status = bytes.withUnsafeBytes { gt_ext_session_write_resource(p, &l, handle, $0.baseAddress, $0.count, &n) }
        return (ExtensionResourceStore.Status(status), n)
    }
    public func finishResource(_ lease: Lease, handle: UInt64, sourceComplete: Bool) throws -> ExtensionResourceStore.Status {
        var l = lease.raw; return ExtensionResourceStore.Status(gt_ext_session_finish_resource(try pointer(), &l, handle, sourceComplete))
    }
    public func cancelResource(_ lease: Lease, handle: UInt64) throws -> ExtensionResourceStore.Status {
        var l = lease.raw; return ExtensionResourceStore.Status(gt_ext_session_cancel_resource(try pointer(), &l, handle))
    }
    public func forgetResource(_ lease: Lease, handle: UInt64) throws -> ExtensionResourceStore.Status {
        var l = lease.raw; return ExtensionResourceStore.Status(gt_ext_session_forget_resource(try pointer(), &l, handle))
    }
    public func rejectResource(_ lease: Lease) throws -> ExtensionResourceStore.Status {
        var l = lease.raw; return ExtensionResourceStore.Status(gt_ext_session_reject_resource(try pointer(), &l))
    }
    public func resourceResult(_ lease: Lease, handle: UInt64) throws -> (ExtensionResourceStore.Status, TransferResult?) {
        var l = lease.raw, v = gt_ext_transfer_result_t()
        let status = ExtensionResourceStore.Status(gt_ext_session_resource_result(try pointer(), &l, handle, &v))
        guard status == .ok else { return (status, nil) }
        return (status, TransferResult(handle: v.handle, accepted: v.accepted, state: TransferState(rawValue: v.state.rawValue) ?? .unknown,
            confirmed: v.confirmed, cancelRequested: v.cancel_requested, resourceID: sessionString(v.resource_id), code: sessionString(v.code), generation: v.generation))
    }
    public func takeResource(_ lease: Lease) throws -> (ExtensionResourceStore.Status, ExtensionResourceStore.Snapshot?) {
        var l = lease.raw, v = gt_ext_resource_snapshot_t()
        let status = ExtensionResourceStore.Status(gt_ext_session_take_resource(try pointer(), &l, &v))
        guard status == .ok, let resourceStore else { return (status, nil) }
        return (status, ExtensionResourceStore.Snapshot(store: resourceStore, lease: lease, native: v))
    }
    public func openSource(_ lease: Lease, rootFD: Int32, relativePath: String, mime: String = "application/octet-stream", byteLimit: UInt64 = 262144) throws -> ExtensionFileSource.Admission {
        let p = try pointer(); guard !relativePath.utf8.contains(0), relativePath.utf8.count <= 4096, !mime.utf8.contains(0), mime.utf8.count <= 128 else { throw GhostermError.failed("源文件参数无效或超限") }
        let source = ExtensionFileSource(owner: owner); var l = lease.raw, rawSource: OpaquePointer?
        let status = relativePath.withCString { path in mime.withCString { type in
            var c = gt_ext_source_config_t(root_fd: rootFD, relative_path: path, mime: type, byte_limit: byteLimit)
            return gt_ext_source_open(p, &l, &c, &rawSource)
        } }
        if let rawSource { source.adopt(rawSource) }
        return ExtensionFileSource.Admission(status: ExtensionFileSource.Status(status), source: status == GT_EXT_SOURCE_OK ? source : nil)
    }
    public func stop() { stopLock.lock(); defer { stopLock.unlock() }; if let raw { gt_ext_session_stop(raw) } }
    internal var runtimeOwner: DispatchQueue { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread); return owner }
    internal var runtimeStore: ExtensionResourceStore? { resourceStore }
    internal func adoptRuntime(config: inout gt_ext_runtime_config_t) throws -> OpaquePointer {
        _ = try pointer(); stopLock.lock(); defer { stopLock.unlock() }
        var runtime: OpaquePointer?
        let status = gt_ext_runtime_open(&raw, &config, &runtime)
        guard status == GT_OK, let runtime else { throw GhostermError.status(Int32(status.rawValue), "运行时未接管会话") }
        return runtime
    }
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
/// Native private-directory owner. A Store never renews remote authorization.
/// Snapshot reads/releases and Session adoption must use this same serial queue.
public final class ExtensionResourceStore {
    public enum Status: UInt32 {
        case ok, wait, invalid, denied, closed, `protocol`, limit, io, unsupported, notFound
        fileprivate init(_ value: gt_ext_resource_status_t) { self = Status(rawValue: value.rawValue) ?? .invalid }
    }
    public struct Descriptor {
        public let resourceID: String, sha256: String, mime: String, displayName: String
        public let generation: UInt64, size: UInt64, sizeKnown: Bool
        public init(sha256: String, mime: String, displayName: String, size: UInt64, sizeKnown: Bool = true, resourceID: String = "", generation: UInt64 = 0) {
            self.sha256 = sha256; self.mime = mime; self.displayName = displayName; self.size = size
            self.sizeKnown = sizeKnown; self.resourceID = resourceID; self.generation = generation
        }
        fileprivate init(_ v: gt_ext_resource_descriptor_t) {
            self.init(sha256: sessionString(v.sha256), mime: sessionString(v.mime), displayName: sessionString(v.display_name), size: v.size,
                      sizeKnown: v.size_known, resourceID: sessionString(v.resource_id), generation: v.generation)
        }
        fileprivate func native() throws -> gt_ext_resource_descriptor_t {
            var d = gt_ext_resource_descriptor_t(); try sessionPut(resourceID, into: &d.resource_id); try sessionPut(sha256, into: &d.sha256)
            try sessionPut(mime, into: &d.mime); try sessionPut(displayName, into: &d.display_name)
            d.generation = generation; d.size = size; d.size_known = sizeKnown; return d
        }
    }
    public struct Usage { public let reservedBytes: UInt64, resources: Int, scopes: Int, sessions: Int }
    public struct Info { public let descriptor: Descriptor, received: UInt64, reserved: UInt64, references: UInt32, published: Bool }
    public final class Snapshot {
        private let store: ExtensionResourceStore, lease: ExtensionSession.Lease
        private var native: gt_ext_resource_snapshot_t?
        public let descriptor: Descriptor
        fileprivate init(store: ExtensionResourceStore, lease: ExtensionSession.Lease, native: gt_ext_resource_snapshot_t) {
            self.store = store; self.lease = lease; self.native = native; descriptor = Descriptor(native.descriptor)
        }
        public func info() throws -> (Status, Info?) {
            let p = try store.pointer(on: store.owner); guard var r = native else { return (.closed, nil) }
            var l = lease.raw, i = gt_ext_resource_info_t(); let status = Status(gt_ext_resource_store_info(p, &l, &r, &i))
            return (status, status == .ok ? Info(descriptor: Descriptor(i.descriptor), received: i.received, reserved: i.reserved, references: i.references, published: i.published) : nil)
        }
        public func read(offset: UInt64, capacity: Int = 16384) throws -> (Status, Data) {
            let p = try store.pointer(on: store.owner); guard var r = native else { return (.closed, Data()) }
            guard capacity > 0 && capacity <= 16384 else { return (.invalid, Data()) }
            var l = lease.raw, n = 0, data = Data(count: capacity)
            let status = data.withUnsafeMutableBytes { Status(gt_ext_resource_store_read(p, &l, &r, offset, $0.baseAddress, $0.count, &n)) }
            guard status == .ok, n <= capacity else { return (status, Data()) }; data.count = n; return (status, data)
        }
        public func retain() throws -> (Status, Snapshot?) {
            let p = try store.pointer(on: store.owner); guard var r = native else { return (.closed, nil) }
            var l = lease.raw; let status = Status(gt_ext_resource_store_retain(p, &l, &r))
            return (status, status == .ok ? Snapshot(store: store, lease: lease, native: r) : nil)
        }
        @discardableResult public func close() throws -> Status {
            let p = try store.pointer(on: store.owner); guard var r = native else { return .closed }
            var l = lease.raw; let status = Status(gt_ext_resource_store_release(p, &l, &r)); native = r.handle == 0 ? nil : r; return status
        }
        deinit {
            if var r = native {
                let s = store; var l = lease.raw
                ExtensionResourceCleanup.enqueue(on: s.owner) {
                    guard let p = try? s.pointer(on: s.owner) else { return false }
                    let status = gt_ext_resource_store_release(p, &l, &r)
                    return status == GT_EXT_RESOURCE_OK || status == GT_EXT_RESOURCE_NOT_FOUND
                }
            }
        }
    }
    fileprivate let owner: DispatchQueue
    private var raw: OpaquePointer?
    fileprivate func pointer(on queue: DispatchQueue) throws -> OpaquePointer {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        guard queue === owner, let raw else { throw GhostermError.failed("资源 Store 已释放或 owner 不匹配") }; return raw
    }
    /// Caller owns the authorized private directory fd; Core duplicates it.
    public init(directoryFD: Int32, owner: DispatchQueue, totalBytes: UInt64 = 1048576, resourceBytes: UInt64 = 262144, slots: Int = 64) throws {
        self.owner = owner; dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        var c = gt_ext_resource_store_config_t(directory_fd: directoryFD, total_bytes: totalBytes, resource_bytes: resourceBytes, slots: slots)
        let status = gt_ext_resource_store_open(&c, &raw)
        guard status == GT_EXT_RESOURCE_OK else { throw GhostermError.failed("无法打开资源 Store：\(status.rawValue)") }
    }
    public func usage() throws -> Usage {
        var u = gt_ext_resource_usage_t(); guard gt_ext_resource_store_usage(try pointer(on: owner), &u) else { throw GhostermError.failed("资源计费状态无效") }
        return Usage(reservedBytes: u.reserved_bytes, resources: u.resources, scopes: u.scopes, sessions: u.sessions)
    }
    @discardableResult public func close() throws -> Status {
        _ = try pointer(on: owner); return Status(gt_ext_resource_store_destroy(&raw))
    }
    public static var pendingCloseCount: Int { ExtensionResourceCleanup.count }
    public static func retryPendingClose(on owner: DispatchQueue) { ExtensionResourceCleanup.retry(owner) }
    deinit {
        if let p = raw {
            var owned: OpaquePointer? = p
            ExtensionResourceCleanup.enqueue(on: owner) {
                guard owned != nil else { return true }
                return gt_ext_resource_store_destroy(&owned) == GT_EXT_RESOURCE_OK
            }
        }
    }
}
private final class ExtensionResourceCleanup {
    private static let lock = NSLock()
    private static var entries: [UUID: ExtensionResourceCleanup] = [:]
    private let id = UUID(), owner: DispatchQueue, attempt: () -> Bool
    init(owner: DispatchQueue, attempt: @escaping () -> Bool) { self.owner = owner; self.attempt = attempt }
    static var count: Int { lock.lock(); defer { lock.unlock() }; return entries.count }
    static func enqueue(on owner: DispatchQueue, attempt: @escaping () -> Bool) {
        let item = ExtensionResourceCleanup(owner: owner, attempt: attempt); lock.lock(); entries[item.id] = item; lock.unlock()
        owner.async { item.run() }
    }
    static func retry(_ owner: DispatchQueue) {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        lock.lock(); let values = entries.values.filter { $0.owner === owner }; lock.unlock(); values.forEach { $0.run() }
    }
    private func run() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        if attempt() { Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock() }
    }
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
            if result == GT_EXT_DRAINING { ExtensionFileSource.reapDroppedSources(on: owner) }
            if result == GT_EXT_CLOSE_ERROR { return }
            if result == GT_EXT_RELEASED {
                guard gt_ext_session_destroy(&self.raw) == GT_OK else { return }
                Self.lock.lock(); Self.entries.removeValue(forKey: id); Self.lock.unlock(); return
            }
        }
    }
}
