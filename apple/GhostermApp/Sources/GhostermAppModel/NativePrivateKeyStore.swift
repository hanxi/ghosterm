import Darwin
import Foundation
import GhostermCore
import Security

/// IdentityFile is an import reference. Authentication material comes back from
/// Keychain, including the first use; Ghosterm never creates a plaintext key file.
enum NativePrivateKeyStore {
    struct Access {
        var get: (String) throws -> String
        var insert: (String, String) throws -> Void
        var replace: (String, String) throws -> Void
        static let system = Access(get: KeychainStore.privateKeyPEM,
            insert: { try KeychainStore.insertPrivateKeyPEM($0, forPath: $1) },
            replace: { try KeychainStore.setPrivateKeyPEM($0, forPath: $1) })
    }
    struct ImportIO {
        var read: (Int32, UnsafeMutableRawPointer?, Int) -> Int = { Darwin.read($0, $1, $2) }
    }
    static func load(_ path: String, access: Access = .system, io: ImportIO = ImportIO(), cancellation: SSHCancellation? = nil) throws -> String? {
        try validateReference(path)
        try checkCancellation(cancellation)
        do {
            let cached = try checked(access.get(path))
            try checkCancellation(cancellation)
            return cached
        }
        catch KeychainStoreError.notFound { }
        try checkCancellation(cancellation)
        guard let imported = try readImport(path, io: io, cancellation: cancellation) else { return nil }
        try checkCancellation(cancellation)
        do { try access.insert(imported, path) }
        catch KeychainStoreError.unexpectedStatus(let status) where status == errSecDuplicateItem {
            // Another import won. Use the stored material; never overwrite it
            // with a stale file snapshot or hand the file's bytes to Core.
        }
        try checkCancellation(cancellation)
        let stored = try checked(access.get(path))
        try checkCancellation(cancellation)
        return stored
    }

    private static func checkCancellation(_ cancellation: SSHCancellation?) throws {
        if cancellation?.isCancelled == true { throw GhostermError.status(8, "私钥读取已取消") }
    }

    /// Explicit user action: replaces the stored material, not the source file.
    static func reimport(_ path: String, access: Access = .system, io: ImportIO = ImportIO()) throws {
        try validateReference(path)
        guard let imported = try readImport(path, io: io) else { throw GhostermError.failed("私钥来源文件不存在，钥匙串原记录保留") }
        try access.replace(imported, path)
        let stored = try checked(access.get(path))
        guard Data(stored.utf8) == Data(imported.utf8) else {
            throw GhostermError.failed("钥匙串已提交，但回读材料不一致；请核对后再连接")
        }
    }

    private static func validateReference(_ path: String) throws {
        guard !path.utf8.contains(0), !path.contains("\n"), !path.contains("\r"), path.utf8.count <= 65536 else {
            throw GhostermError.failed("私钥引用无效")
        }
        if path.hasPrefix("ghosterm-identity:") {
            throw GhostermError.status(7, "此身份属于 Android 加密容器；请在 macOS 重新导入并关联本机私钥")
        }
        guard path.hasPrefix("/") else {
            throw GhostermError.status(7, "macOS 私钥导入需要绝对路径或已展开的 ~ 路径，请重新关联身份文件")
        }
    }

    private static func checked(_ text: String) throws -> String {
        guard !text.utf8.contains(0), !text.isEmpty, text.utf8.count <= 1_048_576,
              Ghosterm.keyPassphraseRequirement(text) != .unknown else {
            throw GhostermError.failed("钥匙串私钥材料格式无效；请从来源文件重新导入")
        }
        return text
    }

    private static func readImport(_ path: String, io: ImportIO, cancellation: SSHCancellation? = nil) throws -> String? {
        let fd = Darwin.open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)
        if fd < 0, errno == ENOENT { return nil }
        guard fd >= 0 else { throw GhostermError.failed("无法打开私钥来源文件") }
        defer { Darwin.close(fd) }
        var before = stat()
        guard fstat(fd, &before) == 0, before.st_mode & mode_t(S_IFMT) == mode_t(S_IFREG),
              before.st_size > 0, before.st_size <= 1_048_576,
              before.st_uid == geteuid(), before.st_mode & 0o077 == 0 else {
            throw GhostermError.failed("私钥来源必须是当前用户的私有常规文件，且不超过 1 MiB")
        }
        var bytes = Data(), chunk = [UInt8](repeating: 0, count: 8192)
        defer { bytes.resetBytes(in: 0..<bytes.count); chunk.withUnsafeMutableBytes { $0.initializeMemory(as: UInt8.self, repeating: 0) } }
        while true {
            try checkCancellation(cancellation)
            let count = chunk.withUnsafeMutableBytes { io.read(fd, $0.baseAddress, $0.count) }
            if count < 0, errno == EINTR { continue }
            guard count >= 0 else { throw GhostermError.failed("私钥来源读取失败") }
            if count == 0 { break }
            guard bytes.count + count <= 1_048_576 else { throw GhostermError.failed("私钥来源超过大小限制") }
            bytes.append(contentsOf: chunk.prefix(count))
        }
        try checkCancellation(cancellation)
        var after = stat(), current = stat()
        guard fstat(fd, &after) == 0, lstat(path, &current) == 0,
              same(before, after), same(after, current), after.st_size == bytes.count,
              let text = String(data: bytes, encoding: .utf8) else {
            throw GhostermError.failed("私钥来源在读取期间变化或不是有效 UTF-8；未导入")
        }
        return try checked(text)
    }

    private static func same(_ a: stat, _ b: stat) -> Bool {
        a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_mode == b.st_mode &&
        a.st_uid == b.st_uid && a.st_gid == b.st_gid && a.st_size == b.st_size &&
        a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
        a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec
    }
}
