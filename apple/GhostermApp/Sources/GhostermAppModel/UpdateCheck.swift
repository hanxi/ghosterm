// UpdateCheck — dev 渠道的「有没有新版」。
//
// **只检测，不安装**：dev 包是 ad-hoc 签名、未公证，每轮 cdhash 都不同，替换二进制
// 的动作必被 Gatekeeper 拦下。清单里的 `signature` / `notarized` 就是这条的机器可读
// 声明，界面必须照着它说话（AGENTS.md 铁律 #6：能力缺失要显式说，不许假装能装）。
//
// 为什么以 commit 为锚、不以版本号：`CFBundleVersion` = `GITHUB_RUN_NUMBER` 是按
// workflow 文件计数的，dev 与 release 两轨不同源单调，拿它比「谁新」会把更旧的读成
// 更新（见 docs/how-to/dev-build.md）。commit 全局唯一，且**本机那份包里有记录**
// ——`Contents/Resources/BUILD-INFO.txt` 的 `源码: git <短 sha>`。
//
// 三态不折叠：查不出来 ≠ 已是最新。本机没有 commit 记录（源码直跑）、包是 dirty 的、
// 网络失败、清单字段不对——全都如实返回「查不出来 + 原因」，绝不返回 upToDate。
import Foundation

/// 本机这份包的安装信息（来自随包 `BUILD-INFO.txt`）。
public struct InstalledBuild: Equatable {
    public var version: String?
    public var build: Int?
    /// `源码: git <短 sha>` 里那串，原样保留（CI 用 `--short=12`）。脏包带 `+dirty`。
    public var commit: String?
    /// `构建时刻: <ISO-8601 UTC>`。展示用；本机 commit 缺失时**不拿它猜**。
    public var builtAt: Date?

    public init(version: String? = nil, build: Int? = nil, commit: String? = nil, builtAt: Date? = nil) {
        self.version = version
        self.build = build
        self.commit = commit
        self.builtAt = builtAt
    }

    /// 从 bundle 里读。开发态直接跑可执行文件（没有打包）→ 全 nil，如实说不知道。
    public static func fromBundle(_ bundle: Bundle = .main) -> InstalledBuild {
        guard let path = bundle.path(forResource: "BUILD-INFO", ofType: "txt"),
              let text = try? String(contentsOfFile: path, encoding: .utf8) else {
            return InstalledBuild()
        }
        return parse(text)
    }

