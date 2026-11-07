import Foundation
import Darwin
import GhostermCore

/// Configuration only: no DNS, sockets, credentials or side effects. The same
/// snapshot can prepare plugin, terminal and SFTP owners independently.
struct SSHConfiguredRoute {
    struct Endpoint {
        var alias: String
        var user: String?
        var port: UInt16?
    }
    static func invalid(_ detail: String) -> GhostermError {
        .failed("ProxyJump 配置无效：" + detail)
    }
    static func parse(_ value: String) throws -> [Endpoint] {
        if value.isEmpty || value.lowercased() == "none" { return [] }
        guard value.utf8.count <= 8192 else { throw invalid("路线过长") }
        let items = value.split(separator: ",", omittingEmptySubsequences: false)
        guard items.count < 8 else { throw invalid("含目标最多支持八跳") }
        return try items.map { item in
            var text = String(item)
            guard !text.isEmpty, !text.unicodeScalars.contains(where: { CharacterSet.whitespacesAndNewlines.contains($0) || CharacterSet.controlCharacters.contains($0) }) else {
                throw invalid("空跳或空白字符")
            }
            if text.hasPrefix("ssh://") { text = String(text.dropFirst(6)) }
            guard !text.contains("/"), !text.contains("%"), !text.contains("?"), !text.contains("#") else { throw invalid("不支持的地址或展开符") }
            var user: String?
            if let at = text.firstIndex(of: "@") {
                user = String(text[..<at]); text = String(text[text.index(after: at)...])
                guard user?.isEmpty == false, !text.contains("@") else { throw invalid("用户名无效") }
            }
            guard !text.isEmpty else { throw invalid("缺少主机名") }
            var host: String, portText: String?
            if text.hasPrefix("[") {
                guard let end = text.firstIndex(of: "]") else { throw invalid("IPv6 括号未闭合") }
                host = String(text[text.index(after: text.startIndex)..<end])
                var address = in6_addr()
                guard host.withCString({ inet_pton(AF_INET6, $0, &address) }) == 1 else { throw invalid("IPv6 地址无效") }
                let suffix = text[text.index(after: end)...]
                if !suffix.isEmpty {
                    guard suffix.first == ":" else { throw invalid("地址后缀无效") }
                    portText = String(suffix.dropFirst())
                }
            } else {
                let parts = text.split(separator: ":", omittingEmptySubsequences: false)
                guard parts.count <= 2 else { throw invalid("IPv6 地址须使用方括号") }
                host = String(parts[0]); if parts.count == 2 { portText = String(parts[1]) }
                guard !host.contains("["), !host.contains("]") else { throw invalid("地址括号无效") }
            }
            guard !host.isEmpty, host.utf8.count <= 255, !host.hasPrefix("-"), !host.contains("*") else { throw invalid("主机名无效") }
            var port: UInt16?
            if let portText {
                guard !portText.isEmpty, portText.utf8.allSatisfy({ (48...57).contains($0) }), let value = UInt16(portText), value > 0 else { throw invalid("端口无效") }
                port = value
            }
            return Endpoint(alias: host, user: user, port: port)
        }
    }

    static func resolve(config: SSHConfig, aliases: [String], knownHosts: String?) throws -> [SSHConnectionTarget] {
        guard (1...8).contains(aliases.count) else { throw invalid("含目标最多支持八跳") }
        var result: [SSHConnectionTarget] = []
        var visiting = Set<String>()
        func append(_ endpoint: Endpoint, expand: Bool) throws {
            guard result.count + visiting.count < 8, visiting.insert(endpoint.alias.lowercased()).inserted else { throw invalid("循环或超过八跳") }
            defer { visiting.remove(endpoint.alias.lowercased()) }
            let raw = try config.effective(host: endpoint.alias, key: "proxyjump")
            let jumps = try parse(raw)
            let issues = try config.connectionIssues(host: endpoint.alias).filter { $0.keyword != "proxyjump" }
            guard issues.isEmpty else { throw GhostermError.failed("不支持 SSH 配置：" + issues.map(\.description).joined(separator: "、")) }
            if expand {
                // OpenSSH's explicit preceding -J list supersedes the later
                // jump's own ProxyJump. Only its first hop inherits a route.
                for (index, jump) in jumps.enumerated() { try append(jump, expand: index == 0) }
            } else if aliases.count > 1, !jumps.isEmpty {
                throw invalid("手工多跳路线不能同时指定 ProxyJump；请选择单个配置目标")
            }
            let resolved = try config.resolve(host: endpoint.alias)
            guard let port = endpoint.port ?? UInt16(exactly: resolved.port), port > 0 else { throw invalid("目标端口无效") }
            let target = SSHConnectionTarget(alias: endpoint.alias, host: resolved.hostname ?? endpoint.alias,
                user: endpoint.user ?? resolved.user ?? NSUserName(), port: port,
                knownHosts: knownHosts, identityPaths: resolved.identityFiles.map { ($0 as NSString).expandingTildeInPath })
            guard !result.contains(where: { $0.host.lowercased() == target.host.lowercased() && $0.port == target.port && $0.user == target.user }) else { throw invalid("重复的连接目标") }
            result.append(target)
        }
        for alias in aliases { try append(Endpoint(alias: alias), expand: aliases.count == 1) }
        return result
    }
}
