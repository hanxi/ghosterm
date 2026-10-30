// GhostermCore — core/include/ghosterm.h 的 Swift 便捷绑定（M1）。
//
// 设计：本层只做「字符串/内存/生命周期」的搬运与安全包装，绝不复制业务逻辑；
// 所有语义（无损写回、三态、错误码）都来自 C 核心。句柄一律 opaque。
import Cghosterm
import Foundation

/// 私钥对口令的需求（对应 core 的 gt_ssh_key_needs_passphrase）。
public enum PrivateKeyPassphraseRequirement: Int32 {
    /// 认不出来（不是 libssh2 能用的私钥文件）；调用方不得据此弹口令框。
    case unknown = -1
    case notNeeded = 0
    case needed = 1
}

public enum Ghosterm {
    /// 运行时版本（gt_version）。
    public static var version: String { String(cString: gt_version()) }

    /// 私钥是否需要口令（**只看文件头，不解密**）。
    ///
    /// 为什么值得一条独立绑定：上层在认证前要决定「问不问用户私钥口令」。
    /// 先前是「文件读得出来就先弹口令框」，没口令的钥匙也被问了（用户实测）。
    /// 三态不折叠（铁律 #6）：`.unknown` 表示「认不出来」，**不是**「不需要」——
    /// 这种文件 libssh2 本来就用不了，弹口令框也解不开，应当把认证失败如实报出去。
    public static func keyPassphraseRequirement(_ pem: String)
        -> PrivateKeyPassphraseRequirement {
        let rc = pem.withCString { gt_ssh_key_needs_passphrase($0) }
        return PrivateKeyPassphraseRequirement(rawValue: rc) ?? .unknown
    }
    /// 最近一次 C 侧错误（线程局部）。
    public static var lastError: String {
        let p = gt_last_error()
        return p == nil ? "" : String(cString: p!)
    }
}

public enum GhostermError: Error, CustomStringConvertible {
    case status(Int32, String)
    case failed(String)

    public var description: String {
        switch self {
        case let .status(code, msg): return "gt_status \(code): \(msg)"
        case let .failed(msg): return msg
        }
    }
}

public extension GhostermError {
    /// 是不是「协作式取消」（GT_ERR_CANCELLED）。
    var isCancelled: Bool {
        if case let .status(code, _) = self { return code == Int32(GT_ERR_CANCELLED.rawValue) }
        return false
    }
}

@discardableResult
private func check(_ rc: Int32) throws -> Int32 {
    guard rc == GT_OK.rawValue else {
        throw GhostermError.status(rc, Ghosterm.lastError)
    }
    return rc
}

// MARK: - ssh_config

public struct ConfigFieldInfo: Equatable {
    public let explicitValue: String?
    public let sourcePath: String
    public let sourceHeader: String
    public let sourceLine: Int
    public let sourceIncluded: Bool
    public let effectiveIsExplicit: Bool
}

public struct ConfigIdentityInfo: Equatable {
    public let value: String
    public let sourcePath: String
    public let sourceHeader: String
    public let sourceLine: Int
    public let included: Bool
    public let explicitValue: Bool
    public let effective: Bool
}

public struct ConfigHostInfo: Equatable {
    public let header: String
    public let sourcePath: String
    public let sourceLine: Int
    public let patternCount: Int
    public let matchingBlocks: Int
    public let included: Bool
    public let hostname: ConfigFieldInfo
    public let user: ConfigFieldInfo
    public let port: ConfigFieldInfo
}

public struct SSHConnectionIssue: Hashable, Sendable {
    public let keyword, sourcePath: String
    public let sourceLine: Int
    public let included: Bool
    public var description: String { "\(keyword)（\(sourcePath.isEmpty ? "当前配置" : sourcePath):\(sourceLine)）" }
}

public struct ResolvedHost {
    public let user: String?
    public let hostname: String?
    public let port: Int
    public let identityFiles: [String]
    /// -1 = 未知（三态不折叠：`未设置` 与 `设成 no` 是两回事）。
    public let forwardAgent: Int
    public let compression: Int
    public let serverAliveInterval: Int
    public let proxyJump: [String]
    public let setEnv: [String]
    public let localForward: [String]
}

/// `~/.ssh/config` 的解析 / 无损写回 / 求值 / 编辑（包 gt_config_t）。
public final class SSHConfig {
    let handle: OpaquePointer

    private init(handle: OpaquePointer) { self.handle = handle }

    public convenience init(contents: String) throws {
        var h: OpaquePointer?
        contents.withCString { h = gt_config_parse($0, strlen($0)) }
        guard let handle = h else { throw GhostermError.failed(Ghosterm.lastError) }
        self.init(handle: handle)
    }

    public convenience init(path: String) throws {
        var h: OpaquePointer?
        path.withCString { h = gt_config_parse_file($0) }
        guard let handle = h else { throw GhostermError.failed(Ghosterm.lastError) }
        self.init(handle: handle)
    }

    deinit { gt_config_free(handle) }

    /// 无损写回；未编辑时与输入逐字节相同。
    public func serialized() -> String {
        guard let p = gt_config_to_string(handle) else { return "" }
        defer { free(p) }
        return String(cString: p)
    }

    /// 声明顺序的 Host 模式。
    public var hostPatterns: [String] {
        (0..<gt_config_host_count(handle)).compactMap { i in
            gt_config_host_pattern(handle, i).map { String(cString: $0) }
        }
    }

    /// Rename only the literal Host token; all sibling names and body bytes survive.
    public func rename(alias: String, to newAlias: String) throws {
        try check(alias.withCString { old in
            newAlias.withCString { gt_config_rename_host(handle, old, $0) }
        })
    }

