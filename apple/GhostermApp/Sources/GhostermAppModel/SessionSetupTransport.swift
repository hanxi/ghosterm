// Exclusive setup Transport. Only prebuilt Linux payloads are uploaded; the
// long-lived session obtains a fresh authenticated connection afterwards.
import Foundation
import GhostermCore

public final class SessionSetupTransport {
    public struct Installed {
        public let linux: SessionSetupConnection.LinuxInfo
        public let installation: SessionSetupConnection.Installation
        public let endpoint: SessionRecord.Endpoint
        public let acceptedAttempt: UInt32
    }
    public enum Failure: Swift.Error, LocalizedError {
        case cancelled(SessionSetupConnection.Installation?)
        case authentication(Int32), unsupportedArchitecture, invalidBundle, timedOut
        case installation(SessionSetupConnection.Installation)
        public var errorDescription: String? {
            switch self {
            case .cancelled(let result): return result?.publicationUnknown == true ? "连接已取消，远程安装结果未知；请重新探测" : "SSH 连接已取消"
            case .authentication(let code): return "SSH 认证或连接失败（\(code)）"
            case .unsupportedArchitecture: return "应用未包含此 Linux 架构的会话安装包"
            case .invalidBundle: return "会话安装包或协议标识无效"
            case .timedOut: return "准备远程会话环境超时"
            case .installation(let result): return result.publicationUnknown ? "远程安装结果未知；请重新探测" : "远程会话安装失败（\(result.status)）"
            }
        }
    }
    public let owner = DispatchQueue(label: "dev.ghosterm.session-setup")
    private let options: SSHOptions
    private let bundles: [String: URL]
    private let contract: String
    private let installRoot: String?
    private let runtimeDirectory: String?
    private let prompts: SessionAuthenticationBroker.Prompts
    private let progress: (String) -> Void
    private let completion: (Result<Installed, Swift.Error>) -> Void
    private let cancellation = SessionCancellation()
    private var flow: SessionAuthenticationFlow?
    private var connection: SessionSetupConnection?
    private var serial: UInt64 = 0
    private var attempt: UInt32 = 0
    private var started = false
    private var finished = false
    private var installing = false
    private var installation: SessionSetupConnection.Installation?

    public init(options: SSHOptions, bundles: [String: URL], contract: String,
                installRoot: String? = nil, runtimeDirectory: String? = nil,
                prompts: SessionAuthenticationBroker.Prompts,
                progress: @escaping (String) -> Void = { _ in },
                completion: @escaping (Result<Installed, Swift.Error>) -> Void) {
        self.options = options; self.bundles = bundles; self.contract = contract
        self.installRoot = installRoot; self.runtimeDirectory = runtimeDirectory
        self.prompts = prompts; self.progress = progress; self.completion = completion
    }
    public func start() {
        owner.async { [self] in
            guard !started, !finished else { return }; started = true
            guard !cancellation.isCancelled else { finish(.failure(Failure.cancelled(nil))); return }
            report("正在连接 SSH…")
            let flow = SessionAuthenticationFlow(owner: owner, options: options, prompts: prompts,
                nextID: { [weak self] in
                    guard let self, self.serial < UInt64.max else { return nil }; self.serial += 1; return self.serial
                }) { [weak self] result in
                    guard let self, !self.finished else { return }
                    self.flow = nil
                    switch result {
                    case .failure(let error): self.finish(.failure(error))
                    case .success(let authenticated):
                        guard !self.cancellation.isCancelled else { self.finish(.failure(Failure.cancelled(nil))); return }
                        self.connection = authenticated.connection; self.attempt = authenticated.attempt; self.installing = true
                        self.install(authenticated.connection)
                    }
                }
            self.flow = flow; flow.start()
        }
    }
    public func cancel() {
        cancellation.cancel()
        owner.async { [self] in if !installing { finish(.failure(Failure.cancelled(installation))) } }
    }
    private func install(_ connection: SessionSetupConnection) {
        // Dedicated setup owner may block in these bounded operations; it never
        // owns a terminal stream. cancel's atomic token remains callable on UI.
        do {
            report("正在检查远程会话环境…")
            let setupDeadline = ProcessInfo.processInfo.systemUptime + 120
            func remaining() throws -> UInt32 {
                let milliseconds = (setupDeadline - ProcessInfo.processInfo.systemUptime) * 1000
                guard milliseconds > 0 else { throw Failure.timedOut }
                return UInt32(min(120000, max(1, milliseconds)))
            }
            let linux = try connection.probe(cancellation: cancellation, timeoutMilliseconds: remaining())
            guard let bundle = bundles[linux.arch] else { throw Failure.unsupportedArchitecture }
            let header = try String(contentsOf: bundle.appendingPathComponent("manifest.tsv"), encoding: .utf8).split(separator: "\n", omittingEmptySubsequences: false).first?.split(separator: "\t", omittingEmptySubsequences: false)
            guard let header, header.count == 6, header[0] == "GTB1", header[2] == Substring(linux.arch), header[5] == Substring(contract),
                  contract.utf8.count == 64, contract.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else { throw Failure.invalidBundle }
            report("正在准备远程会话服务…")
            let root = installRoot ?? (linux.home + "/.local/share/ghosterm/sessiond")
            let result = connection.install(bundleDirectory: bundle.path, root: root, cancellation: cancellation, timeoutMilliseconds: try remaining()) { [weak self] value in
                switch value.stage {
                case .upload:
                    if value.total > 0 { self?.report("正在上传会话服务：\(value.bytes)/\(value.total)") }
                case .verify:
                    self?.report("正在校验远程会话服务…")
                case .activate:
                    self?.report("正在激活远程会话服务…")
                case .done:
                    self?.report("远程会话服务已就绪")
                case .probe, nil: break
                }
            }
            installation = result
            guard !cancellation.isCancelled else { throw Failure.cancelled(result) }
            guard result.status == 0 else { throw Failure.installation(result) }
            let endpoint = SessionRecord.Endpoint(proxy: result.proxy, daemon: result.daemon,
                runtime: runtimeDirectory ?? (linux.home + "/.local/state/ghosterm/" + contract.prefix(16)), contract: contract)
            finish(.success(Installed(linux: linux, installation: result, endpoint: endpoint, acceptedAttempt: attempt)))
        } catch { finish(.failure(error)) }
    }
    private func report(_ message: String) { DispatchQueue.main.async { [progress] in progress(message) } }
    private func finish(_ result: Result<Installed, Swift.Error>) {
        guard !finished else { return }; finished = true
        let old = flow; flow = nil; old?.cancel(); _ = connection?.close(); connection = nil
        let token = cancellation, installed = installation
        DispatchQueue.main.async { [completion] in
            completion(token.isCancelled ? .failure(Failure.cancelled(installed)) : result)
        }
    }
    deinit { if let flow { owner.async { flow.cancel() } } }
}
