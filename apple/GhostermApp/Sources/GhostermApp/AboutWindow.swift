// AboutWindow — 菜单栏「关于 Ghosterm」开的那张小窗口。
//
// 为什么不用系统那张标准面板（`NSApplication.orderFrontStandardAboutPanel`）：
// 它只认 Info.plist 里的几个字段，列不出随包组件（libghostty / libssh2 …）
// 各自的许可，而随包分发第三方组件就得把这些声明亮给用户看。
//
// 为什么也不塞进设置里当一页：「关于」不是可调的设置项，把它放进设置会让
// 「设置 = 可改的东西」这条读者预期破掉；macOS 自家应用（Xcode / Safari /
// 终端）一律是菜单项直接开一张独立小窗口。
//
// 窗口形态照 `TransferWindow`：单例、`isReleasedWhenClosed = false`，关掉后
// 再点菜单复用同一张。尺寸贴内容（`NSHostingController` 自己报），不可拖拽
// 改大——里面是定死的几行信息，拉大只会留白。
import AppKit
import GhostermAppModel
import SwiftUI

@MainActor
enum AboutWindow {
    /// 当前那张关于窗口（只允许一张）。非 private：`AboutWindowTests` 要拿它
    /// 验「菜单项开的是我们自己的窗口」。
    private(set) static var controller: NSWindowController?

    static func open() {
        if let window = controller?.window {
            WindowPresentation.present(window)
            return
        }
        let hosting = NSHostingController(rootView: AboutContentView())
        let win = NSWindow(contentViewController: hosting)
        win.title = "关于 Ghosterm"
        win.styleMask = [.titled, .closable]
        win.isReleasedWhenClosed = false
        win.center()
        WindowPresentation.prepare(win)
        let c = NSWindowController(window: win)
        controller = c
        c.showWindow(nil)
    }

    static func close() { controller?.close() }
}

/// 关于窗口的内容：`AboutPane` 加上窗口自己的内边距与定宽。
struct AboutContentView: View {
    /// 跟设置窗口同宽：两张窗口并排出现时宽度一致，读起来不跳。
    static let width = DS.settingsWidth

    var body: some View {
        AboutPane()
            .padding(.horizontal, DS.Padding.xl)
            .padding(.vertical, DS.Padding.l)
            .frame(width: Self.width)
    }
}

/// 关于：名字、版本、许可与项目地址（照抄 iGhostVT 的关于页形态：
/// 图标 + 名称 + 版本，下面接许可）。
struct AboutPane: View {
    var body: some View {
        VStack(spacing: DS.Padding.l) {
            VStack(spacing: DS.Padding.s) {
                Image(nsImage: NSApp.applicationIconImage)
                    .resizable()
                    .scaledToFit()
                    .frame(width: 72, height: 72)
                    .clipShape(RoundedRectangle(cornerRadius: DS.Radius.l,
                                                style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: DS.Radius.l,
                                              style: .continuous)
                        .strokeBorder(Color.primary.opacity(0.1)))
                    .accessibilityHidden(true)
                Text("Ghosterm").font(DS.Font.title)
                HStack(spacing: DS.Padding.xs) {
                    Text("版本")
                    Text(verbatim: AboutInfo.versionDescription)
                }
                .font(DS.Font.detail)
                .foregroundStyle(.secondary)
                .accessibilityElement(children: .combine)
            }
            .frame(maxWidth: .infinity)

            MacTableFrame {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(AboutInfo.entries.enumerated()), id: \.offset) { index, entry in
                        HStack(alignment: .top, spacing: DS.Padding.m) {
                            Text(verbatim: entry.name)
                                .frame(width: 130, alignment: .leading)
                            Text(verbatim: entry.detail)
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                            Spacer(minLength: 0)
                        }
                        .font(DS.Font.detail)
                        .padding(.horizontal, DS.Padding.m)
                        .padding(.vertical, DS.Padding.s)
                        .background(MacTableStripe.color(index))
                    }
                }
            }

            SettingsNote("本程序按 MIT License 分发；随包组件各自的许可见下表。"
                         + "源码与许可全文：https://github.com/hanxi/ghosterm")
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
        }
    }
}
