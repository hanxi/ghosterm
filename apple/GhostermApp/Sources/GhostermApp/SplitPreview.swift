import AppKit
import GhostermAppModel

/// 只绘制目标区域，不持有会话或模型，不参与鼠标命中与键盘焦点。
/// 作为最上层 NSView 由 AppKit 排序，避免 backing layer 重排遮住覆盖层。
final class SplitPreview: NSView {
    private(set) var paneID: UUID?
    private(set) var axis: PaneLayout.Axis?
    private(set) var path: CGPath?
    override var isFlipped: Bool { true }
    override var mouseDownCanMoveWindow: Bool { false }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        setAccessibilityElement(false)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func show(paneID: UUID, axis: PaneLayout.Axis, content: CGRect) {
        self.paneID = paneID; self.axis = axis
        update(content: content)
    }
    func update(content: CGRect) {
        guard let axis, content.width > 0, content.height > 0 else { clear(); return }
        let rect = axis == .horizontal
            ? CGRect(x: content.midX, y: content.minY, width: content.width / 2, height: content.height)
            : CGRect(x: content.minX, y: content.midY, width: content.width, height: content.height / 2)
        path = CGPath(roundedRect: rect.insetBy(dx: 1, dy: 1), cornerWidth: 5, cornerHeight: 5, transform: nil)
        needsDisplay = true
    }
    func clear() { paneID = nil; axis = nil; path = nil; needsDisplay = true }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        needsDisplay = true
    }
    override func draw(_ dirtyRect: NSRect) {
        guard let path, let context = NSGraphicsContext.current?.cgContext else { return }
        context.saveGState()
        context.addPath(path)
        context.setFillColor(NSColor.controlAccentColor.withAlphaComponent(0.15).cgColor)
        context.setStrokeColor(NSColor.controlAccentColor.cgColor)
        context.setLineWidth(1.5)
        context.setLineDash(phase: 0, lengths: [5, 4])
        context.drawPath(using: .fillStroke)
        context.restoreGState()
    }
}