    /// 纯函数：`BUILD-INFO.txt` 的每一行都可能是缺的，缺什么对应字段就是 nil。
    public static func parse(_ text: String) -> InstalledBuild {
        func value(_ key: String) -> String? {
            for line in text.split(separator: "\n") {
                guard let colon = line.firstIndex(of: ":") else { continue }
                guard line[..<colon].trimmingCharacters(in: .whitespaces) == key else { continue }
                let rest = String(line[line.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
                return rest.isEmpty ? nil : rest
            }
            return nil
        }
        var out = InstalledBuild()
        if let line = value("版本") {
            // "0.1.0-dev (build 21)"
            out.version = line.split(separator: " ").first.map(String.init)
            if let open = line.firstIndex(of: "("), let close = line.firstIndex(of: ")") {
                let inner = line[line.index(after: open)..<close]
                    .replacingOccurrences(of: "build", with: "")
                    .trimmingCharacters(in: .whitespaces)
                out.build = Int(inner)
            }
        }
        if let source = value("源码") {
            // "git d3e9fde742bd" / "git unknown" / "git <sha>+dirty"
            out.commit = source.split(separator: " ").dropFirst().first.map(String.init)
        }
        if let stamp = value("构建时刻"), let date = Self.timestamp(stamp) { out.builtAt = date }
        return out
    }

    static func timestamp(_ raw: String) -> Date? {
        let text = raw.split(separator: " ").first.map(String.init) ?? raw
        let plain = ISO8601DateFormatter()
        plain.formatOptions = [.withInternetDateTime]
        if let date = plain.date(from: text) { return date }
        let fractional = ISO8601DateFormatter()
        fractional.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return fractional.date(from: text)
    }
}

/// `version.json` 里那一版清单（字段口径见 docs/how-to/dev-build.md）。
public struct RemoteBuild: Equatable {
    public var channel: String
    public var version: String
    public var gitCommit: String
    public var buildTime: Date
    public var signature: String
    public var notarized: Bool
    public var minMacOS: String
    public var assetName: String
    public var assetSha256: String
    public var downloadBase: String

    /// 装到机器上的这份能不能被 Gatekeeper 放行：**必须 Developer ID 签名且已公证**
    /// （`signature` 的取值见 `scripts/package-mac.sh`：`adhoc` / `id` / `none`）。
    /// 界面上「有新版」必须连带这句话，否则等于给了个点了装不上的按钮。
    public var installBlocked: Bool { signature != "id" || !notarized }

    public var downloadURL: URL? { URL(string: downloadBase + "/" + assetName) }

    private enum CodingKeys: String, CodingKey {
        case channel, version, signature, notarized, assets, sha256
        case gitCommit = "git_commit", buildTime = "build_time"
        case minMacOS = "min_macos", downloadBase = "download_base"
    }

    /// 解码即严校：少字段 / 时间解析不动 / 渠道不对 → 抛错，由调用方转成「查不出来」。
    /// 给默认值蒙过去，等于把检测做成永远「已是最新」。
    init(dictionary raw: [String: Any]) throws {
        func text(_ key: CodingKeys, _ json: String) throws -> String {
            guard let value = raw[json] as? String, !value.isEmpty else {
                throw Self.missing(json)
            }
            return value
        }
        channel = try text(.channel, "channel")
        version = try text(.version, "version")
        gitCommit = try text(.gitCommit, "git_commit")
        signature = try text(.signature, "signature")
        minMacOS = try text(.minMacOS, "min_macos")
        downloadBase = try text(.downloadBase, "download_base")
        guard let rawTime = raw["build_time"] as? String, let time = InstalledBuild.timestamp(rawTime) else {
            throw Self.missing("build_time")
        }
        buildTime = time
        guard let flag = raw["notarized"] as? Bool else { throw Self.missing("notarized") }
        notarized = flag
        guard let assets = raw["assets"] as? [String: String], let dmg = assets["dmg"], !dmg.isEmpty else {
            throw Self.missing("assets.dmg")
        }
        assetName = dmg
        guard let sums = raw["sha256"] as? [String: String], let sum = sums[dmg], sum.count == 64 else {
            throw Self.missing("sha256[\(dmg)]")
        }
        assetSha256 = sum
    }

    static func missing(_ field: String) -> DecodingError {
        DecodingError.dataCorrupted(.init(codingPath: [],
                                          debugDescription: "version.json 缺「\(field)」或值不对"))
    }

    public static func decode(_ data: Data) throws -> RemoteBuild {
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw DecodingError.dataCorrupted(.init(codingPath: [],
                                                    debugDescription: "version.json 不是对象"))
        }
        return try RemoteBuild(dictionary: object)
    }
}

/// 一次检查的结论。**三态**：有新版 / 已是最新 / 查不出来（带原因）。
public enum UpdateQuery: Equatable {
    case newer(RemoteBuild)
    case upToDate
    case cannotTell(Reason)

    public enum Reason: Equatable, Error {
        case localCommitUnknown
        case localBuildDirty(commit: String)
        case channelMismatch(found: String)
        case manifestUnreachable(String)
        case manifestInvalid(String)
    }
}

public enum UpdateCheck {
    /// dev Release 的固定直链：`releases/download/<tag>/<asset>` 不走 GitHub API，
    /// 因此不占未认证 60 次/小时的额度，也不需要 token。
    /// **fork 必须改这里**——清单与资产都挂在同一个 `dev` tag 下。
    public static let manifestURL = URL(
        string: "https://github.com/hanxi/ghosterm/releases/download/dev/version.json")!

    /// 纯判据，不碰网络也不碰 bundle：变异验证与单测都在这一层做。
    public static func compare(local: InstalledBuild, remote: RemoteBuild) -> UpdateQuery {
        guard remote.channel == "dev" else { return .cannotTell(.channelMismatch(found: remote.channel)) }
        guard let raw = local.commit, raw != "unknown" else { return .cannotTell(.localCommitUnknown) }
        if raw.hasSuffix("+dirty") { return .cannotTell(.localBuildDirty(commit: raw)) }
        let mine = raw.lowercased()
        guard mine.count >= 7, mine.allSatisfy({ "0123456789abcdef".contains($0) }) else {
            return .cannotTell(.localCommitUnknown)
        }
        // CI 记的是短 sha（`git rev-parse --short=12`），清单里是整条 40 位：比前缀。
        let theirs = remote.gitCommit.lowercased()
        return theirs.hasPrefix(mine) ? .upToDate : .newer(remote)
    }

