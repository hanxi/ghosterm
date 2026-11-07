// Shared SSH authentication, independent of terminal/session management.
import Cghosterm
import Foundation

public enum SSHCredential { case password(String), privateKey(pem: String, passphrase: String?) }
public struct SSHAddress {
    public var family: UInt8, bytes: [UInt8]
    public var scope: UInt32
    public init(family: UInt8, bytes: [UInt8], scope: UInt32 = 0) {
        self.family = family; self.bytes = bytes; self.scope = scope
    }
}
public final class SSHCancellation {
    private let lock = NSLock()
    private var value = false
    public init() {}
    public func cancel() { lock.lock(); value = true; lock.unlock() }
    public var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return value }
}
public final class SSHAuthenticator {
    public enum Phase: UInt32 { case tcp, negotiating, hostkey, authenticating, ready, taken, failed, cancelled, credentials }
    public enum Host: UInt32 { case unchecked, known, unknown, mismatch, revoked }
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
    public init(options: SSHOptions, address: SSHAddress, credential: SSHCredential? = nil,
                timeoutMilliseconds: UInt32 = 60000, owner: DispatchQueue) throws {
        self.owner = owner; checkOwner()
        guard address.bytes.count == 16 else { throw GhostermError.failed("解析地址长度无效") }
        var ssh: OpaquePointer? = try Self.unlockedSSH(options: options, credential: credential)
        defer { gt_ssh_close(ssh) }
        var config = gt_ssh_auth_options_t()
        config.family = address.family; config.scope = address.scope; config.timeout_ms = timeoutMilliseconds
        config.defer_credentials = credential == nil
        withUnsafeMutableBytes(of: &config.address) { $0.copyBytes(from: address.bytes) }
        let error = gt_ssh_authenticator_open(&ssh, &config, &raw)
        guard error == GT_OK else { throw GhostermError.status(Int32(error.rawValue), "无法启动 SSH 认证") }
    }
    static func unlockedSSH(options: SSHOptions, credential: SSHCredential?) throws -> OpaquePointer {
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

    public func step() -> Phase { checkOwner(); return Phase(rawValue: gt_ssh_authenticator_step(raw).rawValue) ?? .failed }
    public var info: Info? {
        checkOwner(); var info = gt_ssh_auth_info_t()
        guard gt_ssh_authenticator_info(raw, &info) else { return nil }
        return Info(phase: Phase(rawValue: info.phase.rawValue) ?? .failed, host: Host(rawValue: info.host.rawValue) ?? .unchecked,
                    error: Int32(info.error.rawValue), deadline: info.deadline, methods: info.auth_methods,
                    keyType: setupString(info.key_type), fingerprint: setupString(info.fingerprint))
    }
    public func supply(options: SSHOptions, credential: SSHCredential) throws {
        checkOwner(); var material: OpaquePointer? = try Self.unlockedSSH(options: options, credential: credential)
        defer { gt_ssh_close(material) }
        let result = gt_ssh_authenticator_supply(raw, &material)
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "提交 SSH 认证材料失败") }
    }
    public func decide(_ decision: HostKeyDecision) -> Int32 {
        checkOwner(); return Int32(gt_ssh_authenticator_decide(raw, gt_hostkey_decision_t(rawValue: decision.rawValue)).rawValue)
    }
    public func take() -> SSHSession? {
        checkOwner(); var ssh: OpaquePointer?
        guard gt_ssh_authenticator_take(raw, &ssh), let ssh else { return nil }
        return SSHSession(adopting: ssh)
    }
    public func close() { checkOwner(); gt_ssh_authenticator_close(raw); raw = nil }
    deinit { if let raw { owner.async { gt_ssh_authenticator_close(raw) } } }
}
private func setupString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { bytes in
        String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self)
    }
}

