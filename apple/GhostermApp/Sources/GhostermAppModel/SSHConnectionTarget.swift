import Foundation
import GhostermCore

/// 配置与安全存储引用；不包含后台会话身份或解锁材料。
public struct SSHConnectionTarget: Equatable, Sendable {
    public var alias, host, user: String
    public var port: UInt16
    public var knownHosts: String?
    public var identityPaths: [String]
    public var connectionIssues: [SSHConnectionIssue]
    public init(alias: String, host: String, user: String, port: UInt16,
                knownHosts: String?, identityPaths: [String], connectionIssues: [SSHConnectionIssue] = []) {
        self.alias = alias; self.host = host; self.user = user; self.port = port
        self.knownHosts = knownHosts; self.identityPaths = identityPaths
        self.connectionIssues = connectionIssues
    }
}
