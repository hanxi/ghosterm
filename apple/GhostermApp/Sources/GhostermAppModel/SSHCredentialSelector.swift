// Platform credentials only. Disk/Keychain work has a dedicated owner; prompts
// and scope changes are on main. Connection configuration contains only references.
import Darwin
import Foundation
import GhostermCore
import Security

@MainActor final class SSHCredentialSelector {
    enum Reference: Equatable { case identity(String), password }
    struct Storage {
        var key: (String) throws -> String?
        var secret: (Reference, SSHConnectionTarget) throws -> String?
        var save: (Reference, SSHConnectionTarget, String) throws -> Void
        var remove: (Reference, SSHConnectionTarget) throws -> Void
        static var platform: Storage {
            Storage(key: readKey, secret: { ref, target in
                do {
                    let bytes = try KeychainStore.get(account: account(ref, target))
                    guard let text = String(data: bytes, encoding: .utf8) else { throw GhostermError.failed("钥匙串凭据格式无效") }
                    return text
                } catch KeychainStoreError.notFound { return nil }
            }, save: { ref, target, value in try KeychainStore.set(Data(value.utf8), account: account(ref, target)) }, remove: { ref, target in
                let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: KeychainStore.service, kSecAttrAccount as String: account(ref, target)]
                let status: OSStatus = try KeychainStore.withTimeout("native-remove") { SecItemDelete(query as CFDictionary) }
                guard status == errSecSuccess || status == errSecItemNotFound else { throw KeychainStoreError.unexpectedStatus(status) }
            })
        }
        private static func account(_ ref: Reference, _ target: SSHConnectionTarget) -> String {
            switch ref {
            case .identity(let path): return "native-passphrase/v1/" + path
            case .password:
                // Length-independent encoding avoids host/user/IPv6 separators
                // colliding. No secret is present in the Keychain account name.
                let bytes = try! JSONEncoder().encode([target.host, String(target.port), target.user])
                return "native-password/v1/" + bytes.base64EncodedString()
            }
        }
        private static func readKey(_ path: String) throws -> String? {
            let fd = Darwin.open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)
            if fd < 0, errno == ENOENT { return nil }
            guard fd >= 0 else { throw GhostermError.failed("无法读取私钥文件") }; defer { Darwin.close(fd) }
            var info = stat()
            guard fstat(fd, &info) == 0, info.st_mode & mode_t(S_IFMT) == mode_t(S_IFREG), info.st_size > 0,
                  info.st_size <= 1_048_576, info.st_uid == geteuid(), info.st_mode & 0o077 == 0 else {
                throw GhostermError.failed("私钥必须是当前用户的私有常规文件，且不超过 1 MiB")
            }
            var bytes = Data(), chunk = [UInt8](repeating: 0, count: 8192)
            while true {
                let count = chunk.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, $0.count) }
                if count < 0 { if errno == EINTR { continue }; throw GhostermError.failed("私钥读取失败") }
                if count == 0 { break }
                guard bytes.count + count <= 1_048_576 else { throw GhostermError.failed("私钥超过大小限制") }
                bytes.append(contentsOf: chunk.prefix(count))
            }
            guard let text = String(data: bytes, encoding: .utf8), !text.utf8.contains(0) else { throw GhostermError.failed("私钥文本格式无效") }
            return text
        }
    }
    private final class Request {
        let id: UInt64
        let cancellation = SSHCancellation()
        let done: (Result<SSHCredential, Swift.Error>) -> Void
        init(_ id: UInt64, _ done: @escaping (Result<SSHCredential, Swift.Error>) -> Void) { self.id = id; self.done = done }
    }
    private struct Selection { let id: UInt64; let reference: Reference; let secret: String? }
    private let target: SSHConnectionTarget
    private let storage: Storage
    private let ask: (CredentialPrompt.Kind) -> CredentialAnswer?
    private let hostkey: (String, String) -> HostKeyDecision
    private let notice: (String) -> Void
    private let io = DispatchQueue(label: "dev.ghosterm.native-credentials")
    private var offered: (UInt64, Int32, Int32)?
    private var pending: Request?
    private var lastID: UInt64 = 0
    private var paths: [String] = []
    private var cursor = 0
    private var lastReference: Reference?
    private var selection: Selection?
    private var preferred: Reference?
    private(set) var acceptedIdentityPaths: [String]?

    init(target: SSHConnectionTarget, storage: Storage = .platform,
         ask: @escaping (CredentialPrompt.Kind) -> CredentialAnswer?,
         hostkey: @escaping (String, String) -> HostKeyDecision, notice: @escaping (String) -> Void) {
        self.target = target; self.storage = storage; self.ask = ask; self.hostkey = hostkey; self.notice = notice
        // An empty saved list means password authentication. Config defaults are
        // expanded when a NEW target is built, never invented during recovery.
        paths = target.identityPaths
    }
    var prompts: SSHAuthenticationPrompts {
        .init(credential: { [self] id, attempt, done in begin(id, attempt, done) },
              hostkey: { [self] _, type, fingerprint, done in done(hostkey(type, fingerprint)) },
              cancelled: { [self] id in revoke(id) },
              methods: { [self] id, methods, error in offered = (id, methods, error) },
              accepted: { [self] id, _ in accept(id) })
    }
    private func revoke(_ id: UInt64) {
        if pending?.id == id { pending?.cancellation.cancel(); pending = nil }
        if selection?.id == id { selection = nil }
    }
    private func begin(_ id: UInt64, _ attempt: UInt32, _ done: @escaping (Result<SSHCredential, Swift.Error>) -> Void) {
        guard id > lastID else { return }; lastID = id
        pending?.cancellation.cancel(); let request = Request(id, done); pending = request
        guard let methods = offered, methods.0 == id, methods.1 != 0 else { finish(request, .failure(GhostermError.status(7, "SSH 认证方式尚未确认或不受支持"))); return }
        if attempt == 1 {
            cursor = 0; paths = target.identityPaths
            if case .identity(let path)? = preferred { paths.removeAll { $0 == path }; paths.insert(path, at: 0) }
        }
        if methods.2 == 4, let rejected = lastReference {
            let storage = storage, target = target
            io.async { [weak self] in do { try storage.remove(rejected, target) } catch { DispatchQueue.main.async { self?.notice("无法移除被拒绝的钥匙串缓存") } } }
        }
        if SSHAuthMethods(rawValue: methods.1).contains(.publicKey), (preferred != .password || !SSHAuthMethods(rawValue: methods.1).contains(.password)), cursor < paths.count { tryKey(request, methods: methods.1) }
        else { tryPassword(request, methods: methods.1) }
    }
    private func active(_ request: Request) -> Bool { pending === request && !request.cancellation.isCancelled }
    private func tryKey(_ request: Request, methods: Int32) {
        guard active(request) else { return }
        guard cursor < paths.count else { tryPassword(request, methods: methods); return }
        let path = paths[cursor]; cursor += 1; let storage = storage, target = target
        io.async { [weak self] in
            var warning: String?
            let value: (String, PrivateKeyPassphraseRequirement, String?)?
            do {
                if let pem = try storage.key(path) {
                    let requirement = Ghosterm.keyPassphraseRequirement(pem)
                    var cached: String?
                    if requirement == .needed { do { cached = try storage.secret(.identity(path), target) } catch { warning = "无法读取私钥口令缓存，可手动解锁" } }
                    value = (pem, requirement, cached)
                } else { value = nil }
            } catch { value = nil; warning = "私钥 \((path as NSString).lastPathComponent) 无法读取或权限不符合要求" }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.active(request) else { return }
                if let warning { self.notice(warning) }
                guard let (pem, requirement, cached) = value else { self.tryKey(request, methods: methods); return }
                if requirement == .unknown { self.notice("不支持的私钥格式：\((path as NSString).lastPathComponent)"); self.tryKey(request, methods: methods); return }
                var secret = cached, save: String?
                if requirement == .needed, secret == nil {
                    guard let answer = self.ask(.passphrase), let pass = answer.passphrase, !pass.isEmpty else { self.finish(request, .failure(GhostermError.status(8, "私钥解锁已取消"))); return }
                    guard self.active(request) else { return }; secret = pass; save = pass
                }
                self.deliver(request, ref: .identity(path), save: save, credential: .privateKey(pem: pem, passphrase: secret))
            }
        }
    }
    private func tryPassword(_ request: Request, methods: Int32) {
        guard active(request) else { return }
        guard SSHAuthMethods(rawValue: methods).contains(.password) else { finish(request, .failure(GhostermError.status(SSHAuthMethods(rawValue: methods).contains(.publicKey) ? 4 : 7, "服务器没有可用的私钥/密码认证方式；请检查身份文件或认证配置"))); return }
        let storage = storage, target = target
        io.async { [weak self] in
            var warning: String?, cached: String?
            do { cached = try storage.secret(.password, target) } catch { warning = "无法读取登录密码缓存，可手动输入" }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.active(request) else { return }
                if let warning { self.notice(warning) }
                if let cached, !cached.isEmpty { self.deliver(request, ref: .password, save: nil, credential: .password(cached)); return }
                let reason = self.target.identityPaths.isEmpty ? "服务器要求密码认证。" : "已检查私钥，改用服务器允许的密码认证。"
                guard let answer = self.ask(.password(reason: reason)), let password = answer.password, !password.isEmpty else { self.finish(request, .failure(GhostermError.status(8, "登录已取消"))); return }
                guard self.active(request) else { return }
                self.deliver(request, ref: .password, save: password, credential: .password(password))
            }
        }
    }
    private func deliver(_ request: Request, ref: Reference, save: String?, credential: SSHCredential) {
        guard active(request) else { return }; selection = .init(id: request.id, reference: ref, secret: save); lastReference = ref
        finish(request, .success(credential))
    }
    private func finish(_ request: Request, _ result: Result<SSHCredential, Swift.Error>) {
        guard active(request) else { return }; pending = nil; request.cancellation.cancel(); request.done(result)
    }
    private func accept(_ id: UInt64) {
        guard let value = selection, value.id == id else { return }; selection = nil; preferred = value.reference
        switch value.reference { case .identity(let path): acceptedIdentityPaths = [path]; case .password: acceptedIdentityPaths = [] }
        guard let secret = value.secret else { return }; let storage = storage, target = target
        io.async { [weak self] in do { try storage.save(value.reference, target, secret) } catch { DispatchQueue.main.async { self?.notice("认证成功，但无法保存钥匙串凭据，下次可能需要重新输入") } } }
    }
}
