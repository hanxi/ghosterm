import Foundation
import GhostermCore

protocol SSHAuthenticationOperation: AnyObject { func start(); func cancel() }
extension SSHAuthenticationFlow: SSHAuthenticationOperation {}

/// Ordinary terminal/files authentication, with one selector and trust identity
/// per hop. DNS resolves only the first hop; all inner names go to direct-tcpip.
final class SSHRouteAuthenticationFlow: SSHAuthenticationOperation {
    typealias Failure = SSHAuthenticationFlow.Failure
    typealias Authenticated = SSHAuthenticationFlow.Authenticated
    private let owner: DispatchQueue
    private let options: [SSHOptions]
    private let prompts: [SSHAuthenticationPrompts]
    private let lookup: SSHAuthenticationFlow.AddressLookup
    private let now: () -> TimeInterval
    private let makeRoute: ([SSHRoute.Hop], UInt32, DispatchQueue) throws -> SSHRoute
    private let nextID: () -> UInt64?
    private let completion: (Result<Authenticated, Failure>) -> Void
    private var route: SSHRoute?
    private var resolver: SSHAddressResolver?
    private var timer: DispatchSourceTimer?
    private var attempts: [UInt32], requests: [UInt64]
    private var scope: SSHCancellation?
    private var hop = 0
    private var prompting = false, started = false, finished = false
    private var deadline: TimeInterval = 0
    init(owner: DispatchQueue, options: [SSHOptions], prompts: [SSHAuthenticationPrompts],
         lookup: @escaping SSHAuthenticationFlow.AddressLookup = { host, ipv6, owner, completion in
             SSHAddressResolver.resolve(host: host, preferIPv6: ipv6, owner: owner, completion: completion)
         }, now: @escaping () -> TimeInterval = { ProcessInfo.processInfo.systemUptime },
         makeRoute: @escaping ([SSHRoute.Hop], UInt32, DispatchQueue) throws -> SSHRoute = {
             try SSHRoute(hops: $0, timeoutMilliseconds: $1, owner: $2)
         }, nextID: @escaping () -> UInt64?, completion: @escaping (Result<Authenticated, Failure>) -> Void) {
        self.owner = owner; self.options = options; self.prompts = prompts; self.lookup = lookup
        self.nextID = nextID; self.completion = completion; self.now = now; self.makeRoute = makeRoute
        attempts = Array(repeating: 0, count: options.count); requests = Array(repeating: 0, count: options.count)
    }
    private func checkOwner() { dispatchPrecondition(condition: .onQueue(owner)); precondition(!Thread.isMainThread) }
    func start() {
        checkOwner(); guard !started, !finished else { return }; started = true
        guard (2...8).contains(options.count), prompts.count == options.count else { fail(1, "SSH 跳板路线无效"); return }
        deadline = now() + 60
        let timer = DispatchSource.makeTimerSource(queue: owner)
        timer.schedule(deadline: .now(), repeating: .milliseconds(10), leeway: .milliseconds(1))
        timer.setEventHandler { [weak self] in self?.tick() }; self.timer = timer; timer.resume()
        resolver = lookup(options[0].host, false, owner) { [weak self] result in
            guard let self, !self.finished else { return }; self.resolver = nil
            do {
                let address = try result.get(), remaining = self.deadline - self.now()
                guard remaining > 0 else { self.fail(3, "SSH 跳板认证超时"); return }
                let hops = self.options.enumerated().map { index, option in
                    SSHRoute.Hop(options: option, address: index == 0 ? address : nil, viaHost: index == 0 ? nil : option.host)
                }
                self.route = try self.makeRoute(hops, UInt32(min(60000, max(1, remaining * 1000))), self.owner)
            } catch { self.fail(error) }
        }
    }
    private func newPrompt() -> (UInt64, SSHCancellation)? {
        scope?.cancel()
        let previous = requests[hop], prompt = prompts[hop]
        if previous > 0 { DispatchQueue.main.async { prompt.cancelled(previous) } }
        guard let id = nextID(), id > 0 else { fail(9, "SSH 认证请求编号耗尽"); return nil }
        let scope = SSHCancellation(); self.scope = scope; requests[hop] = id; prompting = true
        return (id, scope)
    }
    private func active(_ id: UInt64, _ index: Int, _ scope: SSHCancellation) -> Bool {
        !finished && hop == index && requests[index] == id && prompting && !scope.isCancelled
    }
    private func tick() {
        checkOwner(); guard !finished else { return }
        guard now() < deadline else { fail(3, "SSH 跳板认证超时"); return }
        guard let route else { return }
        _ = route.step()
        guard let info = route.info, Int(info.hop) < options.count else { fail(9, "无法读取跳板认证状态"); return }
        if hop != Int(info.hop) { scope?.cancel(); scope = nil; prompting = false; hop = Int(info.hop) }
        switch info.phase {
        case .failed:
            let host = info.authentication.host
            fail(info.error, "第\(hop + 1)跳 " + (host == .revoked ? "SSH 主机密钥已撤销" : host == .mismatch ? "SSH 主机密钥已改变" : "SSH 连接或认证失败（\(info.error)）")); return
        case .cancelled: fail(8, "SSH 跳板认证已取消"); return
        case .ready:
            guard let session = route.take() else { fail(9, "无法取得跳板 SSH 会话"); return }
            let requests = requests, attempts = attempts, prompts = prompts
            DispatchQueue.main.async { for index in prompts.indices { prompts[index].accepted(requests[index], attempts[index]) } }
            finish(.success(.init(connection: session, attempt: attempts[hop]))); return
        case .forwarding: return
        case .authenticating: break
        }
        let auth = info.authentication, index = hop, prompt = prompts[hop]
        if auth.phase == .hostkey {
            guard !prompting, let (id, scope) = newPrompt() else { return }
            DispatchQueue.main.async { [weak self, scope] in
                guard let self, !scope.isCancelled else { return }
                prompt.hostkey(id, auth.keyType, auth.fingerprint) { [weak self, scope] decision in
                    self?.owner.async { [weak self, scope] in
                        guard let self, self.active(id, index, scope) else { return }
                        self.prompting = false; scope.cancel()
                        guard decision != .unknown else { self.fail(8, "未确认跳板主机密钥，连接已取消"); return }
                        let status = self.route?.decide(decision) ?? 8
                        if status != 0 { self.fail(status, decision == .reject ? "主机密钥已拒绝" : "无法保存主机信任记录，连接已停止") }
                    }
                }
            }
        } else if auth.phase == .credentials {
            guard !prompting else { return }
            guard attempts[index] < 33 else { fail(4, "本跳 SSH 认证候选已用尽"); return }
            guard let (id, scope) = newPrompt() else { return }; attempts[index] += 1
            let attempt = attempts[index]
            DispatchQueue.main.async { [weak self, scope] in
                guard let self, !scope.isCancelled else { return }
                prompt.methods(id, auth.methods, auth.error)
                prompt.credential(id, attempt) { [weak self, scope] result in
                    self?.owner.async { [weak self, scope] in
                        guard let self, self.active(id, index, scope) else { return }
                        self.prompting = false; scope.cancel()
                        do {
                            guard let route = self.route else { self.fail(9, "跳板认证器已撤销"); return }
                            try route.supply(options: self.options[index], credential: result.get())
                        } catch { self.fail(error) }
                    }
                }
            }
        }
    }
    func cancel() { checkOwner(); fail(8, "SSH 跳板认证已取消") }
    private func fail(_ error: Error) {
        if error is SSHAddressResolver.Failure { fail(3, "无法解析 SSH 跳板首跳地址") }
        else if case GhostermError.status(let status, let message) = error { fail(status, message) }
        else { fail(8, error.localizedDescription) }
    }
    private func fail(_ status: Int32, _ message: String) { finish(.failure(.init(status: status, message: message))) }
    private func finish(_ result: Result<Authenticated, Failure>) {
        guard !finished else { return }; finished = true
        timer?.cancel(); timer = nil; resolver?.cancel(); resolver = nil; scope?.cancel(); scope = nil
        let requests = requests, prompts = prompts
        DispatchQueue.main.async { for index in prompts.indices where index < requests.count && requests[index] > 0 { prompts[index].cancelled(requests[index]) } }
        route?.close(); route = nil; completion(result)
    }
    deinit {
        timer?.cancel(); scope?.cancel()
        if let resolver { owner.async { resolver.cancel() } }
    }
}
