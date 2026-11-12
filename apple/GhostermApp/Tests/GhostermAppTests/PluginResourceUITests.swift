import AppKit
import GhostermAppModel
import SwiftUI
import XCTest
@testable import GhostermApp

final class PluginResourceUITests: XCTestCase {
    func testCancelledPreviewDecodeDiscardsLateResult() async {
        let gate = DecodeGate()
        let decode = Task {
            await PluginResourceDecodeTask.run { await gate.decode() }
        }
        await gate.waitUntilStarted()
        decode.cancel()
        await gate.finish(with: "stale document")
        let result = await decode.value
        let workerWasCancelled = await gate.workerObservedCancellation()
        XCTAssertNil(result, "a late decode must not update a replaced preview")
        XCTAssertTrue(workerWasCancelled, "the detached decode must receive cancellation")
    }

    func testResourceSelectionReconcilesAppendRemovalAndRunStop() {
        let first = resource("first")
        let second = resource("second")
        var selection = PluginResourceSelection()

        selection.reconcile([first.resourceID, second.resourceID])
        XCTAssertEqual(selection.resourceID, first.resourceID)
        selection.select(second.resourceID, available: [first.resourceID, second.resourceID])
        XCTAssertEqual(selection.resourceID, second.resourceID)
        selection.reconcile([first.resourceID, second.resourceID, "third"])
        XCTAssertEqual(selection.resourceID, second.resourceID, "arrival should preserve the user's selection")
        selection.reconcile([first.resourceID, "third"])
        XCTAssertEqual(selection.resourceID, first.resourceID, "removing the selected resource should select a live item")
        selection.select("stale", available: [first.resourceID])
        XCTAssertEqual(selection.resourceID, first.resourceID, "a late stale selection must be ignored")
        selection.reconcile([])
        XCTAssertNil(selection.resourceID, "stopping a run must clear its resource selection")
    }

    @MainActor
    func testResourceBrowserRendersASelectedVerifiedResource() throws {
        let item = resource("first")
        let view = PluginResourceBrowser(resources: [item],
                                         selection: .constant(PluginResourceSelection(resourceID: item.resourceID)))
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(x: 0, y: 0, width: 720, height: 420)
        host.layoutSubtreeIfNeeded()
        XCTAssertGreaterThan(host.fittingSize.width, 0)
        XCTAssertGreaterThan(host.fittingSize.height, 0)
        let bitmap = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: bitmap)
        var visible = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 3) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 3) {
                guard let color = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                if color.redComponent < 0.9 || color.greenComponent < 0.9 || color.blueComponent < 0.9 {
                    visible += 1
                }
            }
        }
        XCTAssertGreaterThan(visible, 20, "selected resource and preview should render nonempty content")
    }

    private func resource(_ id: String) -> ExtensionController.ReceivedResource {
        let bytes = Data("preview \(id)".utf8)
        return ExtensionController.ReceivedResource(resourceID: id,
            sha256: String(repeating: "a", count: 64), mime: "text/plain",
            displayName: "\(id).txt", size: UInt64(bytes.count), bytes: bytes)
    }

    private actor DecodeGate {
        private var started = false
        private var startWaiter: CheckedContinuation<Void, Never>?
        private var resultWaiter: CheckedContinuation<String, Never>?
        private var cancelledAtReturn = false

        func decode() async -> String {
            started = true
            startWaiter?.resume()
            startWaiter = nil
            let value = await withCheckedContinuation { resultWaiter = $0 }
            cancelledAtReturn = Task.isCancelled
            return value
        }

        func waitUntilStarted() async {
            guard !started else { return }
            await withCheckedContinuation { startWaiter = $0 }
        }

        func finish(with value: String) {
            resultWaiter?.resume(returning: value)
            resultWaiter = nil
        }

        func workerObservedCancellation() -> Bool { cancelledAtReturn }
    }
}
