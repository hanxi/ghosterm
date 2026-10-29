import AppKit
import GhostermAppModel
import SwiftUI

/// 使用标准 NSButton，保留原生键盘 / AX 操作和可观测的实际命中范围。
struct NewConnectionButton: NSViewRepresentable {
    var showsTitle = false
    var tint: NSColor = .labelColor
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSButton {
        let button = NSButton(frame: .zero)
        button.cell = ChromeNewConnectionCell(textCell: "")
        button.image = NSImage(systemSymbolName: "plus", accessibilityDescription: "新建连接")!
        button.target = context.coordinator
        button.action = #selector(Coordinator.open(_:))
        button.setButtonType(.momentaryPushIn)
        button.isBordered = false
        button.contentTintColor = tint
        button.title = showsTitle ? "新建标签页" : ""
        button.imagePosition = showsTitle ? .imageLeading : .imageOnly
        button.alignment = showsTitle ? .left : .center
        button.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 14, weight: .medium)
        button.identifier = NSUserInterfaceItemIdentifier("new-connection")
        button.toolTip = "新建连接"
        button.setAccessibilityLabel("新建连接")
        return button
    }
    func updateNSView(_ button: NSButton, context: Context) {
        button.contentTintColor = tint
        button.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 14, weight: .medium)
            .applying(NSImage.SymbolConfiguration(paletteColors: [tint]))
        button.attributedTitle = NSAttributedString(string: showsTitle ? "新建标签页" : "",
                                                    attributes: [.foregroundColor: tint, .font: NSFont.systemFont(ofSize: 13)])
    }
    final class Coordinator: NSObject {
        @objc func open(_ sender: Any?) {
            NotificationCenter.default.post(name: .ghostermNewConnection, object: nil)
        }
    }
}

/// 侧栏中的原生无边框按钮会重着色文字；按终端前景色绘制，保留原生按钮的命中和键盘行为。
private final class ChromeNewConnectionCell: NSButtonCell {
    override func drawTitle(_ title: NSAttributedString, withFrame frame: NSRect, in controlView: NSView) -> NSRect {
        let height = attributedTitle.size().height
        let rect = NSRect(x: frame.minX, y: frame.midY - height / 2, width: frame.width, height: height)
        NSGraphicsContext.saveGraphicsState()
        attributedTitle.draw(in: rect)
        NSGraphicsContext.restoreGraphicsState()
        return rect
    }
}

struct TerminalConfigurationError: View {
    let message: String
    let openSettings: () -> Void
    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 16)).foregroundStyle(.orange)
            Text("终端配置无效：\(message)").font(DS.Font.callout).textSelection(.enabled)
            Spacer(minLength: 0)
            Button("打开设置", action: openSettings)
        }
        .padding(10)
        .background(Color.orange.opacity(0.1))
        .accessibilityIdentifier("terminal-configuration-error")
    }
}
