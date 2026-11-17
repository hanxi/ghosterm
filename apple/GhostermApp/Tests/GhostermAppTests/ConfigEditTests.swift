// ConfigEditTests — AppModel 编辑流（M3 的模型层 e2e）。
//
// 覆盖（铁律 #4 的 UI 侧闭环）：
//   1. applyEdits：编辑 → 落盘 → 列表更新，且**除目标行外逐字节不变**；
//   2. addHost：新建块追加到文件末尾，已有内容不动；
//   3. 冲突：编辑期间外部改文件 → save 拒绝，文件保持外部内容；
//   4. 备份：save 时 .bak 有原文件内容；
//   5. 通配块编辑（M1 遗留项）：`Host *` 可改，改的是它自己的行。
//   6. directives：编辑器初值来自块内实际指令（非求值结果）。
import Darwin
@testable import GhostermAppModel
import XCTest

@MainActor
final class ConfigEditTests: XCTestCase {
    private func makeConfig(_ text: String) throws -> (path: String, model: AppModel) {
        let dir = NSTemporaryDirectory() + "cfgedit-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/config"
        try text.write(toFile: path, atomically: true, encoding: .utf8)
        let model = isolatedAppModel(configPath: path, knownHostsPath: dir + "/known_hosts")
        model.reload()
        return (path, model)
    }

    func testApplyEditsPersistsAndUpdatesList() throws {
        let original = "# 顶部注释\nHost web\n  HostName 10.0.0.5\n  User deploy\n\nHost db\n  HostName 10.0.0.6\n"
        let (path, model) = try makeConfig(original)
        try model.applyEdits(alias: "web", [("User", ["ops"])])

        let saved = try String(contentsOfFile: path, encoding: .utf8)
        XCTAssertEqual(saved,
                       "# 顶部注释\nHost web\n  HostName 10.0.0.5\n  User ops\n\nHost db\n  HostName 10.0.0.6\n",
                       "除目标行外逐字节不变（铁律 #4）")
        XCTAssertEqual(model.hosts.first { $0.pattern == "web" }?.user, "ops",
                       "列表随保存更新")
    }

    func testAddHostAppendsWithoutTouchingExisting() throws {
        let original = "Host web\n  HostName 10.0.0.5\n"
        let (path, model) = try makeConfig(original)
        try model.addHost(alias: "db", [("HostName", ["10.0.0.6"]), ("User", ["ops"])])

        let saved = try String(contentsOfFile: path, encoding: .utf8)
        XCTAssertTrue(saved.hasPrefix("Host web\n  HostName 10.0.0.5\n"),
                      "已有块逐字节不变")
        XCTAssertTrue(saved.contains("Host db"), "新块已追加")
        XCTAssertTrue(model.hosts.contains { $0.pattern == "db" }, "列表含新主机")
    }

