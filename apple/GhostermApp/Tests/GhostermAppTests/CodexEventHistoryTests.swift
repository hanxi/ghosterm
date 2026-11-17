import Foundation
@testable import GhostermAppModel
import XCTest

final class CodexEventHistoryTests: XCTestCase {
    private func event(_ sequence: UInt64, text: String = "sample") throws -> String {
        let value: [String: Any] = ["sequence": sequence, "kind": "notification",
            "notification": ["method": "thread/status/changed", "params": ["text": text]]]
        return String(decoding: try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys, .withoutEscapingSlashes]), as: UTF8.self)
    }

    func testContinuousHistoryRetiresPayloadsWithoutResettingSequence() throws {
        var history: [String] = []
        for sequence in UInt64(1)...420 {
            history = try XCTUnwrap(CodexEventHistory.appending(event(sequence), kind: "notification",
                sequence: sequence, after: sequence - 1, to: history))
        }
        XCTAssertEqual(history.count, 128)
        XCTAssertEqual(history.first, try event(293))
        XCTAssertEqual(history.last, try event(420))
        XCTAssertNil(CodexEventHistory.appending(try event(1), kind: "notification", sequence: 1, after: 420, to: history))
        XCTAssertNil(CodexEventHistory.appending(try event(420), kind: "notification", sequence: 420, after: 420, to: history))
        XCTAssertNil(CodexEventHistory.appending(try event(422), kind: "notification", sequence: 422, after: 420, to: history))
        let next = try XCTUnwrap(CodexEventHistory.appending(event(421), kind: "notification", sequence: 421, after: 420, to: history))
        XCTAssertEqual(next.count, 128)
        XCTAssertEqual(next.first, try event(294))
        XCTAssertEqual(next.last, try event(421))
    }

    func testUnicodeBytesEvictOldestPayloadsWithinBothOriginalBudgets() throws {
        let payload = String(repeating: "界😀", count: 400) + "e\u{301}é"
        var history: [String] = []
        for sequence in UInt64(1)...150 {
            history = try XCTUnwrap(CodexEventHistory.appending(event(sequence, text: payload), kind: "notification",
                sequence: sequence, after: sequence - 1, to: history))
        }
        let bytes = history.reduce(0) { $0 + $1.utf8.count }
        XCTAssertLessThan(history.count, 128, "The UTF-8 cap must apply before the count cap")
        XCTAssertLessThanOrEqual(bytes, 262144)
        XCTAssertGreaterThan(bytes, 262144 - 3072, "Retain the largest permitted recent suffix")
        XCTAssertTrue(try XCTUnwrap(history.last).utf8.elementsEqual(event(150, text: payload).utf8))
        for encoded in history {
            let value = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(encoded.utf8)) as? [String: Any])
            let notice = try XCTUnwrap(value["notification"] as? [String: Any])
            let params = try XCTUnwrap(notice["params"] as? [String: Any])
            XCTAssertTrue(try XCTUnwrap(params["text"] as? String).utf8.elementsEqual(payload.utf8))
        }
    }

    func testHistoryRotationDoesNotAdmitOversizedOrUnknownEventsOrSequenceOverflow() throws {
        let history = [try event(1)]
        let tooLarge = try event(2, text: String(repeating: "😀", count: 800))
        XCTAssertGreaterThan(tooLarge.utf8.count, 3072)
        XCTAssertNil(CodexEventHistory.appending(tooLarge, kind: "notification", sequence: 2, after: 1, to: history))
        XCTAssertNil(CodexEventHistory.appending(try event(2), kind: "invented", sequence: 2, after: 1, to: history))
        XCTAssertNil(CodexEventHistory.appending(try event(1), kind: "notification", sequence: 1, after: .max, to: history))
        XCTAssertEqual(history, [try event(1)], "A rejected event cannot replace the retained history")
    }
}
