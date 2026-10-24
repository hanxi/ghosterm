// FilesDragDropTests — 拖放路由 + 预览临时路径 + 完成回调（无网可跑）。
import GhostermCore
import Foundation
@testable import GhostermAppModel
import XCTest

final class FilesDragDropTests: XCTestCase {
    func testDragItemRoundTripEscaping() {
        let item = DragItem(side: .remote, path: "/a/b\nc\\d.txt", name: "x\ny",
                            isDirectory: true)
        let back = DragItem.decode(item.encoded())
        XCTAssertEqual(back, item, "编码→解码应无损（含换行/反斜杠）")
    }

    func testDropRouterInternal() {
        let remote = DragItem(side: .remote, path: "/r/f", name: "f", isDirectory: false)
        let local = DragItem(side: .local, path: "/l/f", name: "f", isDirectory: false)

        XCTAssertEqual(DropRouter.route(item: remote, onto: .local),
                       .download(remote: "/r/f", name: "f", isDirectory: false))
        XCTAssertEqual(DropRouter.route(item: local, onto: .remote),
                       .upload(local: "/l/f", name: "f", isDirectory: false))
        XCTAssertEqual(DropRouter.route(item: remote, onto: .remote), .none,
                       "同栏互拖不做（v1）")
        XCTAssertEqual(DropRouter.route(item: local, onto: .local), .none)
    }

    func testDropRouterFinder() {
        XCTAssertEqual(DropRouter.route(finderPath: "/x/y.txt", name: "y.txt",
                                        isDirectory: false, onto: .remote),
                       .upload(local: "/x/y.txt", name: "y.txt", isDirectory: false))
        XCTAssertEqual(DropRouter.route(finderPath: "/x/y.txt", name: "y.txt",
                                        isDirectory: false, onto: .local), .none,
                       "Finder 拖到本地栏 = 不做（本地栏本来就看得见）")
    }

    @MainActor
    func testQuickLookTempPath() {
        let p = QuickLookPaths.tempPreviewPath(for: "foo.txt", token: "T")
        XCTAssertTrue(p.hasSuffix("/ghosterm-preview/T/foo.txt"), "临时副本路径形状（得到 \(p)）")
    }

    @MainActor
    func testOnFinishFires() {
        let b = FakeTransferBackend2()
        let m = TransferManager(pump: CoreEventPump())
        var fired: TransferManager.Status?
        m.enqueueDownload(b, remote: "/r/a", local: NSTemporaryDirectory() + "gt-finish", name: "a",
                          isDirectory: false, onFinish: { fired = $0 })
        let core = m.jobs[0].coreJob
        m.handle(CoreEvent(kind: .jobDone, sessionID: 0, jobID: core, code: 0, message: ""))
        XCTAssertEqual(fired, .done, "完成回调应在 jobDone 时触发")
    }
}

/// 极简假后端（只要 download 返回一个 job id）。
final class FakeTransferBackend2: TransferBackend {
    var displayName = "fake2"
    var initialPath = "/"
    func list(_ path: String) throws -> [FileEntry] { [] }
    func mkdir(_ path: String, mode: UInt32) throws {}
    func remove(_ path: String, recursive: Bool) throws {}
    func rename(_ from: String, _ to: String) throws {}
    func chmod(_ path: String, mode: UInt32) throws {}
    func realpath(_ path: String) throws -> String { path }
    func walk(root: String, maxDepth: Int, visit: @escaping (FileEntry) -> Bool) throws {}
    func exists(_ path: String) -> Bool { false }
    func download(remote: String, local: String) throws -> UInt64 { 1 }
    func upload(local: String, remote: String) throws -> UInt64 { 2 }
    func pause(jobID: UInt64, _ p: Bool) throws {}
    func cancel(jobID: UInt64) throws {}
    func search(root: String, pattern: String, maxDepth: Int,
                onHit: @escaping (FileEntry) -> Bool,
                onScanned: @escaping (Int) -> Void) throws -> Bool { true }
}