    func testConflictRefusesAndKeepsExternalContent() throws {
        let original = "Host web\n  User deploy\n"
        let (path, model) = try makeConfig(original)
        // 编辑期间外部修改（用户手动改了）：
        Thread.sleep(forTimeInterval: 0.05)
        try "Host web\n  User someone-else\n".write(toFile: path, atomically: true, encoding: .utf8)

        XCTAssertThrowsError(try model.applyEdits(alias: "web", [("User", ["ops"])])) { error in
            guard case ConfigStoreError.conflict = error else {
                return XCTFail("外部修改后必须抛 conflict，得到 \(error)")
            }
        }
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8),
                       "Host web\n  User someone-else\n",
                       "外部内容不被覆盖")
    }

    func testSaveLeavesBackup() throws {
        let original = "Host web\n  User deploy\n"
        let (path, model) = try makeConfig(original)
        try model.applyEdits(alias: "web", [("User", ["ops"])])
        let backup = (path as NSString).deletingPathExtension + ".bak"
        XCTAssertEqual(try? String(contentsOfFile: backup, encoding: .utf8), original,
                       "写回前强制备份（铁律 #4）")
    }

    /// M1 遗留项：`Host *` 通配块不可点连接，但编辑器要能改它。
    func testWildcardBlockIsEditable() throws {
        let original = "Host web\n  HostName 10.0.0.5\n\nHost *\n  ServerAliveInterval 60\n"
        let (path, model) = try makeConfig(original)

        XCTAssertEqual(model.wildcards, ["*"], "通配块应出现在 wildcards")
        XCTAssertFalse(model.hosts.contains { $0.pattern == "*" },
                       "通配块不是可点主机")

        try model.applyEdits(alias: "*", [("ServerAliveInterval", ["120"])])
        let saved = try String(contentsOfFile: path, encoding: .utf8)
        XCTAssertEqual(saved,
                       "Host web\n  HostName 10.0.0.5\n\nHost *\n  ServerAliveInterval 120\n",
                       "通配块的编辑落盘，其余行逐字节不变")
        XCTAssertEqual(model.wildcards, ["*"])
    }

    func testDirectivesComeFromBlockNotResolution() throws {
        // HostName 缺省时 resolve 会回落为 pattern——编辑器初值必须是
        // **块内实际写的行**（空），不能把求值结果当成已写指令。
        let (_, model) = try makeConfig("Host bare\n  User me\n")
        let ds = model.directives(of: "bare")
        XCTAssertEqual(ds.map { $0.keyword.lowercased() }, ["user"],
                       "只看块内实际指令（保留原拼写）")
        XCTAssertEqual(ds.first?.values, ["me"])
    }

    func testBackupFailureCannotPublishOrLeakRejectedEdits() throws {
        let original = "Host web\n  User deploy\n"
        let (path, model) = try makeConfig(original)
        let backup = (path as NSString).deletingPathExtension + ".bak"
        try FileManager.default.createDirectory(atPath: backup, withIntermediateDirectories: false)
        var rejected = false, nextSaved = false
        do { try model.applyEdits(alias: "web", [("User", ["rejected"])]) } catch { rejected = true }
        let unchanged = model.configModel?.rawConfig == original && model.hosts.first?.user == "deploy"
        try FileManager.default.removeItem(atPath: backup)
        do { try model.applyEdits(alias: "web", [("Port", ["2222"])]); nextSaved = true } catch { }
        XCTAssertTrue(rejected && unchanged && nextSaved && model.hosts.first?.user == "deploy"
                      && model.hosts.first?.port == 2222,
                      "a rejected candidate must not reach the published model or a later independent save")
    }

    func testFailedNewHostCannotRemainInTheModel() throws {
        let (path, model) = try makeConfig("Host web\n  User deploy\n")
        let before = model.configModel?.rawConfig
        let backup = (path as NSString).deletingPathExtension + ".bak"
        try FileManager.default.createDirectory(atPath: backup, withIntermediateDirectories: false)
        var rejected = false
        do { try model.addHost(alias: "rejected", [("User", ["ops"])]) } catch { rejected = true }
        XCTAssertTrue(rejected && model.configModel?.rawConfig == before
                      && model.hosts.map(\.pattern) == ["web"],
                      "a failed new host must not pollute either the AST or the visible host list")
    }

    func testSuccessfulEditPublishesOnlyAfterRealCommit() throws {
        let original = "Host web\n  User deploy\n"
        let (path, model) = try makeConfig(original)
        var operations = ConfigStore.FileOperations(), observedOldModel = false
        var previous: HostListModel?
        weak var observedModel = model
        let rename = operations.rename
        operations.rename = { directory, source, target, replace in
            if target == "config", let model = observedModel {
                observedOldModel = model.configModel === previous
                    && previous?.rawConfig == original && model.hosts.first?.user == "deploy"
            }
            return rename(directory, source, target, replace)
        }
        model.makeConfigStore = { ConfigStore(path: $0, operations: operations) }
        model.reload()
        previous = model.configModel
        try model.applyEdits(alias: "web", [("User", ["ops"])])
        XCTAssertTrue(observedOldModel && previous?.rawConfig == original
                      && model.configModel !== previous && model.hosts.first?.user == "ops"
                      && (try? String(contentsOfFile: path, encoding: .utf8))?.contains("User ops") == true,
                      "only a durable successful candidate may replace the published configuration")
    }

    func testCommittedErrorReloadsObservedDiskAndStillReportsFailure() throws {
        let (path, model) = try makeConfig("Host web\n  User deploy\n")
        var operations = ConfigStore.FileOperations(), syncs = 0
        let synchronize = operations.sync
        operations.sync = { fd in
            syncs += 1
            if syncs == 4 { errno = EIO; return -1 }
            return synchronize(fd)
        }
        model.makeConfigStore = { ConfigStore(path: $0, operations: operations) }
        model.reload()
        var reportedCommitted = false
        do { try model.applyEdits(alias: "web", [("User", ["ops"])]) }
        catch { if case ConfigStoreError.committed = error { reportedCommitted = true } }
        XCTAssertTrue(reportedCommitted && model.hosts.first?.user == "ops"
                      && model.configModel?.rawConfig == (try? String(contentsOfFile: path, encoding: .utf8)),
                      "a committed storage error must reload the observed file and remain an explicit failure")
    }

    func testExternalMainBytesAreNotAdoptedAsTheEditBase() throws {
        let (path, model) = try makeConfig("Host web\n  User deploy\n")
        let before = model.configModel?.rawConfig
        try "Host web\n  User external\n".write(toFile: path, atomically: true, encoding: .utf8)
        var rejected = false
        do { try model.applyEdits(alias: "web", [("Port", ["2222"])]) }
        catch { if case ConfigStoreError.conflict = error { rejected = true } }
        XCTAssertTrue(rejected && model.configModel?.rawConfig == before
                      && model.hosts.first?.user == "deploy",
                      "external changes require explicit reload rather than silently becoming a candidate base")
    }

    func testLoadStoreRaceCannotAdoptChangedMainBytes() throws {
        let original = "Host web\n  User deploy\n"
        let (path, model) = try makeConfig(original)
        model.makeConfigStore = { path in
            try? "Host web\n  User external\n".write(toFile: path, atomically: false, encoding: .utf8)
            return ConfigStore(path: path)
        }
        model.reload()
        var rejected = false
        do { try model.applyEdits(alias: "web", [("Port", ["2222"])]) }
        catch { if case ConfigStoreError.conflict = error { rejected = true } }
        XCTAssertTrue(rejected && model.configModel?.rawConfig == original
                      && model.hosts.first?.user == "deploy"
                      && (try? String(contentsOfFile: path, encoding: .utf8)) == "Host web\n  User external\n",
                      "a parse-to-store race must not adopt unseen bytes as the editor's candidate base")
    }

    func testNewWildcardPublishesRuleListAfterCommit() throws {
        let (path, model) = try makeConfig("Host web\n  User deploy\n")
        try model.addHost(alias: "*", [("ServerAliveInterval", ["0"])])
        XCTAssertTrue(model.wildcards == ["*"] && model.hosts.map(\.pattern) == ["web"]
                      && (try? String(contentsOfFile: path, encoding: .utf8))?.contains("Host *") == true,
                      "successful configuration publication must update rule choices without adding a connectable wildcard")
    }
}
