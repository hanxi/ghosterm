import Darwin
import Foundation
import XCTest
@testable import GhostermCore

final class ExtensionMultiplexerTests: XCTestCase {
    func testEightSessionsShareOneConnectionAndCloseIndependently() {
        let done = expectation(description: "multiplex worker")
        let owner = DispatchQueue(label: "ghosterm.multiplex.tests")
        owner.async {
            do { try self.exercise(owner) } catch { XCTFail("multiplex: \(error)") }
            owner.async { done.fulfill() }
        }
        wait(for: [done], timeout: 30)
    }

    private func exercise(_ owner: DispatchQueue) throws {
        var fd: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_STREAM, 0, &fd) == 0 else { throw GhostermError.failed("socketpair") }
        let channels = try fd.map { try ExtensionChannel(readFD: $0, writeFD: $0, owner: owner) }
        let connection = String(repeating: "a", count: 32)
        let muxes = try channels.map { try ExtensionMultiplexer(channel: $0, connection: connection) }
        XCTAssertThrowsError(try channels[0].info())
        var sessions: [[ExtensionSession]] = [[], []]
        defer {
            muxes.forEach { $0.stop() }
            for side in sessions { for session in side { _ = try? session.closeStep() } }
            muxes.forEach { XCTAssertTrue((try? $0.closeStep()) == true) }
        }
        let config = ExtensionSession.Config(pluginID: "org.ghosterm.mux", pluginVersion: "0.1.0",
            clientDigest: String(repeating: "b", count: 64), executorDigest: String(repeating: "c", count: 64),
            hostVersion: "0.1.0", required: ["session.rpc"], available: ["session.rpc"],
            peerKind: .local, peerIdentity: "owned-mux", methods: [
                .init(name: "fixture.echo", capability: "session.rpc", receiver: .executor, inputShape: .object, outputShape: .object)])
        func id(_ slot: Int) -> String { String(format: "%032x", slot + 1) }
        for side in 0..<2 {
            for slot in 0..<8 {
                let channel = try muxes[side].openChannel(instance: id(slot), epoch: UInt64(slot + 1))
                sessions[side].append(try ExtensionSession(channel: channel, role: side == 0 ? .host : .executor, config: config))
            }
            XCTAssertEqual(try muxes[side].info().instances, 8)
        }
        func pump() throws {
            for side in 0..<2 {
                for session in sessions[side] { _ = try session.step() }
                XCTAssertEqual(try muxes[side].step(), 0)
            }
        }
        for _ in 0..<2000 {
            try pump()
            for session in sessions[0] {
                let state = try session.info()
                if state.phase == .consent { XCTAssertEqual(try session.decide(ticket: state.consentToken, decision: .allow), 0) }
            }
            if try sessions.flatMap({ $0 }).allSatisfy({ try $0.info().phase == .active }) { break }
        }
        for side in sessions { for session in side {
            XCTAssertEqual(try session.info().phase, .active)
            XCTAssertEqual(try session.info().connection, connection)
        } }
        let hostLeases = try sessions[0].map { try XCTUnwrap($0.lease()) }
        let executorLeases = try sessions[1].map { try XCTUnwrap($0.lease()) }
        var handles: [UInt64] = []
        for slot in 0..<8 {
            let admission = try sessions[0][slot].request(hostLeases[slot], method: "fixture.echo", input: Data("{\"slot\":\(slot)}".utf8))
            XCTAssertEqual(admission.status, .ok); handles.append(try XCTUnwrap(admission.handle))
        }
        var completed = Set<Int>()
        for _ in 0..<2000 {
            try pump()
            for slot in 0..<8 {
                if let request = try sessions[1][slot].takeRequest(executorLeases[slot]).1 {
                    XCTAssertEqual(request.input, Data("{\"slot\":\(slot)}".utf8))
                    XCTAssertEqual(try sessions[1][slot].respond(executorLeases[slot], ticket: request.ticket,
                        outcome: Data("{\"kind\":\"success\",\"value\":{\"slot\":\(slot)}}".utf8)), .ok)
                }
                if let result = try sessions[0][slot].requestResult(hostLeases[slot], handle: handles[slot]).1, result.confirmed {
                    XCTAssertEqual(result.state, .succeeded); completed.insert(slot)
                }
            }
            if completed.count == 8 { break }
        }
        XCTAssertEqual(completed.count, 8)
        XCTAssertGreaterThan(try muxes[0].transportInfo().sent, 0)
        XCTAssertThrowsError(try muxes[0].processObservation())
        for _ in 0..<1000 { if try sessions[0][3].closeStep() { break }; try pump() }
        sessions[0].remove(at: 3)
        for _ in 0..<50 { try pump() }
        XCTAssertEqual(try sessions[1][3].info().end, .peer)
        XCTAssertTrue(try sessions[0].allSatisfy { try $0.info().phase == .active })
        let final = try muxes[0].scopeInfo(instance: id(3), epoch: 4)
        XCTAssertTrue(final.released && final.closeSent)
        XCTAssertThrowsError(try muxes[0].scopeInfo(instance: id(3), epoch: 99))
        XCTAssertFalse(try muxes[0].closeStep())
        XCTAssertEqual(try muxes[0].info().instances, 7)
    }
}
