// HostListModel — 主机列表（VS Code 式 ssh_config 管理的第一块）。
//
// 「文件即真相」（铁律 #4）：列表只是 `~/.ssh/config` 的**读取视图**；
// 本类型不缓存副本、不改文件，改配置一律走 SSHConfig.edit 的稀疏补丁。
import Foundation
import GhostermCore

/// 列表里的一台主机：原始 Host 模式 + 求值后的连接参数。
public struct HostEntry: Identifiable, Equatable {
    public var id: String { pattern }
    public let pattern: String
    /// 真实主机名（HostName 缺省时回落为 pattern，与 OpenSSH 一致）。
    public let hostname: String
    public let user: String?
    public let port: Int
    public let identityFiles: [String]

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

    /// 通配模式（`*` / `?`）：不是可点主机，但编辑器可见可改。
    public var wildcardPatterns: [String] {
        config.hostPatterns.filter { $0.contains("*") || $0.contains("?") }
    }

    /// 重新求值全部 Host 模式。
    public func reload(defaultUser: String? = nil) throws {
        var out: [HostEntry] = []
        for pattern in config.hostPatterns {
            // 通配模式（`*` / `?`）不是可点的主机，跳过——列表不该出现
            // 「Host *」这种兜底块。
            if pattern.contains("*") || pattern.contains("?") { continue }
            let r = try config.resolve(host: pattern, defaultUser: defaultUser)
            out.append(HostEntry(
                pattern: pattern,
                hostname: r.hostname ?? pattern,
                user: r.user,
                port: r.port,
                identityFiles: r.identityFiles
            ))
        }
        hosts = out
    }

    /// 读一个 Host 块的原始指令（编辑器的初值）。alias 不存在时返回空表。
    public func directives(of alias: String) -> [(keyword: String, values: [String])] {
        // resolve 只覆盖 typed 字段；编辑器要看**块内实际行**。core 未暴露
        // 块级视图，这里按行扫描 rawConfig（语义与 core 的行解析一致）。
        var out: [(String, [String])] = []
        var inBlock = false
        for line in rawConfig.split(separator: "\n", omittingEmptySubsequences: false) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.lowercased().hasPrefix("host ") {
                inBlock = trimmed.dropFirst(5).trimmingCharacters(in: .whitespaces)
                    .split(separator: " ").map(String.init).contains(alias)
                continue
            }
            guard inBlock, !trimmed.isEmpty, !trimmed.hasPrefix("#") else { continue }
            let parts = trimmed.split(separator: " ", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { continue }
            out.append((parts[0], parts[1].split(separator: " ").map(String.init)))
        }
        return out
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