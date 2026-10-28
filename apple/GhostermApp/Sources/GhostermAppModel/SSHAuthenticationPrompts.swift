import Foundation
import GhostermCore

public struct SSHAuthenticationPrompts {
    public var credential: (UInt64, UInt32, @escaping (Result<SSHCredential, Swift.Error>) -> Void) -> Void
    public var hostkey: (UInt64, String, String, @escaping (HostKeyDecision) -> Void) -> Void
    public var cancelled: (UInt64) -> Void
    public var methods: (UInt64, Int32, Int32) -> Void
    public var accepted: (UInt64, UInt32) -> Void
    public init(credential: @escaping (UInt64, UInt32, @escaping (Result<SSHCredential, Swift.Error>) -> Void) -> Void,
                hostkey: @escaping (UInt64, String, String, @escaping (HostKeyDecision) -> Void) -> Void,
                cancelled: @escaping (UInt64) -> Void = { _ in },
                methods: @escaping (UInt64, Int32, Int32) -> Void = { _, _, _ in },
                accepted: @escaping (UInt64, UInt32) -> Void = { _, _ in }) {
        self.credential = credential; self.hostkey = hostkey; self.cancelled = cancelled; self.methods = methods; self.accepted = accepted
    }
}
