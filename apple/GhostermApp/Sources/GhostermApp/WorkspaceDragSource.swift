import AppKit
import SwiftUI

extension Notification.Name {
    static let workspacePaneDragBegan = Notification.Name("Ghosterm.workspacePaneDragBegan")
    /// 窗格拖动会话结束（正常放下或被 Esc/系统取消）。
    static let workspacePaneDragEnded = Notification.Name("Ghosterm.workspacePaneDragEnded")
}

/// 窗格拖动明确声明 move；不借用文件拖动的 copy 操作掩码。
struct WorkspaceDragSource: NSViewRepresentable {
    let provider: () -> NSItemProvider
    func makeNSView(context: Context) -> WorkspaceDragSourceView { WorkspaceDragSourceView() }
    func updateNSView(_ view: WorkspaceDragSourceView, context: Context) { view.provider = provider }
}

@MainActor final class WorkspaceDragSourceView: NSView, NSDraggingSource {
    var provider: (() -> NSItemProvider)?
    private var down: NSEvent?
    private var starting = false
    /// 窗格拖动把手**必须拒绝**「拖动=移窗」。侧栏展开时右列没有顶栏
    /// （侧边标签布局），最上排窗格的标题栏正好顶进系统标题栏带
    /// （顶部约 28pt，fullSizeContentView 下这条带仍然存在）；带内 AppKit
    /// 按命中视图的 `mouseDownCanMoveWindow`（非不透明视图默认 YES）把
    /// 拖动劫持成「移动窗口」，被劫持的视图收不到 mouseDragged——4pt 起拖
    /// 永远不触发，用户看到的就是「拖窗格把整个窗口拖走了」。
    override var mouseDownCanMoveWindow: Bool { false }
    override func mouseDown(with event: NSEvent) { down = event; starting = false }
    override func mouseUp(with event: NSEvent) { down = nil; starting = false }
    override func mouseDragged(with event: NSEvent) {
        guard !starting, let down, let provider,
              hypot(event.locationInWindow.x - down.locationInWindow.x,
                    event.locationInWindow.y - down.locationInWindow.y) >= 4 else { return }
        starting = true
        provider().loadDataRepresentation(forTypeIdentifier: workspacePaneType) { [weak self] data, _ in
            DispatchQueue.main.async {
                guard let self, let data, NSEvent.pressedMouseButtons & 1 != 0 else { return }
                let item = NSPasteboardItem()
                item.setData(data, forType: .init(workspacePaneType))
                let dragging = NSDraggingItem(pasteboardWriter: item)
                let image = NSImage(size: NSSize(width: max(80, self.bounds.width), height: 26))
                image.lockFocus()
                NSColor.windowBackgroundColor.withAlphaComponent(0.9).setFill()
                NSBezierPath(roundedRect: NSRect(origin: .zero, size: image.size), xRadius: 6, yRadius: 6).fill()
                ("移动窗格" as NSString).draw(at: NSPoint(x: 8, y: 6), withAttributes: [.foregroundColor: NSColor.labelColor])
                image.unlockFocus()
                dragging.setDraggingFrame(NSRect(origin: .zero, size: image.size), contents: image)
                self.beginDraggingSession(with: [dragging], event: event, source: self)
            }
        }
    }
    static func operationMask(for context: NSDraggingContext) -> NSDragOperation {
        context == .withinApplication ? .move : []
    }
    func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
        Self.operationMask(for: context)
    }
    func draggingSession(_ session: NSDraggingSession, willBeginAt screenPoint: NSPoint) {
        beginDrag()
    }
    /// 只在系统真正开始会话后通知；异步读取 provider 期间松手不算开始拖动。
    func beginDrag() {
        NotificationCenter.default.post(name: .workspacePaneDragBegan, object: nil)
    }
    func draggingSession(_ session: NSDraggingSession, endedAt screenPoint: NSPoint, operation: NSDragOperation) {
        finishDrag()
    }
    /// 无论松手还是 Esc 取消，会话都会走到这里；统一通知宿主清预览。
    func finishDrag() {
        down = nil; starting = false
        NotificationCenter.default.post(name: .workspacePaneDragEnded, object: nil)
    }
}
