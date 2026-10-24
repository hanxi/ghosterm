// ConfigStoreTests — 「文件即真相」写侧的硬断言。
//
// 覆盖（每条对应铁律 #4 的一个执行面）：
//   1. 原子写：落盘内容逐字节等于提交内容；
//   2. 备份：save 前旧文件被复制到 .bak；
//   3. 冲突检测：load 后外部修改 → save 拒绝（不静默覆盖别人）；
//   4. 二次 save：快照刷新后可继续写；
//   5. 新文件（原不存在）：不备份、直接原子创建。
import GhostermAppModel
import XCTest

final class ConfigStoreTests: XCTestCase {
    private func makeFile(_ initial: String?) throws -> String {
        let dir = NSTemporaryDirectory() + "cfgstore-\(UUID().uuidString)"
        try FileManager.default.createDirectory(atPath: dir,
                                                withIntermediateDirectories: true)
        addTeardownBlock { try? FileManager.default.removeItem(atPath: dir) }
        let path = dir + "/config"
        if let initial { try initial.write(toFile: path, atomically: true, encoding: .utf8) }
        return path
    }

    func testAtomicWriteRoundTrip() throws {
        let path = try makeFile("Host web\n  User deploy\n")
        let store = ConfigStore(path: path)
        try store.save("Host web\n  User ops\n")
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8),
                       "Host web\n  User ops\n", "落盘内容必须逐字节等于提交内容")
        // 半份文件不存在（原子 rename 的临时文件已清理）。
        let dir = (path as NSString).deletingLastPathComponent
        let leftovers = try FileManager.default.contentsOfDirectory(atPath: dir)
            .filter { $0.hasPrefix(".config.tmp") }
        XCTAssertTrue(leftovers.isEmpty, "不得残留临时文件（得到 \(leftovers)）")
    }

    func testBackupBeforeOverwrite() throws {
        let original = "Host web\n  User deploy\n"
        let path = try makeFile(original)
        let store = ConfigStore(path: path)
        try store.save("Host web\n  User ops\n")
        let backup = (path as NSString).deletingPathExtension + ".bak"
        XCTAssertEqual(try? String(contentsOfFile: backup, encoding: .utf8), original,
                       "save 前必须把原文件备份到 .bak（内容逐字节）")
    }

    func testExternalModificationRefusesSave() throws {
        let path = try makeFile("Host web\n  User deploy\n")
        let store = ConfigStore(path: path)
        // 外部（比如用户用 vim 改了）：
        Thread.sleep(forTimeInterval: 0.05)
        try "Host web\n  User someone-else\n".write(toFile: path, atomically: true, encoding: .utf8)
        XCTAssertThrowsError(try store.save("Host web\n  User ops\n")) { error in
            guard case ConfigStoreError.conflict = error else {
                return XCTFail("外部修改后 save 必须抛 conflict，得到 \(error)")
            }
        }
        // 且外部内容未被覆盖：
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8),
                       "Host web\n  User someone-else\n")
    }

    func testSnapshotRefreshesAfterSave() throws {
        let path = try makeFile("Host web\n  User deploy\n")
        let store = ConfigStore(path: path)
        try store.save("v1\n")
        XCTAssertFalse(store.isExternallyModified(), "save 后快照应刷新")
        try store.save("v2\n") // 不应抛 conflict
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8), "v2\n")
    }

    func testCreateNewFileDoesNotBackup() throws {
        let path = try makeFile(nil) // 不存在
        let store = ConfigStore(path: path)
        try store.save("Host web\n  User deploy\n")
        let backup = (path as NSString).deletingPathExtension + ".bak"
        XCTAssertFalse(FileManager.default.fileExists(atPath: backup),
                       "原文件不存在时不得凭空造备份")
        XCTAssertEqual(try String(contentsOfFile: path, encoding: .utf8),
                       "Host web\n  User deploy\n")
    }

    /// 文件在 load 之后才出现（原本没有 config）也是冲突。
    func testAppearedAfterLoadIsConflict() throws {
        let path = try makeFile(nil)
        let store = ConfigStore(path: path)
        try "external\n".write(toFile: path, atomically: true, encoding: .utf8)
        XCTAssertTrue(store.isExternallyModified())
        XCTAssertThrowsError(try store.save("mine\n"))
    }
}
