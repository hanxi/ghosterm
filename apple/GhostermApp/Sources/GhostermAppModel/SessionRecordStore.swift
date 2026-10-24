// Durable metadata only: never terminal output, launch arguments/environment,
// passwords, unlocked keys, or attachment tokens. One non-UI storage owner.
import Darwin
import Foundation

public struct SessionRecord: Codable, Equatable, Identifiable {
    public enum Kind: String, Codable { case local, ssh }
    public enum Disposition: String, Codable { case session, stopPending, stopUnknown, stopRejected, ended }
    public struct Endpoint: Codable, Equatable {
        public var proxy, daemon, runtime, contract: String
        public init(proxy: String, daemon: String, runtime: String, contract: String) {
            self.proxy = proxy; self.daemon = daemon; self.runtime = runtime; self.contract = contract
        }
    }
    public struct Target: Codable, Equatable {
        public var alias, host, user: String
        public var port: UInt16
        public var knownHosts: String?
        // Keychain is addressed by the same host/port and identity paths as the
        // existing credential store. These are references, never key contents.
        public var identityPaths: [String]
        public init(alias: String, host: String, user: String, port: UInt16, knownHosts: String?, identityPaths: [String]) {
            self.alias = alias; self.host = host; self.user = user; self.port = port
            self.knownHosts = knownHosts; self.identityPaths = identityPaths
        }
    }
    public let id: UUID
    public var kind: Kind
    public var endpoint: Endpoint
    public var target: Target?
    public var nonce: [UInt8]?
    public var instance: [UInt8]
    public var session: UInt64
    public var admitted: Bool
    public var disposition: Disposition
    public var resultKnown: Bool
    public var exitCode: Int32?
    public var stopStatus: UInt32?
    public var stopError: Int32?
    public init(id: UUID = UUID(), kind: Kind, endpoint: Endpoint, target: Target? = nil,
                nonce: [UInt8]?, instance: [UInt8] = Array(repeating: 0, count: 16), session: UInt64 = 0,
                admitted: Bool = false, disposition: Disposition = .session, resultKnown: Bool = false, exitCode: Int32? = nil,
                stopStatus: UInt32? = nil, stopError: Int32? = nil) {
        self.id = id; self.kind = kind; self.endpoint = endpoint; self.target = target; self.nonce = nonce
        self.instance = instance; self.session = session; self.admitted = admitted; self.disposition = disposition
        self.resultKnown = resultKnown; self.exitCode = exitCode; self.stopStatus = stopStatus; self.stopError = stopError
    }
    fileprivate func validate() throws {
        func text(_ s: String) -> Bool { !s.isEmpty && s.utf8.count <= 1024 && !s.unicodeScalars.contains { $0.value < 32 || $0.value == 127 } }
        func path(_ s: String) -> Bool { s.hasPrefix("/") && text(s) }
        guard path(endpoint.proxy), path(endpoint.daemon), path(endpoint.runtime),
              endpoint.contract.utf8.count == 64, endpoint.contract.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
              instance.count == 16, nonce == nil || (nonce!.count == 16 && nonce!.contains(where: { $0 != 0 })),
              (session == 0 ? nonce != nil : instance.contains(where: { $0 != 0 })),
              !admitted || (nonce != nil && instance.contains(where: { $0 != 0 })),
              (!resultKnown || (session > 0 && disposition == .ended)), exitCode == nil || resultKnown,
              stopStatus == nil || stopStatus! <= 4, stopError == nil || (0...9).contains(stopError!),
              disposition != .stopUnknown || stopStatus == nil,
              disposition != .stopRejected || stopStatus == 2 || stopStatus == 3 || stopStatus == 4,
              disposition == .session || admitted || session > 0 else { throw SessionRecordStore.Error.invalid }
        if kind == .ssh {
            guard let target, text(target.alias), text(target.host), text(target.user), target.port > 0,
                  target.knownHosts == nil || path(target.knownHosts!), target.identityPaths.count <= 32,
                  target.identityPaths.allSatisfy({ path($0) && !$0.contains("PRIVATE KEY") }) else { throw SessionRecordStore.Error.invalid }
        } else if target != nil { throw SessionRecordStore.Error.invalid }
    }
    fileprivate func preserves(_ old: SessionRecord) -> Bool {
        sameAuthority(as: old) && nonce == old.nonce &&
        (old.instance.allSatisfy { $0 == 0 } || instance == old.instance) &&
        (old.session == 0 || session == old.session) && (!old.admitted || admitted) &&
        (!old.resultKnown || resultKnown) && (old.exitCode == nil || exitCode == old.exitCode) &&
        (old.disposition != .ended || disposition == .ended) &&
        (disposition != .session || old.disposition == .session)
    }
    fileprivate func sameAuthority(as other: SessionRecord) -> Bool {
        kind == other.kind && endpoint.runtime == other.endpoint.runtime && endpoint.contract == other.endpoint.contract &&
        target?.host == other.target?.host && target?.user == other.target?.user && target?.port == other.target?.port
    }
    fileprivate func conflicts(with other: SessionRecord) -> Bool {
        if id == other.id { return false }
        if session > 0 && session == other.session && instance == other.instance { return true }
        return nonce != nil && nonce == other.nonce && sameAuthority(as: other) &&
            (instance.allSatisfy { $0 == 0 } || other.instance.allSatisfy { $0 == 0 } || instance == other.instance)
    }
}