    /// Copies borrowed Core provenance before the snapshot can be edited/freed.
    public func inspect(alias: String) throws -> ConfigHostInfo {
        var info = gt_config_host_info_t()
        try check(alias.withCString { gt_config_inspect_host(handle, $0, &info) })
        func field(_ value: gt_config_field_info_t) -> ConfigFieldInfo {
            ConfigFieldInfo(explicitValue: value.explicit_value.map { String(cString: $0) },
                sourcePath: value.source_path.map { String(cString: $0) } ?? "",
                sourceHeader: value.source_header.map { String(cString: $0) } ?? "",
                sourceLine: Int(value.source_line), sourceIncluded: value.source_included,
                effectiveIsExplicit: value.effective_is_explicit)
        }
        return ConfigHostInfo(header: info.header.map { String(cString: $0) } ?? "",
            sourcePath: info.source_path.map { String(cString: $0) } ?? "", sourceLine: Int(info.source_line),
            patternCount: Int(info.pattern_count), matchingBlocks: Int(info.matching_blocks), included: info.included,
            hostname: field(info.hostname), user: field(info.user), port: field(info.port))
    }

    /// Copies cumulative IdentityFile entries and their ownership, without expansion/defaults.
    /// Unsupported conditional/invalid directives throw; they never become an empty list.
    public func identities(alias: String) throws -> [ConfigIdentityInfo] {
        _ = try inspect(alias: alias)
        var entries: [ConfigIdentityInfo] = []
        for index in 0...4096 {
            var item = gt_config_identity_info_t()
            let rc = alias.withCString { gt_config_identity_at(handle, $0, index, &item) }
            if rc == GT_ERR_NOT_FOUND.rawValue { return entries }
            try check(rc)
            guard index < 4096 else { throw GhostermError.failed("身份条目过多（最多 4096）") }
            entries.append(ConfigIdentityInfo(value: String(cString: item.value),
                sourcePath: String(cString: item.source_path), sourceHeader: String(cString: item.source_header),
                sourceLine: Int(item.source_line), included: item.included,
                explicitValue: item.explicit_value, effective: item.effective))
        }
        return entries
    }

    /// 稀疏编辑（除目标行外逐字节不变）；`values == nil` 表示删除该指令。
    public func edit(alias: String, _ edits: [(String, [String]?)], reset: Bool = false) throws {
        let kwsMut = edits.map { strdup($0.0) }
        var flatMut: [UnsafeMutablePointer<CChar>?] = []
        var offsets: [Int32] = []
        var groupBufs: [[UnsafeMutablePointer<CChar>?]] = []
        for (_, vals) in edits {
            offsets.append(Int32(flatMut.count))
            if let vals = vals {
                var buf = vals.map { strdup($0) }
                buf.append(nil)
                groupBufs.append(buf)
                flatMut.append(contentsOf: buf)
            } else {
                flatMut.append(nil) // 组首 NULL = 删除
            }
        }
        let kws = kwsMut.map { UnsafePointer($0) }
        let flat = flatMut.map { $0.map { UnsafePointer($0) } }
        defer {
            kwsMut.forEach { free($0) }
            groupBufs.forEach { $0.forEach { free($0) } }
        }
        let rc: Int32 = alias.withCString { a in
            kws.withUnsafeBufferPointer { kb in
                flat.withUnsafeBufferPointer { vb in
                    offsets.withUnsafeBufferPointer { ob in
                        ghosterm_config_edit_flat(handle, a, kb.baseAddress,
                                                  vb.baseAddress, ob.baseAddress,
                                                  Int32(edits.count), reset ? 1 : 0)
                    }
                }
            }
        }
        try check(rc)
    }

    public func connectionIssues(host: String) throws -> [SSHConnectionIssue] {
        var issues: [SSHConnectionIssue] = []
        while issues.count < 256 {
            var info = gt_config_connection_issue_t()
            let code = host.withCString { gt_config_check_connection(handle, $0, issues.count, &info) }
            if code == 0 { return issues }
            guard code == 7 else { try check(code); return issues }
            issues.append(.init(keyword: String(cString: info.keyword), sourcePath: String(cString: info.source_path),
                                sourceLine: Int(info.source_line), included: info.included))
        }
        throw GhostermError.failed("不支持的连接配置过多（超过 255 项），连接已停止")
    }

    /// 求值（typed 视图）：单值字段 first-wins，IdentityFile 按策略回落。
    /// `defaultUser == nil` 时由核心取当前进程用户；策略默认 9.6 档。
    public func resolve(host: String, defaultUser: String? = nil,
                        policy: IdentityPolicy = .openssh_9_6) throws -> ResolvedHost {
        var resolved: UnsafeMutablePointer<gt_resolved_t>?
        host.withCString { h in
            defaultUser.withCStringOrNil { u in
                var opts = gt_resolve_opts_t()
                opts.default_user = u
                opts.identity_policy = gt_identity_policy_t(policy.rawValue)
                opts.custom_identity_files = nil
                resolved = gt_config_resolve(handle, h, &opts)
            }
        }
        guard let r = resolved else { throw GhostermError.failed(Ghosterm.lastError) }
        defer { gt_resolved_free(r) }
        let user = r.pointee.user.map { String(cString: $0) }
        let hostname = r.pointee.hostname.map { String(cString: $0) }
        let port = Int(r.pointee.port)
        let identityFiles = stringArray(r.pointee.identity_files)
        let forwardAgent = Int(r.pointee.forward_agent)
        let compression = Int(r.pointee.compression)
        let serverAlive = Int(r.pointee.server_alive_interval)
        let proxyJump = stringArray(r.pointee.proxy_jump)
        let setEnv = stringArray(r.pointee.setenv)
        let localForward = stringArray(r.pointee.local_forward)
        return ResolvedHost(
            user: user, hostname: hostname, port: port, identityFiles: identityFiles,
            forwardAgent: forwardAgent, compression: compression,
            serverAliveInterval: serverAlive, proxyJump: proxyJump,
            setEnv: setEnv, localForward: localForward
        )
    }
}

/// 默认 IdentityFile 候选链策略（版本相关行为，不是常量）。
public enum IdentityPolicy: UInt32 {
    case openssh_9_6 = 0, openssh_10_3 = 1, openssh_master = 2, none = 3, custom = 4
}

private func stringArray(_ ptr: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>?) -> [String] {
    guard let ptr = ptr else { return [] }
    var out: [String] = []
    var i = 0
    while let s = ptr[i] {
        out.append(String(cString: s))
        i += 1
    }
    return out
}

// MARK: - SSH 会话

