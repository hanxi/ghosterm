// UpdateCheckTests — dev 渠道「有没有新版」的判据护栏。
//
// 这里钉的是三类**编译照过、界面照样能画**的坏法：
//   · 「查不出来」被折叠成「已是最新」——网络挂了、清单缺字段，用户看到的是
//     一句安慰话，永远不再手动去查（AGENTS.md 铁律 #6）；
//   · 拿 `build` 号或版本号比大小——两条 workflow 轨的 `GITHUB_RUN_NUMBER` 不同源
//     单调，比出来的「更新」可以是更旧的那一个；
//   · 本机记的是短 sha（`--short=12`）、清单里是整条 40 位，直接 `==` 会永远判
//     「有新版」，于是每次点都弹一个「去下载现在这一版」。
// 变异验证见 `scripts/mutation-check-app-update.sh`。
import Foundation
import XCTest
@testable import GhostermAppModel

final class UpdateCheckTests: XCTestCase {
    // MARK: 夹具

    /// 一份真 CI 产物的形状（字段名与 docs/how-to/dev-build.md 的表一致）。
    private func manifest(commit: String = String(repeating: "d", count: 40),
                          channel: String = "dev",
                          buildTime: String = "2026-10-25T03:17:24Z",
                          signature: String = "adhoc",
                          notarized: Bool = false,
                          sha: String = String(repeating: "a", count: 64)) -> Data {
        let json: [String: Any] = [
            "channel": channel,
            "version": "0.1.0-dev",
            "build": 23,
            "git_commit": commit,
            "build_time": buildTime,
            "arch": "arm64",
            "min_macos": "15.0",
            "signature": signature,
            "notarized": notarized,
            "session_contract": "50137ac92a74-f83bc06576038ca2",
            "min_glibc": ["x86_64": "2.34", "aarch64": "2.34"],
            "assets": ["dmg": "Ghosterm-0.1.0-dev-arm64.dmg", "zip": "Ghosterm-0.1.0-dev-arm64.zip",
                       "checksums": "SHA256SUMS", "build_info": "BUILD-INFO.txt"],
            "sha256": ["Ghosterm-0.1.0-dev-arm64.dmg": sha, "Ghosterm-0.1.0-dev-arm64.zip": sha],
            "download_base": "https://github.com/hanxi/ghosterm/releases/download/dev",
        ]
        return try! JSONSerialization.data(withJSONObject: json)
    }

    private func remote(_ mutate: ((inout [String: Any]) -> Void)? = nil) throws -> RemoteBuild {
        var object = try JSONSerialization.jsonObject(with: manifest()) as! [String: Any]
        mutate?(&object)
        return try RemoteBuild.decode(try JSONSerialization.data(withJSONObject: object))
    }

    // MARK: 本机 BUILD-INFO

    func testBuildInfoParsesRealPackagingShape() {
        // 与 scripts/package-mac.sh 写的那几行逐字同形（对齐缩进与列名）。
        let text = """
        Ghosterm macOS 包
        版本:        0.1.0-dev (build 23)
        构建时刻:    2026-10-25T03:17:24Z
        架构:        arm64
        签名:        adhoc
        源码:        git d3e9fde742bd
        """
        let local = InstalledBuild.parse(text)
        XCTAssertEqual(local.version, "0.1.0-dev")
        XCTAssertEqual(local.build, 23)
        XCTAssertEqual(local.commit, "d3e9fde742bd")
        // 独立算法（不借实现里的格式化器）算出同一时刻：解析错了才会红。
        var parts = DateComponents()
        parts.year = 2026; parts.month = 10; parts.day = 25
        parts.hour = 3; parts.minute = 17; parts.second = 24
        var utc = Calendar(identifier: .iso8601); utc.timeZone = TimeZone(identifier: "UTC")!
        XCTAssertEqual(local.builtAt, utc.date(from: parts), "构建时刻解析错了（展示与兜底都靠它）")
    }

    func testMissingBuildInfoLinesBecomeNilNotZeros() {
        // 「没这一行」必须是 nil。落成 0 / "" 会被下游当成「已知且很旧」。
        let local = InstalledBuild.parse("Ghosterm macOS 包\n架构: arm64\n")
        XCTAssertNil(local.version)
        XCTAssertNil(local.build)
        XCTAssertNil(local.commit)
        XCTAssertNil(local.builtAt)
    }

    func testDirtyBuildKeepsItsMarker() {
        let local = InstalledBuild.parse("源码:        git d3e9fde742bd+dirty\n")
        XCTAssertEqual(local.commit, "d3e9fde742bd+dirty")
    }

    // MARK: 判据

    func testShortCommitPrefixMatchesFullCommit() throws {
        // 本机 --short=12、清单整条 40 位：必须是前缀匹配，否则每次都报「有新版」。
        let full = "9ef21070d320b7f9f126e8e12c15cb0457760eaf"
        let local = InstalledBuild(commit: String(full.prefix(12)))
        let release = try remote { $0["git_commit"] = full }
        XCTAssertEqual(UpdateCheck.compare(local: local, remote: release), .upToDate)
    }

    func testDifferentCommitMeansNewer() throws {
        let release = try remote { $0["git_commit"] = "9ef21070d320b7f9f126e8e12c15cb0457760eaf" }
        let query = UpdateCheck.compare(local: InstalledBuild(commit: "d3e9fde742bd"), remote: release)
        guard case .newer = query else { return XCTFail("commit 不等却判成 \(query)") }
    }