    /// 取清单 → 解码 → 比对。任何一步不成都返回 `.cannotTell`，不抛、不静默。
    public static func query(local: InstalledBuild, timeout: TimeInterval = 10) async -> UpdateQuery {
        var request = URLRequest(url: manifestURL)
        request.timeoutInterval = timeout
        let data: Data
        do {
            let (body, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
                let code = (response as? HTTPURLResponse)?.statusCode ?? -1
                return .cannotTell(.manifestUnreachable("HTTP \(code)"))
            }
            data = body
        } catch {
            return .cannotTell(.manifestUnreachable(error.localizedDescription))
        }
        let remote: RemoteBuild
        do { remote = try RemoteBuild.decode(data) }
        catch { return .cannotTell(.manifestInvalid(error.localizedDescription)) }
        return compare(local: local, remote: remote)
    }
}

// MARK: - 展示口径

/// `构建时刻` 的展示格式（与 version.json / BUILD-INFO 里同一口径，固定 UTC 不受本机时区影响）。
private let updateDisplayFormatter: DateFormatter = {
    let formatter = DateFormatter()
    formatter.locale = Locale(identifier: "en_US_POSIX")
    formatter.timeZone = TimeZone(identifier: "UTC")
    formatter.dateFormat = "yyyy-MM-dd'T'HH:mm:ss'Z'"
    return formatter
}()

public extension InstalledBuild {
    /// 「0.1.0-dev (23) · d3e9fde742bd · 2026-10-25T03:17:24Z」——缺哪段就明写「无记录」，
    /// 不留空（留空看起来像「这一段本来就没有」，用户会以为包是完整的）。
    var summary: String {
        guard commit != nil || version != nil || builtAt != nil else {
            return "开发构建（包内没有 BUILD-INFO.txt）"
        }
        var label = "版本无记录"
        if let version { label = build.map { "\(version) (\($0))" } ?? version }
        let parts = [label,
                     commit ?? "commit 无记录",
                     builtAt.map { updateDisplayFormatter.string(from: $0) } ?? "构建时刻无记录"]
        return parts.joined(separator: " · ")
    }
}

public extension UpdateQuery {
    /// 界面上那一句结论。**「查不出来」绝不能长得像「已是最新」**——这条文案就是
    /// 铁律 #6 在 UI 上的落点，所以放在模型层里由测试钉住，而不是散在视图里。
    var statusText: String {
        switch self {
        case .newer(let remote): return "有新版（\(String(remote.gitCommit.prefix(12)))）"
        case .upToDate: return "已是最新"
        case .cannotTell(let reason): return "查不出来：\(reason.text)"
        }
    }
}

private extension UpdateQuery.Reason {
    var text: String {
        switch self {
        case .localCommitUnknown:
            return "本机这份包里没有 commit 记录，无从比较"
        case .localBuildDirty(let commit):
            return "本机这份带未提交改动（\(commit)），与任何发布版都不同源"
        case .channelMismatch(let found):
            return "清单渠道是「\(found)」，不是 dev"
        case .manifestUnreachable(let detail):
            return "取不到 version.json（\(detail)）"
        case .manifestInvalid(let detail):
            return "version.json 读不懂（\(detail)）"
        }
    }
}

/// 关于窗口那个按钮背后的状态机。
///
/// **只在用户点「检查更新」时打网络**：没有启动时自动检查、没有后台轮询。
/// dev 包 ad-hoc + 未公证，检测出来也装不上，静默联网换不来任何用户能用的东西。
@MainActor
public final class UpdateChecker: ObservableObject {
    @Published public private(set) var isChecking = false
    @Published public private(set) var query: UpdateQuery?
    /// 本机这份包的信息：开窗口就按 bundle 里的 `BUILD-INFO.txt` 填好，不等检查。
    /// （检查时再快照一次，保证结论与显示同源。）
    @Published public private(set) var installed = InstalledBuild.fromBundle()

    public init() {}

    /// 在途时重复点不算第二次检查（与 songloft 的 memoized Future 同口径，
    /// 但这里只是不重发，不缓存结论——用户就是要再问一次网络）。
    public func checkNow(local: InstalledBuild = InstalledBuild.fromBundle()) {
        guard !isChecking else { return }
        installed = local
        query = nil
        isChecking = true
        Task { @MainActor in
            self.query = await UpdateCheck.query(local: local)
            self.isChecking = false
        }
    }
}
