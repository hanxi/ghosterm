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
    func testScopedPublicationCopiesRootAndWaitsForReceipt() {
        runtimeWorker(self) { q in
            let (hs, hd) = try runtimeStore(q), (es, ed) = try runtimeStore(q), dir = try runtimeDirectory()
            defer { for d in [hd, ed, dir] { try? FileManager.default.removeItem(at: d) } }
            let bytes = Data((0..<262144).map { UInt8($0 % 251) }); try bytes.write(to: dir.appendingPathComponent("file.bin"))
            let fd = Darwin.open(dir.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); XCTAssertGreaterThanOrEqual(fd, 0)
            let (h, e, hl, _) = try runtimePair(q, hostStore: hs, executorStore: es)
            let plugin = "export async function activate(ctx){if(ctx.resources.workspaces[0]!=='workspace')throw Error('root');const c=ctx.resources.publish({root:'workspace',relativePath:'file.bin',mediaType:'application/octet-stream'},{byteLimit:'262144',timeoutMs:10000});const o=await c.outcome;if(o.kind!=='success'||o.value.size.bytes!=='262144')throw Error('receipt');return {dispose(){}}}"
            let modules = try runtimeModules(plugin, resources: true)
            XCTAssertThrowsError(try ExtensionRuntime(session: e, config: .init(modules: modules, entry: "plugin:/entry.js", role: .executor, roots: [.init(id: "workspace", directoryFD: -1, byteLimit: 262144)])))
            XCTAssertEqual(try e.info().phase, .active)
            let r = try ExtensionRuntime(session: e, config: .init(modules: modules, entry: "plugin:/entry.js", role: .executor, cpuMilliseconds: 500, roots: [.init(id: "workspace", directoryFD: fd, byteLimit: 262144)]))
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
    func testResourceReadRejectsCopiedReferenceAndReleasesPin() {
        runtimeWorker(self) { q in
            let (hs, hd) = try runtimeStore(q), (es, ed) = try runtimeStore(q)
            defer { for d in [hd, ed] { try? FileManager.default.removeItem(at: d) } }
            let (h, e, hl, _) = try runtimePair(q, hostStore: hs, executorStore: es)
            let plugin = "import {object,text,integer} from '@ghosterm/plugin-sdk';export function activate(ctx){const codec=object({text:text(40),count:integer(0,100)});ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{const t=ctx.resources.take();if(t.status!=='ok'||ctx.resources.open({...t.ref},{byteLimit:'3',timeoutMs:1000}).status!=='invalid')throw Error('ref');const o=ctx.resources.open(t.ref,{byteLimit:'3',timeoutMs:1000});if(o.status!=='ok')throw Error('open');const a=await o.stream.read();c.check();const b=await o.stream.read();c.check();if(a.kind!=='success'||String(a.value.bytes)!=='97,98,99'||b.kind!=='success'||!b.value.done||ctx.resources.release(t.ref)!=='ok')throw Error('read');return v});return {dispose(){}}}"
            let r = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(plugin, resources: true), entry: "plugin:/entry.js", role: .executor, cpuMilliseconds: 500))
            for _ in 0..<100 { _ = try h.step(); if try r.step() == .active { break } }
            let d = ExtensionResourceStore.Descriptor(sha256: "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", mime: "application/octet-stream", displayName: "file.bin", size: 3)
            let admission = try h.sendResource(hl, descriptor: d), transfer = try XCTUnwrap(admission.handle)
            XCTAssertEqual(try h.writeResource(hl, handle: transfer, bytes: Data("abc".utf8)).1, 3); XCTAssertEqual(try h.finishResource(hl, handle: transfer, sourceComplete: true), .ok)
            for _ in 0..<1000 { _ = try h.step(); _ = try r.step(); if try h.resourceResult(hl, handle: transfer).1?.confirmed == true { break } }
            XCTAssertEqual(try h.forgetResource(hl, handle: transfer), .ok)
            let handle = try XCTUnwrap(h.request(hl, method: "fixture.echo", input: Data("{\"text\":\"read\",\"count\":17}".utf8)).handle)
            for _ in 0..<1000 { _ = try h.step(); _ = try r.step(); if try h.requestResult(hl, handle: handle).1?.confirmed == true { break } }
            XCTAssertEqual(try h.requestResult(hl, handle: handle).1?.state, .succeeded); XCTAssertEqual(try h.forgetRequest(hl, handle: handle), .ok); XCTAssertEqual(try es.usage().resources, 0)
            try runtimeClose(r); try runtimeClose(h); XCTAssertEqual(try hs.close(), .ok); XCTAssertEqual(try es.close(), .ok)
        }
    }
    func testRootsWithoutResourceSessionAreUnsupported() {
        runtimeWorker(self) { q in
            let (h, e, _, _) = try runtimePair(q)
            do { _ = try ExtensionRuntime(session: e, config: .init(modules: runtimeModules(runtimeEcho), entry: "plugin:/entry.js", role: .executor, roots: [.init(id: "workspace", directoryFD: -1, byteLimit: 3)])); XCTFail("root accepted without resource Session") }
            catch GhostermError.status(let code, _) { XCTAssertEqual(code, 7) }
            XCTAssertEqual(try e.info().phase, .active); try runtimeClose(e); try runtimeClose(h)
        }
    }
}
