import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionPackageTests: XCTestCase {
    func testFixedPackageAdoptionAndRPC() throws {
        guard ExtensionRuntime.supportStatus == 0 else { throw XCTSkip("Requires enabled Core profile") }
        runtimeWorker(self) { q in
            let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            let dir = root.appendingPathComponent("tmp/package-rpc-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false)
            defer { try? FileManager.default.removeItem(at: dir) }
            let entry = "executor/中文-" + String(repeating: "long-component/", count: 20) + "entry.js"
            let manifest: [String: Any] = ["schemaVersion": 1, "id": "org.ghosterm.fixture", "pluginVersion": "0.1.0", "sdkApiRange": "^1.0.0", "bridgeProtocolRange": [1],
                "publisher": "测试 publisher", "license": "MIT", "commands": [["path": ["fixture"], "title": "Fixture", "argsSchema": "schemas/input.json"]],
                "executor": ["variants": [["id": "portable", "runtime": "portable-js", "entry": entry, "requires": ["session.rpc"]]]], "permissions": []]
            let files: [String: Data] = ["plugin.json": try JSONSerialization.data(withJSONObject: manifest, options: [.sortedKeys]),
                "README": Data("fixture".utf8), "LICENSES/MIT.txt": Data("fixture".utf8), "schemas/input.json": Data("{}".utf8), entry: Data(runtimeEcho.utf8)]
            var members: [[String: Any]] = []
            for (path, bytes) in files {
                let url = dir.appendingPathComponent(path)
                try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                try bytes.write(to: url)
                members.append(["path": path, "bytes": bytes.count, "sha256": SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()])
            }
            try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "members": members], options: [.sortedKeys]).write(to: dir.appendingPathComponent("integrity.json"))
            let fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); XCTAssertGreaterThanOrEqual(fd, 0); defer { Darwin.close(fd) }
            let package = try ExtensionPackage(directoryFD: fd, owner: q)
            for _ in 0..<1000 { if try package.step() != .capturing { break } }
            let host = "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],\"runtimes\":[\"portable-js\"],\"capabilities\":[\"session.rpc\"]}"
            XCTAssertEqual(try package.check(hostJSON: host), .checked)
            let metadata = try package.metadata()
            XCTAssertEqual(metadata.entry, entry); XCTAssertEqual(metadata.publisher, "测试 publisher")
            XCTAssertEqual(metadata.requires, ["session.rpc"])
            guard metadata.requires == ["session.rpc"] else { try package.close(); return }
            XCTAssertEqual(metadata.protocols, [1])
            XCTAssertFalse(metadata.authorizationGranted); XCTAssertFalse(metadata.publisherAuthenticated)
            let commands = try package.commands()
            XCTAssertEqual(commands.count, 1); XCTAssertEqual(commands.first?.path, "fixture")
            XCTAssertEqual(commands.first?.title, "Fixture"); XCTAssertEqual(commands.first?.argsSchema, "schemas/input.json")
            let registry = try ExtensionRegistry(owner: q)
            XCTAssertEqual(try registry.put(package, source: .unknown), .unknown)
            XCTAssertEqual(try registry.put(package, source: .denied), .denied)
            XCTAssertEqual(try registry.put(package, source: .allowed), .ok)
            XCTAssertEqual(try registry.put(package, source: .allowed), .conflict)
            XCTAssertEqual(try registry.resolve(command: "fixture", executor: true, selectedID: metadata.id, selectedDigest: String(repeating: "0", count: 64)).status, .stale)
            package.stop()
            XCTAssertEqual(try registry.put(package, source: .allowed), .cancelled)
            let selection = try registry.resolve(command: "fixture", executor: true, selectedID: metadata.id, selectedDigest: metadata.packageDigest)
            XCTAssertEqual(selection.status, .ok); XCTAssertEqual(selection.command?.path, "fixture")
            let selected = try XCTUnwrap(selection.package)
            XCTAssertEqual(try selected.info().bytes, try package.info().bytes)
            XCTAssertEqual(try registry.remove(id: metadata.id, executor: true, expectedDigest: metadata.packageDigest), .ok)
            XCTAssertEqual(try registry.close(), .ok); XCTAssertThrowsError(try registry.resolve(command: "fixture", executor: true))
            let config = ExtensionSession.Config(pluginID: metadata.id, pluginVersion: metadata.version,
                clientDigest: metadata.packageDigest, executorDigest: metadata.packageDigest, hostVersion: "0.1.0",
                required: metadata.requires, available: metadata.requires, peerKind: .local, peerIdentity: "package-worker",
                methods: [.init(name: "fixture.echo", capability: "session.rpc", receiver: .executor, inputShape: .object, outputShape: .object)])
            var sockets: [Int32] = [-1, -1]; XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &sockets), 0)
            let h = try ExtensionSession(channel: ExtensionChannel(readFD: sockets[0], writeFD: sockets[0], owner: q), role: .host, config: config)
            let e = try ExtensionSession(channel: ExtensionChannel(readFD: sockets[1], writeFD: sockets[1], owner: q), role: .executor, config: config)
            for _ in 0..<2000 {
                _ = try h.step(); _ = try e.step()
                if try h.info().phase == .consent { XCTAssertEqual(try h.decide(ticket: h.info().consentToken, decision: .allow), 0) }
                if try h.info().phase == .active { break }
            }
            let lease = try XCTUnwrap(h.lease())
            let runtime = try ExtensionRuntime(session: e, package: selected, role: .executor, cpuMilliseconds: 500)
            XCTAssertThrowsError(try e.info()); e.stop()
            try selected.close(); try package.close(); XCTAssertThrowsError(try package.metadata())
            try FileManager.default.removeItem(at: dir.appendingPathComponent(entry))
            let request = try XCTUnwrap(h.request(lease, method: "fixture.echo", input: Data("{\"text\":\"固定快照\",\"count\":17}".utf8)).handle)
            for _ in 0..<2000 { _ = try h.step(); _ = try runtime.step(); if try h.requestResult(lease, handle: request).1?.confirmed == true { break } }
            let result = try XCTUnwrap(h.requestResult(lease, handle: request).1)
            XCTAssertTrue(result.confirmed); XCTAssertEqual(result.state, .succeeded)
            XCTAssertTrue(String(decoding: result.outcome, as: UTF8.self).contains("固定快照"))
            XCTAssertEqual(try h.forgetRequest(lease, handle: request), .ok)
            try runtimeClose(runtime); try runtimeClose(h)
        }
    }
    func testCapabilityAndOwnerLifetime() {
        runtimeWorker(self) { q in
            let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
                .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            let dir = root.appendingPathComponent("tmp/package-swift-\(UUID().uuidString)")
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false)
            defer { try? FileManager.default.removeItem(at: dir) }
            let fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
            XCTAssertGreaterThanOrEqual(fd, 0); defer { Darwin.close(fd) }
            if ExtensionRuntime.supportStatus != 0 {
                XCTAssertThrowsError(try ExtensionRegistry(owner: q)) { error in
                    XCTAssertEqual((error as? ExtensionRegistry.RegistryError)?.status, .unsupported)
                }
                XCTAssertThrowsError(try ExtensionPackage(directoryFD: fd, owner: q)) { error in
                    guard case GhostermError.status(7, _) = error else { return XCTFail("Expected explicit unsupported: \(error)") }
                }
                XCTAssertGreaterThanOrEqual(fcntl(fd, F_GETFD), 0)
                return
            }
            let cancelled = try ExtensionPackage(directoryFD: fd, owner: q)
            DispatchQueue.global().sync { cancelled.stop() }
            let cancelledPhase = try cancelled.step()
            XCTAssertEqual(cancelledPhase, .failed)
            guard cancelledPhase == .failed else { try cancelled.close(); return }
            XCTAssertEqual(try cancelled.info().error, .cancelled)
            XCTAssertThrowsError(try cancelled.metadata())
            try cancelled.close(); cancelled.stop()
            XCTAssertThrowsError(try cancelled.info())
            let empty = try ExtensionPackage(directoryFD: fd, owner: q)
            for _ in 0..<100 { if try empty.step() != .capturing { break } }
            XCTAssertEqual(try empty.info().phase, .captured)
            XCTAssertEqual(try empty.info().members, 0)
            XCTAssertEqual(try empty.check(hostJSON: nil), .failed)
            XCTAssertNotEqual(try empty.info().error, .none)
            XCTAssertThrowsError(try empty.metadata()); try empty.close()
            XCTAssertGreaterThanOrEqual(fcntl(fd, F_GETFD), 0)
        }
    }
}
