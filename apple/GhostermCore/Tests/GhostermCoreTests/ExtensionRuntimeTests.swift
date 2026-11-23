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
            let r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: runtimeEntry, role: .executor, cpuMilliseconds: 500))
            XCTAssertThrowsError(try e.info()); e.stop(); XCTAssertEqual(try r.info().phase, .created)
            let handle = try XCTUnwrap(h.request(hl, method: "fixture.echo", input: Data("{\"text\":\"中文\",\"count\":17}".utf8), timeoutMilliseconds: 10000).handle)
            var completed = false
            for _ in 0..<2000 { _ = try h.step(); _ = try r.step(); if try h.requestResult(hl, handle: handle).1?.confirmed == true { completed = true; break } }
            XCTAssertTrue(completed); let result = try XCTUnwrap(h.requestResult(hl, handle: handle).1); XCTAssertEqual(result.state, .succeeded); XCTAssertTrue(String(decoding: result.outcome, as: UTF8.self).contains("中文"))
            XCTAssertEqual(try h.forgetRequest(hl, handle: handle), .ok); let saved = try r.info(); XCTAssertEqual(saved.phase, .active); XCTAssertGreaterThan(saved.steps, 0); XCTAssertGreaterThan(saved.moduleBytes, 0)
            try runtimeClose(r); try runtimeClose(h); XCTAssertGreaterThan(saved.moduleBytes, 0)
        }
    }
    func testNativeHostRuntimeCanCallRemoteExecutorAndPollItsOutcome() {
        runtimeWorker(self) { q in
            let (h, e, _, el) = try runtimePair(q)
            let hostPlugin = "return {}"
            let runtime = try ExtensionRuntime(session: h,
                config: .init(modules: runtimeModules(hostPlugin), entry: runtimeEntry,
                              role: .host, cpuMilliseconds: 500))
            for _ in 0..<100 { if try runtime.step() == .active { break } }
            XCTAssertEqual(try runtime.info().phase, .active)

            let input = Data("{\"threadId\":\"thread-a\"}".utf8)
            let admission = try runtime.request(method: "fixture.echo", input: input,
                                                timeoutMilliseconds: 10000)
            XCTAssertEqual(admission.status, .ok)
            let handle = try XCTUnwrap(admission.handle)
            var received: ExtensionSession.RPCRequest?
            for _ in 0..<2000 {
                _ = try runtime.step(); _ = try e.step()
                let (status, request) = try e.takeRequest(el)
                if let request { XCTAssertEqual(status, .ok); received = request; break }
                XCTAssertEqual(status, .wait)
            }
            let request = try XCTUnwrap(received)
            XCTAssertEqual(request.method, "fixture.echo")
            XCTAssertEqual(request.input, input)
            let outcome = Data("{\"kind\":\"success\",\"value\":{\"threadId\":\"thread-a\",\"turnId\":\"turn-b\"}}".utf8)
            XCTAssertEqual(try e.respond(el, ticket: request.ticket, outcome: outcome), .ok)

            var result: ExtensionSession.RPCResult?
            for _ in 0..<2000 {
                _ = try runtime.step(); _ = try e.step()
                let (status, value) = try runtime.requestResult(handle: handle)
                XCTAssertEqual(status, .ok)
                if value?.confirmed == true { result = value; break }
            }
            let completed = try XCTUnwrap(result)
            XCTAssertEqual(completed.state, .succeeded)
            XCTAssertEqual(completed.outcome, outcome)
            XCTAssertEqual(try runtime.forgetRequest(handle: handle), .ok)
            runtime.stop()
            XCTAssertEqual(try runtime.step(), .stopped)
            let stopped = try runtime.request(method: "fixture.echo", input: input)
            XCTAssertEqual(stopped.status, .closed)
            XCTAssertNil(stopped.handle)
            try runtimeClose(runtime); try runtimeClose(e)
        }
    }
    func testHostRuntimeReceivesAndRepliesToExecutorRequestAfterAdoptingSession() {
        runtimeWorker(self) { q in
            let (h, e, _, el) = try runtimePair(q)
            let hostPlugin = "return {}"
            let runtime = try ExtensionRuntime(session: h,
                config: .init(modules: runtimeModules(hostPlugin), entry: runtimeEntry,
                              role: .host, cpuMilliseconds: 500))
            for _ in 0..<100 { if try runtime.step() == .active { break } }
            let runtimeInfo = try runtime.info()
            XCTAssertEqual(runtimeInfo.phase, .active, "Runtime error: \(runtimeInfo.error)")

            let input = Data("{\"sequence\":1,\"kind\":\"notification\"}".utf8)
            let handle = try XCTUnwrap(e.request(el, method: "fixture.ask", input: input,
                                                  timeoutMilliseconds: 10000).handle)
            var received: ExtensionSession.RPCRequest?
            for _ in 0..<1000 {
                _ = try e.step(); _ = try runtime.step()
                let (status, request) = try runtime.takeHostRequest()
                if let request { XCTAssertEqual(status, .ok); received = request; break }
                XCTAssertEqual(status, .wait)
            }
            let request = try XCTUnwrap(received)
            XCTAssertEqual(request.method, "fixture.ask")
            XCTAssertEqual(request.input, input)
            let outcome = Data("{\"kind\":\"success\",\"value\":{\"accepted\":true}}".utf8)
            XCTAssertEqual(try runtime.respondHostRequest(ticket: request.ticket, outcome: outcome), .ok)

            var result: ExtensionSession.RPCResult?
            for _ in 0..<1000 {
                _ = try e.step(); _ = try runtime.step()
                let (status, value) = try e.requestResult(el, handle: handle)
                XCTAssertEqual(status, .ok)
                if value?.confirmed == true { result = value; break }
            }
            let completed = try XCTUnwrap(result)
            XCTAssertEqual(completed.state, .succeeded)
            XCTAssertEqual(completed.outcome, outcome)
            XCTAssertEqual(try e.forgetRequest(el, handle: handle), .ok)
            try runtimeClose(runtime); try runtimeClose(e)
        }
    }
    func testRuntimeRequestRequiresAnActiveHostRuntime() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q)
            let hostPlugin = "return {}"
            let runtime = try ExtensionRuntime(session: h,
                config: .init(modules: runtimeModules(hostPlugin), entry: runtimeEntry,
                              role: .host, cpuMilliseconds: 500))
            XCTAssertEqual(try runtime.info().phase, .created)
            let admission = try runtime.request(method: "fixture.echo", input: Data("{}".utf8))
            XCTAssertEqual(admission.status, .closed)
            XCTAssertNil(admission.handle)
            try runtimeClose(runtime); try runtimeClose(e)
        }
    }
    func testNativeHostCanTakeResourceAfterRuntimeAdoptsSession() {
        runtimeWorker(self) { q in
            let (hs, hd) = try runtimeStore(q), (es, ed) = try runtimeStore(q)
            defer { try? FileManager.default.removeItem(at: hd); try? FileManager.default.removeItem(at: ed) }
            let (h, e, _, el) = try runtimePair(q, hostStore: hs, executorStore: es)
            let hostPlugin = "return {}"
            let runtime = try ExtensionRuntime(session: h,
                config: .init(modules: runtimeModules(hostPlugin),
                              entry: runtimeEntry, role: .host, cpuMilliseconds: 500))
            for _ in 0..<100 { if try runtime.step() == .active { break } }
            XCTAssertEqual(try runtime.info().phase, .active)

            let bytes = Data("resource while runtime owns session\n".utf8)
            let digest = SHA256.hash(data: bytes).map { String(format: "%02x", $0) }.joined()
            let descriptor = ExtensionResourceStore.Descriptor(sha256: digest, mime: "text/plain",
                                                                 displayName: "fixture.txt", size: UInt64(bytes.count))
            let transfer = try XCTUnwrap(e.sendResource(el, descriptor: descriptor).handle)
            XCTAssertEqual(try e.writeResource(el, handle: transfer, bytes: bytes).1, bytes.count)
            XCTAssertEqual(try e.finishResource(el, handle: transfer, sourceComplete: true), .ok)

            var received: ExtensionSession.ResourceContent?
            for _ in 0..<1000 {
                _ = try e.step()
                _ = try runtime.step()
                let (status, content) = try runtime.takeResourceData(maxBytes: 1024)
                if status == .ok { received = content; break }
                XCTAssertEqual(status, .wait)
            }
            let content = try XCTUnwrap(received)
            XCTAssertEqual(content.bytes, bytes)
            XCTAssertEqual(content.size, UInt64(bytes.count))
            XCTAssertEqual(content.mime, "text/plain")
            XCTAssertEqual(content.displayName, "fixture.txt")
            XCTAssertEqual(content.sha256, digest)
            for _ in 0..<20 {
                _ = try e.step()
                _ = try runtime.step()
                if try e.resourceResult(el, handle: transfer).1?.confirmed == true { break }
            }
            XCTAssertEqual(try e.resourceResult(el, handle: transfer).1?.confirmed, true)
            XCTAssertEqual(try e.forgetResource(el, handle: transfer), .ok)
            let oversized = Data("second resource with a different digest".utf8)
            let second = ExtensionResourceStore.Descriptor(
                sha256: SHA256.hash(data: oversized).map { String(format: "%02x", $0) }.joined(),
                mime: "text/plain", displayName: "second.txt", size: UInt64(oversized.count))
            let limited = try XCTUnwrap(e.sendResource(el, descriptor: second).handle)
            XCTAssertEqual(try e.writeResource(el, handle: limited, bytes: oversized).1, oversized.count)
            XCTAssertEqual(try e.finishResource(el, handle: limited, sourceComplete: true), .ok)
            for _ in 0..<1000 {
                _ = try e.step()
                _ = try runtime.step()
                if try e.resourceResult(el, handle: limited).1?.confirmed == true { break }
            }
            XCTAssertEqual(try e.resourceResult(el, handle: limited).1?.confirmed, true)
            XCTAssertEqual(try runtime.takeResourceData(maxBytes: 1).0, .limit)
            XCTAssertEqual(try e.forgetResource(el, handle: limited), .ok)
            XCTAssertEqual(try hs.usage().resources, 0)
            XCTAssertEqual(try es.usage().resources, 0)
            try runtimeClose(runtime)
            XCTAssertEqual(try h.takeRuntimeResourceData(maxBytes: 1).0, .closed)
            try runtimeClose(e)
            XCTAssertEqual(try hs.usage().resources, 0)
            XCTAssertEqual(try hs.close(), .ok)
            XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testExistingRequestRejectsAdoption() {
        runtimeWorker(self) { q in
            let (h, e, _, el) = try runtimePair(q), handle = try XCTUnwrap(e.request(el, method: "fixture.ask", input: Data("{}".utf8)).handle)
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: runtimeEntry, role: .executor)))
            XCTAssertEqual(try e.info().phase, .active); XCTAssertEqual(try e.cancelRequest(el, handle: handle), .ok); XCTAssertEqual(try e.forgetRequest(el, handle: handle), .ok)
            try runtimeClose(e); try runtimeClose(h)
        }
    }
    func testCPUAndImportFailureRemainDistinct() {
        runtimeWorker(self) { q in
            for (plugin, expected) in [("while true do end", ExtensionRuntime.Failure.cpu), ("require('host:/native'); return {}", .script)] {
                let (h, e, _, _) = try runtimePair(q), r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(plugin), entry: runtimeEntry, role: .executor, cpuMilliseconds: expected == .cpu ? 10 : 500))
                for _ in 0..<100 { if try r.step() == .failed { break } }
                let info = try r.info(); XCTAssertEqual(info.phase, .failed); XCTAssertEqual(info.error, expected); try runtimeClose(r); try runtimeClose(h)
            }
        }
    }
    func testStopFromAnotherThread() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q), r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: runtimeEntry, role: .executor, cpuMilliseconds: 500))
            for _ in 0..<100 { if try r.step() == .active { break } }
            DispatchQueue.global().sync { r.stop() }; XCTAssertEqual(try r.step(), .stopped); XCTAssertEqual(try r.info().error, .cancelled); try runtimeClose(r); try runtimeClose(h)
        }
    }
    func testDigestAndRoleFailuresKeepSession() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q), modules = runtimeModules(runtimeEcho)
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: modules, entry: runtimeEntry, role: .host)))
            var broken = modules; let last = try XCTUnwrap(broken.last); broken[broken.count - 1] = .init(name: last.name, source: last.source, sha256: String(repeating: "0", count: 64))
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: broken, entry: runtimeEntry, role: .executor)))
            XCTAssertEqual(try e.info().phase, .active); try runtimeClose(e); try runtimeClose(h)
        }
    }
    func testExplicitCloseInertsHandle() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q), r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: runtimeEntry, role: .executor, cpuMilliseconds: 500))
            for _ in 0..<100 { if try r.step() == .active { break } }
            try runtimeClose(r); XCTAssertThrowsError(try r.info()); try runtimeClose(h)
        }
    }
    func testDroppedRuntimeCleansOnOwnerQueue() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q); var r: ExtensionRuntime? = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: runtimeEntry, role: .executor, cpuMilliseconds: 500))
            for _ in 0..<100 { if try r?.step() == .active { break } }
            weak let weakRuntime = r; r = nil; XCTAssertNil(weakRuntime)
            q.async { do { XCTAssertEqual(ExtensionRuntime.pendingCloseCount, 0); ExtensionRuntime.retryPendingClose(on: q); try runtimeClose(h) } catch { XCTFail("runtime reaper: \(error)") } }
        }
    }
    func testScopedPublicationCopiesRootAndWaitsForReceipt() {
        runtimeWorker(self) { q in
            let (hs, hd) = try runtimeStore(q), (es, ed) = try runtimeStore(q), dir = try runtimeDirectory()
            defer { for d in [hd, ed, dir] { try? FileManager.default.removeItem(at: d) } }
            let bytes = Data((0..<262144).map { UInt8($0 % 251) }); try bytes.write(to: dir.appendingPathComponent("file.bin"))
            let fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); XCTAssertGreaterThanOrEqual(fd, 0)
            let (h, e, hl, _) = try runtimePair(q, hostStore: hs, executorStore: es)
            let plugin = """
                local ctx = ...
                local receipt = assert(ctx.resources.publish("workspace", "file.bin", "application/octet-stream", 262144))
                assert(receipt.size == 262144)
                return {}
                """
            let modules = runtimeModules(plugin)
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: modules, entry: runtimeEntry, role: .executor, roots: [.init(id: "workspace", directoryFD: -1, byteLimit: 262144)])))
            XCTAssertEqual(try e.info().phase, .active)
            let r = try ExtensionRuntime(session: e, config: .init(modules: modules, entry: runtimeEntry, role: .executor, cpuMilliseconds: 500, roots: [.init(id: "workspace", directoryFD: fd, byteLimit: 262144)]))
            Darwin.close(fd)
            for _ in 0..<10000 { _ = try h.step(); if try r.step() == .active { break } }
            guard try r.info().phase == .active else {
                XCTFail("scoped publication did not receive a confirmed result")
                try runtimeClose(r); try runtimeClose(h); _ = try hs.close(); _ = try es.close(); return
            }
            let (status, value) = try h.takeResource(hl); XCTAssertEqual(status, .ok); let snap = try XCTUnwrap(value)
            XCTAssertEqual(snap.descriptor.size, 262144); var received = Data()
            while true { let (st, block) = try snap.read(offset: UInt64(received.count)); XCTAssertEqual(st, .ok); if block.isEmpty { break }; received.append(block) }
            XCTAssertEqual(received, bytes); XCTAssertEqual(try snap.close(), .ok)
            try runtimeClose(r); try runtimeClose(h); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testRootsWithoutResourceSessionAreUnsupported() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q)
            do { _ = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: runtimeEntry, role: .executor, roots: [.init(id: "workspace", directoryFD: -1, byteLimit: 3)])); XCTFail("root accepted without resource Session") }
            catch GhostermError.status(let code, _) { XCTAssertEqual(code, 7) }
            XCTAssertEqual(try e.info().phase, .active); try runtimeClose(e); try runtimeClose(h)
        }
    }
}
