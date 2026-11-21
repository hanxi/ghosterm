import CryptoKit
import Darwin
import Foundation

/// Private local selection, never a source decision or a capability grant.
/// Cooperates with install transactions using the same directory flock.
final class PluginReleaseCatalog {
    static let selectionName = "selection-v1.json"
    static let removalName = "removal-v1.jsonl"
    enum Failure: String, Error, LocalizedError {
        case root, busy, changed, invalid, conflict, inactive, io
        var errorDescription: String? {
            switch self {
            case .root: return "无法读取私有安装根目录。"
            case .busy: return "安装目录正由另一项操作使用，请完成或取消后重试。"
            case .changed: return "安装目录或文件身份已变化，请刷新后重新核对。"
            case .invalid: return "已安装版本或活动版本记录无效，不能据此授权新运行。"
            case .conflict: return "活动版本已被另一项操作修改，请刷新后重试。"
            case .inactive: return "此版本未被选为活动版本，或已禁用；请先核对并选择活动版本。"
            case .io: return "无法写入或同步活动版本记录。"
            }
        }
    }
    struct Choice: Sendable, Equatable {
        let releaseID: String
        let enabled: Bool
        let generation: String
    }
    struct Selection: Sendable, Equatable {
        let revision: String?
        var choices: [String: Choice]
    }
    struct Version: Sendable, Equatable {
        let releaseID: String, pluginID: String, version: String, publisher: String, packageSHA256: String
        let inUse: Bool?
        var requiresChannelPolicy = false
    }
    struct Inventory {
        let versions: [Version]
        let issues: [String]
        let maintenanceCount: Int
    }
    final class VersionHandle {
        let fd: Int32
        let id: String
        init(_ fd: Int32, id: String) { self.fd = fd; self.id = id }
        deinit { Darwin.close(fd) }
        func read(_ name: String, limit: Int) throws -> Data { try PluginReleaseCatalog.read(fd, name: name, limit: limit) }
        func requiredChannelRoot() throws -> String? {
            var info = stat()
            if fstatat(fd, "channel-policy-required", &info, AT_SYMLINK_NOFOLLOW) != 0 {
                guard errno == ENOENT else { throw Failure.invalid }
                return nil
            }
            let digest = String(decoding: try read("channel-policy-required", limit: 64), as: UTF8.self)
            guard PluginReleaseCatalog.digest(digest) else { throw Failure.invalid }
            return digest
        }
        func pin() throws {
            guard flock(fd, LOCK_SH | LOCK_NB) == 0 else { throw Failure.busy }
        }
        func exclusiveUse() throws -> Bool {
            if flock(fd, LOCK_EX | LOCK_NB) == 0 { return true }
            guard errno == EWOULDBLOCK else { throw Failure.io }; return false
        }
        func useObservation() -> Bool? {
            do {
                guard try exclusiveUse() else { return true }
                _ = flock(fd, LOCK_UN); return false
            } catch { return nil }
        }
        func payload() throws -> Int32 {
            let result = openat(fd, "payload", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
            guard result >= 0 else { throw Failure.invalid }
            var value = stat()
            guard fstat(result, &value) == 0, value.st_uid == geteuid(), value.st_mode & 0o222 == 0 else {
                Darwin.close(result); throw Failure.invalid
            }
            return result
        }
    }
    let url: URL
    private var fd: Int32 = -1
    private var identity = stat()
    private let exclusive: Bool
    private let parentSync: (Int32) -> Int32
    init?(directory: URL, exclusive: Bool, parentSync: @escaping (Int32) -> Int32 = { fsync($0) }) throws {
        self.url = directory; self.exclusive = exclusive; self.parentSync = parentSync
        guard directory.isFileURL, directory.path.utf8.count <= 3500, !directory.path.utf8.contains(0) else { throw Failure.root }
        fd = open(directory.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        if fd < 0 && errno == ENOENT { return nil }
        guard fd >= 0 else { throw Failure.root }
        guard fstat(fd, &identity) == 0, identity.st_uid == geteuid(), identity.st_mode & 0o077 == 0 else {
            Darwin.close(fd); fd = -1; throw Failure.root
        }
        guard flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == 0 else {
            Darwin.close(fd); fd = -1; throw Failure.busy
        }
    }
    deinit { if fd >= 0 { Darwin.close(fd) } }
    init(borrowing stage: PluginReleaseStage) throws {
        url = stage.rootURL; exclusive = false; parentSync = { fsync($0) }
        fd = try stage.catalogLease()
        guard fstat(fd, &identity) == 0, identity.st_uid == geteuid(), identity.st_mode & 0o077 == 0 else {
            Darwin.close(fd); fd = -1; throw Failure.root
        }
    }
    func validate() throws {
        let current = open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard current >= 0 else { throw Failure.changed }
        defer { Darwin.close(current) }
        var value = stat()
        guard fstat(current, &value) == 0, value.st_dev == identity.st_dev, value.st_ino == identity.st_ino,
              value.st_uid == geteuid(), value.st_mode & 0o077 == 0 else { throw Failure.changed }
    }
    static func digest(_ value: String) -> Bool {
        value.utf8.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
    private static func pluginID(_ value: String) -> Bool {
        value.utf8.count <= 128 && value.range(of: "^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$", options: .regularExpression) == value.startIndex..<value.endIndex
    }
    static func sha(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    static func read(_ parent: Int32, name: String, limit: Int) throws -> Data {
        let file = openat(parent, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)
        guard file >= 0 else { throw Failure.invalid }
        defer { Darwin.close(file) }
        var before = stat(), after = stat()
        guard fstat(file, &before) == 0, before.st_mode & S_IFMT == S_IFREG,
              before.st_uid == geteuid(), before.st_size >= 0, before.st_size <= limit else { throw Failure.invalid }
        var result = Data(), bytes = [UInt8](repeating: 0, count: 8192)
        while true {
            let count = bytes.withUnsafeMutableBytes { Darwin.read(file, $0.baseAddress, $0.count) }
            if count < 0 && errno == EINTR { continue }
            guard count >= 0 else { throw Failure.io }; if count == 0 { break }
            guard result.count <= limit - count else { throw Failure.invalid }
            result.append(contentsOf: bytes.prefix(count))
        }
        guard fstat(file, &after) == 0, before.st_dev == after.st_dev, before.st_ino == after.st_ino,
              before.st_size == after.st_size, result.count == before.st_size,
              before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec, before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec,
              before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec, before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec else { throw Failure.changed }
        return result
    }
    func openVersion(_ id: String) throws -> VersionHandle {
        guard Self.digest(id) else { throw Failure.invalid }
        try validate()
        let child = openat(fd, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard child >= 0 else { throw Failure.invalid }
        var value = stat()
        guard fstat(child, &value) == 0, value.st_uid == geteuid(), value.st_mode & 0o222 == 0 else {
            Darwin.close(child); throw Failure.invalid
        }
        return VersionHandle(child, id: id)
    }
    func inventory() throws -> Inventory {
        try validate()
        let copy = fcntl(fd, F_DUPFD_CLOEXEC, 3)
        guard copy >= 0 else { throw Failure.io }
        guard let stream = fdopendir(copy) else { Darwin.close(copy); throw Failure.io }
        defer { closedir(stream) }
        rewinddir(stream)
        var versions: [Version] = [], issues: [String] = [], count = 0, maintenance = 0
        while true {
            errno = 0
            guard let item = readdir(stream) else { guard errno == 0 else { throw Failure.io }; break }
            let name = withUnsafePointer(to: &item.pointee.d_name) { $0.withMemoryRebound(to: CChar.self, capacity: 1024) { String(cString: $0) } }
            if name == "." || name == ".." || name == Self.selectionName || name == Self.removalName || name == PluginInstallJournal.directoryName { continue }
            count += 1; guard count <= 128 else { throw Failure.invalid }
            guard Self.digest(name) else { maintenance += 1; continue }
            do {
                let handle = try openVersion(name), descriptor = try handle.read("release.json", limit: 65536)
                guard Self.sha(descriptor) == name, try handle.read("release.sig", limit: 64).count == 64 else { throw Failure.invalid }
                let identity = try PluginDirectorySignatureVerifier.declaration(descriptor)
                versions.append(Version(releaseID: name, pluginID: identity.id, version: identity.version,
                    publisher: identity.publisher, packageSHA256: identity.packageSHA256, inUse: handle.useObservation(), requiresChannelPolicy: try handle.requiredChannelRoot() != nil))
            } catch { issues.append("\(name)：\(error)") }
        }
        try validate()
        return Inventory(versions: versions.sorted { $0.releaseID < $1.releaseID }, issues: issues, maintenanceCount: maintenance)
    }
    func validateVersion(_ id: String, handle: VersionHandle, payload: Int32) throws {
        try validate()
        var expected = stat(), actual = stat()
        guard fstat(handle.fd, &expected) == 0, fstatat(fd, id, &actual, AT_SYMLINK_NOFOLLOW) == 0,
              expected.st_dev == actual.st_dev, expected.st_ino == actual.st_ino,
              fstat(payload, &expected) == 0, fstatat(handle.fd, "payload", &actual, AT_SYMLINK_NOFOLLOW) == 0,
              expected.st_dev == actual.st_dev, expected.st_ino == actual.st_ino else { throw Failure.changed }
    }
    func selection() throws -> Selection {
        try validate()
        var value = stat()
        if fstatat(fd, Self.selectionName, &value, AT_SYMLINK_NOFOLLOW) != 0 {
            guard errno == ENOENT else { throw Failure.invalid }
            return Selection(revision: nil, choices: [:])
        }
        let raw = try Self.read(fd, name: Self.selectionName, limit: 131072)
        do { return try decodeSelection(raw) }
        catch { throw Failure.invalid }
    }
    private func decodeSelection(_ raw: Data) throws -> Selection {
        let object = try ReleaseJSON.parse(raw).object()
        guard Set(object.keys) == ["schemaVersion", "nonce", "choices"],
              case .number("1")? = object["schemaVersion"],
              let nonce = try object["nonce"]?.string(), UUID(uuidString: nonce) != nil,
              let choices = try object["choices"]?.object(), choices.count <= 64 else { throw Failure.invalid }
        var result: [String: Choice] = [:]
        for (id, value) in choices {
            let fields = try value.object()
            guard Self.pluginID(id), Set(fields.keys) == ["releaseID", "enabled", "generation"],
                  let releaseID = try fields["releaseID"]?.string(), Self.digest(releaseID),
                  let enabled = try fields["enabled"]?.bool(), let generation = try fields["generation"]?.string(),
                  UUID(uuidString: generation) != nil else { throw Failure.invalid }
            result[id] = Choice(releaseID: releaseID, enabled: enabled, generation: generation)
        }
        return Selection(revision: Self.sha(raw), choices: result)
    }
    func writeSelection(_ proposed: Selection, expected: String?, beforeRename: (() throws -> Void)? = nil) throws -> (Selection, Bool) {
        guard exclusive else { throw Failure.io }
        try validate()
        guard try selection().revision == expected else { throw Failure.conflict }
        var choices: [String: Any] = [:]
        for (id, choice) in proposed.choices {
            guard Self.pluginID(id), Self.digest(choice.releaseID), UUID(uuidString: choice.generation) != nil else { throw Failure.invalid }
            choices[id] = ["releaseID": choice.releaseID, "enabled": choice.enabled, "generation": choice.generation]
        }
        guard choices.count <= 64 else { throw Failure.invalid }
        let raw = try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "nonce": UUID().uuidString, "choices": choices], options: [.sortedKeys])
        guard raw.count <= 131072 else { throw Failure.invalid }
        let durable = try publishRecord(raw, as: Self.selectionName) {
            try beforeRename?()
            guard try selection().revision == expected else { throw Failure.conflict }
        }
        return (Selection(revision: Self.sha(raw), choices: proposed.choices), durable)
    }

    private struct ChannelHistory: Codable {
        let root: String
        let generation: UInt64
        let digest: String
        let observed: UInt64
        let envelope: PluginDirectorySignatureVerifier.ChannelEnvelope?
    }

    // JSON encodes the bounded 64 KiB statement and 128 KiB policy as base64.
    private static let channelHistoryLimit = 270_000
    private static func channelHistoryName(_ channel: String) -> String {
        "channel-policy-v1-" + sha(Data(channel.utf8)) + ".json"
    }

    func readChannelEnvelope(_ channel: String) throws -> (envelope: PluginDirectorySignatureVerifier.ChannelEnvelope, generation: UInt64, digest: String, root: String, observed: UInt64) {
        try validate()
        let raw = try Self.read(fd, name: Self.channelHistoryName(channel), limit: Self.channelHistoryLimit)
        let record = try JSONDecoder().decode(ChannelHistory.self, from: raw)
        guard let envelope = record.envelope else { throw PluginDirectorySignatureVerifier.Failure.channelHistory }
        return (envelope, record.generation, record.digest, record.root, record.observed)
    }

    func recordChannelPolicy(_ policy: PluginDirectorySignatureVerifier.VerifiedChannelPolicy, now: UInt64,
                             requireHistory: Bool = false, beforeRename: (() throws -> Void)? = nil) throws {
        guard exclusive else { throw Failure.io }
        try validate()
        guard policy.issuedAt <= now, now < policy.expiresAt else {
            throw PluginDirectorySignatureVerifier.Failure.channelFreshness
        }
        let name = Self.channelHistoryName(policy.channel)
        var info = stat()
        if fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) == 0 {
            let raw = try Self.read(fd, name: name, limit: Self.channelHistoryLimit)
            let previous = try JSONDecoder().decode(ChannelHistory.self, from: raw)
            typealias Verifier = PluginDirectorySignatureVerifier
            guard previous.root == policy.rootSHA256, previous.observed > 0, now >= previous.observed,
                  let envelope = previous.envelope,
                  let verified = try? Verifier.verifyChannelPolicy(statement: envelope.statement,
                      signature: envelope.signature, policy: envelope.policy, channel: policy.channel,
                      channelRoot: policy.channelRoot, now: previous.observed),
                  previous.generation == verified.generation, previous.digest == verified.statementSHA256,
                  policy.generation >= verified.generation,
                  policy.generation != verified.generation || policy.statementSHA256 == verified.statementSHA256 else {
                throw Verifier.Failure.channelHistory
            }
        } else {
            guard errno == ENOENT else { throw Failure.io }
            guard !requireHistory else { throw PluginDirectorySignatureVerifier.Failure.channelHistory }
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = .withoutEscapingSlashes
        let raw = try encoder.encode(ChannelHistory(root: policy.rootSHA256, generation: policy.generation,
            digest: policy.statementSHA256, observed: now, envelope: policy.envelope))
        guard raw.count <= Self.channelHistoryLimit else { throw Failure.invalid }
        guard try publishRecord(raw, as: name, beforeRename: { try beforeRename?() }) else { throw Failure.io }
    }

    private func publishRecord(_ raw: Data, as destination: String, beforeRename: () throws -> Void = {}) throws -> Bool {
        let name = ".selection-" + UUID().uuidString
        let output = openat(fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
        guard output >= 0 else { throw Failure.io }
        var published = false
        defer { Darwin.close(output); if !published { _ = unlinkat(fd, name, 0) } }
        try raw.withUnsafeBytes { bytes in
            var at = 0
            while at < bytes.count {
                let count = Darwin.write(output, bytes.baseAddress!.advanced(by: at), bytes.count - at)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw Failure.io }; at += count
            }
        }
        guard fsync(output) == 0 else { throw Failure.io }
        try validate(); try beforeRename()
        guard renameat(fd, name, fd, destination) == 0 else { throw Failure.io }
        published = true
        return parentSync(fd) == 0
    }
    func removalRoot() throws -> Int32 {
        guard exclusive else { throw Failure.io }
        try validate()
        let root = open(url.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard root >= 0 else { throw Failure.root }
        var value = stat()
        guard fstat(root, &value) == 0, value.st_dev == identity.st_dev, value.st_ino == identity.st_ino else {
            Darwin.close(root); throw Failure.changed
        }
        return root
    }
    func journalRoot() throws -> Int32 {
        try validate()
        let copy = fcntl(fd, F_DUPFD_CLOEXEC, 3)
        guard copy >= 0 else { throw Failure.io }; return copy
    }
    func hasPendingRemoval() throws -> Bool {
        try validate()
        var value = stat()
        if fstatat(fd, Self.removalName, &value, AT_SYMLINK_NOFOLLOW) == 0 { return true }
        guard errno == ENOENT else { throw Failure.io }; return false
    }
}