    func testBuildNumberAloneNeverDecides() throws {
        // 清单里 build 更大，但 commit 相同 → 仍是「已是最新」：这条钉住「不比运行编号」。
        let full = String(repeating: "e", count: 40)
        let release = try remote { $0["git_commit"] = full }
        let query = UpdateCheck.compare(local: InstalledBuild(commit: String(full.prefix(12))), remote: release)
        XCTAssertEqual(query, .upToDate)
    }

    func testUnknownLocalCommitIsNotReportedUpToDate() {
        // 「查不出来」与「已是最新」是两个结论，折叠即谎报。
        for commit in [nil, "unknown", "abc", "xyz12345", "d3e9fde742bd+dirty"] {
            let query = UpdateCheck.compare(local: InstalledBuild(commit: commit),
                                            remote: try! remote())
            guard case .cannotTell = query else {
                return XCTFail("本机 commit \(commit ?? "nil") 时应判查不出来，实际 \(query)")
            }
        }
    }

    func testForeignChannelIsRejectedNotSilentlyAccepted() throws {
        let query = UpdateCheck.compare(local: InstalledBuild(commit: String(repeating: "d", count: 12)),
                                        remote: try remote { $0["channel"] = "stable" })
        XCTAssertEqual(query, .cannotTell(.channelMismatch(found: "stable")))
    }

    // MARK: 清单解码：少一项就是错，不给默认值

    func testManifestRejectsMissingOrMalformedFields() {
        let holes: [String: (inout [String: Any]) -> Void] = [
            "git_commit 缺失": { $0["git_commit"] = nil },
            "git_commit 空串": { $0["git_commit"] = "" },
            "channel 缺失": { $0["channel"] = nil },
            "build_time 非 ISO": { $0["build_time"] = "2026-10-25 03:17" },
            "build_time 空串": { $0["build_time"] = "" },
            "notarized 缺失": { $0["notarized"] = nil },
            // 「签名」这一栏不能给默认值：缺了就当 `id`（能装）是把「查不出来」
            // 读成「能自动装」——比谎报新旧更危险。
            "signature 缺失": { $0["signature"] = nil },
            "signature 空串": { $0["signature"] = "" },
            "min_macos 缺失": { $0["min_macos"] = nil },
            "download_base 缺失": { $0["download_base"] = nil },
            "assets.dmg 缺失": { $0["assets"] = ["zip": "a.zip"] },
            "sha256 不像 sha256": { $0["sha256"] = ["Ghosterm-0.1.0-dev-arm64.dmg": "deadbeef"] },
            "sha256 键对不上 dmg": { $0["sha256"] = ["other.dmg": String(repeating: "a", count: 64)] },
        ]
        for (name, punch) in holes {
            XCTAssertThrowsError(try remote(punch), "「\(name)」本该解码失败")
        }
    }

    func testAdhocUnnotarizedManifestBlocksInstall() throws {
        // 「有新版」必须连带说清装不装得上：dev 轨恒 ad-hoc + 未公证。
        let build = try remote()
        XCTAssertTrue(build.installBlocked)
        XCTAssertEqual(build.downloadURL?.absoluteString,
                       "https://github.com/hanxi/ghosterm/releases/download/dev/Ghosterm-0.1.0-dev-arm64.dmg")
        // 免费个人账号的 Apple Development 档（`scripts/package-mac.sh` 记为 `dev`）：
        // 证书是真的，但这一档不能公证，用户下载来照样被 Gatekeeper 拦——判据写成
        // 「不等于 id 就拦」，所以任何没枚举过的新档位都默认落在「不许自动装」一侧。
        let devSigned = try remote { $0["signature"] = "dev" }
        XCTAssertTrue(devSigned.installBlocked)
        let signed = try remote {
            $0["signature"] = "id"
            $0["notarized"] = true
        }
        XCTAssertFalse(signed.installBlocked)
    }

    // MARK: 展示口径（文案也是判据的一部分）

    func testCannotTellNeverWearsUpToDateClothing() {
        // 用户读的是这一句。把「查不出来」写成「已是最新」＝谎报，铁律 #6。
        let reasons: [UpdateQuery.Reason] = [
            .localCommitUnknown,
            .localBuildDirty(commit: "d3e9fde742bd+dirty"),
            .channelMismatch(found: "stable"),
            .manifestUnreachable("HTTP 404"),
            .manifestInvalid("version.json 缺「git_commit」或值不对"),
        ]
        for reason in reasons {
            let text = UpdateQuery.cannotTell(reason).statusText
            XCTAssertTrue(text.hasPrefix("查不出来"), "\(reason) 的文案不是「查不出来」开头：\(text)")
            XCTAssertFalse(text.contains("已是最新"), "谎报：\(text)")
            XCTAssertFalse(text.isEmpty)
        }
        XCTAssertEqual(UpdateQuery.upToDate.statusText, "已是最新")
    }

    func testSummaryShowsAllThreeOrSaysNone() {
        XCTAssertEqual(InstalledBuild().summary, "开发构建（包内没有 BUILD-INFO.txt）")
        let local = InstalledBuild(version: "0.1.0-dev", build: 23, commit: "d3e9fde742bd",
                                   builtAt: InstalledBuild.timestamp("2026-10-25T03:17:24Z"))
        let text = local.summary
        XCTAssertTrue(text.contains("0.1.0-dev (23)"), text)
        XCTAssertTrue(text.contains("d3e9fde742bd"), text)
        XCTAssertTrue(text.contains("2026-10-25T03:17:24Z"), text)
        // 缺一段就明写缺，不能留空（留空看着像本来就没这项）。
        XCTAssertTrue(InstalledBuild(version: "0.1.0-dev").summary.contains("commit 无记录"))
    }
}