public enum HostKeyDecision: UInt32 {
    case accept = 0, reject = 1, unknown = 2
}

/// 服务器允许的认证方式（握手后才有值）。
public struct SSHAuthMethods: OptionSet, Sendable {
    public let rawValue: Int32
    public init(rawValue: Int32) { self.rawValue = rawValue }
    public static let publicKey = SSHAuthMethods(rawValue: 1 << 0)
    public static let password = SSHAuthMethods(rawValue: 1 << 1)
    public static let keyboardInteractive = SSHAuthMethods(rawValue: 1 << 2)
    public static let hostBased = SSHAuthMethods(rawValue: 1 << 3)
}

public struct SSHOptions {
    public var host: String
    public var port: UInt16
    public var username: String
    public var knownHostsPath: String?
    /// TCP + 握手的预算（毫秒）。0 = 默认（15s）。
    ///
    /// 为什么必须有：阻塞的 `connect()` 在防火墙丢包时不返回，用户点连接
    /// 之后就永远停在「连接中…」——既没失败也没超时，只能强退。
    public var connectTimeoutMilliseconds: Int32
    public init(host: String, port: UInt16 = 22, username: String,
                knownHostsPath: String? = nil,
                connectTimeoutMilliseconds: Int32 = 0) {
        self.host = host; self.port = port
        self.username = username; self.knownHostsPath = knownHostsPath
        self.connectTimeoutMilliseconds = connectTimeoutMilliseconds
    }
}

/// 主机密钥裁决回调（TOFU 提示）；返回 .unknown 即视为拒绝并抛错。
public typealias HostKeyDecider = (_ host: String, _ port: UInt16,
                                   _ keyType: String, _ fingerprint: String) -> HostKeyDecision

public final class SSHSession {
    nonisolated let handle: OpaquePointer
    private var deciderBox: Box?
    private var closed = false

    private final class Box {
        let decider: HostKeyDecider
        init(_ d: @escaping HostKeyDecider) { decider = d }
    }

    init(adopting handle: OpaquePointer) { self.handle = handle }

    public init(_ opts: SSHOptions) throws {
        var h: OpaquePointer?
        opts.host.withCString { host in
            opts.username.withCString { user in
                opts.knownHostsPath.withCStringOrNil { kh in
                    var o = gt_ssh_opts_t()
                    o.host = host
                    o.username = user
                    o.known_hosts_path = kh
                    o.port = opts.port
                    o.connect_timeout_ms = opts.connectTimeoutMilliseconds
                    h = gt_ssh_new(&o)
                }
            }
        }
        guard let handle = h else { throw GhostermError.failed(Ghosterm.lastError) }
        self.handle = handle
    }

    /// 服务器允许的认证方式（**握手成功后**才有值；连接前为空）。
    ///
    /// 上层据此决定问什么凭据：只带私钥去连一台只收密码的服务器时，
    /// 认证必然失败，而失败原因从 libssh2 的错误串里读不出来——没有这张
    /// 方法表，UI 只能猜（最容易猜成「口令错了」，于是反复要口令）。
    public var authMethods: SSHAuthMethods {
        SSHAuthMethods(rawValue: gt_ssh_auth_methods(handle))
    }

    /// 握手：TCP + 密钥交换 + 主机密钥裁决 + **服务器认证方式发现**（幂等）。
    ///
    /// 与 `authenticate()` 拆开，是为了让上层在认证前就能读到 `authMethods`：
    /// 只带私钥去连一台只收密码的服务器时，这张表是「该问登录密码还是该问
    /// 私钥口令」的唯一依据（先前一步 connect 时表恒为空，只能猜「口令」）。
    public func handshake() throws {
        try check(gt_ssh_handshake(handle))
    }

    /// 用 `authenticate(password:)` / `authenticate(privateKeyPEM:passphrase:)`
    /// 存下的凭据做一次 userauth。**可在同一会话上多次调用**（换一把钥匙重试）。
    public func authenticate() throws {
        try check(gt_ssh_authenticate(handle))
    }

    /// Owner-only: interrupt the socket before releasing every channel and this session.
    /// Never call concurrently with Core operations or use the connection afterwards.
    public func abort() throws {
        guard !closed else { throw GhostermError.failed("SSH session closed") }
        try check(gt_ssh_abort(handle))
    }

    /// 显式关闭（幂等）。libssh2 阻塞语义：调用方负责在会话线程上调用。
    public func close() {
        guard !closed else { return }
        closed = true
        gt_ssh_close(handle)
    }

    private var releaseOwner: DispatchQueue?
    public func releaseOn(_ owner: DispatchQueue) { releaseOwner = owner }
    deinit {
        if !closed, let releaseOwner {
            let raw = handle
            releaseOwner.async { gt_ssh_close(raw) }
        } else { close() }
    }

    public func setHostKeyDecider(_ decider: @escaping HostKeyDecider) {
        let box = Box(decider)
        deciderBox = box
        let ud = Unmanaged.passUnretained(box).toOpaque()
        gt_ssh_set_hostkey_policy(handle, { ud, host, port, kt, fp in
            guard let ud = ud else { return GT_HOSTKEY_UNKNOWN }
            let box = Unmanaged<Box>.fromOpaque(ud).takeUnretainedValue()
            let d = box.decider(host.map { String(cString: $0) } ?? "",
                                port,
                                kt.map { String(cString: $0) } ?? "",
                                fp.map { String(cString: $0) } ?? "")
            return gt_hostkey_decision_t(rawValue: d.rawValue)
        }, ud)
    }

    public func authenticate(password: String) throws {
        try check(gt_ssh_auth_password(handle, password))
    }

    public func authenticate(privateKeyPEM: String, passphrase: String? = nil) throws {
        let rc = privateKeyPEM.withCString { pem in
            passphrase.withCStringOrNil { pp in
                gt_ssh_auth_private_key(handle, pem, pp)
            }
        }
        try check(rc)
    }

    /// 一步到位（握手 + 认证）。等价于 `handshake()` 后 `authenticate()`。
    public func connect() throws { try check(gt_ssh_connect(handle)) }