/// Serial-owner authentication chain. Taking the session transfers the whole
/// route lifetime to it; close its PTY/SFTP children before closing the session.
public final class SSHRoute {
    public struct Hop {
        public let options: SSHOptions
        public let address: SSHAddress?
        public let viaHost: String?
        public let credential: SSHCredential?
        public init(options: SSHOptions, address: SSHAddress? = nil, viaHost: String? = nil,
                    credential: SSHCredential? = nil) {
            self.options = options; self.address = address; self.viaHost = viaHost; self.credential = credential
        }
    }
    public enum Phase: UInt32 { case authenticating, forwarding, ready, failed, cancelled }
    public struct Info {
        public let phase: Phase
        public let error: Int32
        public let hop, count: UInt32
        public let deadline: Bool
        public let authentication: SSHAuthenticator.Info
    }
    private let owner: DispatchQueue
    private var raw: OpaquePointer?
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    public init(hops: [Hop], timeoutMilliseconds: UInt32 = 60000, owner: DispatchQueue) throws {
        self.owner = owner; checkOwner()
        guard (1...8).contains(hops.count), hops.enumerated().allSatisfy({ index, hop in
            index == 0 ? hop.address?.bytes.count == 16 && hop.viaHost == nil : hop.address == nil && hop.viaHost != nil
        }), !hops.contains(where: { $0.viaHost?.utf8.contains(0) == true }) else {
            throw GhostermError.failed("SSH 跳板配置无效")
        }
        var handles: [OpaquePointer?] = []
        var names: [UnsafeMutablePointer<CChar>?] = []
        defer { handles.forEach { gt_ssh_close($0) }; names.forEach { free($0) } }
        var configs: [gt_ssh_route_hop_t] = []
        for hop in hops {
            handles.append(try SSHAuthenticator.unlockedSSH(options: hop.options, credential: hop.credential))
            let name = hop.viaHost.flatMap { strdup($0) }; names.append(name)
            if hop.viaHost != nil && name == nil { throw GhostermError.failed("无法分配跳板地址") }
            var config = gt_ssh_route_hop_t()
            config.via_host = name.map { UnsafePointer($0) }
            config.authentication.defer_credentials = hop.credential == nil
            config.authentication.timeout_ms = timeoutMilliseconds
            if let address = hop.address {
                config.authentication.family = address.family; config.authentication.scope = address.scope
                withUnsafeMutableBytes(of: &config.authentication.address) { $0.copyBytes(from: address.bytes) }
            }
            configs.append(config)
        }
        let result = gt_ssh_route_open(&handles, &configs, UInt32(hops.count), timeoutMilliseconds, &raw)
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "无法启动 SSH 跳板认证") }
    }
    public func step() -> Phase { checkOwner(); return Phase(rawValue: gt_ssh_route_step(raw).rawValue) ?? .failed }
    public var info: Info? {
        checkOwner(); var value = gt_ssh_route_info_t()
        guard gt_ssh_route_info(raw, &value) else { return nil }
        let a = value.authentication
        return Info(phase: Phase(rawValue: value.phase.rawValue) ?? .failed, error: Int32(value.error.rawValue),
                    hop: value.hop, count: value.count, deadline: value.deadline,
                    authentication: SSHAuthenticator.Info(phase: SSHAuthenticator.Phase(rawValue: a.phase.rawValue) ?? .failed,
                        host: SSHAuthenticator.Host(rawValue: a.host.rawValue) ?? .unchecked, error: Int32(a.error.rawValue),
                        deadline: a.deadline, methods: a.auth_methods, keyType: setupString(a.key_type), fingerprint: setupString(a.fingerprint)))
    }
    public func decide(_ decision: HostKeyDecision) -> Int32 {
        checkOwner(); return Int32(gt_ssh_route_decide(raw, gt_hostkey_decision_t(rawValue: decision.rawValue)).rawValue)
    }
    public func supply(options: SSHOptions, credential: SSHCredential) throws {
        checkOwner(); var material: OpaquePointer? = try SSHAuthenticator.unlockedSSH(options: options, credential: credential)
        defer { gt_ssh_close(material) }
        let result = gt_ssh_route_supply(raw, &material)
        guard result == GT_OK else { throw GhostermError.status(Int32(result.rawValue), "提交跳板认证材料失败") }
    }
    public func take() -> SSHSession? {
        checkOwner(); var session: OpaquePointer?
        guard let route = raw, gt_ssh_route_borrow(route, &session) == GT_OK, let session else { return nil }
        raw = nil
        let owner = self.owner
        return SSHSession(adopting: session, routeRelease: { SSHRouteDisposal.start(route, owner: owner) })
    }
    public func close() { checkOwner(); if let raw { self.raw = nil; SSHRouteDisposal.start(raw, owner: owner) } }
    deinit { if let raw { SSHRouteDisposal.start(raw, owner: owner) } }
}

// An in-flight cleanup retains its raw owner across retries, including errors.
// Never discard a partially released chain or block the UI/serial IO queue.
private final class SSHRouteDisposal {
    private var raw: OpaquePointer?
    private let owner: DispatchQueue
    private init(_ raw: OpaquePointer, owner: DispatchQueue) { self.raw = raw; self.owner = owner }
    static func start(_ raw: OpaquePointer, owner: DispatchQueue) {
        let cleanup = SSHRouteDisposal(raw, owner: owner)
        owner.async { cleanup.step() }
    }
    private func step() {
        dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread)
        var released = false
        let status = gt_ssh_route_dispose(&raw, &released)
        if released { return }
        owner.asyncAfter(deadline: .now() + (status == GT_OK ? 0.001 : 0.01)) { self.step() }
    }
}
