import AppKit
import SwiftUI

/// Shared Native controls. Backend state and decision schemas stay in each
/// conversation adapter; these views do not invent a common tool protocol.
struct ConversationComposer: View {
    let placeholder: String
    @Binding var draft: String
    let busy: Bool
    let canSend: Bool
    let prefix: String
    let send: () -> Void
    var body: some View {
        HStack {
            TextField(placeholder, text: $draft, axis: .vertical)
                .lineLimit(1...4)
                .accessibilityIdentifier(prefix + "-message")
            Button(busy ? "正在发送…" : "发送", action: send)
                .fixedSize(horizontal: true, vertical: false)
                .disabled(busy || !canSend || draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                .accessibilityIdentifier(prefix + "-send")
        }
    }
}

struct ConversationDecisionButtons: View {
    let prefix: String
    let busy: Bool
    let canAllow: Bool
    var allowLabel = "批准一次"
    let deny: () -> Void
    let allow: () -> Void
    var body: some View {
        HStack {
            Button("拒绝", action: deny).fixedSize(horizontal: true, vertical: false).accessibilityIdentifier(prefix + "-deny")
            Button(allowLabel, action: allow).buttonStyle(.borderedProminent)
                .fixedSize(horizontal: true, vertical: false)
                .disabled(!canAllow).accessibilityIdentifier(prefix + "-allow")
        }.disabled(busy)
    }
}

struct ConversationReadOnlyText: NSViewRepresentable {
    let text: String
    let identifier: String
    let label: String
    func makeNSView(context: Context) -> NSScrollView {
        Self.makeTextView(identifier: identifier, label: label)
    }
    static func makeTextView(identifier: String, label: String) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true; scroll.hasHorizontalScroller = true; scroll.borderType = .bezelBorder
        let view = NSTextView(frame: NSRect(x: 0, y: 0, width: 300, height: 300))
        view.isEditable = false; view.isSelectable = true; view.isRichText = false
        view.isAutomaticLinkDetectionEnabled = false; view.isContinuousSpellCheckingEnabled = false
        view.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        view.isHorizontallyResizable = true; view.isVerticallyResizable = true
        view.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        view.textContainer?.containerSize = view.maxSize; view.textContainer?.widthTracksTextView = false
        view.identifier = NSUserInterfaceItemIdentifier(identifier); view.setAccessibilityLabel(label)
        scroll.documentView = view
        return scroll
    }
    func updateNSView(_ scroll: NSScrollView, context: Context) {
        Self.updateText(text, in: scroll)
    }
    static func updateText(_ text: String, in scroll: NSScrollView) {
        guard let view = scroll.documentView as? NSTextView,
              !view.string.utf8.elementsEqual(text.utf8) else { return }
        view.string = text; view.textColor = .labelColor
    }
}
