import AppKit
import SwiftUI

struct HoverTracking: NSViewRepresentable {
    @Binding var hovering: Bool
    var identifier: String?
    func makeNSView(context: Context) -> HoverTrackingView {
        let view = HoverTrackingView()
        view.identifier = identifier.map { NSUserInterfaceItemIdentifier(rawValue: $0) }
        view.changed = { hovering = $0 }
        return view
    }
    func updateNSView(_ view: HoverTrackingView, context: Context) {
        view.identifier = identifier.map { NSUserInterfaceItemIdentifier(rawValue: $0) }
        view.changed = { hovering = $0 }
    }
}

final class HoverTrackingView: NSView {
    var changed: ((Bool) -> Void)?
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }
    override func mouseEntered(with event: NSEvent) { changed?(true) }
    override func mouseExited(with event: NSEvent) { changed?(false) }
}