    /// 在已连接会话上开 SFTP 子系统（M4）。服务器不支持时显式抛错。
    public func sftp() throws -> SFTPSession {
        guard let h = gt_ssh_sftp(handle) else {
            throw GhostermError.failed(Ghosterm.lastError)
        }
        return SFTPSession(handle: h)
    }

    /// Dedicated SSH owner only; failure after IO invalidates this connection.
    /// This bounds subsystem admission, not subsequent SFTP operations or close.
    public func sftp(timeoutMilliseconds: UInt32, cancellation: SSHCancellation? = nil) throws -> SFTPSession {
        var status = GT_OK
        let context = cancellation.map { Unmanaged.passUnretained($0).toOpaque() }
        let raw = withExtendedLifetime(cancellation) {
            gt_ssh_sftp_controlled(handle, timeoutMilliseconds, { pointer in
                guard let pointer else { return false }
                let request = Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue()
                return request.isCancelled
            }, context, &status)
        }
        guard let raw else { throw GhostermError.status(Int32(status.rawValue), Ghosterm.lastError) }
        return SFTPSession(handle: raw)
    }

    /// 开一条 exec 通道跑单条远端命令（用于远端加速：find / grep / du）。
    /// 服务器禁 exec（纯 sftp 账号）时抛 GT_ERR_UNSUPPORTED（**不静默降级**）。
    public func exec(_ command: String) throws -> SSHExec {
        var status = GT_OK
        let h = command.withCString { gt_ssh_exec(handle, $0, &status) }
        guard let h else {
            throw GhostermError.status(Int32(status.rawValue), Ghosterm.lastError)
        }
        return SSHExec(handle: h, session: self)
    }
}

/// 一条远端 exec 通道（stdout/stderr 多路复用 + 退出码）。
/// 线程纪律同会话：read/exitStatus/close 都放会话线程。
public final class SSHExec {
    nonisolated let handle: OpaquePointer
    private let session: SSHSession   // 谁建谁撑住：会话必须活过 exec 通道
    private var closed = false

    init(handle: OpaquePointer, session: SSHSession) {
        self.handle = handle
        self.session = session
    }

    /// 一次读取的结果（三态不折叠）。
    public enum ReadResult: Equatable {
        case data(Data, isStderr: Bool)
        case timeout
        case eof
    }

    /// 服务器不允许 exec（GT_ERR_UNSUPPORTED）——上层据此显式回退 SFTP walk。
    public static let unsupportedRaw: Int32 = Int32(GT_ERR_UNSUPPORTED.rawValue)

    public func read(max: Int = 65536, timeoutMS: Int32 = 200) throws -> ReadResult {
        var buf = [UInt8](repeating: 0, count: Swift.max(1, max))
        var isErr = false
        let n = buf.withUnsafeMutableBytes { raw -> Int32 in
            gt_ssh_exec_read(handle, raw.baseAddress!.assumingMemoryBound(to: UInt8.self),
                             raw.count, timeoutMS, &isErr)
        }
        if n > 0 { return .data(Data(buf[0..<Int(n)]), isStderr: isErr) }
        if n == 0 { return .eof }        // GT_EXEC_READ_EOF
        if n == -2 { return .timeout }   // GT_EXEC_READ_TIMEOUT
        throw GhostermError.failed(Ghosterm.lastError)
    }

    /// 远端退出码（通道关闭后才有效；拿不到返回 nil）。
    public func exitStatus() -> Int32? {
        var code: Int32 = 0
        return gt_ssh_exec_exit_status(handle, &code) ? code : nil
    }

    public func close() {
        guard !closed else { return }
        closed = true
        gt_ssh_exec_close(handle)
    }

    deinit { close() }
}

// MARK: - SFTP（libssh2_sftp 薄封装的 Swift 面）

/// SFTP 目录条目（name 经 full_path 重建；stat 为类型化属性）。
public struct SFTPEntry: Equatable, Identifiable {
    public var id: String { fullPath }
    public let name: String
    public let fullPath: String
    public let size: UInt64
    public let mode: UInt32
    public let isDirectory: Bool
    public let isSymlink: Bool
    public let mtime: Date?
    public let uid: UInt32
    public let gid: UInt32
    public let hasOwner: Bool
    public let hasSize: Bool
    public let isRegularFile: Bool

    public init(name: String, fullPath: String, size: UInt64, mode: UInt32,
                isDirectory: Bool, isSymlink: Bool, mtime: Date?,
                uid: UInt32 = 0, gid: UInt32 = 0, hasOwner: Bool = false, hasSize: Bool = true, isRegularFile: Bool? = nil) {
        self.name = name
        self.fullPath = fullPath
        self.size = size
        self.mode = mode
        self.isDirectory = isDirectory
        self.isSymlink = isSymlink
        self.mtime = mtime
        self.uid = uid
        self.gid = gid
        self.hasOwner = hasOwner
        self.hasSize = hasSize
        self.isRegularFile = isRegularFile ?? (!isDirectory && !isSymlink)
    }
}

public struct SFTPStat {
    public let size: UInt64
    public let mode: UInt32
    public let isDirectory: Bool
    public let isSymlink: Bool
    public let mtime: Date?
    public let uid: UInt32
    public let gid: UInt32
    public let hasOwner: Bool
    public let hasSize: Bool
    public let isRegularFile: Bool

    public init(size: UInt64, mode: UInt32, isDirectory: Bool, isSymlink: Bool,
                mtime: Date?, uid: UInt32 = 0, gid: UInt32 = 0,
                hasOwner: Bool = false, hasSize: Bool = true, isRegularFile: Bool? = nil) {
        self.size = size
        self.mode = mode
        self.isDirectory = isDirectory
        self.isSymlink = isSymlink
        self.mtime = mtime
        self.uid = uid
        self.gid = gid
        self.hasOwner = hasOwner
        self.hasSize = hasSize
        self.isRegularFile = isRegularFile ?? (!isDirectory && !isSymlink)
    }
}

/// 递归枚举的一个条目（gt_sftp_walk 的 visitor 入参）。
public struct SFTPWalkEntry {
    public let path: String
    public let name: String
    public let stat: SFTPStat
}

