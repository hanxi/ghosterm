import Cghosterm
import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionRuntimeTests: XCTestCase {
    override func setUpWithError() throws {
        XCTAssertEqual(ExtensionRuntime.supportStatus, 0, "explicit enabled Core profile required")
    }
    func testAdoptionAndActualPluginRPC() {
        runtimeWorker(self) { q in
            let (h, e, hl, _) = try runtimePair(q)
            let r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: "plugin:/entry.js", role: .executor, cpuMilliseconds: 500))
            XCTAssertThrowsError(try e.info()); e.stop(); XCTAssertEqual(try r.info().phase, .created)
            let handle = try XCTUnwrap(h.request(hl, method: "fixture.echo", input: Data("{\"text\":\"中文\",\"count\":17}".utf8), timeoutMilliseconds: 10000).handle)
            var completed = false
            for _ in 0..<2000 { _ = try h.step(); _ = try r.step(); if try h.requestResult(hl, handle: handle).1?.confirmed == true { completed = true; break } }
            XCTAssertTrue(completed); let result = try XCTUnwrap(h.requestResult(hl, handle: handle).1); XCTAssertEqual(result.state, .succeeded); XCTAssertTrue(String(decoding: result.outcome, as: UTF8.self).contains("中文"))
            XCTAssertEqual(try h.forgetRequest(hl, handle: handle), .ok); let saved = try r.info(); XCTAssertEqual(saved.phase, .active); XCTAssertGreaterThan(saved.jobs, 0); XCTAssertGreaterThan(saved.moduleBytes, 0)
            try runtimeClose(r); try runtimeClose(h); XCTAssertGreaterThan(saved.moduleBytes, 0)
        }
    }
    func testExistingRequestRejectsAdoption() {
        runtimeWorker(self) { q in
            let (h, e, _, el) = try runtimePair(q), handle = try XCTUnwrap(e.request(el, method: "fixture.ask", input: Data("{}".utf8)).handle)
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: "plugin:/entry.js", role: .executor)))
            XCTAssertEqual(try e.info().phase, .active); XCTAssertEqual(try e.cancelRequest(el, handle: handle), .ok); XCTAssertEqual(try e.forgetRequest(el, handle: handle), .ok)
            try runtimeClose(e); try runtimeClose(h)
        }
    }
    func testCPUAndImportFailureRemainDistinct() {
        runtimeWorker(self) { q in
            for (plugin, expected) in [("export function activate(){let x=0;for(let i=0;i<20000000;i++)x+=i;return {dispose(){}}}", ExtensionRuntime.Failure.cpu), ("import 'host:/native';export function activate(){return {dispose(){}}}", .script)] {
                let (h, e, _, _) = try runtimePair(q), r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(plugin), entry: "plugin:/entry.js", role: .executor, cpuMilliseconds: expected == .cpu ? 10 : 500))
                for _ in 0..<100 { if try r.step() == .failed { break } }
                let info = try r.info(); XCTAssertEqual(info.phase, .failed); XCTAssertEqual(info.error, expected); try runtimeClose(r); try runtimeClose(h)
            }
        }
    }
    func testStopFromAnotherThread() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q), r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: "plugin:/entry.js", role: .executor, cpuMilliseconds: 500))
            for _ in 0..<100 { if try r.step() == .active { break } }
            DispatchQueue.global().sync { r.stop() }; XCTAssertEqual(try r.step(), .stopped); XCTAssertEqual(try r.info().error, .cancelled); try runtimeClose(r); try runtimeClose(h)
        }
    }
    func testDigestAndRoleFailuresKeepSession() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q), modules = try runtimeModules(runtimeEcho)
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: modules, entry: "plugin:/entry.js", role: .host)))
            var broken = modules; let last = try XCTUnwrap(broken.last); broken[broken.count - 1] = .init(name: last.name, source: last.source, sha256: String(repeating: "0", count: 64))
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: broken, entry: "plugin:/entry.js", role: .executor)))
            XCTAssertEqual(try e.info().phase, .active); try runtimeClose(e); try runtimeClose(h)
        }
    }
    func testExplicitCloseInertsHandle() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q), r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: "plugin:/entry.js", role: .executor, cpuMilliseconds: 500))
            for _ in 0..<100 { if try r.step() == .active { break } }
            try runtimeClose(r); XCTAssertThrowsError(try r.info()); try runtimeClose(h)
        }
    }
    func testDroppedRuntimeCleansOnOwnerQueue() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q); var r: ExtensionRuntime? = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: "plugin:/entry.js", role: .executor, cpuMilliseconds: 500))
            for _ in 0..<100 { if try r?.step() == .active { break } }
            weak var weakRuntime = r; r = nil; XCTAssertNil(weakRuntime)
            q.async { do { XCTAssertEqual(ExtensionRuntime.pendingCloseCount, 0); ExtensionRuntime.retryPendingClose(on: q); try runtimeClose(h) } catch { XCTFail("runtime reaper: \(error)") } }
        }
    }
}
