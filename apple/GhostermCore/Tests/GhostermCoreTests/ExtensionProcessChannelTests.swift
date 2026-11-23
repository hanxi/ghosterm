import CryptoKit
import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionProcessChannelTests: XCTestCase {
    private func worker(_ body: @escaping (DispatchQueue, String, Int32, String) throws -> Void) {
        let done = expectation(description: "owned process channel"), queue = DispatchQueue(label: "ghosterm.process-channel.test")
        queue.async {
            do {
                var root = URL(fileURLWithPath: #filePath)
                for _ in 0..<5 { root.deleteLastPathComponent() }
                let helper = ProcessInfo.processInfo.environment["GHOSTERM_PROCESS_CHANNEL_CHILD"] ?? root.appendingPathComponent("core/build/test_process_channel_child").path
                guard FileManager.default.isExecutableFile(atPath: helper) else { throw GhostermError.failed("缺少本轮编译的进程测试夹具") }
                let cwd = root.appendingPathComponent("tmp/process-channel-\(UUID().uuidString)")
                try FileManager.default.createDirectory(at: cwd, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
                defer { try? FileManager.default.removeItem(at: cwd) }
                let fd = Darwin.open(cwd.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                guard fd >= 0 else { throw GhostermError.failed("fixture cwd") }
                defer { Darwin.close(fd) }
                try body(queue, helper, fd, cwd.resolvingSymlinksInPath().path)
            } catch { XCTFail("process channel: \(error)") }
            done.fulfill()
        }
        wait(for: [done], timeout: 20)
    }
    private func config(_ helper: String, _ fd: Int32, _ mode: String) -> ExtensionChannel.ProcessConfiguration {
        .init(executable: helper, arguments: ["raw argv zero", mode], directoryFD: fd,
              environment: ["GT_SELECTED=only"], executionAuthorized: true)
    }
    private func close(_ operation: () throws -> Bool) throws {
        let end = Date().addingTimeInterval(2)
        while Date() < end { if try operation() { return }; Thread.sleep(forTimeInterval: 0.001) }
        XCTFail("process owner not released within existing two-second cleanup budget")
    }
    private func ready(_ channel: ExtensionChannel) throws {
        let end = Date().addingTimeInterval(2)
        while Date() < end {
            let (io, data) = try channel.read(capacity: 1, milliseconds: 5)
            if io.state == .data { XCTAssertEqual(data, Data([82])); return }
            XCTAssertEqual(io.state, .wait)
        }
        XCTFail("fixture not ready")
    }
    func testRawArgumentsDirectoryEnvironmentAndFinalExit() {
        worker { owner, helper, fd, cwd in
            let args = ["raw argv zero", "context", "", "目录⚠️", "$(touch nope); ' \\"]
            let channel = try ExtensionChannel(process: .init(executable: helper, arguments: args,
                directoryFD: fd, environment: ["GT_SELECTED=only"], executionAuthorized: true), owner: owner)
            defer { try? self.close { try channel.closeStep(milliseconds: 1) } }
            var bytes = Data(); let end = Date().addingTimeInterval(2)
            while Date() < end {
                let (io, data) = try channel.read(milliseconds: 5); bytes.append(data)
                if try io.state == .eof && channel.processObservation().reaped { break }
                XCTAssertTrue(io.state == .data || io.state == .wait || io.state == .eof)
            }
            let expected = (args + [cwd, "only", "no-home", "no-path"]).joined(separator: "\0") + "\0"
            XCTAssertEqual(bytes, Data(expected.utf8))
            let observation = try channel.processObservation()
            XCTAssertTrue(observation.childExited && observation.reaped && observation.stdoutEOF)
            XCTAssertEqual(observation.exitCode, 23); XCTAssertNil(observation.termSignal)
            XCTAssertEqual(observation.diagnostic, Data([68, 0, 255, 10]))
            XCTAssertEqual(try channel.info().exitStatus, 23)
            try self.close { try channel.closeStep(milliseconds: 1) }
            XCTAssertEqual(try channel.processObservation().exitCode, 23)
            XCTAssertThrowsError(try channel.read())
            XCTAssertGreaterThanOrEqual(fcntl(fd, F_GETFD), 0)
        }
    }
    func testDuplex256KiBAndFinalStopObservation() {
        worker { owner, helper, fd, _ in
            let channel = try ExtensionChannel(process: self.config(helper, fd, "echo"), owner: owner)
            defer { try? self.close { try channel.closeStep(milliseconds: 1) } }
            try self.ready(channel)
            for block in 0..<16 {
                let input = Data((0..<16384).map { UInt8(($0 + block) % 251) })
                XCTAssertEqual(try channel.write(Data(input)).count, input.count)
                var output = Data(); let end = Date().addingTimeInterval(2)
                while output.count < input.count && Date() < end {
                    XCTAssertEqual(try channel.step(milliseconds: 1), .ready)
                    let (io, bytes) = try channel.read(milliseconds: 1)
                    XCTAssertTrue(io.state == .data || io.state == .wait); output.append(bytes)
                }
                XCTAssertEqual(output, input)
            }
            XCTAssertEqual(try channel.processObservation().stdoutBytes, 262145)
            try self.close { try channel.closeStep(milliseconds: 1) }
            XCTAssertTrue(try channel.processObservation().reaped)
            XCTAssertEqual(try channel.processObservation().diagnostic, Data([68, 0, 255, 10]))
        }
    }
    func testEOFDoesNotClaimExitAndStopWakesRead() {
        worker { owner, helper, fd, _ in
            let eof = try ExtensionChannel(process: self.config(helper, fd, "eof-hold"), owner: owner)
            defer { try? self.close { try eof.closeStep(milliseconds: 1) } }
            let end = Date().addingTimeInterval(2)
            var state = ExtensionChannel.IOState.wait
            while state == .wait && Date() < end { state = try eof.read(milliseconds: 5).0.state }
            XCTAssertEqual(state, .eof); XCTAssertFalse(try eof.processObservation().childExited)
            XCTAssertFalse(try eof.processObservation().reaped)
            try self.close { try eof.closeStep(milliseconds: 1) }
            XCTAssertTrue(try eof.processObservation().reaped)
            let channel = try ExtensionChannel(process: self.config(helper, fd, "hold"), owner: owner)
            defer { try? self.close { try channel.closeStep(milliseconds: 1) } }
            try self.ready(channel)
            let stopped = DispatchSemaphore(value: 0)
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.02) { channel.stop(); stopped.signal() }
            let start = Date(); XCTAssertEqual(try channel.read(milliseconds: 1000).0.state, .closed)
            XCTAssertEqual(stopped.wait(timeout: .now() + 1), .success)
            XCTAssertLessThan(Date().timeIntervalSince(start), 0.5)
            try self.close { try channel.closeStep(milliseconds: 1) }
            XCTAssertTrue(try channel.processObservation().stopping && channel.processObservation().reaped)
        }
    }
    private func session(_ channel: ExtensionChannel) throws -> ExtensionSession {
        let config = ExtensionSession.Config(pluginID: "org.ghosterm.process-fixture", pluginVersion: "0.1.0",
            clientDigest: String(repeating: "d", count: 64), executorDigest: String(repeating: "e", count: 64),
            hostVersion: "0.1.0", required: ["ui.form"], available: ["ui.form"], peerKind: .local,
            peerIdentity: "process-fixture", methods: [.init(name: "fixture.echo", capability: "ui.form", receiver: .executor, inputShape: .object, outputShape: .object)])
        let session = try ExtensionSession(channel: channel, role: .host, config: config)
        let end = Date().addingTimeInterval(2); var consent = false
        while Date() < end {
            let phase = try session.step(milliseconds: 1)
            if phase == .consent {
                XCTAssertEqual(try session.info().decision, .unknown); consent = true
                XCTAssertEqual(try session.decide(ticket: session.info().consentToken, decision: .allow), 0)
            }
            if phase == .active { XCTAssertTrue(consent); return session }
        }
        throw GhostermError.failed("explicit fixture consent handshake failed")
    }
    func testSessionAdoptionPreservesProcessAndFinalDiagnostic() {
        worker { owner, helper, fd, _ in
            let channel = try ExtensionChannel(process: self.config(helper, fd, "session"), owner: owner)
            let session = try self.session(channel)
            defer { try? self.close { try session.closeStep(milliseconds: 1) } }
            XCTAssertThrowsError(try channel.processObservation())
            XCTAssertFalse(try session.processObservation().childExited)
            XCTAssertEqual(try session.processObservation().diagnostic, Data([68, 0, 255, 10]))
            try self.close { try session.closeStep(milliseconds: 1) }
            XCTAssertTrue(try session.processObservation().reaped)
            XCTAssertEqual(try session.processObservation().exitCode, 37)
        }
    }
    func testRuntimeAdoptionRetainsFinalTransportSnapshot() throws {
        guard ExtensionRuntime.supportStatus == 0 else { throw XCTSkip("explicit Lua-enabled runtime profile required") }
        worker { owner, helper, fd, _ in
            let channel = try ExtensionChannel(process: self.config(helper, fd, "session"), owner: owner)
            let session = try self.session(channel)
            let source = Data("return {}".utf8)
            let entry = "plugin:/entry.lua"
            let module = ExtensionRuntime.Module(name: entry, source: source, sha256: SHA256.hash(data: source).map { String(format: "%02x", $0) }.joined())
            let runtime = try ExtensionRuntime(session: session, config: .init(modules: [module], entry: entry, role: .host))
            defer { try? self.close { try runtime.closeStep() } }
            XCTAssertThrowsError(try session.processObservation())
            XCTAssertFalse(try runtime.transportProcessObservation().childExited)
            XCTAssertEqual(try runtime.step(), .active)
            try self.close { try runtime.closeStep() }
            let final = try runtime.transportProcessObservation()
            XCTAssertTrue(final.reaped && final.childExited); XCTAssertEqual(final.exitCode, 37)
            XCTAssertEqual(final.diagnostic, Data([68, 0, 255, 10]))
        }
    }
    func testMissingApprovalAndNulRefuseWithoutTakingDirectory() {
        worker { owner, helper, fd, _ in
            XCTAssertThrowsError(try ExtensionChannel(process: .init(executable: helper, arguments: [helper, "hold"], directoryFD: fd, executionAuthorized: false), owner: owner))
            XCTAssertThrowsError(try ExtensionChannel(process: .init(executable: helper, arguments: [helper, "bad\0argument"], directoryFD: fd, executionAuthorized: true), owner: owner))
            XCTAssertGreaterThanOrEqual(fcntl(fd, F_GETFD), 0)
        }
    }
}
