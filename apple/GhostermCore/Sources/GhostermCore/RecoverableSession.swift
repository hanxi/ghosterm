// One serial IO owner. Engine payloads are borrowed only during sink callbacks;
// this binding never dispatches terminal bytes to the main queue.
import Cghosterm
import Darwin
import Foundation

public final class RecoverableSession {
    public enum Kind { case local, ssh }
    public enum State: UInt32 { case starting, recovering, restoring, live, readonly, ended, failed, closed, catalog }
    public enum Admission { case accepted, busy, failed(Int32) }
    public enum Command: UInt32 { case input, resize, color, style }
    public enum Submission: UInt32 { case queued, backpressure, locked, invalid, unavailable }
    public struct Size: Equatable {
        public var cols, rows: UInt16
        public var pixelWidth, pixelHeight: UInt32
        public init(cols: UInt16, rows: UInt16, pixelWidth: UInt32 = 0, pixelHeight: UInt32 = 0) {
            self.cols = cols; self.rows = rows; self.pixelWidth = pixelWidth; self.pixelHeight = pixelHeight
        }
    }
    public struct Stamp: Equatable {
        public let generation, command, sequence: UInt64
    }
    public struct Applied {
        public let generation, sequence: UInt64
        public init(generation: UInt64, sequence: UInt64) { self.generation = generation; self.sequence = sequence }
    }
    public enum Event { case output(UnsafeRawBufferPointer), resize(Size), color(dark: Bool), style(Style) }
    public struct Receipt {
        public let stamp: Stamp
        public let kind: UInt32, status: Int32, delivery: UInt32, rejection: UInt32
        public let rejected: Bool
        // delivery 0: proven not executed; 1: operation accepted; 2: outcome unknown.
        // Acceptance is never proof that a shell command executed.
    }
    public enum StopState: UInt32 { case none, pending, known, unknown }
    public struct Stop {
        public let state: StopState
        public let session, request: UInt64
        public let status: UInt32 // meaningful only for .known
        public let error: Int32
    }
    public struct Row {
        public let session, sequence: UInt64
        public let phase: UInt32
        public let terminating, hasResult, hasSequence, hasExitCode: Bool
        public let exitCode: Int32
        public let terminalKind: UInt32
        public let status, terminationStatus: Int32
    }
    public struct Info {
        public let state: State
        public let error: Int32
        public let instance: [UInt8]
        public let session, generation, sequence: UInt64
        public let attempts, reason, busyReplies: UInt32
        public let hasSequence, readonly, hasTerminal, hasExitCode: Bool
        public let exitCode: Int32
        public let terminalKind: UInt32
        public let terminalStatus, terminationStatus, stateError: Int32
        public let hasProcess: Bool
        public let childState: Transport.ChildState
        public let cwd: String?
        public let hasAdmittedCreate: Bool
        public let stop: Stop
    }
    public struct Sink {
        public var snapshot: (UInt64, UInt64, UInt32, UnsafeRawBufferPointer, Bool) -> Admission
        public var event: (UInt64, UInt64, Event) -> Admission
        public var applied: () -> Applied?
        public var invalidated: (UInt64, Int32) -> Void
        public var receipt: (Receipt) -> Void
        public init(snapshot: @escaping (UInt64, UInt64, UInt32, UnsafeRawBufferPointer, Bool) -> Admission,
                    event: @escaping (UInt64, UInt64, Event) -> Admission,
                    applied: @escaping () -> Applied?, invalidated: @escaping (UInt64, Int32) -> Void,
                    receipt: @escaping (Receipt) -> Void) {
            self.snapshot = snapshot; self.event = event; self.applied = applied
            self.invalidated = invalidated; self.receipt = receipt
        }
    }
    public struct Authentication {
        public var requested: (UInt64, UInt32) -> Void
        public var cancelled: (UInt64) -> Void
        public var hostkey: (UInt64, String, String) -> Void
        public init(requested: @escaping (UInt64, UInt32) -> Void, cancelled: @escaping (UInt64) -> Void,
                    hostkey: @escaping (UInt64, String, String) -> Void) {
            self.requested = requested; self.cancelled = cancelled; self.hostkey = hostkey
        }
    }
    public struct Style: Equatable {
        public let dark: Bool
        public let colors: [UInt8] // bg/fg/cursor + 256 RGB entries, exactly 777 bytes
        public init(dark: Bool, colors: [UInt8]) { self.dark = dark; self.colors = colors }
        fileprivate init(_ value: gt_session_style_t) {
            dark = value.dark != 0
            colors = withUnsafeBytes(of: value.background) { Array($0) } +
                withUnsafeBytes(of: value.foreground) { Array($0) } +
                withUnsafeBytes(of: value.cursor) { Array($0) } +
                withUnsafeBytes(of: value.palette) { Array($0) }
        }
        fileprivate var native: gt_session_style_t? {
            guard colors.count == 777 else { return nil }
            var result = gt_session_style_t(); result.dark = dark ? 1 : 0
            withUnsafeMutableBytes(of: &result.background) { $0.copyBytes(from: colors[0..<3]) }
            withUnsafeMutableBytes(of: &result.foreground) { $0.copyBytes(from: colors[3..<6]) }
            withUnsafeMutableBytes(of: &result.cursor) { $0.copyBytes(from: colors[6..<9]) }
            withUnsafeMutableBytes(of: &result.palette) { $0.copyBytes(from: colors[9..<777]) }
            return result
        }
    }
    public struct Create {
        public var nonce: [UInt8]
        public var executable, cwd: String
        public var arguments, environment: [String]
        public var term: String
        public var size: Size
        public var direct: Bool
        public init(nonce: [UInt8], executable: String, cwd: String, arguments: [String] = [],
                    environment: [String] = [], term: String = "xterm-256color", size: Size, direct: Bool = false) {
            self.nonce = nonce; self.executable = executable; self.cwd = cwd; self.arguments = arguments
            self.environment = environment; self.term = term; self.size = size; self.direct = direct
        }
    }
    public struct Options {
        public var kind: Kind
        public var proxy, daemon, runtime: String
        public var compatibility: [UInt8]
        public var instance: [UInt8] = Array(repeating: 0, count: 16)
        public var session: UInt64 = 0
        public var pendingNonce: [UInt8] = Array(repeating: 0, count: 16)
        public var catalogOnly = false
        public var create: Create?
        public var format: UInt32
        public var snapshotLimit: Int = 16 * 1024 * 1024
        public var recoveryTimeout: UInt32 = 60000
        public var authenticationTimeout: UInt32 = 60000
        public init(kind: Kind, proxy: String, daemon: String, runtime: String,
                    compatibility: [UInt8], format: UInt32, create: Create? = nil) {
            self.kind = kind; self.proxy = proxy; self.daemon = daemon; self.runtime = runtime
            self.compatibility = compatibility; self.format = format; self.create = create
        }
    }
    public enum Credential { case password(String), privateKey(pem: String, passphrase: String?) }
    public struct Address {
        public var family: UInt8, bytes: [UInt8]
        public var scope: UInt32
        public init(family: UInt8, bytes: [UInt8], scope: UInt32 = 0) {
            self.family = family; self.bytes = bytes; self.scope = scope
        }
    }
    private final class Context {
        let sink: Sink
        let authentication: Authentication?
        init(_ sink: Sink, _ auth: Authentication?) { self.sink = sink; authentication = auth }
    }
    private let owner: DispatchQueue
    private let context: Context
    private var raw: OpaquePointer?
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    private static func box(_ pointer: UnsafeMutableRawPointer?) -> Context {
        Unmanaged<Context>.fromOpaque(pointer!).takeUnretainedValue()
    }
    private static func admission(_ value: Admission, _ error: UnsafeMutablePointer<gt_status_t>?) -> gt_session_sink_result_t {
        error?.pointee = GT_OK
        switch value {
        case .accepted: return GT_SESSION_SINK_ACCEPTED
        case .busy: return GT_SESSION_SINK_BUSY
        case .failed(let code): error?.pointee = gt_status_t(rawValue: (1...9).contains(code) ? UInt32(code) : GT_ERR_INTERNAL.rawValue); return GT_SESSION_SINK_FAILED
        }
    }
    public init(options: Options, sink: Sink? = nil, authentication: Authentication? = nil, owner: DispatchQueue) throws {
        self.owner = owner; context = Context(sink ?? Self.metadataSink, authentication); checkOwner()
        guard options.compatibility.count == 32, options.instance.count == 16, options.pendingNonce.count == 16, options.snapshotLimit >= 0,
              options.catalogOnly || sink != nil,
              options.create?.nonce.count ?? 16 == 16 else { throw GhostermError.failed("会话身份或配额参数无效") }
        if options.kind == .ssh && authentication == nil { throw GhostermError.failed("SSH 会话缺少异步认证处理器") }
        var texts = [options.proxy, options.daemon, options.runtime]
        if let c = options.create {
            guard c.arguments.count <= 32, c.environment.count <= 64 else { throw GhostermError.failed("会话参数过多") }
            texts += [c.executable, c.cwd, c.term] + c.arguments + c.environment
        }
        guard !texts.contains(where: { $0.utf8.contains(0) }) else { throw GhostermError.failed("会话参数含 NUL") }
        guard !texts.contains(where: { $0.utf8.count > 8192 }),
              texts.reduce(0, { $0 + $1.utf8.count }) <= 16384 else { throw GhostermError.failed("会话参数过长") }
        let arena = Strings()
        var config = gt_session_options_t()
        config.kind = options.kind == .local ? GT_SESSION_LOCAL : GT_SESSION_SSH
        config.proxy = arena.string(options.proxy); config.daemon = arena.string(options.daemon)
        config.runtime = arena.string(options.runtime); config.snapshot_format = options.format
        config.snapshot_limit = options.snapshotLimit; config.recovery_timeout_ms = options.recoveryTimeout
        config.authentication_timeout_ms = options.authenticationTimeout; config.session = options.session
        config.catalog_only = options.catalogOnly
        withUnsafeMutableBytes(of: &config.pending_nonce) { $0.copyBytes(from: options.pendingNonce) }
        withUnsafeMutableBytes(of: &config.compatibility) { $0.copyBytes(from: options.compatibility) }
        withUnsafeMutableBytes(of: &config.instance) { $0.copyBytes(from: options.instance) }
        let pointer = Unmanaged.passUnretained(context).toOpaque()
        config.sink.context = pointer
        config.sink.snapshot = { p, g, s, f, bytes, n, ro, error in
            RecoverableSession.admission(RecoverableSession.box(p).sink.snapshot(g, s, f, UnsafeRawBufferPointer(start: bytes, count: n), ro), error)
        }
        config.sink.event = { p, g, s, event, error in
            guard let e = event?.pointee else { error?.pointee = GT_ERR_INVALID_ARG; return GT_SESSION_SINK_FAILED }
            let value: Event
            switch e.kind {
            case GT_SESSION_OUTPUT: value = .output(UnsafeRawBufferPointer(start: e.bytes, count: e.length))
            case GT_SESSION_RESIZE: value = .resize(Size(cols: e.size.cols, rows: e.size.rows, pixelWidth: e.size.px_width, pixelHeight: e.size.px_height))
            case GT_SESSION_COLOR: value = .color(dark: e.dark)
            case GT_SESSION_STYLE: value = .style(Style(e.style))
            default: error?.pointee = GT_ERR_UNSUPPORTED; return GT_SESSION_SINK_FAILED
            }
            return RecoverableSession.admission(RecoverableSession.box(p).sink.event(g, s, value), error)
        }
        config.sink.applied = { p, out in
            out?.pointee = gt_session_applied_t()
            if let a = RecoverableSession.box(p).sink.applied() {
                out?.pointee = gt_session_applied_t(known: true, generation: a.generation, sequence: a.sequence)
            }
            return GT_OK
        }
        config.sink.invalidated = { p, g, error in RecoverableSession.box(p).sink.invalidated(g, Int32(error.rawValue)) }
        config.sink.receipt = { p, receipt in
            guard let r = receipt?.pointee else { return }
            RecoverableSession.box(p).sink.receipt(Receipt(stamp: Stamp(generation: r.stamp.generation, command: r.stamp.command_id, sequence: r.stamp.sequence),
                                       kind: r.kind.rawValue, status: Int32(r.status.rawValue), delivery: r.delivery.rawValue,
                                       rejection: r.rejection, rejected: r.rejected))
        }
        config.authentication.context = pointer
        config.authentication.requested = { p, id, attempt in RecoverableSession.box(p).authentication?.requested(id, attempt) }
        config.authentication.cancelled = { p, id in RecoverableSession.box(p).authentication?.cancelled(id) }
        config.authentication.hostkey = { p, id, type, fingerprint in
            RecoverableSession.box(p).authentication?.hostkey(id, type.map { String(cString: $0) } ?? "", fingerprint.map { String(cString: $0) } ?? "")
        }
        var create = gt_session_create_t()
        if let c = options.create {
            withUnsafeMutableBytes(of: &create.nonce) { $0.copyBytes(from: c.nonce) }
            create.process.executable = arena.string(c.executable); create.process.cwd = arena.string(c.cwd)
            create.process.term = arena.string(c.term); create.process.argv = arena.array(c.arguments)
            create.process.env = arena.array(c.environment); create.process.cmd_kind = c.direct ? GT_PTY_CMD_DIRECT : GT_PTY_CMD_SHELL
            create.process.cols = c.size.cols; create.process.rows = c.size.rows
            create.process.px_width = c.size.pixelWidth; create.process.px_height = c.size.pixelHeight
        }
        let error: gt_status_t = withExtendedLifetime(arena) {
            withUnsafePointer(to: &create) { ptr in
                config.create = options.create == nil ? nil : ptr
                return gt_session_open(&config, &raw)
            }
        }
        guard error == GT_OK else { throw GhostermError.status(Int32(error.rawValue), "无法创建可恢复会话") }
    }
    public func step() -> State {
        checkOwner(); return State(rawValue: gt_session_step(raw).rawValue) ?? .failed
    }
    private static func child(_ value: gt_transport_child_t) -> Transport.ChildState {
        switch value {
        case GT_TRANSPORT_CHILD_EXITED: return .exited
        case GT_TRANSPORT_CHILD_IDLE: return .idle
        case GT_TRANSPORT_CHILD_JOB: return .job
        default: return .unknown
        }
    }
    public var info: Info? {
        checkOwner(); var i = gt_session_info_t(); guard gt_session_info(raw, &i) else { return nil }
        return Info(state: State(rawValue: i.state.rawValue) ?? .failed, error: Int32(i.error.rawValue),
                    instance: withUnsafeBytes(of: i.instance) { Array($0) }, session: i.session,
                    generation: i.generation, sequence: i.sequence, attempts: i.attempts, reason: i.reason, busyReplies: i.busy_replies, hasSequence: i.has_sequence,
                    readonly: i.readonly, hasTerminal: i.has_terminal, hasExitCode: i.has_exit_code,
                    exitCode: i.exit_code, terminalKind: i.terminal_kind, terminalStatus: Int32(i.terminal_status.rawValue),
                    terminationStatus: Int32(i.termination_status.rawValue), stateError: Int32(i.state_error.rawValue),
                    hasProcess: i.has_process, childState: Self.child(i.process.child),
                    cwd: i.has_process && i.process.has_cwd ? withUnsafeBytes(of: i.process.cwd) { bytes in
                        String(bytes: bytes.prefix { $0 != 0 }, encoding: .utf8)
                    } : nil,
                    hasAdmittedCreate: i.has_admitted_create,
                    stop: Stop(state: StopState(rawValue: i.stop.state.rawValue) ?? .unknown, session: i.stop.session, request: i.stop.request, status: i.stop.status.rawValue, error: Int32(i.stop.error.rawValue)))
    }
    /// nil means not loaded; [] is an observed empty catalog. One IO owner.
    public func catalog() throws -> [Row]? {
        checkOwner(); var rows = Array(repeating: gt_session_row_t(), count: 64), count = 0
        let result = rows.withUnsafeMutableBufferPointer { gt_session_catalog(raw, $0.baseAddress, $0.count, &count) }
        if result == GT_ERR_NOT_FOUND { return nil }
        guard result == GT_OK, count <= rows.count else { throw GhostermError.status(Int32(result.rawValue), "会话列表不可用") }
        return rows.prefix(count).map { Row(session: $0.session, sequence: $0.sequence, phase: $0.phase,
            terminating: $0.terminating, hasResult: $0.has_result, hasSequence: $0.has_sequence,
            hasExitCode: $0.has_exit_code, exitCode: $0.exit_code, terminalKind: $0.terminal_kind,
            status: Int32($0.status.rawValue), terminationStatus: Int32($0.termination_status.rawValue)) }
    }
    @discardableResult public func refreshCatalog() -> Submission {
        checkOwner(); return Submission(rawValue: gt_session_catalog_refresh(raw).rawValue) ?? .unavailable
    }
    /// Known admission in info.stop is not a process-exit observation.
    @discardableResult public func terminate(session: UInt64) -> Submission {
        checkOwner(); return Submission(rawValue: gt_session_terminate(raw, session).rawValue) ?? .unavailable
    }
    public func input(generation: UInt64, sequence: UInt64, bytes: UnsafeRawBufferPointer) -> (Submission, Stamp?) {
        checkOwner(); var stamp = gt_session_stamp_t()
        let r = gt_session_input(raw, generation, sequence, bytes.baseAddress, bytes.count, &stamp)
        return submitted(r, stamp)
    }
    public func resize(generation: UInt64, sequence: UInt64, size: Size) -> (Submission, Stamp?) {
        checkOwner(); var stamp = gt_session_stamp_t()
        let r = gt_session_resize(raw, generation, sequence,
                                 gt_session_size_t(cols: size.cols, rows: size.rows, px_width: size.pixelWidth, px_height: size.pixelHeight), &stamp)
        return submitted(r, stamp)
    }
    public func color(generation: UInt64, sequence: UInt64, dark: Bool) -> (Submission, Stamp?) {
        checkOwner(); var stamp = gt_session_stamp_t()
        return submitted(gt_session_color(raw, generation, sequence, dark, &stamp), stamp)
    }
    public func style(generation: UInt64, sequence: UInt64, style: Style) -> (Submission, Stamp?) {
        checkOwner(); var stamp = gt_session_stamp_t()
        guard var native = style.native else { return (.invalid, nil) }
        return submitted(gt_session_style(raw, generation, sequence, &native, &stamp), stamp)
    }
    private func submitted(_ result: gt_session_submit_t, _ stamp: gt_session_stamp_t) -> (Submission, Stamp?) {
        (Submission(rawValue: result.rawValue) ?? .unavailable,
         result == GT_SESSION_SUBMIT_QUEUED ? Stamp(generation: stamp.generation, command: stamp.command_id, sequence: stamp.sequence) : nil)
    }
    /// Fresh credentials only. Original host identity and resolved address remain separate.
    /// Late/cancelled requests consume no SSH object and close their own temporary materials.
    public func supply(request: UInt64, options: SSHOptions, address: Address, credential: Credential) throws {
        checkOwner()
        guard let raw, address.bytes.count == 16 else { throw GhostermError.failed("会话已关闭或地址无效") }
        var ssh: OpaquePointer? = try Self.unlockedSSH(options: options, credential: credential)
        defer { gt_ssh_close(ssh) }
        let result = address.bytes.withUnsafeBufferPointer {
            gt_session_auth_supply(raw, request, &ssh, address.family, $0.baseAddress, address.scope)
        }
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "SSH 请求已失效或地址无效") }
    }
    static func unlockedSSH(options: SSHOptions, credential: Credential?) throws -> OpaquePointer {
        var values = [options.host, options.username, options.knownHostsPath ?? ""]
        if let credential { switch credential { case .password(let value): values.append(value)
        case .privateKey(let pem, let passphrase): values += [pem, passphrase ?? ""] } }
        guard !values.contains(where: { $0.utf8.contains(0) }) else { throw GhostermError.failed("SSH 参数含 NUL") }
        var ssh: OpaquePointer?
        options.host.withCString { host in options.username.withCString { user in
            if let known = options.knownHostsPath {
                known.withCString { path in var o = gt_ssh_opts_t(host: host, port: options.port, username: user, known_hosts_path: path, connect_timeout_ms: options.connectTimeoutMilliseconds); ssh = gt_ssh_new(&o) }
            } else {
                var o = gt_ssh_opts_t(host: host, port: options.port, username: user, known_hosts_path: nil, connect_timeout_ms: options.connectTimeoutMilliseconds); ssh = gt_ssh_new(&o)
            }
        } }
        guard ssh != nil else { throw GhostermError.failed("无法准备 SSH 凭据") }
        guard let credential else { return ssh! }
        let auth: Int32
        switch credential {
        case .password(let password): auth = password.withCString { gt_ssh_auth_password(ssh, $0) }
        case .privateKey(let pem, let passphrase):
            auth = pem.withCString { key in
                if let passphrase { return passphrase.withCString { gt_ssh_auth_private_key(ssh, key, $0) } }
                return gt_ssh_auth_private_key(ssh, key, nil)
            }
        }
        guard auth == 0 else { gt_ssh_close(ssh); throw GhostermError.status(auth, "准备 SSH 凭据失败") }
        return ssh!
    }
    func adopt(request: UInt64, connection: inout OpaquePointer?) -> Int32 {
        checkOwner(); return Int32(gt_session_auth_adopt(raw, request, &connection).rawValue)
    }
    public func reject(request: UInt64, error: Int32) -> Int32 {
        checkOwner(); return Int32(gt_session_auth_reject(raw, request, gt_status_t(rawValue: UInt32(clamping: error))).rawValue)
    }
    public func decide(request: UInt64, decision: HostKeyDecision) -> Int32 {
        checkOwner(); return Int32(gt_session_auth_decide(raw, request, gt_hostkey_decision_t(rawValue: decision.rawValue)).rawValue)
    }
    public func cancel() { checkOwner(); gt_session_cancel(raw) }
    /// false retains the handle: poll again on owner until local proxies are reaped.
    @discardableResult public func close() -> Bool {
        checkOwner(); guard gt_session_close(raw) else { return false }; raw = nil; return true
    }
    deinit {
        if let raw { Self.reap(raw, context: context, owner: owner) }
    }
    private static func reap(_ raw: OpaquePointer, context: Context, owner: DispatchQueue) {
        owner.asyncAfter(deadline: .now() + .milliseconds(1)) {
            withExtendedLifetime(context) {
                if !gt_session_close(raw) { reap(raw, context: context, owner: owner) }
            }
        }
    }
    private static var metadataSink: Sink {
        Sink(snapshot: { _, _, _, _, _ in .failed(7) }, event: { _, _, _ in .failed(7) },
             applied: { nil }, invalidated: { _, _ in }, receipt: { _ in })
    }
    private final class Strings {
        var strings: [UnsafeMutablePointer<CChar>] = []
        var arrays: [UnsafeMutablePointer<UnsafePointer<CChar>?>] = []
        func string(_ value: String) -> UnsafePointer<CChar> {
            let p = strdup(value)!; strings.append(p); return UnsafePointer(p)
        }
        func array(_ values: [String]) -> UnsafePointer<UnsafePointer<CChar>?> {
            let p = UnsafeMutablePointer<UnsafePointer<CChar>?>.allocate(capacity: values.count + 1)
            for (i, value) in values.enumerated() { p.advanced(by: i).initialize(to: string(value)) }
            p.advanced(by: values.count).initialize(to: nil); arrays.append(p); return UnsafePointer(p)
        }
        deinit { for p in arrays { p.deallocate() }; for p in strings { free(p) } }
    }
}