/// 用 C 的 stat 结构构造 Swift 值。放在**文件作用域**：walk 的 C 回调是
/// @convention(c)，不能捕获 dynamic Self，故不能写成 SFTPSession 的方法。
private func sftpStat(_ st: gt_sftp_stat_t) -> SFTPStat {
    SFTPStat(
        size: st.size,
        mode: st.mode,
        isDirectory: st.is_dir,
        isSymlink: st.is_symlink,
        mtime: st.mtime_unix >= 0
            ? Date(timeIntervalSince1970: TimeInterval(st.mtime_unix)) : nil,
        uid: st.uid, gid: st.gid, hasOwner: st.has_owner,
        hasSize: st.has_size, isRegularFile: st.is_regular
    )
}

/// 一个 SFTP 会话。线程纪律同 SSH：全部调用放会话线程；job 的事件
/// 经 `Ghosterm.pollEvents` 低频收取。
public struct SFTPDirectory {
    public let path: String
    public let entries: [SFTPEntry]
}

public final class SFTPSession {
    nonisolated let handle: OpaquePointer
    private var closed = false

    init(handle: OpaquePointer) { self.handle = handle }

    /// 显式关闭（幂等）。**必须**在对应 SSHSession.close **之前**调用——
    /// libssh2_sftp_shutdown 依赖 session 仍存活；释放顺序由持有者管理。
    public func close() {
        guard !closed else { return }
        closed = true
        gt_sftp_close(handle)
    }

    /// 不在 deinit 里自动关：顺序（先 sftp 后 ssh）由持有者显式管理。

    /// 列目录（跳过 `.` / `..`，按 name 排序）。
    public func list(path: String) throws -> [SFTPEntry] {
        var entries: UnsafeMutablePointer<gt_sftp_entry_t>?
        var count: Int = 0
        let rc: Int32 = path.withCString { p in
            gt_sftp_list(handle, p, &entries, &count)
        }
        try check(rc)
        defer { gt_sftp_list_free(entries, count) }
        var out: [SFTPEntry] = []
        if let entries = entries {
            for i in 0..<count {
                let e = entries[i]
                let full = e.full_path.map { String(cString: $0) } ?? ""
                let name = (full as NSString).lastPathComponent
                out.append(SFTPEntry(
                    name: name,
                    fullPath: full,
                    size: e.stat.size,
                    mode: e.stat.mode,
                    isDirectory: e.stat.is_dir,
                    isSymlink: e.stat.is_symlink,
                    mtime: e.stat.mtime_unix >= 0
                        ? Date(timeIntervalSince1970: TimeInterval(e.stat.mtime_unix))
                        : nil,
                    uid: e.stat.uid, gid: e.stat.gid, hasOwner: e.stat.has_owner,
                    hasSize: e.stat.has_size, isRegularFile: e.stat.is_regular
                ))
            }
        }
        out.sort { $0.name < $1.name }
        return out
    }

    /// Complete raw file bytes, bounded to 1 MiB; no partial preview on failure.
    /// Dedicated owner only. The caller decides encoding and owns preview lifetime.
    public func preview(path: String, timeoutMilliseconds: UInt32,
                        maxBytes: Int = 256 * 1024, cancellation: SSHCancellation? = nil) throws -> Data {
        guard !path.isEmpty, !path.utf8.contains(0), maxBytes > 0, maxBytes <= 1048576 else {
            throw GhostermError.status(Int32(GT_ERR_INVALID_ARG.rawValue), "Invalid preview path or limit")
        }
        var bytes = [UInt8](repeating: 0, count: maxBytes)
        var count = 0
        let context = cancellation.map { Unmanaged.passUnretained($0).toOpaque() }
        let rc = withExtendedLifetime(cancellation) {
            path.withCString { p in
                gt_sftp_preview_controlled(handle, p, &bytes, bytes.count, &count,
                    timeoutMilliseconds, { pointer in
                        guard let pointer else { return false }
                        return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled
                    }, context)
            }
        }
        try check(rc)
        return Data(bytes.prefix(count))
    }

    /// Dedicated owner only. Core IO failure invalidates this SSH/SFTP pair.
    /// UTF-8 conversion can throw after a successful Core operation without aborting it.
    /// The budget includes realpath, the complete listing and directory close.
    public func browse(path: String, timeoutMilliseconds: UInt32,
                       maxEntries: Int = 65536, cancellation: SSHCancellation? = nil) throws -> SFTPDirectory {
        guard !path.isEmpty, !path.utf8.contains(0), maxEntries > 0, maxEntries <= 65536 else {
            throw GhostermError.status(Int32(GT_ERR_INVALID_ARG.rawValue), "Invalid browse path or limit")
        }
        var resolved = [CChar](repeating: 0, count: 65536)
        var entries: UnsafeMutablePointer<gt_sftp_entry_t>?
        var count = 0
        let context = cancellation.map { Unmanaged.passUnretained($0).toOpaque() }
        let rc = withExtendedLifetime(cancellation) {
            path.withCString { p in
                gt_sftp_browse_controlled(handle, p, &resolved, resolved.count, &entries, &count,
                    timeoutMilliseconds, maxEntries, { pointer in
                        guard let pointer else { return false }
                        return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled
                    }, context)
            }
        }
        defer { gt_sftp_list_free(entries, count) }
        try check(rc)
        guard let directory = String(validatingCString: resolved) else {
            throw GhostermError.failed("Remote directory path is not UTF-8")
        }
        var result: [SFTPEntry] = []
        if let entries {
            for i in 0..<count {
                let e = entries[i]
                guard let raw = e.full_path, let full = String(validatingCString: raw) else {
                    throw GhostermError.failed("Remote filename is not UTF-8")
                }
                let metadata = sftpStat(e.stat)
                result.append(SFTPEntry(name: (full as NSString).lastPathComponent,
                    fullPath: full, size: metadata.size, mode: metadata.mode,
                    isDirectory: metadata.isDirectory, isSymlink: metadata.isSymlink,
                    mtime: metadata.mtime, uid: metadata.uid, gid: metadata.gid,
                    hasOwner: metadata.hasOwner, hasSize: metadata.hasSize,
                    isRegularFile: metadata.isRegularFile))
            }
        }
        return SFTPDirectory(path: directory, entries: result)
    }

