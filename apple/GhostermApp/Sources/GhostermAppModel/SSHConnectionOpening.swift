import Foundation
import GhostermCore

/// One admission lifetime, from the original UI request through authenticated
/// PTY/shell or SFTP handoff. Cancel is independent of the busy IO owner.
final class SSHConnectionOpening: @unchecked Sendable {
    let cancellation = SSHCancellation()
    let deadline: TimeInterval
    private let now: () -> TimeInterval
    init(timeout: TimeInterval = 60, now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime }) {
        self.now = now; self.deadline = now() + timeout
    }
    func cancel() { cancellation.cancel() }
    func remainingMilliseconds(maximum: UInt32 = 60_000) throws -> UInt32 {
        guard !cancellation.isCancelled else { throw GhostermError.status(8, "SSH 连接打开已取消") }
        let remaining = deadline - now()
        guard remaining.isFinite, remaining > 0, maximum > 0 else {
            throw GhostermError.status(3, "SSH 连接打开超时")
        }
        return UInt32(min(Double(maximum), max(1, remaining * 1000)))
    }
}

/// Tests can observe the actual boundary arguments while delegating to these
/// production bindings. No host authentication or channel fallback lives here.
struct SSHOpeningOperations {
    var shell: (SSHSession, UInt32, SSHCancellation) throws -> Transport = {
        try Transport.openSSHShell($0, timeoutMilliseconds: $1, cancellation: $2)
    }
    var sftp: (SSHSession, UInt32, SSHCancellation) throws -> SFTPSession = {
        try $0.sftp(timeoutMilliseconds: $1, cancellation: $2)
    }
}
