import Darwin
import Foundation
import XCTest
@testable import GhostermAppModel

final class SessionRecordStoreTests: XCTestCase {
    private func record(id: UUID = UUID()) -> SessionRecord {
        var value = id.uuid
        let nonce = withUnsafeBytes(of: &value) { Array($0) }
        return SessionRecord(id: id, kind: .local, endpoint: .init(proxy: "/fixture/proxy", daemon: "/fixture/daemon", runtime: "/fixture/runtime", contract: String(repeating: "a", count: 64)), nonce: nonce)
    }
    private func fixture(_ body: @escaping (SessionRecordStore, URL, DispatchQueue) throws -> Void) {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let directory = root.appendingPathComponent("tmp/session-record-" + UUID().uuidString)
        let done = expectation(description: "private record IO")
        let owner = DispatchQueue(label: "dev.ghosterm.test.records")
        owner.async {
            defer { try? FileManager.default.removeItem(at: directory); done.fulfill() }
            do {
                let store = SessionRecordStore(directory: directory, owner: owner)
                try body(store, directory, owner)
            } catch { XCTFail("\(error)") }
        }
        wait(for: [done], timeout: 10)
    }
    func testRoundtripPromotionAndKnownZero() {
        let initial = record()
        fixture { store, directory, _ in
            XCTAssertEqual(try store.load(), [])
            try store.upsert(initial); XCTAssertEqual(try store.load(), [initial])
            var promoted = initial; promoted.instance = Array(repeating: 1, count: 16); promoted.session = UInt64.max; promoted.admitted = true
            try store.upsert(promoted)
            promoted.resultKnown = true; promoted.exitCode = 0; promoted.disposition = .ended
            try store.upsert(promoted); XCTAssertEqual(try store.load(), [promoted])
            XCTAssertThrowsError(try store.upsert(initial)); XCTAssertEqual(try store.load(), [promoted])
            var dropped = promoted; dropped.exitCode = nil; XCTAssertThrowsError(try store.upsert(dropped))
            let file = directory.appendingPathComponent("sessions.json")
            var info = stat(); XCTAssertEqual(file.path.withCString { lstat($0, &info) }, 0)
            XCTAssertEqual(info.st_mode & 0o777, 0o600)
            XCTAssertFalse(try String(contentsOf: file).contains("PRIVATE KEY"))
            XCTAssertFalse(try FileManager.default.contentsOfDirectory(atPath: directory.path).contains(where: { $0.hasPrefix(".sessions-") }))
            try store.forget(initial.id); XCTAssertEqual(try store.load(), [])
        }
    }
    func testWritersReloadAndPreserveAuthority() {
        let first = record(), second = record()
        fixture { store, directory, owner in
            let other = SessionRecordStore(directory: directory, owner: owner)
            XCTAssertEqual(try store.load(), []); XCTAssertEqual(try other.load(), [])
            try store.upsert(first); try other.upsert(second)
            XCTAssertEqual(Set(try store.load().map(\.id)), Set([first.id, second.id]))
            var wrong = first; wrong.endpoint.runtime = "/other/runtime"
            XCTAssertThrowsError(try store.upsert(wrong))
            var migrated = first; migrated.endpoint.proxy = "/moved/app/proxy"; migrated.endpoint.daemon = "/moved/app/daemon"
            try store.upsert(migrated)
            XCTAssertEqual(try other.load().first { $0.id == first.id }, migrated)
            var duplicate = self.record(); duplicate.nonce = first.nonce
            XCTAssertThrowsError(try store.upsert(duplicate))
        }
    }
    func testCorruptionAndFutureVersionNeverOverwrite() {
        let valid = record()
        fixture { store, directory, _ in
            _ = try store.load()
            let file = directory.appendingPathComponent("sessions.json")
            for bytes in [Data("{broken".utf8), Data("{\"format\":2,\"records\":[]}".utf8)] {
                try bytes.write(to: file); XCTAssertEqual(chmod(file.path, 0o600), 0)
                XCTAssertThrowsError(try store.load()); XCTAssertThrowsError(try store.upsert(valid))
                XCTAssertEqual(try Data(contentsOf: file), bytes)
            }
        }
    }
    func testSymlinkAndPermissionsRejected() {
        let valid = record()
        fixture { store, directory, owner in
            try store.upsert(valid)
            let file = directory.appendingPathComponent("sessions.json")
            XCTAssertEqual(chmod(file.path, 0o644), 0); XCTAssertThrowsError(try store.load()); XCTAssertEqual(chmod(file.path, 0o600), 0)
            XCTAssertEqual(chmod(directory.path, 0o755), 0); XCTAssertThrowsError(try store.load()); XCTAssertEqual(chmod(directory.path, 0o700), 0)
            let kept = try Data(contentsOf: file)
            let real = directory.appendingPathComponent("kept.json")
            try FileManager.default.moveItem(at: file, to: real)
            XCTAssertEqual(symlink(real.path, file.path), 0); XCTAssertThrowsError(try store.load()); XCTAssertThrowsError(try store.upsert(valid))
            XCTAssertEqual(try Data(contentsOf: real), kept)
            XCTAssertTrue(FileManager.default.fileExists(atPath: real.path))
            let alias = directory.appendingPathComponent("alias")
            XCTAssertEqual(symlink(directory.path, alias.path), 0)
            XCTAssertThrowsError(try SessionRecordStore(directory: alias, owner: owner).load())
        }
    }
    func testLockContentionIsBounded() {
        fixture { store, directory, _ in
            _ = try store.load()
            let fd = open(directory.appendingPathComponent("sessions.lock").path, O_RDWR | O_CLOEXEC | O_NOFOLLOW)
            XCTAssertGreaterThanOrEqual(fd, 0); guard fd >= 0 else { return }; defer { close(fd) }
            XCTAssertEqual(flock(fd, LOCK_EX | LOCK_NB), 0)
            let began = ProcessInfo.processInfo.systemUptime
            XCTAssertThrowsError(try store.load()) { guard case SessionRecordStore.Error.busy = $0 else { return XCTFail("expected explicit busy") } }
            XCTAssertLessThan(ProcessInfo.processInfo.systemUptime - began, 0.5)
            XCTAssertEqual(flock(fd, LOCK_UN), 0); XCTAssertEqual(try store.load(), [])
        }
    }
    func testInvalidMetadataAndAuthorityRegressionRejected() {
        let valid = record()
        fixture { store, _, _ in
            var bad = valid; bad.admitted = true; XCTAssertThrowsError(try store.upsert(bad))
            bad = valid; bad.nonce = nil; XCTAssertThrowsError(try store.upsert(bad))
            bad = valid; bad.instance.removeLast(); XCTAssertThrowsError(try store.upsert(bad))
            bad = valid; bad.endpoint.proxy = "relative"; XCTAssertThrowsError(try store.upsert(bad))
            bad = valid; bad.exitCode = 0; XCTAssertThrowsError(try store.upsert(bad))
            bad = valid; bad.kind = .ssh; XCTAssertThrowsError(try store.upsert(bad))
            try store.upsert(valid)
            var promoted = valid; promoted.instance[0] = 1; promoted.admitted = true; promoted.session = 5
            try store.upsert(promoted)
            bad = promoted; bad.instance[0] = 2; XCTAssertThrowsError(try store.upsert(bad))
            bad = promoted; bad.session = 6; XCTAssertThrowsError(try store.upsert(bad))
            bad = promoted; bad.admitted = false; XCTAssertThrowsError(try store.upsert(bad))
            bad = promoted; bad.disposition = .stopUnknown; bad.stopStatus = 0; XCTAssertThrowsError(try store.upsert(bad))
            bad = promoted; bad.disposition = .stopRejected; bad.stopStatus = nil; XCTAssertThrowsError(try store.upsert(bad))
            XCTAssertEqual(try store.load(), [promoted])
            var busy = promoted; busy.disposition = .stopRejected; busy.stopStatus = 2
            try store.upsert(busy); XCTAssertEqual(try store.load(), [busy])
        }
    }
    func testStopUncertaintyAndEndedStateCannotBeLostByStaleWriter() {
        var value = record(); value.instance[0] = 1; value.session = 5; value.admitted = true
        let initial = value
        fixture { store, _, _ in
            try store.upsert(initial)
            var pending = initial; pending.disposition = .stopUnknown
            try store.upsert(pending)
            XCTAssertThrowsError(try store.upsert(initial), "a stale live observer must not erase uncertain TERMINATE")
            XCTAssertEqual(try store.load(), [pending])
            var invalid = pending; invalid.resultKnown = true
            XCTAssertThrowsError(try store.upsert(invalid), "known result requires ended disposition")
            var ended = pending; ended.disposition = .ended
            try store.upsert(ended)
            XCTAssertThrowsError(try store.upsert(pending), "a confirmed absent Source cannot be resurrected without a known exit code")
            XCTAssertEqual(try store.load(), [ended])
        }
    }
    func testBoundedValidJSONAndRecordCount() {
        let records = (0..<1025).map { _ in record() }
        fixture { store, directory, _ in
            _ = try store.load(); let file = directory.appendingPathComponent("sessions.json")
            let array = try JSONSerialization.jsonObject(with: JSONEncoder().encode(records))
            var bytes = try JSONSerialization.data(withJSONObject: ["format": 1, "records": array])
            XCTAssertLessThan(bytes.count, 1_048_576)
            try bytes.write(to: file); XCTAssertEqual(chmod(file.path, 0o600), 0); XCTAssertThrowsError(try store.load())
            bytes = try JSONSerialization.data(withJSONObject: ["format": 1, "records": [], "padding": String(repeating: "x", count: 1_048_577)])
            try bytes.write(to: file); XCTAssertEqual(chmod(file.path, 0o600), 0); XCTAssertThrowsError(try store.load())
        }
    }
}