    public func stat(path: String) throws -> SFTPStat {
        var st = gt_sftp_stat_t()
        let rc: Int32 = path.withCString { p in
            gt_sftp_stat(handle, p, &st)
        }
        try check(rc)
        return SFTPStat(
            size: st.size,
            mode: st.mode,
            isDirectory: st.is_dir,
            isSymlink: st.is_symlink,
            mtime: st.mtime_unix >= 0
                ? Date(timeIntervalSince1970: TimeInterval(st.mtime_unix)) : nil,
            uid: st.uid, gid: st.gid, hasOwner: st.has_owner,
            hasSize: st.has_size, isRegularFile: st.is_regular
        )
    }

    /// lstat：**不跟随**符号链接。
    public func lstat(path: String) throws -> SFTPStat {
        var st = gt_sftp_stat_t()
        let rc: Int32 = path.withCString { gt_sftp_lstat(handle, $0, &st) }
        try check(rc)
        return sftpStat(st)
    }

    /// readlink：读符号链接目标。
    public func readlink(path: String) throws -> String {
        var buf = [CChar](repeating: 0, count: 4096)
        let rc: Int32 = path.withCString {
            gt_sftp_readlink(handle, $0, &buf, buf.count)
        }
        try check(rc)
        return String(cString: buf)
    }

    /// realpath：服务器端规范化路径。
    public func realpath(_ path: String) throws -> String {
        var buf = [CChar](repeating: 0, count: 4096)
        let rc: Int32 = path.withCString {
            gt_sftp_realpath(handle, $0, &buf, buf.count)
        }
        try check(rc)
        return String(cString: buf)
    }

    public func mkdir(path: String, mode: UInt32 = 0o755) throws {
        try check(path.withCString { gt_sftp_mkdir(handle, $0, mode) })
    }
    public func rmdir(path: String) throws {
        try check(path.withCString { gt_sftp_rmdir(handle, $0) })
    }
    /// 删除文件 / 符号链接本身（不跟随）。
    public func remove(path: String) throws {
        try check(path.withCString { gt_sftp_remove(handle, $0) })
    }
    /// 递归删除。
    public func removeAll(path: String) throws {
        try check(path.withCString { gt_sftp_remove_all(handle, $0) })
    }
    public func rename(from: String, to: String) throws {
        let rc: Int32 = from.withCString { f in
            to.withCString { t in gt_sftp_rename(handle, f, t) }
        }
        try check(rc)
    }
    public func chmod(path: String, mode: UInt32) throws {
        try check(path.withCString { gt_sftp_chmod(handle, $0, mode) })
    }

    /// 暂停 / 继续一个进行中的 job（协作式）。
    public func pause(jobID: UInt64, _ paused: Bool) throws {
        try check(gt_sftp_pause(handle, jobID, paused))
    }

    private final class WalkBox {
        let visitor: (SFTPWalkEntry) -> Bool
        init(_ v: @escaping (SFTPWalkEntry) -> Bool) { visitor = v }
    }

    /// 递归枚举（调用线程同步执行；visitor 返回 false = 请求停止 → 抛 CANCELLED）。
    public func walk(root: String, maxDepth: Int = -1,
                     visitor: @escaping (SFTPWalkEntry) -> Bool) throws {
        let box = WalkBox(visitor)
        let ud = Unmanaged.passUnretained(box).toOpaque()
        let cb: @convention(c) (UnsafeMutableRawPointer?,
                                UnsafePointer<gt_sftp_walk_ent_t>?) -> Int32 = {
            udPtr, entPtr in
            guard let udPtr, let entPtr else { return 1 }
            let box = Unmanaged<WalkBox>.fromOpaque(udPtr).takeUnretainedValue()
            let ent = entPtr.pointee
            let e = SFTPWalkEntry(
                path: ent.path.map { String(cString: $0) } ?? "",
                name: ent.name.map { String(cString: $0) } ?? "",
                stat: ent.stat.map { sftpStat($0.pointee) }
                    ?? SFTPStat(size: 0, mode: 0, isDirectory: false,
                                isSymlink: false, mtime: nil)
            )
            return box.visitor(e) ? 0 : 1
        }
        var rc: Int32 = Int32(GT_OK.rawValue)
        root.withCString { r in
            rc = gt_sftp_walk(handle, r, Int32(maxDepth), cb, ud)
        }
        try check(rc)
    }



    /// 发起下载 job。进度与结果经事件队列上报。
    @discardableResult
    public func download(remote: String, local: String) throws -> UInt64 {
        let id: UInt64 = remote.withCString { r in
            local.withCString { l in
                gt_sftp_download(handle, r, l)
            }
        }
        guard id != 0 else { throw GhostermError.failed(Ghosterm.lastError) }
        return id
    }

    /// 发起上传 job。
    @discardableResult
    public func upload(local: String, remote: String) throws -> UInt64 {
        let id: UInt64 = local.withCString { l in
            remote.withCString { r in
                gt_sftp_upload(handle, l, r)
            }
        }
        guard id != 0 else { throw GhostermError.failed(Ghosterm.lastError) }
        return id
    }

    /// 取消一个进行中的 job（协作式：已写部分保留）。
    public func cancel(jobID: UInt64) throws {
        try check(gt_sftp_cancel(handle, jobID))
    }
}

/// 低频事件（SFTP job 进度 / 结果 / 状态）。
public struct CoreEvent: Equatable {
    public enum Kind: Equatable { case status, progress, jobDone, log }
    public enum JobResult: Equatable { case ok, failed, cancelled, other(Int32) }

    public let kind: Kind
    public let sessionID: UInt64
    public let jobID: UInt64
    public let code: Int32
    public let message: String

    public init(kind: Kind, sessionID: UInt64, jobID: UInt64, code: Int32,
                message: String) {
        self.kind = kind
        self.sessionID = sessionID
        self.jobID = jobID
        self.code = code
        self.message = message
    }

