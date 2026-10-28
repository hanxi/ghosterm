// One native IO owner checks the host and methods before requesting unlocked
// material. Terminal and management connections share this flow and deadline.
import Foundation
import GhostermCore

final class SSHAuthenticationFlow {
    struct Authenticated { let connection: SSHSession; let attempt: UInt32 }
    struct Failure: Swift.Error, LocalizedError {
        let status: Int32, message: String
        var errorDescription: String? { message }
    }
    private let owner: DispatchQueue
    private let options: SSHOptions
    private let preferIPv6: Bool
    private let prompts: SSHAuthenticationPrompts
    private let nextID: () -> UInt64?
    private let completion: (Result<Authenticated, Failure>) -> Void
    private var resolver: SSHAddressResolver?
    private var authenticator: SSHAuthenticator?
    private var timer: DispatchSourceTimer?
    private var scope: SSHCancellation?
    private var request: UInt64 = 0
    private var attempt: UInt32 = 0
    private var deadline: TimeInterval = 0
    private var prompting = false
    private var started = false
    private var finished = false

    init(owner: DispatchQueue, options: SSHOptions, preferIPv6: Bool = false, prompts: SSHAuthenticationPrompts,
         nextID: @escaping () -> UInt64?, completion: @escaping (Result<Authenticated, Failure>) -> Void) {
        self.owner = owner; self.options = options; self.preferIPv6 = preferIPv6; self.prompts = prompts; self.nextID = nextID; self.completion = completion
    }
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    func start() {
        checkOwner(); guard !started, !finished else { return }; started = true
        deadline = ProcessInfo.processInfo.systemUptime + 60
        let timer = DispatchSource.makeTimerSource(queue: owner)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.tick() }; self.timer = timer; timer.resume()
        resolver = SSHAddressResolver.resolve(host: options.host, preferIPv6: preferIPv6, owner: owner) { [weak self] result in
            guard let self, !self.finished else { return }; self.resolver = nil
            do {
                let address = try result.get(), remaining = self.deadline - ProcessInfo.processInfo.systemUptime
                guard remaining > 0 else { self.fail(3, "SSH 认证超时"); return }
                self.authenticator = try SSHAuthenticator(options: self.options, address: address,
                    timeoutMilliseconds: UInt32(min(60000, max(1, remaining * 1000))), owner: self.owner)
            } catch let error as GhostermError {
                switch error {
                case .status(let code, _): self.fail(code, error.description)
                case .failed(let message): self.fail(9, message)
                }
            } catch { self.fail(3, "无法解析 SSH 地址") }
        }
    }
    private func revokePrompt() {
        scope?.cancel(); scope = nil; prompting = false
        if request > 0 { let old = request; request = 0; DispatchQueue.main.async { [prompts] in prompts.cancelled(old) } }
    }
    private func newPrompt() -> (UInt64, SSHCancellation)? {
        revokePrompt()
        guard let id = nextID(), id > 0 else { fail(9, "SSH 认证请求编号耗尽"); return nil }
        let scope = SSHCancellation(); self.scope = scope; request = id; prompting = true
        return (id, scope)
    }
    private func tick() {
        checkOwner(); guard !finished else { return }
        guard ProcessInfo.processInfo.systemUptime < deadline else { fail(3, "SSH 认证超时"); return }
        guard let authenticator, let info = step(authenticator) else { return }
        switch info.phase {
        case .hostkey:
            guard !prompting, let (id, scope) = newPrompt() else { return }
            DispatchQueue.main.async { [weak self, scope] in
                guard let self, !scope.isCancelled else { return }
                self.prompts.hostkey(id, info.keyType, info.fingerprint) { [weak self, scope] decision in
                    self?.owner.async { [weak self, scope] in
                        guard let self, !self.finished, self.request == id, self.prompting, !scope.isCancelled else { return }
                        self.prompting = false; scope.cancel()
                        if decision == .unknown { self.fail(8, "未确认主机密钥，SSH 连接已取消"); return }
                        let status = self.authenticator?.decide(decision) ?? 8
                        if status != 0 { self.fail(status, decision == .reject ? "主机密钥已拒绝" : "无法保存主机密钥裁决") }
                    }
                }
            }
        case .credentials:
            guard !prompting else { return }
            guard attempt < 33 else { fail(4, "SSH 认证候选已用尽"); return }
            guard let (id, scope) = newPrompt() else { return }; attempt += 1
            let current = attempt
            DispatchQueue.main.async { [weak self, scope] in
                guard let self, !scope.isCancelled else { return }
                self.prompts.methods(id, info.methods, info.error)
                self.prompts.credential(id, current) { [weak self, scope] result in
                    self?.owner.async { [weak self, scope] in
                        guard let self, !self.finished, self.request == id, self.prompting, !scope.isCancelled else { return }
                        // Consume once before any Core call or error completion.
                        self.prompting = false; scope.cancel()
                        do {
                            guard let authenticator = self.authenticator else { self.fail(9, "SSH 认证器已撤销"); return }
                            try authenticator.supply(options: self.options, credential: result.get())
                        }
                        catch let error as GhostermError {
                            switch error {
                            case .status(let code, _): self.fail(code, error.description)
                            case .failed(let message): self.fail(8, message)
                            }
                        } catch { self.fail(8, "SSH 认证已取消或无法取得凭据") }
                    }
                }
            }
        case .ready:
            guard let connection = authenticator.take() else { fail(9, "无法取得已认证 SSH 连接"); return }
            let id = request, accepted = attempt
            DispatchQueue.main.async { [prompts] in prompts.accepted(id, accepted) }
            finish(.success(.init(connection: connection, attempt: accepted)))
        case .failed: fail(info.error, info.host == .mismatch ? "SSH 主机密钥已改变，连接已拒绝" : "SSH 连接或认证失败（\(info.error)）")
        case .cancelled: fail(8, "SSH 认证已取消")
        default: break
        }
    }
    private func step(_ auth: SSHAuthenticator) -> SSHAuthenticator.Info? {
        _ = auth.step(); guard let info = auth.info else { fail(9, "无法读取 SSH 认证状态"); return nil }; return info
    }
    func cancel() { checkOwner(); fail(8, "SSH 认证已取消") }
    private func fail(_ status: Int32, _ message: String) { finish(.failure(.init(status: status, message: message))) }
    private func finish(_ result: Result<Authenticated, Failure>) {
        guard !finished else { return }; finished = true; timer?.cancel(); timer = nil
        resolver?.cancel(); resolver = nil; revokePrompt(); authenticator?.close(); authenticator = nil
        completion(result)
    }
    deinit {
        timer?.cancel(); scope?.cancel()
        if let resolver { owner.async { resolver.cancel() } }
    }
}
