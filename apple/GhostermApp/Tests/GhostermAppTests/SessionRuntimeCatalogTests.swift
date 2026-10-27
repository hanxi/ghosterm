import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

final class SessionRuntimeCatalogTests: XCTestCase {
    private struct Fixture {
        let root: URL
        let contract = String(repeating: "a", count: 64)
        let release = "fixture-release-1"
        let localFiles = ["ghosterm-sessiond", "ghosterm-session-proxy", "lib/libghostty-vt.dylib", "LICENSE", "SOURCE-INFO.json"]
        let linuxFiles = ["ghosterm-sessiond", "ghosterm-session-proxy", "lib/libghostty-vt.so.0", "LICENSE", "SOURCE-INFO.json", "sources.tar.gz"]
        var local: URL { root.appendingPathComponent("macos/" + release) }
        var index: URL { root.appendingPathComponent("catalog.json") }
        func linux(_ arch: String) -> URL { root.appendingPathComponent("linux/" + arch + "/" + release) }
        func write(_ bytes: Data, _ file: URL) throws {
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
            try bytes.write(to: file); XCTAssertEqual(chmod(file.path, 0o600), 0)
        }
        func json(_ value: [String: Any], _ file: URL) throws { try write(JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]), file) }
        func hash(_ bytes: Data) -> String { SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined() }
        func prepare() throws {
            var hashes: [String: String] = [:]
            for name in localFiles { let data = Data(("fixture " + name).utf8); try write(data, local.appendingPathComponent(name)); hashes[name] = hash(data) }
            try json(["format": 1, "platform": "macos", "arch": "arm64", "contract": contract, "release": release, "files": hashes], local.appendingPathComponent("runtime.json"))
            for arch in ["aarch64", "x86_64"] {
                var table = "GTB1\t\(release)\t\(arch)\t2.34\t/lib/fixture-loader\t\(contract)\n"
                for name in linuxFiles {
                    let bytes = Data(("fixture " + arch + name).utf8)
                    try write(bytes, linux(arch).appendingPathComponent(name))
                    table += "\(name)\t\(bytes.count)\t\(hash(bytes))\n"
                }
                try write(Data(table.utf8), linux(arch).appendingPathComponent("manifest.tsv"))
            }
            try json(["format": 1, "contract": contract, "local": release, "linux": ["aarch64": release, "x86_64": release]], index)
        }
        func load() throws -> SessionRuntimeCatalog { try .load(index: index, macosRoot: root.appendingPathComponent("macos"), linuxRoot: root.appendingPathComponent("linux")) }
    }
    private func fixture(_ body: @escaping (Fixture) throws -> Void) {
        var project = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { project.deleteLastPathComponent() }
        let root = project.appendingPathComponent("tmp/catalog-test-" + UUID().uuidString)
        let done = expectation(description: "background catalog IO")
        DispatchQueue.global().async {
            defer { try? FileManager.default.removeItem(at: root); done.fulfill() }
            do { let f = Fixture(root: root); try f.prepare(); try body(f) } catch { XCTFail("\(error)") }
        }
        wait(for: [done], timeout: 8)
    }
    func testMatchingNativeAndBothLinuxInventories() {
        fixture { f in
            let catalog = try f.load()
            XCTAssertEqual(catalog.contract, f.contract); XCTAssertEqual(catalog.localDirectory, f.local)
            XCTAssertEqual(Set(catalog.linuxBundles.keys), ["aarch64", "x86_64"])
            XCTAssertEqual(catalog.compatibility, Array(repeating: 0xaa, count: 32))
            let record = SessionRecord(kind: .local, endpoint: .init(proxy: "/fixture/proxy", daemon: "/fixture/daemon", runtime: "/fixture/runtime", contract: f.contract), nonce: Array(repeating: 1, count: 16))
            XCTAssertNoThrow(try catalog.requireCompatible(record))
            var old = record; old.endpoint.contract = String(repeating: "b", count: 64)
            XCTAssertThrowsError(try catalog.requireCompatible(old)) { error in
                guard case .incompatibleSession? = error as? SessionRuntimeCatalog.Failure else {
                    return XCTFail("旧会话不兼容必须与随包运行环境不兼容区分")
                }
            }
        }
    }
    func testFutureCatalogAndUnsafeReleaseRejected() {
        fixture { f in
            for change in [["format": 2, "contract": f.contract, "local": f.release, "linux": [:]], ["format": 1, "contract": f.contract, "local": "../outside", "linux": [:]]] as [[String: Any]] {
                try f.json(change, f.index); XCTAssertThrowsError(try f.load()) { error in
                    guard case .unsupported? = error as? SessionRuntimeCatalog.Failure else { return XCTFail("unsafe/version identity must fail before file traversal") }
                }
            }
        }
    }
    func testNativeDigestPermissionsAndSymlinkRejected() {
        fixture { f in
            let file = f.local.appendingPathComponent("ghosterm-sessiond"), kept = try Data(contentsOf: f.local.appendingPathComponent("ghosterm-sessiond"))
            try f.write(Data("damaged".utf8), file); XCTAssertThrowsError(try f.load())
            try f.write(kept, file); XCTAssertEqual(chmod(file.path, 0o666), 0); XCTAssertThrowsError(try f.load()); XCTAssertEqual(chmod(file.path, 0o600), 0)
            let real = f.root.appendingPathComponent("kept-native")
            try FileManager.default.moveItem(at: file, to: real); XCTAssertEqual(symlink(real.path, file.path), 0)
            XCTAssertThrowsError(try f.load()); XCTAssertEqual(try Data(contentsOf: real), kept)
        }
    }
    func testNativeManifestFilesMatchExactly() {
        fixture { f in
            let metadata = f.local.appendingPathComponent("runtime.json")
            var document = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: metadata)) as? [String: Any])
            var files = try XCTUnwrap(document["files"] as? [String: String]); files.removeValue(forKey: "SOURCE-INFO.json"); document["files"] = files
            try f.json(document, metadata); XCTAssertThrowsError(try f.load())
        }
    }
    func testLinuxIdentityDigestAndDuplicateInventoryRejected() {
        fixture { f in
            let table = f.linux("x86_64").appendingPathComponent("manifest.tsv"), original = try String(contentsOf: f.linux("x86_64").appendingPathComponent("manifest.tsv"))
            try f.write(Data(original.replacingOccurrences(of: f.contract, with: String(repeating: "b", count: 64)).utf8), table); XCTAssertThrowsError(try f.load())
            var rows = original.components(separatedBy: "\n"); var first = rows[1].components(separatedBy: "\t"); first[2] = String(repeating: "0", count: 64); rows[1] = first.joined(separator: "\t")
            try f.write(Data(rows.joined(separator: "\n").utf8), table); XCTAssertThrowsError(try f.load())
            rows = original.components(separatedBy: "\n"); rows[1] = rows[2]
            try f.write(Data(rows.joined(separator: "\n").utf8), table); XCTAssertThrowsError(try f.load())
        }
    }
    func testLinuxSourceArchiveMustBeListedAndUnmodified() {
        fixture { f in
            let archive = f.linux("aarch64").appendingPathComponent("sources.tar.gz")
            let original = try Data(contentsOf: archive)
            try f.write(Data(repeating: 0x78, count: original.count), archive)
            XCTAssertThrowsError(try f.load(), "篡改源码包必须被哈希校验拦截")
            try f.write(original, archive)
            let manifest = f.linux("aarch64").appendingPathComponent("manifest.tsv")
            let rows = try String(contentsOf: manifest).split(separator: "\n").filter { !$0.hasPrefix("sources.tar.gz\t") }
            try f.write(Data((rows.joined(separator: "\n") + "\n").utf8), manifest)
            XCTAssertThrowsError(try f.load(), "漏列源码包不能被当成旧格式接受")
        }
    }
    func testBoundedMetadataPreservesOriginal() {
        fixture { f in
            let data = try JSONSerialization.data(withJSONObject: ["format": 1, "contract": f.contract, "local": f.release, "linux": [:], "padding": String(repeating: "x", count: 9000)])
            try f.write(data, f.index); XCTAssertThrowsError(try f.load()); XCTAssertEqual(try Data(contentsOf: f.index), data)
        }
    }
}