    public var jobResult: JobResult {
        guard kind == .jobDone else { return .other(code) }
        switch code {
        case Int32(GT_JOB_OK.rawValue): return .ok
        case Int32(GT_JOB_FAILED.rawValue): return .failed
        case Int32(GT_JOB_CANCELLED.rawValue): return .cancelled
        default: return .other(code)
        }
    }
}

extension Ghosterm {
    /// 非阻塞取事件（批量消费）。
    public static func pollEvents(max: Int = 16) -> [CoreEvent] {
        guard max > 0 else { return [] }
        let buf = UnsafeMutablePointer<gt_event_t>.allocate(capacity: max)
        defer { buf.deallocate() }
        let n = gt_core_poll_events(buf, max)
        var out: [CoreEvent] = []
        for i in 0..<Int(n) {
            let e = buf[i]
            let kind: CoreEvent.Kind
            switch e.kind {
            case GT_EVENT_STATUS: kind = .status
            case GT_EVENT_PROGRESS: kind = .progress
            case GT_EVENT_JOB_DONE: kind = .jobDone
            case GT_EVENT_LOG: kind = .log
            default: kind = .status
            }
            let message = withUnsafeBytes(of: e.message) { raw -> String in
                guard let base = raw.baseAddress?.assumingMemoryBound(to: CChar.self) else {
                    return ""
                }
                return String(cString: base)
            }
            out.append(CoreEvent(
                kind: kind,
                sessionID: e.session_id,
                jobID: e.job_id,
                code: e.code,
                message: message
            ))
        }
        return out
    }
}

// MARK: - 字节流 Transport

public enum TransportKind: Equatable {
    case localPTY, sshShell
}

public final class Transport {
    let handle: OpaquePointer

    init(handle: OpaquePointer, sshOwner: SSHSession? = nil) {
        self.handle = handle; self.sshOwner = sshOwner
    }
    // C Transport 借用 SSH session，绑定必须保证 close 在 owner 释放前发生。
    // 包括取消连接后尚未交给 SessionController 的 transport。
    private let sshOwner: SSHSession?
    private var releaseOwner: DispatchQueue?
    /// Set before handing a newly opened transport to another thread.
    public func releaseOn(_ owner: DispatchQueue) { releaseOwner = owner }
    deinit {
        let raw = handle, ssh = sshOwner
        if let releaseOwner { releaseOwner.async { gt_transport_close(raw); withExtendedLifetime(ssh) {} } }
        else { gt_transport_close(raw) }
    }

    /// 起的是「用户的交互 shell」还是「一条具体命令」。
    ///
    /// 这**不能**从 `executable` 给没给推断：用户在设置里挑了 /bin/zsh，那依然
    /// 是他的交互 shell，照样要登录语义（macOS 上 PATH 全靠它）。
    public enum PTYCommandKind: Equatable {
        case shell   // 交互 shell → 起成登录 shell
        case direct  // 跑一条命令 → 不加任何登录语义

        var raw: Int32 { self == .shell ? 0 : 1 }
    }

    /// 打开本地 PTY（POSIX forkpty）。
    ///
    /// `kind` **故意不给默认值**：登录语义是个会静默影响 PATH 的决定，每个
    /// 调用点必须自己表态（铁律 #6）。
    public static func openPTY(executable: String? = nil, argv: [String] = [],
                               env: [String] = [], cwd: String? = nil,
                               term: String = "xterm-256color",
                               kind: PTYCommandKind,
                               cols: UInt16 = 80, rows: UInt16 = 24) throws -> Transport {
        let argvMut = argv.map { strdup($0) } + [nil]
        let envMut = env.map { strdup($0) } + [nil]
        let argvBuf = argvMut.map { $0.map { UnsafePointer($0) } }
        let envBuf = envMut.map { $0.map { UnsafePointer($0) } }
        defer { argvMut.forEach { if let p = $0 { free(p) } }
                envMut.forEach { if let p = $0 { free(p) } } }
        func open(_ exe: UnsafePointer<CChar>?, _ cwd: UnsafePointer<CChar>?,
                  _ term: UnsafePointer<CChar>) -> OpaquePointer? {
            argvBuf.withUnsafeBufferPointer { ab in
                envBuf.withUnsafeBufferPointer { eb in
                    ghosterm_open_pty_flat(exe, ab.baseAddress, eb.baseAddress,
                                           cwd, term, kind.raw, cols, rows)
                }
            }
        }
        var h: OpaquePointer?
        term.withCString { t in
            executable.withCStringOrNil { exe in
                cwd.withCStringOrNil { cw in
                    h = open(exe, cw, t)
                }
            }
        }
        guard let handle = h else { throw GhostermError.failed(Ghosterm.lastError) }
        return Transport(handle: handle)
    }

    /// 打开 SSH 交互式 shell（pty-req + shell）。
    public static func openSSHShell(_ session: SSHSession, term: String = "xterm-256color",
                                    cols: UInt16 = 80, rows: UInt16 = 24) throws -> Transport {
        var h: OpaquePointer?
        term.withCString { t in
            h = ghosterm_open_ssh_shell(session.handle, t, cols, rows)
        }
        guard let handle = h else { throw GhostermError.failed(Ghosterm.lastError) }
        return Transport(handle: handle, sshOwner: session)
    }

    /// Remaining connection budget; cancellation is observed on the calling IO owner.
    public static func openSSHShell(_ session: SSHSession, term: String = "xterm-256color",
                                    cols: UInt16 = 80, rows: UInt16 = 24,
                                    timeoutMilliseconds: UInt32,
                                    cancellation: SSHCancellation? = nil) throws -> Transport {
        guard !term.utf8.contains(0) else { throw GhostermError.failed("终端类型包含 NUL") }
        var size = gt_shell_opts_t(); size.cols = cols; size.rows = rows
        var status = GT_OK
        let context = cancellation.map { Unmanaged.passUnretained($0).toOpaque() }
        let raw = withExtendedLifetime(cancellation) {
            term.withCString { value in
                size.term = value
                return gt_transport_open_ssh_shell_controlled(session.handle, &size, timeoutMilliseconds,
                    { pointer in
                        guard let pointer else { return false }
                        return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled
                    }, context, &status)
            }
        }
        guard let raw else { throw GhostermError.status(Int32(status.rawValue), Ghosterm.lastError) }
        return Transport(handle: raw, sshOwner: session)
    }

