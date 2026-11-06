import Cghosterm
import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

func runtimeConfig(resources: Bool = false) -> ExtensionSession.Config {
    .init(pluginID: "org.ghosterm.preview", pluginVersion: "0.1.0", clientDigest: String(repeating: "d", count: 64), executorDigest: String(repeating: "e", count: 64), hostVersion: "0.1.0",
        required: resources ? ["ui.form", "resource.receive", "file.readScoped"] : ["ui.form"], available: resources ? ["ui.form", "resource.receive", "file.readScoped"] : ["ui.form"], peerKind: .local, peerIdentity: "runtime-worker", methods: [
            .init(name: "fixture.echo", capability: "ui.form", receiver: .executor, inputShape: .object, outputShape: .object),
            .init(name: "fixture.ask", capability: "ui.form", receiver: .host, inputShape: .object, outputShape: .object)])
}
func runtimePair(_ q: DispatchQueue, hostStore: ExtensionResourceStore? = nil, executorStore: ExtensionResourceStore? = nil) throws -> (ExtensionSession, ExtensionSession, ExtensionSession.Lease, ExtensionSession.Lease) {
    var f: [Int32] = [-1, -1]; guard socketpair(AF_UNIX, SOCK_STREAM, 0, &f) == 0 else { throw GhostermError.failed("socketpair") }
    let h = try ExtensionSession(channel: ExtensionChannel(readFD: f[0], writeFD: f[0], owner: q), role: .host, config: runtimeConfig(resources: hostStore != nil), resourceStore: hostStore)
    let e = try ExtensionSession(channel: ExtensionChannel(readFD: f[1], writeFD: f[1], owner: q), role: .executor, config: runtimeConfig(resources: executorStore != nil), resourceStore: executorStore)
    for _ in 0..<2000 { _ = try h.step(); _ = try e.step(); if try h.info().phase == .consent { XCTAssertEqual(try h.decide(ticket: h.info().consentToken, decision: .allow), 0) }
        if try h.info().phase == .active { return (h, e, try XCTUnwrap(h.lease()), try XCTUnwrap(e.lease())) } }
    throw GhostermError.failed("runtime handshake")
}
func runtimeClose(_ s: ExtensionSession) throws { for _ in 0..<64 { if try s.closeStep() { return } }; throw GhostermError.failed("Session draining") }
func runtimeClose(_ r: ExtensionRuntime) throws { for _ in 0..<64 { if try r.closeStep() { return } }; throw GhostermError.failed("runtime draining") }
func runtimeWorker(_ test: XCTestCase, _ body: @escaping (DispatchQueue) throws -> Void) {
    let done = test.expectation(description: "runtime owner"), q = DispatchQueue(label: "ghosterm.runtime.test")
    q.async { do { try body(q) } catch { XCTFail("runtime owner: \(error)") }; q.async { q.async { done.fulfill() } } }; test.wait(for: [done], timeout: 40)
}
func runtimeModule(_ name: String, _ text: Data) -> ExtensionRuntime.Module {
    .init(name: name, source: text, sha256: SHA256.hash(data: text).map { String(format: "%02x", $0) }.joined())
}
func runtimeModules(_ plugin: String, resources: Bool = false) throws -> [ExtensionRuntime.Module] {
    guard let root = ProcessInfo.processInfo.environment["GHOSTERM_RUNTIME_TEST_SDK"] else { throw GhostermError.failed("compiled SDK fixture missing") }
    var names = ["index.js", "codec.js", "session.js", "wire-contract.js"]; if resources { names.append("resources.js") }
    var result = try names.map {
        try runtimeModule("sdk:/" + $0, Data(contentsOf: URL(fileURLWithPath: root).appendingPathComponent($0)))
    }
    result.append(runtimeModule("plugin:/entry.js", Data(plugin.utf8))); return result
}
let runtimeEcho = "import {object,text,integer} from '@ghosterm/plugin-sdk';export function activate(ctx){const codec=object({text:text(40),count:integer(0,100)});ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{await Promise.resolve();c.check();return v});return {dispose(){}}}"

func runtimeDirectory() throws -> URL {
    guard let root = ProcessInfo.processInfo.environment["GHOSTERM_RUNTIME_TEST_SDK"] else { throw GhostermError.failed("SDK fixture missing") }
    let dir = URL(fileURLWithPath: root).appendingPathComponent("resource-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700]); return dir
}
func runtimeStore(_ q: DispatchQueue) throws -> (ExtensionResourceStore, URL) {
    let dir = try runtimeDirectory(), fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); guard fd >= 0 else { throw GhostermError.failed("Store fd") }; defer { Darwin.close(fd) }
    return (try ExtensionResourceStore(directoryFD: fd, owner: q), dir)
}

final class ExtensionRuntimeAvailabilityTests: XCTestCase {
    func testBuildCapabilityAndDisabledOwnership() {
        let enabled = ProcessInfo.processInfo.environment["GHOSTERM_CORE_TEST_PLUGIN_JS"] == "1"
        XCTAssertEqual(ExtensionRuntime.supportStatus, enabled ? 0 : 7)
        guard !enabled else { return }
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q)
            let config = ExtensionRuntime.Config(modules: [runtimeModule("plugin:/entry.js", Data("export const value=1".utf8))], entry: "plugin:/entry.js", role: .executor)
            do { _ = try ExtensionRuntime(session: e, config: config); XCTFail("unsupported runtime admitted") }
            catch GhostermError.status(let code, _) { XCTAssertEqual(code, 7) }
            XCTAssertEqual(try e.info().phase, .active); try runtimeClose(e); try runtimeClose(h)
        }
    }
}
