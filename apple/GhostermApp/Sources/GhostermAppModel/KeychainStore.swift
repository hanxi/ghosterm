// KeychainStore — 私钥材料的平台安全存储（AGENTS.md 铁律 #5）。
//
// 职责边界（严格）：
//   - **只**存「已解锁的密钥材料」：PEM 文本或口令，service/account 寻址；
//   - 读取时原样返回，不做任何解析/持久化到磁盘；
//   - 不进日志、不进测试快照——测试用一次性随机 service 隔离（见 tests）。
//
// 为什么放在 GhostermAppModel 而不是 GhostermCore：Keychain 是**平台**安全
// 存储（Security.framework），属 Apple 端实现；core 是纯 C11 + POSIX，
// 只接受已解锁材料（ghosterm.h 契约）。
//
// 线程模型：SecItem* 是阻塞调用，通常毫秒级。但**读 secret**（kSecReturnData）
// 走 securityd 的 CSSM 解密通道——在无签名进程 / 无授权 UI 的环境里（闸门、
// 无头冒烟），securityd 的应答可能永远不来（实测：连接线程卡死在 mach_msg）。
// 故所有调用都带超时闸：超时按「读不到」处理，绝不让连接线程被 Keychain 拖死。
// 代价：超时场景泄漏一条卡住的线程——只在异常环境发生，且进程本来就要退出。
import Foundation
import Security

public enum KeychainStoreError: Error, CustomStringConvertible {
    case unexpectedStatus(OSStatus)
    case notFound
    case timeout

    public var description: String {
        switch self {
        case .unexpectedStatus(let s):
            return "Keychain 操作失败（status=\(s)）"
        case .notFound:
            return "Keychain 中没有这条记录"
        case .timeout:
            return "Keychain 无响应（无签名/无授权环境）"
        }
    }
}

/// 私钥材料在 Keychain 里的寻址方式。
///
/// 账号字段约定（与 WinSCP/SecureCRT 的「会话级凭据」对齐，但按本项目
/// 语义命名）：按 **主机指纹**（host:port）存口令、按 **文件路径** 存 PEM。
public enum KeychainStore {
    static let service = "dev.ghosterm.keys"
    /// SecItem 调用的超时闸（秒）。正常环境毫秒级返回；securityd 不应答
    /// （无签名进程读 secret）时止损，不让调用线程永远卡死。
    static let callTimeout: TimeInterval = 2

    // MARK: - 带超时的执行器

    /// 在后台线程跑 SecItem 调用，限时等结果；超时抛 .timeout。
    /// 返回值必须是值类型（OSStatus/Data）——闭包里的引用不跨线程带出。
    static func withTimeout<T>(_ label: String,
                               _ body: @escaping @Sendable () -> T) throws -> T {
        let box = ResultBox<T>()
        let done = DispatchSemaphore(value: 0)
        DispatchQueue.global(qos: .userInitiated).async {
            box.value = body()
            done.signal()
        }
        if done.wait(timeout: .now() + callTimeout) == .timedOut {
            throw KeychainStoreError.timeout
        }
        return box.value!
    }

    /// 单值信箱（ResultBox）；只被后台线程写、限时后调用线程读。
    private final class ResultBox<T>: @unchecked Sendable {
        var value: T?
    }

    // MARK: - 通用读写

    static func set(_ data: Data, account: String) throws {
        // 先删后加：SecItemUpdate 对 kSecValueData 需要走 data 属性更新，
        // 删-加两步在「不存在/已存在」两种初始状态下都幂等，且避免了
        // errSecDuplicateItem 的分支处理。
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let status: OSStatus = try withTimeout("set") {
            SecItemDelete(base as CFDictionary) // 不存在时 errSecItemNotFound，无害
            var add = base
            add[kSecValueData as String] = data
            add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
            return SecItemAdd(add as CFDictionary, nil)
        }
        guard status == errSecSuccess else {
            throw KeychainStoreError.unexpectedStatus(status)
        }
    }

