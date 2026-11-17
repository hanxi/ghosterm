// Channel comparison and shared update presentation. Installation is handled by Sparkle in the macOS host.
import Foundation

/// 本机这份包的安装信息（来自随包 `BUILD-INFO.txt`）。
public struct InstalledBuild: Equatable {
    public var channel: String?
    public var version: String?
    public var build: Int?
    /// `源码: git <短 sha>` 里那串，原样保留（CI 用 `--short=12`）。脏包带 `+dirty`。
    public var commit: String?
    /// `构建时刻: <ISO-8601 UTC>`。展示用；本机 commit 缺失时**不拿它猜**。
    public var builtAt: Date?

    public init(version: String? = nil, build: Int? = nil, commit: String? = nil, builtAt: Date? = nil, channel: String? = "dev") {
        self.channel = channel
        self.version = version
        self.build = build
        self.commit = commit
        self.builtAt = builtAt
    }

    /// 从 bundle 里读。开发态直接跑可执行文件（没有打包）→ 全 nil，如实说不知道。
    public static func fromBundle(_ bundle: Bundle = .main) -> InstalledBuild {
        guard let path = bundle.path(forResource: "BUILD-INFO", ofType: "txt"),
              let text = try? String(contentsOfFile: path, encoding: .utf8) else {
            return InstalledBuild(channel: nil)
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
        out.channel = value("渠道") ?? out.version.map { $0.contains("-dev") ? "dev" : "release" }
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

    /// Apple code signing is independent of Sparkle archive signing. An ad-hoc
    /// package is eligible when it has an Ed25519 archive signature.
    public var updateVersion: String?
    public var updateSignature: String?
    public var installBlocked: Bool { updateSignature == nil || updateVersion?.allSatisfy(\.isNumber) != true || updateVersion?.isEmpty != false }

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
        updateVersion = raw["update_version"] as? String
        updateSignature = (raw["update_signature"] as? String).flatMap {
            Data(base64Encoded: $0)?.count == 64 ? $0 : nil
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
        guard sum.allSatisfy({ "0123456789abcdefABCDEF".contains($0) }),
              gitCommit.count == 40, gitCommit.allSatisfy({ "0123456789abcdefABCDEF".contains($0) }),
              let base = URL(string: downloadBase), base.scheme == "https", base.host == "github.com",
              downloadBase.hasPrefix("https://github.com/hanxi/ghosterm/releases/download/"),
              !dmg.contains("/"), !dmg.contains("\\") else { throw Self.missing("asset/source") }
        if channel == "dev" {
            guard downloadBase == "https://github.com/hanxi/ghosterm/releases/download/dev" else {
                throw Self.missing("dev download_base")
            }
        } else if channel == "release" {
            guard UpdateCheck.stableVersion(version),
                  downloadBase == "https://github.com/hanxi/ghosterm/releases/download/v\(version)" else {
                throw Self.missing("release download_base")
            }
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
        case localChannelUnknown
        case localVersionUnknown
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

    public static func manifestURL(channel: String) -> URL {
        channel == "dev" ? manifestURL : URL(string:
            "https://github.com/hanxi/ghosterm/releases/latest/download/version.json")!
    }

    public static func feedURL(channel: String) -> URL {
        URL(string: channel == "dev"
            ? "https://github.com/hanxi/ghosterm/releases/download/dev/appcast.xml"
            : "https://github.com/hanxi/ghosterm/releases/latest/download/appcast.xml")!
    }

    /// 纯判据，不碰网络也不碰 bundle：变异验证与单测都在这一层做。
    public static func compare(local: InstalledBuild, remote: RemoteBuild) -> UpdateQuery {
        guard let channel = local.channel, ["dev", "release"].contains(channel) else {
            return .cannotTell(.localChannelUnknown)
        }
        guard remote.channel == channel else { return .cannotTell(.channelMismatch(found: remote.channel)) }
        if local.commit?.hasSuffix("+dirty") == true {
            return .cannotTell(.localBuildDirty(commit: local.commit!))
        }
        if channel == "release" {
            guard let mine = local.version, stableVersion(mine), stableVersion(remote.version) else {
                return .cannotTell(.localVersionUnknown)
            }
            return remote.version.compare(mine, options: .numeric) == .orderedDescending
                ? .newer(remote) : .upToDate
        }
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

    public static func stableVersion(_ version: String) -> Bool {
        let parts = version.split(separator: ".", omittingEmptySubsequences: false)
        return parts.count == 3 && parts.allSatisfy { !$0.isEmpty && $0.allSatisfy(\.isNumber) && Int($0) != nil }
    }

    /// 取清单 → 解码 → 比对。任何一步不成都返回 `.cannotTell`，不抛、不静默。
    public static func query(local: InstalledBuild, timeout: TimeInterval = 10) async -> UpdateQuery {
        guard let channel = local.channel, ["dev", "release"].contains(channel) else {
            return .cannotTell(.localChannelUnknown)
        }
        var request = URLRequest(url: manifestURL(channel: channel))
        request.cachePolicy = .reloadIgnoringLocalCacheData
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
        case .localChannelUnknown:
            return "本机渠道无记录，无法选择更新渠道"
        case .localVersionUnknown:
            return "正式版本号无记录或格式不受支持"
        case .localCommitUnknown:
            return "本机这份包里没有 commit 记录，无从比较"
        case .localBuildDirty(let commit):
            return "本机这份带未提交改动（\(commit)），与任何发布版都不同源"
        case .channelMismatch(let found):
            return "清单渠道「\(found)」与本机不同"
        case .manifestUnreachable(let detail):
            return "取不到 version.json（\(detail)）"
        case .manifestInvalid(let detail):
            return "version.json 读不懂（\(detail)）"
        }
    }
}

/// One app-wide model: closing About or a terminal window does not cancel a download.
@MainActor
public final class UpdateChecker: ObservableObject {
    public static let shared = UpdateChecker()
    @Published public private(set) var isChecking = false
    @Published public private(set) var query: UpdateQuery?
    @Published public private(set) var installed = InstalledBuild.fromBundle()
    @Published public var noticeVisible = false
    @Published public var phase: UpdatePhase = .idle
    public var downloadAction: ((RemoteBuild) -> Void)?
    public var cancelAction: (() -> Void)?
    public var restartAction: (() -> Void)?

    public init() {}

    public func checkNow(local: InstalledBuild = InstalledBuild.fromBundle()) {
        noticeVisible = true
        guard !isChecking, !phase.isBusy else { return }
        if case .ready = phase { return }
        installed = local
        query = nil
        phase = .idle
        isChecking = true
        Task { @MainActor in
            self.query = await UpdateCheck.query(local: local)
            self.isChecking = false
        }
    }

    public func download() {
        guard !phase.isBusy, case .newer(let remote) = query else { return }
        noticeVisible = true
        guard !remote.installBlocked else {
            phase = .failed("此发布尚未提供更新签名，请从下载页手动安装。")
            return
        }
        guard let downloadAction else {
            phase = .failed("此构建未配置更新公钥，请从下载页手动安装。")
            return
        }
        downloadAction(remote)
    }
}

public enum UpdatePhase: Equatable {
    case idle
    case downloading(received: UInt64, total: UInt64?)
    case extracting(Double)
    case ready
    case installing
    case failed(String)

    public var isBusy: Bool {
        switch self {
        case .downloading, .extracting, .installing: return true
        default: return false
        }
    }
}
