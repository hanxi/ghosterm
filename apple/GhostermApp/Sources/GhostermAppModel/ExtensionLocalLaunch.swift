import CryptoKit
import Darwin
import Foundation

/// Explicit Native execution decision, separate from package source and Session
/// grants. The digest pins selected bytes, not publisher identity or an OS sandbox.
public struct ExtensionLocalLaunch: Sendable, Equatable {
    public let executableSHA256, workingDirectory, temporaryDirectory: String
    public let authorized: Bool
    public init(executableSHA256: String, workingDirectory: String, temporaryDirectory: String, authorized: Bool) {
        self.executableSHA256 = executableSHA256; self.workingDirectory = workingDirectory
        self.temporaryDirectory = temporaryDirectory; self.authorized = authorized
    }
    public var reviewDescription: String {
        "本机进程 · SHA-256：\(executableSHA256)\n工作目录：\(workingDirectory)\n临时根目录：\(temporaryDirectory)\n每次新建私有子目录，结束后清理其内容；不会清理根目录或工作目录。\n本次执行与临时清理：" + (authorized ? "已明确允许" : "未允许")
    }
    /// Runs only on the Native IO worker. Keeps the verified executable fd until
    /// construction returns; checks the path again before spawn. This does not
    /// defend against malicious same-UID replacement between check and execve.
    func withValidatedDirectory<T>(executable: String, _ body: (Int32, Int32) throws -> T) throws -> T {
        guard authorized, executable.hasPrefix("/"), executable.utf8.count <= 4096,
              !executable.utf8.contains(0), executableSHA256.utf8.count == 64,
              executableSHA256.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else {
            throw GhostermLocalLaunchError.invalid("本机程序需要绝对路径、精确 SHA-256 和本次明确允许")
        }
        let program = executable.withCString { open($0, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK) }
        guard program >= 0 else { throw GhostermLocalLaunchError.invalid("无法打开所选本机程序") }
        defer { Darwin.close(program) }
        var before = stat(), after = stat(), path = stat()
        guard fstat(program, &before) == 0, (before.st_mode & S_IFMT) == S_IFREG,
              before.st_uid == geteuid() || before.st_uid == 0,
              before.st_mode & 0o022 == 0, before.st_size > 0, before.st_size <= 64 * 1024 * 1024,
              access(executable, X_OK) == 0 else { throw GhostermLocalLaunchError.invalid("本机程序类型、权限或大小不符合要求") }
        var hash = SHA256(), buffer = [UInt8](repeating: 0, count: 32768)
        var total = 0
        while true {
            let size = Darwin.read(program, &buffer, buffer.count)
            if size < 0 && errno == EINTR { continue }
            guard size >= 0, total <= 64 * 1024 * 1024 - size else { throw GhostermLocalLaunchError.invalid("本机程序读取失败或发生变化") }
            if size == 0 { break }
            hash.update(data: Data(buffer.prefix(size))); total += size
        }
        let digest = hash.finalize().map { String(format: "%02x", $0) }.joined()
        guard digest == executableSHA256, fstat(program, &after) == 0,
              lstat(executable, &path) == 0, same(before, after), same(after, path), total == before.st_size else {
            throw GhostermLocalLaunchError.invalid("本机程序身份或摘要已变化，请重新核对并允许")
        }
        let directory = try openDirectory(workingDirectory)
        defer { Darwin.close(directory) }
        let temporary = try openDirectory(temporaryDirectory)
        defer { Darwin.close(temporary) }
        return try body(directory, temporary)
    }
    private func same(_ a: stat, _ b: stat) -> Bool {
        a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && a.st_mode == b.st_mode &&
        a.st_uid == b.st_uid && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
        a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec
    }
    private func openDirectory(_ path: String) throws -> Int32 {
        guard path.hasPrefix("/"), path.utf8.count <= 4096, !path.utf8.contains(0) else { throw GhostermLocalLaunchError.invalid("本机目录必须是明确的绝对路径") }
        let fd = path.withCString { open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) }
        guard fd >= 0 else { throw GhostermLocalLaunchError.invalid("无法打开所选本机目录") }
        return fd
    }
}
enum GhostermLocalLaunchError: Error { case invalid(String) }