    static func get(account: String) throws -> Data {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        let result: (OSStatus, Data?) = try withTimeout("get") {
            var out: AnyObject?
            let st = SecItemCopyMatching(query as CFDictionary, &out)
            return (st, out as? Data)
        }
        if result.0 == errSecItemNotFound { throw KeychainStoreError.notFound }
        guard result.0 == errSecSuccess, let data = result.1 else {
            throw KeychainStoreError.unexpectedStatus(result.0)
        }
        return data
    }

    static func delete(account: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        // 删除不取回 secret，通常不卡；同样限时，超时静默放弃（幂等语义）。
        _ = try? withTimeout("delete") {
            SecItemDelete(query as CFDictionary) as OSStatus
        }
    }

    // MARK: - 语义化入口（host:port / 私钥路径）

    /// 按 host:port 存取私钥**口令**（Keychain 推荐做法：口令很短且
    /// 高频复用，设备本地访问约束足够）。
    public static func setPassphrase(_ passphrase: String, forHost host: String, port: Int) throws {
        try set(Data(passphrase.utf8), account: hostAccount(host, port: port))
    }

    public static func passphrase(forHost host: String, port: Int) throws -> String {
        let d = try get(account: hostAccount(host, port: port))
        return String(decoding: d, as: UTF8.self)
    }

    public static func removePassphrase(forHost host: String, port: Int) {
        delete(account: hostAccount(host, port: port))
    }

    // MARK: - SSH 登录密码（与私钥口令分开存）

    public static func setPassword(_ password: String, forHost host: String,
                                   port: Int) throws {
        try set(Data(password.utf8), account: passwordAccount(host, port: port))
    }

    public static func password(forHost host: String, port: Int) throws -> String {
        let d = try get(account: passwordAccount(host, port: port))
        return String(decoding: d, as: UTF8.self)
    }

    public static func removePassword(forHost host: String, port: Int) {
        delete(account: passwordAccount(host, port: port))
    }

    /// 按文件路径存取**已解锁的 PEM**（材料本体）。落盘明文由此被替换：
    /// SSH 私钥文件路径只在「导入」时读一次，之后 Keychain 即真相。
    public static func setPrivateKeyPEM(_ pem: String, forPath path: String) throws {
        try set(Data(pem.utf8), account: keyAccount(path))
    }

    public static func privateKeyPEM(forPath path: String) throws -> String {
        let d = try get(account: keyAccount(path))
        return String(decoding: d, as: UTF8.self)
    }

    public static func removePrivateKeyPEM(forPath path: String) {
        delete(account: keyAccount(path))
    }

    // MARK: - account 组装

    /// host:port 的 Keychain account。22 端口保持裸 host（与 known_hosts
    /// 的 `[host]:port` 约定一致）。公开给测试断言 account 布局。
    public static func hostAccount(_ host: String, port: Int) -> String {
        port == 22 ? "pass:\(host)" : "pass:\(host):\(port)"
    }

    public static func keyAccount(_ path: String) -> String {
        "key:\(path)"
    }

    /// SSH **登录密码**的 account。与 `passphrase` 分开存：一个是「解开私钥
    /// 文件的口令」，一个是「服务器认的登录密码」，混在一条记录里会让
    /// 「同一个 host 换了认证方式」变成互相覆盖。
    public static func passwordAccount(_ host: String, port: Int) -> String {
        port == 22 ? "sshpass:\(host)" : "sshpass:\(host):\(port)"
    }
}

/// 测试钩子：让测试用**随机 account** 直捅底层读写（不碰生产 account 布局），
/// 以此隔离「测的是 Keychain 真行为」与「不污染任何真实记录」两件事。
public enum KeychainStoreTestHooks {
    public static func set(_ data: Data, account: String) throws {
        try KeychainStore.set(data, account: account)
    }

    public static func get(account: String) throws -> Data {
        try KeychainStore.get(account: account)
    }

    public static func delete(_ account: String) {
        KeychainStore.delete(account: account)
    }
}
