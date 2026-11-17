// HostListModel — 主机列表（VS Code 式 ssh_config 管理的第一块）。
//
// 「文件即真相」（铁律 #4）：列表只是 `~/.ssh/config` 的**读取视图**；
// 本类型不缓存副本、不改文件，改配置一律走 SSHConfig.edit 的稀疏补丁。
import Foundation
import GhostermCore

/// 列表里的一台主机：原始 Host 模式 + 求值后的连接参数。
public struct HostEntry: Identifiable, Equatable {
    public var id: String { pattern.utf8.map { String(format: "%02x", $0) }.joined() }
    public static func patternKey(_ value: String) -> Data {
        Data(value.utf8.map { (65...90).contains($0) ? $0 + 32 : $0 })
    }
    public func matchesPattern(_ candidate: String) -> Bool {
        Self.patternKey(pattern) == Self.patternKey(candidate)
    }
    public let pattern: String
    /// 真实主机名（HostName 缺省时回落为 pattern，与 OpenSSH 一致）。
    public let hostname: String
    public let user: String?
    public let port: Int
    public let identityFiles: [String]
    public var connectionIssues: [SSHConnectionIssue] = []

    public var displayName: String { pattern }
    /// `ssh user@host:port` 风格的一行摘要（UI 副标题）。
    public var summary: String {
        let u = user.map { "\($0)@" } ?? ""
        let p = port == 22 ? "" : ":\(port)"
        return "\(u)\(hostname)\(p)"
    }
}

public final class HostListModel {
    private let config: SSHConfig
    public private(set) var hosts: [HostEntry] = []

    /// 从一个 ssh_config 文件构建。文件不存在按空配置处理（不是错误：
    /// 没配过 SSH 的用户本来就该看到空列表）。
    public init(path: String) throws {
        self.config = try SSHConfig(path: path)
        try reload()
    }

    /// 从内联文本构建（UI 预览 / 测试用）。
    public convenience init(contents: String) throws {
        let cfg = try SSHConfig(contents: contents)
        try self.init(config: cfg)
    }

    private init(config: SSHConfig) throws {
        self.config = config
        try reload()
    }

    /// 所有 Host 模式（含通配块——编辑器要能改 `Host *`，M1 遗留项）。
    public var allPatterns: [String] { config.hostPatterns }

    func sshRoute(aliases: [String], knownHosts: String?) throws -> [SSHConnectionTarget] {
        try SSHConfiguredRoute.resolve(config: config, aliases: aliases, knownHosts: knownHosts)
    }

    /// 通配模式（`*` / `?`）：不是可点主机，但编辑器可见可改。
    public var wildcardPatterns: [String] {
        var seen: Set<Data> = []
        return config.hostPatterns.filter {
            ($0.hasPrefix("!") || $0.contains("*") || $0.contains("?")) && seen.insert(HostEntry.patternKey($0)).inserted
        }
    }

    /// 重新求值全部 Host 模式。
    public func reload(defaultUser: String? = nil) throws {
        var out: [HostEntry] = []
        var seen: Set<Data> = []
        for pattern in config.hostPatterns {
            // 通配模式（`*` / `?`）不是可点的主机，跳过——列表不该出现
            // 「Host *」这种兜底块。
            if pattern.hasPrefix("!") || pattern.contains("*") || pattern.contains("?") { continue }
            guard seen.insert(HostEntry.patternKey(pattern)).inserted else { continue }
            let r = try config.resolve(host: pattern, defaultUser: defaultUser)
            out.append(HostEntry(
                pattern: pattern,
                hostname: r.hostname ?? pattern,
                user: r.user,
                port: r.port,
                identityFiles: r.identityFiles,
                connectionIssues: try config.connectionIssues(host: pattern)
            ))
        }
        hosts = out
    }

    public func directiveSnapshot(of alias: String) throws -> [ConfigDirectiveInfo] {
        try config.directives(alias: alias)
    }

    public func inspect(alias: String) throws -> ConfigHostInfo { try config.inspect(alias: alias) }
    public func identities(alias: String) throws -> [ConfigIdentityInfo] { try config.identities(alias: alias) }
    public func resolved(alias: String) throws -> ResolvedHost { try config.resolve(host: alias) }
    public func directives(of alias: String) throws -> [(keyword: String, values: [String])] {
        try directiveSnapshot(of: alias).map { ($0.keyword, $0.arguments) }
    }
    public func contains(alias: String) -> Bool {
        allPatterns.contains { HostEntry.patternKey($0) == HostEntry.patternKey(alias) }
    }
    public func rename(alias: String, to replacement: String) throws {
        try config.rename(alias: alias, to: replacement)
        try reload()
    }

    /// 配置原文（无损写回；未编辑时逐字节等于输入）。
    public var rawConfig: String { config.serialized() }

    /// 稀疏编辑：除目标行外逐字节不变。
    public func edit(alias: String, _ edits: [(String, [String]?)], reset: Bool = false) throws {
        try config.edit(alias: alias, edits, reset: reset)
        try reload()
    }

    /// 编辑后的全文（无损；未编辑时逐字节等于原文）。
    public var serialized: String { config.serialized() }
}