    public static func openSSHCommand(_ session: SSHSession, command: String,
                                      cols: UInt16 = 80, rows: UInt16 = 24,
                                      timeoutMilliseconds: UInt32 = 5000) throws -> Transport {
        guard !command.utf8.contains(0) else { throw GhostermError.failed("SSH 命令包含 NUL") }
        var size = gt_shell_opts_t(); size.cols = cols; size.rows = rows
        var status = GT_OK
        let raw = command.withCString { gt_transport_open_ssh_command(session.handle, $0, &size, timeoutMilliseconds, nil, nil, &status) }
        guard let raw else { throw GhostermError.status(Int32(status.rawValue), Ghosterm.lastError) }
        return Transport(handle: raw, sshOwner: session)
    }

    public var kind: TransportKind {
        gt_transport_kind(handle) == GT_TRANSPORT_SSH_SHELL ? .sshShell : .localPTY
    }

    /// 前台作业判定（关闭确认的判据）。四态，**不折叠**（铁律 #6）：
    /// `.unknown` 表示「查不出来」（SSH shell 的远端前台本地看不见 / ConPTY /
    /// 查询失败）——调用方必须当「可能有作业」，绝不折叠成 `.idle`。
    public enum ChildState: Equatable {
        case unknown
        case exited
        case idle   // 前台进程组就是 shell 自己（停在提示符）
        case job    // 前台是别的进程组（有作业在跑）
    }

    public var childState: ChildState {
        switch gt_transport_child_state(handle) {
        case GT_TRANSPORT_CHILD_EXITED: return .exited
        case GT_TRANSPORT_CHILD_IDLE: return .idle
        case GT_TRANSPORT_CHILD_JOB: return .job
        default: return .unknown
        }
    }

    /// 子进程的当前工作目录（**本地 PTY 才有**）。SSH / 平台不支持 / 已退出
    /// 一律返回 nil——**不猜**（铁律 #6）。走内核查询，不依赖 shell 集成。
    public var childCwd: String? {
        var buf = [CChar](repeating: 0, count: 4096)   // 足够放 PATH_MAX
        guard gt_transport_child_cwd(handle, &buf, buf.count) else { return nil }
        return String(cString: buf)
    }

    /// 三态读：.data(>0) / .timeout / .eof / 抛错。超时不折叠成 EOF。
    public enum ReadResult { case data([UInt8]), timeout, eof }

    public func read(max: Int = 65536, timeoutMilliseconds: Int32 = 100) throws -> ReadResult {
        var buf = [UInt8](repeating: 0, count: max)
        let n = buf.withUnsafeMutableBufferPointer { b in
            gt_transport_read(handle, b.baseAddress, max, timeoutMilliseconds)
        }
        if n > 0 {
            return .data(Array(buf[0..<Int(n)]))
        } else if n == 0 {
            return .eof
        } else if n == -2 { // GT_TRANSPORT_READ_TIMEOUT
            return .timeout
        } else {
            throw GhostermError.failed(Ghosterm.lastError)
        }
    }

    public func write(_ bytes: [UInt8]) throws {
        let rc: Int32 = bytes.withUnsafeBufferPointer { b in
            gt_transport_write(handle, b.baseAddress, bytes.count)
        }
        try check(rc)
    }

    public func write(_ string: String) throws { try write(Array(string.utf8)) }

    public func resize(cols: UInt16, rows: UInt16,
                       pixelWidth: UInt32 = 0, pixelHeight: UInt32 = 0) throws {
        try check(gt_transport_resize(handle, cols, rows, pixelWidth, pixelHeight))
    }

    public func exited(exitCode: inout Int32) -> Bool {
        gt_transport_exited(handle, &exitCode)
    }
}

extension Optional where Wrapped == String {
    func withCStringOrNil<R>(_ body: (UnsafePointer<CChar>?) -> R) -> R {
        switch self {
        case .none: return body(nil)
        case .some(let s): return s.withCString { body($0) }
        }
    }
}

/// Source records only. Recognition is field shape, never a trust decision.
public struct KnownHostRecord: Equatable {
    public let sourceLine, byteStart, byteEnd: Int
    public let marker, hosts, keyType, keyBase64: String
    public let recognized, hashed: Bool
}

public enum KnownHosts {
    /// Always inspect committed even on failure; a successful rename cannot be
    /// represented as a rollback when later sync/readback fails.
    public struct UpdateResult: Equatable {
        public let status: Int32
        public let committed, conflict: Bool
        public let message: String
        public var succeeded: Bool { status == 0 }
    }

    public static func remove(path: String, expected: String, index: Int) -> UpdateResult {
        guard index >= 0, !path.utf8.contains(0) else {
            return UpdateResult(status: 1, committed: false, conflict: false, message: "主机验证记录参数无效")
        }
        var result = gt_known_hosts_update_result_t()
        let status = path.withCString { file in
            expected.withCString { revision in
                gt_known_hosts_remove(file, revision, expected.utf8.count, index, &result)
            }
        }
        return UpdateResult(status: Int32(status.rawValue), committed: result.committed,
            conflict: result.conflict, message: status == GT_OK ? "" : Ghosterm.lastError)
    }

    public static func records(_ text: String) throws -> [KnownHostRecord] {
        let snapshot = text.withCString { gt_known_hosts_parse($0, text.utf8.count) }
        guard let snapshot else { throw GhostermError.failed(Ghosterm.lastError) }
        defer { gt_known_hosts_free(snapshot) }
        return try (0..<gt_known_hosts_count(snapshot)).map { index in
            var record = gt_known_host_entry_t()
            try check(Int32(gt_known_hosts_entry_at(snapshot, index, &record).rawValue))
            return KnownHostRecord(sourceLine: Int(record.source_line), byteStart: Int(record.byte_start),
                byteEnd: Int(record.byte_end), marker: String(cString: record.marker), hosts: String(cString: record.hosts),
                keyType: String(cString: record.key_type), keyBase64: String(cString: record.key_base64),
                recognized: record.recognized, hashed: record.hashed)
        }
    }
}
