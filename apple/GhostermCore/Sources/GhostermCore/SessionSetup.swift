// Native setup owns an exclusive temporary SSH connection. The long-lived
// Transport reauthenticates through the shared provider after installation.
import Cghosterm
import Foundation

public final class SessionCancellation {
    private let lock = NSLock()
    private var value = false
    public init() {}
    public func cancel() { lock.lock(); value = true; lock.unlock() }
    public var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return value }
}
public final class SessionAuthenticator {
    public enum Phase: UInt32 { case tcp, negotiating, hostkey, authenticating, ready, taken, failed, cancelled, credentials }
    public enum Host: UInt32 { case unchecked, known, unknown, mismatch }
    public struct Info {
        public let phase: Phase
        public let host: Host
        public let error: Int32
        public let deadline: Bool
        public let methods: Int32
        public let keyType, fingerprint: String
    }
    private let owner: DispatchQueue
    private var raw: OpaquePointer?
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    public init(options: SSHOptions, address: RecoverableSession.Address, credential: RecoverableSession.Credential? = nil,
                timeoutMilliseconds: UInt32 = 60000, owner: DispatchQueue) throws {
        self.owner = owner; checkOwner()
        guard address.bytes.count == 16 else { throw GhostermError.failed("解析地址长度无效") }
        var ssh: OpaquePointer? = try RecoverableSession.unlockedSSH(options: options, credential: credential)
        defer { gt_ssh_close(ssh) }
        var config = gt_session_auth_options_t()
        config.family = address.family; config.scope = address.scope; config.timeout_ms = timeoutMilliseconds
        config.defer_credentials = credential == nil
        withUnsafeMutableBytes(of: &config.address) { $0.copyBytes(from: address.bytes) }
        let error = gt_session_authenticator_open(&ssh, &config, &raw)
        guard error == GT_OK else { throw GhostermError.status(Int32(error.rawValue), "无法启动 SSH 认证") }
    }
    public func step() -> Phase { checkOwner(); return Phase(rawValue: gt_session_authenticator_step(raw).rawValue) ?? .failed }
    public var info: Info? {
        checkOwner(); var info = gt_session_auth_info_t()
        guard gt_session_authenticator_info(raw, &info) else { return nil }
        return Info(phase: Phase(rawValue: info.phase.rawValue) ?? .failed, host: Host(rawValue: info.host.rawValue) ?? .unchecked,
                    error: Int32(info.error.rawValue), deadline: info.deadline, methods: info.auth_methods,
                    keyType: setupString(info.key_type), fingerprint: setupString(info.fingerprint))
    }
    public func supply(options: SSHOptions, credential: RecoverableSession.Credential) throws {
        checkOwner(); var material: OpaquePointer? = try RecoverableSession.unlockedSSH(options: options, credential: credential)
        defer { gt_ssh_close(material) }
        let result = gt_session_authenticator_supply(raw, &material)
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "提交 SSH 认证材料失败") }
    }
    public func decide(_ decision: HostKeyDecision) -> Int32 {
        checkOwner(); return Int32(gt_session_authenticator_decide(raw, gt_hostkey_decision_t(rawValue: decision.rawValue)).rawValue)
    }
    public func take() -> SessionSetupConnection? {
        checkOwner(); var ssh: OpaquePointer?
        guard gt_session_authenticator_take(raw, &ssh), let ssh else { return nil }
        return SessionSetupConnection(raw: ssh, owner: owner)
    }
    public func close() { checkOwner(); gt_session_authenticator_close(raw); raw = nil }
    deinit { if let raw { owner.async { gt_session_authenticator_close(raw) } } }
}
private func setupString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { bytes in
        String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self)
    }
}
public final class SessionSetupConnection {
    public struct LinuxInfo {
        public let arch, glibc, home: String
        public let uid: UInt32
    }
    public struct Installation {
        public let status: Int32
        public let reused, published, publicationUnknown: Bool
        public let daemon, proxy: String
    }
    public struct Progress {
        /// Mirrors `gt_session_install_phase_t`; C enum constants are not
        /// re-exported to app targets, so expose a Swift-only view.
        public enum Phase: UInt32 { case probe = 0, upload = 1, verify = 2, activate = 3, done = 4 }
        public let phase: UInt32, file: UInt32
        public let bytes, total: UInt64
        public var stage: Phase? { Phase(rawValue: phase) }
    }
    private final class Callbacks {
        let cancellation: SessionCancellation
        let progress: ((Progress) -> Void)?
        var last: TimeInterval = 0
        var phase: UInt32 = UInt32.max
        init(_ cancellation: SessionCancellation, _ progress: ((Progress) -> Void)?) {
            self.cancellation = cancellation; self.progress = progress
        }
        func report(_ value: Progress) {
            let now = ProcessInfo.processInfo.systemUptime
            guard phase != value.phase || now - last >= 0.1 || value.phase == GT_SESSION_INSTALL_DONE.rawValue else { return }
            phase = value.phase; last = now; progress?(value)
        }
    }
    private var raw: OpaquePointer?
    private let owner: DispatchQueue
    private var busy = false
    fileprivate init(raw: OpaquePointer, owner: DispatchQueue) { self.raw = raw; self.owner = owner }
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    private func options(_ context: Callbacks, _ timeout: UInt32) -> gt_session_install_options_t {
        var config = gt_session_install_options_t()
        config.timeout_ms = timeout; config.context = Unmanaged.passUnretained(context).toOpaque()
        config.cancelled = { p in Unmanaged<Callbacks>.fromOpaque(p!).takeUnretainedValue().cancellation.isCancelled }
        config.progress = { p, phase, file, bytes, total in
            Unmanaged<Callbacks>.fromOpaque(p!).takeUnretainedValue().report(Progress(phase: phase.rawValue, file: file, bytes: bytes, total: total))
        }
        return config
    }
    /// Transfer this exclusive connection to a session on the same serial IO owner.
    /// A busy installation/SFTP or stale request never transfers ownership.
    public func adopt(into session: RecoverableSession, request: UInt64) throws {
        checkOwner(); guard raw != nil, !busy else { throw GhostermError.failed("SSH 连接正在使用或已转移") }
        let status = session.adopt(request: request, connection: &raw)
        guard status == 0 else { throw GhostermError.status(status, "会话认证请求已失效或不能接收连接") }
    }
    /// Bounded blocking setup IO, never call on a UI or live terminal IO owner.
    public func probe(cancellation: SessionCancellation, timeoutMilliseconds: UInt32 = 120000,
                      progress: ((Progress) -> Void)? = nil) throws -> LinuxInfo {
        checkOwner(); guard raw != nil, !busy else { throw GhostermError.failed("SSH 安装连接不可用") }
        busy = true; defer { busy = false }
        let context = Callbacks(cancellation, progress); var config = options(context, timeoutMilliseconds)
        var info = gt_session_linux_info_t()
        let error = withExtendedLifetime(context) { gt_session_linux_probe(raw, &config, &info) }
        guard error == GT_OK else { throw GhostermError.status(Int32(error.rawValue), "Linux 环境探测失败") }
        return LinuxInfo(arch: setupString(info.arch), glibc: setupString(info.glibc), home: setupString(info.home), uid: info.uid)
    }
    /// Always inspect status/publicationUnknown, including failures: interrupted
    /// publication is not proof of rollback. Existing daemon/session is retained.
    public func install(bundleDirectory: String, root: String, cancellation: SessionCancellation,
                        timeoutMilliseconds: UInt32 = 120000, progress: ((Progress) -> Void)? = nil) -> Installation {
        checkOwner()
        guard raw != nil, !busy, !bundleDirectory.utf8.contains(0), !root.utf8.contains(0) else {
            return Installation(status: 1, reused: false, published: false, publicationUnknown: false, daemon: "", proxy: "")
        }
        busy = true; defer { busy = false }
        let context = Callbacks(cancellation, progress); var config = options(context, timeoutMilliseconds)
        var result = gt_session_install_result_t()
        let error = withExtendedLifetime(context) {
            bundleDirectory.withCString { directory in root.withCString { path in
                gt_session_linux_install(raw, directory, path, &config, &result)
            } }
        }
        return Installation(status: Int32(error.rawValue), reused: result.reused, published: result.published,
                            publicationUnknown: result.publication_unknown, daemon: setupString(result.daemon), proxy: setupString(result.proxy))
    }
    @discardableResult public func close() -> Bool {
        checkOwner(); guard !busy else { return false }; gt_ssh_close(raw); raw = nil; return true
    }
    deinit { if let raw { owner.async { gt_ssh_close(raw) } } }
}
