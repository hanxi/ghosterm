import AppKit
import Foundation
import GhostermCore
import SwiftUI
@testable import GhostermAppModel
@testable import GhostermApp
import XCTest

@MainActor
final class CodexChangeReviewTests: XCTestCase {
    override func setUp() { TestWindows.enterAutomationMode() }

    private func fixture(item: String = "item", snapshot: String = "change-1", long: Bool = false,
                         kind: String = "update", version: Int = 1) throws -> (CodexChangeReference, Data) {
        let diff = long ? (0..<600).map { "+行\($0) 😀" }.joined(separator: "\n") + "\n+FINAL-LINE" : "+中文😀\n-old\n"
        let files: [[String: Any]] = (0..<(long ? 10 : 1)).map { index in
            ["path": "完整/file-\(index).swift", "kind": ["type": kind, "move_path": NSNull()], "diff": diff]
        }
        let body: [String: Any] = ["schemaVersion": version, "threadId": "thread", "turnId": "turn",
                                   "itemId": item, "changes": files]
        let data = try JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])
        let text = try XCTUnwrap(String(data: data, encoding: .utf8))
        let marker: [String: Any] = ["snapshotID": snapshot, "snapshotUnits": text.utf16.count, "snapshotBytes": data.count]
        let reference = try XCTUnwrap(CodexChangeReference.decode(marker: marker, thread: "thread", turn: "turn", item: item))
        return (reference, data)
    }

    private func chunks(_ data: Data, reference: CodexChangeReference) -> [CodexChangeChunk] {
        let scalars = Array(String(decoding: data, as: UTF8.self).unicodeScalars)
        var result: [CodexChangeChunk] = [], offset = 0
        for start in stride(from: 0, to: scalars.count, by: 128) {
            let end = min(start + 128, scalars.count)
            let content = scalars[start..<end].reduce(into: "") { $0.unicodeScalars.append($1) }
            let next = offset + content.utf16.count
            result.append(CodexChangeChunk(status: "ok", snapshotID: reference.snapshotID,
                offset: offset, nextOffset: next, totalUnits: reference.totalUnits,
                totalBytes: reference.totalBytes, content: content, done: end == scalars.count))
            offset = next
        }
        return result
    }

    private func altered(_ value: CodexChangeChunk, status: String? = nil, id: String? = nil,
                         offset: Int? = nil, next: Int? = nil, units: Int? = nil, bytes: Int? = nil,
                         done: Bool? = nil, content: String? = nil) -> CodexChangeChunk {
        CodexChangeChunk(status: status ?? value.status, snapshotID: id ?? value.snapshotID,
            offset: offset ?? value.offset, nextOffset: next ?? value.nextOffset,
            totalUnits: units ?? value.totalUnits, totalBytes: bytes ?? value.totalBytes,
            content: content ?? value.content, done: done ?? value.done)
    }

    func testReferenceRejectsBooleanCountsNoncanonicalIDsAndOversizedBudgets() throws {
        let (reference, _) = try fixture()
        var marker: [String: Any] = ["snapshotID": reference.snapshotID, "snapshotUnits": reference.totalUnits,
                                   "snapshotBytes": reference.totalBytes]
        func decode() -> CodexChangeReference? {
            CodexChangeReference.decode(marker: marker, thread: "thread", turn: "turn", item: "item")
        }
        XCTAssertEqual(decode(), reference)
        for id in ["change-0", "change-01", "change-+1", "change-9007199254740992", "untrusted"] {
            marker["snapshotID"] = id; XCTAssertNil(decode(), id)
        }
        marker["snapshotID"] = reference.snapshotID
        marker["snapshotUnits"] = true; XCTAssertNil(decode())
        marker["snapshotUnits"] = 0; XCTAssertNil(decode())
        marker["snapshotUnits"] = reference.totalUnits
        marker["snapshotBytes"] = 524289; XCTAssertNil(decode())
        marker["snapshotBytes"] = reference.totalUnits - 1; XCTAssertNil(decode())
    }

    func testApprovalRequiresACompleteCurrentDocumentForItsExactIdentity() throws {
        let (reference, data) = try fixture()
        var assembler = CodexChangeAssembler(reference: reference)
        for chunk in chunks(data, reference: reference) { try assembler.append(chunk) }
        let document = try assembler.document()
        let identity = CodexApprovalIdentity(threadID: "thread", turnID: "turn", itemID: "item")
        let review = CodexChangeReviewState(reference: reference, receivedBytes: data.count, document: document, issue: nil)
        XCTAssertEqual(identity.reviewedSnapshot(review, isCurrent: { _ in true }), reference.snapshotID)
        XCTAssertNil(identity.reviewedSnapshot(nil, isCurrent: { _ in true }))
        XCTAssertNil(identity.reviewedSnapshot(review, isCurrent: { _ in false }), "Changed patch must not authorize an old review")
        XCTAssertNil(identity.reviewedSnapshot(CodexChangeReviewState(reference: reference, receivedBytes: 0,
            document: document, issue: nil), isCurrent: { _ in true }))
        XCTAssertNil(identity.reviewedSnapshot(CodexChangeReviewState(reference: reference, receivedBytes: data.count,
            document: nil, issue: nil), isCurrent: { _ in true }))
        XCTAssertNil(identity.reviewedSnapshot(CodexChangeReviewState(reference: reference, receivedBytes: data.count,
            document: document, issue: "failed"), isCurrent: { _ in true }))
        XCTAssertNil(CodexApprovalIdentity(threadID: "other", turnID: "turn", itemID: "item")
            .reviewedSnapshot(review, isCurrent: { _ in true }))
        let changed = CodexChangeReference(snapshotID: reference.snapshotID, threadID: "thréad", turnID: "turn", itemID: "item",
            totalUnits: reference.totalUnits, totalBytes: reference.totalBytes)
        XCTAssertFalse(CodexApprovalIdentity(threadID: "thre\u{301}ad", turnID: "turn", itemID: "item").matches(changed))
    }

    func testTimelineKeepsReadableSnapshotWhenEveryPreviewFileWasOmitted() throws {
        let (reference, _) = try fixture()
        let marker: [String: Any] = ["snapshotID": reference.snapshotID, "snapshotUnits": reference.totalUnits,
                                   "snapshotBytes": reference.totalBytes, "omittedChanges": 10]
        let root: [String: Any] = ["threadId": "thread", "turnId": "turn", "itemId": "item",
                                 "changes": [], "_ghosterm": marker]
        let data = try JSONSerialization.data(withJSONObject: root)
        let rows = CodexTimelineReducer.apply([], sequence: 1, method: "item/fileChange/patchUpdated", params: data)
        XCTAssertEqual(rows.single?.changeReference, reference)
        XCTAssertEqual(rows.single?.omittedFileCount, 10)
        XCTAssertTrue(rows.single?.fileChanges.isEmpty == true)
        let nested: [String: Any] = ["threadId": "thread", "turnId": "turn",
            "item": ["id": "item", "type": "fileChange", "changes": [], "_ghosterm": marker]]
        let next = CodexTimelineReducer.apply(rows, sequence: 2, method: "item/completed",
            params: try JSONSerialization.data(withJSONObject: nested))
        XCTAssertEqual(next.single?.changeReference, reference)
    }

    func testAssemblerRetainsEveryFileAndEveryUnicodeDiffLine() throws {
        let (reference, data) = try fixture(long: true)
        var assembler = CodexChangeAssembler(reference: reference)
        XCTAssertThrowsError(try assembler.document())
        for chunk in chunks(data, reference: reference) { try assembler.append(chunk) }
        let document = try assembler.document()
        XCTAssertEqual(document.changes.count, 10)
        XCTAssertEqual(document.changes[9].path, "完整/file-9.swift")
        XCTAssertTrue(document.changes[0].diff.hasSuffix("+FINAL-LINE"))
        XCTAssertEqual(document.changes[0].diff.components(separatedBy: "\n").count, 601)
        XCTAssertEqual(assembler.receivedBytes, data.count)
        XCTAssertEqual(assembler.nextOffset, reference.totalUnits)
    }

    func testAssemblerRejectsMismatchedIdentity() throws {
        let (reference, data) = try fixture()
        let first = try XCTUnwrap(chunks(data, reference: reference).first)
        var assembler = CodexChangeAssembler(reference: reference)
        XCTAssertThrowsError(try assembler.append(altered(first, id: "change-2")), "Snapshot identity must match")
    }

    func testAssemblerRejectsDiscontinuousOffsets() throws {
        let (reference, data) = try fixture()
        let first = try XCTUnwrap(chunks(data, reference: reference).first)
        var assembler = CodexChangeAssembler(reference: reference)
        XCTAssertThrowsError(try assembler.append(altered(first, offset: 1, next: first.nextOffset + 1)), "Chunks must be contiguous")
    }

    func testAssemblerUsesUTF16OffsetsForEmoji() throws {
        let (reference, data) = try fixture(long: true)
        let first = try XCTUnwrap(chunks(data, reference: reference).first)
        XCTAssertNotEqual(first.content.count, first.content.utf16.count)
        var assembler = CodexChangeAssembler(reference: reference)
        XCTAssertNoThrow(try assembler.append(first), "UTF16 offsets must count surrogate pairs")
    }

    func testAssemblerRejectsEOFWithWrongActualByteCount() throws {
        let (original, data) = try fixture()
        let reference = CodexChangeReference(snapshotID: original.snapshotID, threadID: original.threadID,
            turnID: original.turnID, itemID: original.itemID, totalUnits: original.totalUnits, totalBytes: original.totalBytes + 1)
        let parts = chunks(data, reference: reference)
        var assembler = CodexChangeAssembler(reference: reference)
        for chunk in parts.dropLast() { try assembler.append(chunk) }
        XCTAssertThrowsError(try assembler.append(XCTUnwrap(parts.last)), "EOF must match advertised UTF8 bytes")
    }

    func testAssemblerRejectsMalformedLengthsProgressAndTerminalStatus() throws {
        let (reference, data) = try fixture()
        let first = try XCTUnwrap(chunks(data, reference: reference).first)
        let invalid = [altered(first, status: "expired"), altered(first, status: "invalid"),
            altered(first, next: first.nextOffset + 1), altered(first, units: reference.totalUnits + 1),
            altered(first, bytes: reference.totalBytes + 1), altered(first, done: true),
            altered(first, next: 0, content: "")]
        for chunk in invalid {
            var assembler = CodexChangeAssembler(reference: reference)
            XCTAssertThrowsError(try assembler.append(chunk))
            XCTAssertThrowsError(try assembler.document())
        }
        var complete = CodexChangeAssembler(reference: reference)
        for chunk in chunks(data, reference: reference) { try complete.append(chunk) }
        XCTAssertThrowsError(try complete.append(first))
    }

    func testDocumentRejectsWrongBackendIdentityVersionAndUnknownChangeKind() throws {
        for (item, version, kind) in [("other", 1, "update"), ("item", 2, "update"), ("item", 1, "unknown")] {
            let (original, data) = try fixture(item: item, kind: kind, version: version)
            let reference = CodexChangeReference(snapshotID: original.snapshotID, threadID: "thread",
                turnID: "turn", itemID: "item", totalUnits: original.totalUnits, totalBytes: original.totalBytes)
            var assembler = CodexChangeAssembler(reference: reference)
            for chunk in chunks(data, reference: reference) { try assembler.append(chunk) }
            XCTAssertThrowsError(try assembler.document())
        }
    }

    func testCancelledOldReadCannotReplaceANewerReview() async throws {
        let model = isolatedAppModel()
        let (oldReference, oldData) = try fixture()
        let (newReference, newData) = try fixture(item: "new-item", snapshot: "change-2")
        let oldChunks = chunks(oldData, reference: oldReference)
        let newChunks = chunks(newData, reference: newReference)
        let entered = expectation(description: "old read is suspended")
        var continuation: CheckedContinuation<CodexChangeChunk, Never>?
        let old = model.beginCodexChangeRead(reference: oldReference, read: { _ in
            await withCheckedContinuation { continuation = $0; entered.fulfill() }
        }, isCurrent: { true })
        await fulfillment(of: [entered], timeout: 2)
        model.cancelCodexChangeReview()
        let next = model.beginCodexChangeRead(reference: newReference, read: { offset in
            try XCTUnwrap(newChunks.first { $0.offset == offset })
        }, isCurrent: { true })
        continuation?.resume(returning: try XCTUnwrap(oldChunks.first))
        await old.value; await next.value
        XCTAssertEqual(model.codexChangeReview?.reference, newReference)
        XCTAssertEqual(model.codexChangeReview?.document?.itemId, "new-item")
    }

    func testChangedReferenceDiscardsAnUncancelledLateRead() async throws {
        let model = isolatedAppModel()
        let (reference, data) = try fixture()
        let parts = chunks(data, reference: reference)
        let entered = expectation(description: "read awaiting backend")
        var current = true
        var continuation: CheckedContinuation<CodexChangeChunk, Never>?
        let task = model.beginCodexChangeRead(reference: reference, read: { offset in
            if offset == 0 {
                return await withCheckedContinuation { continuation = $0; entered.fulfill() }
            }
            return try XCTUnwrap(parts.first { $0.offset == offset })
        }, isCurrent: { current })
        await fulfillment(of: [entered], timeout: 2)
        current = false
        continuation?.resume(returning: try XCTUnwrap(parts.first))
        await task.value
        XCTAssertNil(model.codexChangeReview, "Changed references must discard late data")
    }

    func testClosedEventDropsPendingCallback() async throws {
        let model = isolatedAppModel()
        let (reference, data) = try fixture()
        let entered = expectation(description: "read suspended before close")
        var continuation: CheckedContinuation<CodexChangeChunk, Never>?
        let task = model.beginCodexChangeRead(reference: reference, read: { _ in
            await withCheckedContinuation { continuation = $0; entered.fulfill() }
        }, isCurrent: { true })
        await fulfillment(of: [entered], timeout: 2)
        model.applyCodexApprovalEvent(kind: "closed")
        continuation?.resume(returning: try XCTUnwrap(chunks(data, reference: reference).first))
        await task.value
        XCTAssertNil(model.codexChangeReview)
    }

    func testClosedEventCancelsReviewAndExpiredReplyNeverBecomesADocument() async throws {
        let model = isolatedAppModel()
        let (reference, data) = try fixture()
        let first = try XCTUnwrap(chunks(data, reference: reference).first)
        let task = model.beginCodexChangeRead(reference: reference, read: { _ in
            self.altered(first, status: "expired")
        }, isCurrent: { true })
        await task.value
        XCTAssertNotNil(model.codexChangeReview?.issue)
        XCTAssertNil(model.codexChangeReview?.document)
        model.applyCodexApprovalEvent(kind: "closed")
        XCTAssertNil(model.codexChangeReview)
    }

    func testNativeReadOnlyViewContainsLinesBeyondTheOldPreviewLimit() async throws {
        let (reference, data) = try fixture(long: true)
        var assembler = CodexChangeAssembler(reference: reference)
        for chunk in chunks(data, reference: reference) { try assembler.append(chunk) }
        let document = try assembler.document()
        let host = NSHostingView(rootView: CodexCompleteChangesView(document: document))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 460),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        func textView(_ view: NSView) -> NSTextView? {
            if let text = view as? NSTextView, text.identifier?.rawValue == "plugin-codex-full-diff-text" { return text }
            return view.subviews.lazy.compactMap { textView($0) }.first
        }
        var found: NSTextView?
        for _ in 0..<80 {
            host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
            found = textView(host)
            if found != nil { break }
            try await Task.sleep(for: .milliseconds(10))
        }
        let text = try XCTUnwrap(found)
        XCTAssertEqual(text.string, document.changes[0].diff)
        XCTAssertTrue(text.string.hasSuffix("+FINAL-LINE"), "Full review must include lines beyond 256")
        XCTAssertFalse(text.isEditable, "Full diff text must remain read only")
        XCTAssertTrue(text.isSelectable)
    }

    func testNativeTextUpdatePreservesExactUnicodeBytes() async throws {
        let old = "e\u{301}\n", replacement = "\u{e9}\n"
        XCTAssertEqual(old, replacement, "Swift String equality treats these spellings as equivalent")
        XCTAssertNotEqual(Array(old.utf8), Array(replacement.utf8))
        let host = NSHostingView(rootView: CodexCompleteDiffText(text: old))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 220),
            styleMask: [.titled], backing: .buffered, defer: false)
        window.contentView = host
        TestWindows.presentOffscreen(window)
        defer { window.orderOut(nil) }
        func contents(_ view: NSView) -> NSTextView? {
            if let text = view as? NSTextView, text.identifier?.rawValue == "plugin-codex-full-diff-text" { return text }
            return view.subviews.lazy.compactMap { contents($0) }.first
        }
        host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
        let view = try XCTUnwrap(contents(host))
        XCTAssertEqual(Array(view.string.utf8), Array(old.utf8))
        host.rootView = CodexCompleteDiffText(text: replacement)
        for _ in 0..<80 {
            host.layoutSubtreeIfNeeded(); window.displayIfNeeded()
            if Array(view.string.utf8) == Array(replacement.utf8) { break }
            try await Task.sleep(for: .milliseconds(10))
        }
        XCTAssertEqual(Array(view.string.utf8), Array(replacement.utf8), "Full diff must update byte-distinct Unicode text")
    }
}

private extension Array {
    var single: Element? { count == 1 ? first : nil }
}
