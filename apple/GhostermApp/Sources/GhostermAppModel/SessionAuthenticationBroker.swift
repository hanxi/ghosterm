// Asynchronous native authentication glue for both terminal and catalog owners.
// UI receives only prompts; DNS and all public Core calls stay on the IO owner.
import Foundation
import GhostermCore

public final class SessionAuthenticationBroker {
    public struct Prompts {
        public var credential: (UInt64, UInt32, @escaping (Result<RecoverableSession.Credential, Swift.Error>) -> Void) -> Void
        public var hostkey: (UInt64, String, String, @escaping (HostKeyDecision) -> Void) -> Void
        public var cancelled: (UInt64) -> Void
        public var methods: (UInt64, Int32, Int32) -> Void
        public var accepted: (UInt64, UInt32) -> Void
        public init(credential: @escaping (UInt64, UInt32, @escaping (Result<RecoverableSession.Credential, Swift.Error>) -> Void) -> Void,
                    hostkey: @escaping (UInt64, String, String, @escaping (HostKeyDecision) -> Void) -> Void,
                    cancelled: @escaping (UInt64) -> Void = { _ in },
                    methods: @escaping (UInt64, Int32, Int32) -> Void = { _, _, _ in },
                    accepted: @escaping (UInt64, UInt32) -> Void = { _, _ in }) {
            self.credential = credential; self.hostkey = hostkey; self.cancelled = cancelled; self.methods = methods; self.accepted = accepted
        }
    }
    private let owner: DispatchQueue
    private let options: SSHOptions
    private let prompts: Prompts
    private let endpoint: (@escaping (RecoverableSession) -> Void) -> Void
    private let failure: (String) -> Void
    private var pending: UInt64?
    private var serial: UInt64 = 0
    private var flow: SessionAuthenticationFlow?
    /// endpoint must capture its terminal/catalog owner weakly. Every invocation
    /// is queued outside Core callbacks, including synchronous DNS/open failure.
    public init(owner: DispatchQueue, options: SSHOptions, prompts: Prompts,
                endpoint: @escaping (@escaping (RecoverableSession) -> Void) -> Void,
                failure: @escaping (String) -> Void) {
        self.owner = owner; self.options = options; self.prompts = prompts; self.endpoint = endpoint; self.failure = failure
    }
    public var callbacks: RecoverableSession.Authentication {
        .init(requested: { [self] request, attempt in begin(request, attempt: attempt) },
              cancelled: { [self] request in revoke(request) },
              hostkey: { [self] request, _, _ in unexpectedHostkey(request) })
    }
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    private func revoke(_ id: UInt64) {
        checkOwner(); guard pending == id else { return }
        pending = nil; let old = flow; flow = nil; old?.cancel()
    }
    private func begin(_ id: UInt64, attempt: UInt32) {
        checkOwner(); if let old = pending { revoke(old) }; pending = id
        // Core requested is a callback: construction/handshake/adoption starts
        // on the next owner turn so a failure cannot reenter that coordinator.
        owner.async { [weak self] in
            guard let self, self.pending == id else { return }
            let flow = SessionAuthenticationFlow(owner: self.owner, options: self.options, preferIPv6: attempt % 2 == 0, prompts: self.prompts,
                nextID: { [weak self] in
                    guard let self, self.serial < UInt64.max else { return nil }; self.serial += 1; return self.serial
                }) { [weak self] result in
                    guard let self, self.pending == id else { return }
                    self.pending = nil; self.flow = nil
                    switch result {
                    case .success(let authenticated):
                        self.endpoint { [weak self] session in
                            do { try authenticated.connection.adopt(into: session, request: id) }
                            catch { self?.report("已认证 SSH 连接无法交给原会话请求") }
                        }
                    case .failure(let error):
                        self.endpoint { session in
                            if error.status == 8 { session.cancel() }
                            else { _ = session.reject(request: id, error: error.status) }
                        }
                        if error.status != 3 { self.report(error.message) }
                    }
                }
            self.flow = flow; flow.start()
        }
    }
    private func unexpectedHostkey(_ id: UInt64) {
        checkOwner(); guard pending == id else { return }; revoke(id)
        owner.async { [weak self] in self?.endpoint { $0.cancel() } }
        report("SSH 认证来源不一致，连接已取消")
    }
    private func report(_ message: String) { DispatchQueue.main.async { [failure] in failure(message) } }
    deinit { if let flow { owner.async { flow.cancel() } } }
}