public final class SessionRecordStore {
    public enum Error: Swift.Error, LocalizedError {
        case invalid, unsafe, busy, corrupt, unsupported, full, io(Int32), publicationUnknown
        public var errorDescription: String? {
            switch self {
            case .invalid: return "会话记录身份或字段无效"
            case .unsafe: return "会话记录路径或权限不安全"
            case .busy: return "其他进程正在更新会话记录"
            case .corrupt: return "会话记录损坏；已保留原文件"
            case .unsupported: return "会话记录版本不受支持；已保留原文件"
            case .full: return "会话记录超过容量限制"
            case .io(let code): return "会话记录读写失败（\(code)）"
            case .publicationUnknown: return "会话记录已重命名，但持久化结果未知"
            }
        }
    }
    private struct Envelope: Codable { var format: Int; var records: [SessionRecord] }
    private struct SourceKey: Hashable { let instance: [UInt8]; let session: UInt64 }
    private struct CreationKey: Hashable {
        let kind, runtime, contract, host, user: String
        let port: UInt16
        let nonce: [UInt8]
    }
    private static let maxBytes = 1_048_576
    private static let maxRecords = 1024
    private let directory: URL
    private let owner: DispatchQueue
    public init(directory: URL, owner: DispatchQueue) { self.directory = directory; self.owner = owner }
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    private func openDirectory(create: Bool = true) throws -> Int32 {
        let path = directory.path
        guard path.hasPrefix("/"), !path.utf8.contains(0), getuid() == geteuid(), getgid() == getegid() else { throw Error.unsafe }
        let components = path.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
        guard !components.isEmpty, !components.contains("."), !components.contains("..") else { throw Error.unsafe }
        var fd = Darwin.open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)
        guard fd >= 0 else { throw Error.io(errno) }
        do {
            for (index, component) in components.enumerated() {
                var next = component.withCString { openat(fd, $0, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW) }
                if next < 0, errno == ENOENT, create {
                    let made = component.withCString { mkdirat(fd, $0, 0o700) }
                    guard made == 0 || errno == EEXIST else { throw Error.io(errno) }
                    next = component.withCString { openat(fd, $0, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW) }
                }
                guard next >= 0 else { throw Error.unsafe }
                var info = stat()
                let safe = fstat(next, &info) == 0 && (info.st_mode & mode_t(S_IFMT)) == mode_t(S_IFDIR) &&
                    (info.st_uid == 0 || info.st_uid == geteuid()) && (info.st_mode & 0o022) == 0
                let final = index == components.count - 1
                guard safe, !final || (info.st_uid == geteuid() && (info.st_mode & 0o077) == 0) else { Darwin.close(next); throw Error.unsafe }
                Darwin.close(fd); fd = next
            }
            return fd
        } catch { Darwin.close(fd); throw error }
    }
    private func privateFile(_ fd: Int32) -> Bool {
        var info = stat()
        return fstat(fd, &info) == 0 && (info.st_mode & mode_t(S_IFMT)) == mode_t(S_IFREG) &&
            info.st_uid == geteuid() && info.st_nlink == 1 && (info.st_mode & 0o077) == 0
    }
    private func transaction<T>(_ body: (Int32) throws -> T) throws -> T {
        checkOwner(); let dir = try openDirectory(); defer { Darwin.close(dir) }
        let lock = openat(dir, "sessions.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0o600)
        guard lock >= 0 else { throw Error.unsafe }; defer { Darwin.close(lock) }
        guard privateFile(lock) else { throw Error.unsafe }
        guard flock(lock, LOCK_EX | LOCK_NB) == 0 else {
            if errno == EWOULDBLOCK || errno == EAGAIN { throw Error.busy }; throw Error.io(errno)
        }
        defer { _ = flock(lock, LOCK_UN) }
        return try body(dir)
    }
    private func read(_ dir: Int32) throws -> [SessionRecord] {
        let fd = openat(dir, "sessions.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
        if fd < 0, errno == ENOENT { return [] }
        guard fd >= 0 else { throw Error.unsafe }; defer { Darwin.close(fd) }
        guard privateFile(fd) else { throw Error.unsafe }
        var info = stat(); guard fstat(fd, &info) == 0 else { throw Error.io(errno) }
        guard info.st_size >= 0, info.st_size <= Self.maxBytes else { throw Error.full }
        var data = Data(), buffer = [UInt8](repeating: 0, count: 8192)
        data.reserveCapacity(Int(info.st_size))
        while true {
            let n = buffer.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
            if n < 0 { if errno == EINTR { continue }; throw Error.io(errno) }
            if n == 0 { break }
            guard data.count <= Self.maxBytes - n else { throw Error.full }
            data.append(contentsOf: buffer.prefix(n))
        }
        let envelope: Envelope
        do { envelope = try JSONDecoder().decode(Envelope.self, from: data) } catch { throw Error.corrupt }
        guard envelope.format == 1 else { throw Error.unsupported }
        guard envelope.records.count <= Self.maxRecords else { throw Error.full }
        var ids = Set<UUID>(), sources = Set<SourceKey>()
        var creations: [CreationKey: Set<[UInt8]>] = [:]
        let unknown = Array(repeating: UInt8(0), count: 16)
        for record in envelope.records {
            try record.validate(); guard ids.insert(record.id).inserted else { throw Error.corrupt }
            if record.session > 0 {
                guard sources.insert(SourceKey(instance: record.instance, session: record.session)).inserted else { throw Error.corrupt }
            }
            if let nonce = record.nonce {
                let key = CreationKey(kind: record.kind.rawValue, runtime: record.endpoint.runtime, contract: record.endpoint.contract,
                                      host: record.target?.host ?? "", user: record.target?.user ?? "", port: record.target?.port ?? 0, nonce: nonce)
                let prior = creations[key] ?? []
                guard !prior.contains(record.instance), !prior.contains(unknown), record.instance != unknown || prior.isEmpty else { throw Error.corrupt }
                creations[key, default: []].insert(record.instance)
            }
        }
        return envelope.records
    }
    private func write(_ records: [SessionRecord], _ dir: Int32) throws {
        guard records.count <= Self.maxRecords else { throw Error.full }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(Envelope(format: 1, records: records.sorted { $0.id.uuidString < $1.id.uuidString }))
        guard data.count <= Self.maxBytes else { throw Error.full }
        let name = ".sessions-" + UUID().uuidString
        let fd = name.withCString { openat(dir, $0, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0o600) }
        guard fd >= 0 else { throw Error.io(errno) }
        defer { Darwin.close(fd); _ = name.withCString { unlinkat(dir, $0, 0) } }
        try data.withUnsafeBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                let n = Darwin.write(fd, bytes.baseAddress!.advanced(by: offset), bytes.count - offset)
                if n < 0 { if errno == EINTR { continue }; throw Error.io(errno) }
                guard n > 0 else { throw Error.io(EIO) }; offset += n
            }
        }
        guard fsync(fd) == 0 else { throw Error.io(errno) }
        // Re-resolve the path before publication so a renamed directory cannot
        // silently receive a durable record under an abandoned pathname.
        let current = try openDirectory(create: false); defer { Darwin.close(current) }
        var a = stat(), b = stat()
        guard fstat(dir, &a) == 0, fstat(current, &b) == 0, a.st_dev == b.st_dev, a.st_ino == b.st_ino else { throw Error.unsafe }
        let renamed = name.withCString { renameat(dir, $0, dir, "sessions.json") }
        guard renamed == 0 else { throw Error.io(errno) }
        guard fsync(dir) == 0 else { throw Error.publicationUnknown }
    }
    public func load() throws -> [SessionRecord] { try transaction { try read($0) } }
    public func upsert(_ record: SessionRecord) throws {
        try record.validate()
        try transaction { dir in
            var records = try read(dir)
            guard !records.contains(where: { record.conflicts(with: $0) }) else { throw Error.invalid }
            if let index = records.firstIndex(where: { $0.id == record.id }) {
                guard record.preserves(records[index]) else { throw Error.invalid }
                records[index] = record
            } else { records.append(record) }
            try write(records, dir)
        }
    }
    public func forget(_ id: UUID) throws {
        try transaction { dir in var records = try read(dir); records.removeAll { $0.id == id }; try write(records, dir) }
    }
}
