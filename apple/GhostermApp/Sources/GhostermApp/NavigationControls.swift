import AppKit
import GhostermAppModel
import SwiftUI

struct NavigationControls: View {
    var compact = false
    var body: some View {
        if compact {
            VStack(spacing: 0) {
                Color.clear.frame(height: WindowChrome.barHeight)
                NewConnectionButton().frame(width: DS.controlSize, height: DS.controlSize)
                    .frame(maxWidth: .infinity).frame(height: WindowChrome.barHeight)
            }
        } else {
            HStack(spacing: DS.Padding.s) {
                Color.clear.frame(width: WindowChrome.trafficLightsWidth, height: 1)
                NewConnectionButton().frame(width: DS.controlSize, height: DS.controlSize)
            }.frame(height: WindowChrome.barHeight)
        }
    }
}

/// 使用标准 NSButton，保留原生键盘 / AX 操作和可观测的实际命中范围。
struct NewConnectionButton: NSViewRepresentable {
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> NSButton {
        let button = NSButton(image: NSImage(systemSymbolName: "plus", accessibilityDescription: "新建连接")!,
                              target: context.coordinator, action: #selector(Coordinator.open(_:)))
        button.isBordered = false
        button.contentTintColor = .labelColor
        button.imagePosition = .imageOnly
        button.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 14, weight: .medium)
        button.identifier = NSUserInterfaceItemIdentifier("new-connection")
        button.toolTip = "新建连接"
        button.setAccessibilityLabel("新建连接")
        return button
    }
    func updateNSView(_ button: NSButton, context: Context) {}
    final class Coordinator: NSObject {
        @objc func open(_ sender: Any?) {
            NotificationCenter.default.post(name: .ghostermNewConnection, object: nil)
        }
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
