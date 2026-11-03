import AppKit
import GhostermAppModel
import SwiftUI

@MainActor
final class TerminalFontPickerModel: ObservableObject {
    @Published private(set) var fonts: [TerminalFontChoice] = []
    @Published var query = ""
    @Published var selection: String?
    @Published private(set) var loading = false
    @Published private(set) var error: String?
    private let model: AppModel
    private var generation = 0
    var visible: [TerminalFontChoice] { TerminalFonts.filtered(fonts, query: query) }
    var selected: TerminalFontChoice? { fonts.first { $0.id == selection } }
    var canApply: Bool { !loading && selected != nil && !model.terminalFontSelection.external }

    init(model: AppModel) {
        self.model = model
        let current = model.terminalFontSelection
        selection = current.custom || current.external ? nil : current.family ?? ""
    }
    func refresh() {
        generation += 1
        let request = generation
        loading = true
        error = nil
        Task {
            let items = await Task.detached(priority: .userInitiated) { TerminalFonts.available() }.value
            guard request == generation else { return }
            fonts = items; loading = false; error = TerminalFonts.issue
        }
    }
    @discardableResult func apply() -> Bool {
        guard canApply, let selected else { return false }
        do { try model.selectTerminalFont(family: selected.family); error = nil; return true }
        catch { self.error = String(describing: error); return false }
    }
}

struct TerminalFontControl: View {
    @ObservedObject var model: AppModel
    @State private var presented = false
    var body: some View {
        Button { presented = true } label: { PopupLabel(title: model.terminalFontSelection.title) }
            .buttonStyle(.plain)
            .accessibilityLabel("终端字体")
            .accessibilityValue(model.terminalFontSelection.title)
            .accessibilityIdentifier("terminal-font-picker")
            .popover(isPresented: $presented, arrowEdge: .bottom) {
                TerminalFontPicker(model: model) { presented = false }
            }
    }
}

struct TerminalFontPicker: View {
    @StateObject private var picker: TerminalFontPickerModel
    private let model: AppModel
    let dismiss: () -> Void
    init(model: AppModel, dismiss: @escaping () -> Void) {
        self.model = model; self.dismiss = dismiss
        _picker = StateObject(wrappedValue: TerminalFontPickerModel(model: model))
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("终端字体").font(.headline)
                Spacer()
                if picker.loading { ProgressView().controlSize(.small) }
                Button(action: picker.refresh) { Image(systemName: "arrow.clockwise") }
                    .help("重新读取本机字体").accessibilityLabel("刷新字体")
            }
            TextField("搜索字体名称", text: $picker.query)
                .textFieldStyle(.roundedBorder).accessibilityIdentifier("terminal-font-search")
            List(selection: $picker.selection) {
                ForEach(picker.visible) { item in
                    HStack {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(verbatim: item.name)
                            if let face = item.previewName, let font = NSFont(name: face, size: 15) {
                                Text("Aa 0O 1l { } → 终端").font(Font(font)).lineLimit(1)
                            }
                        }
                        Spacer()
                        Text(item.source.rawValue).font(.caption).foregroundStyle(.secondary)
                    }.padding(.vertical, 5).tag(item.id)
                }
            }.listStyle(.inset).accessibilityIdentifier("terminal-font-list")
            if picker.visible.isEmpty, !picker.loading { Text("没有找到等宽字体").foregroundStyle(.secondary) }
            Group {
                if let item = picker.selected, let face = item.previewName, let font = NSFont(name: face, size: 20) {
                    Text("$ git status\n0O 1l  { }  [ ]  →  中英文与符号").font(Font(font))
                        .accessibilityIdentifier("terminal-font-preview")
                } else if picker.selected?.source == .engine {
                    Text("默认 JetBrains Mono 由终端引擎提供，实际效果可在终端查看。")
                } else {
                    Text(model.terminalFontSelection.title + " · 选择字体后预览")
                }
            }.frame(maxWidth: .infinity, minHeight: 58, alignment: .leading)
                .padding(10).background(Color(nsColor: .textBackgroundColor)).cornerRadius(8)
            if model.terminalFontSelection.external {
                Text("字体由附加配置文件管理，请在“配置文件”页修改。").foregroundStyle(.secondary)
            } else if model.terminalFontSelection.custom {
                Text("当前保留自定义字体列表；应用一项会替换全部 font-family 条目。").foregroundStyle(.secondary)
            }
            if let error = picker.error { Text(error).foregroundStyle(.red).accessibilityIdentifier("terminal-font-error") }
            HStack {
                Text("系统字体可通过 macOS 字体册安装。").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("取消", action: dismiss).keyboardShortcut(.cancelAction)
                Button("应用") { if picker.apply() { dismiss() } }.disabled(!picker.canApply)
                    .keyboardShortcut(.defaultAction).accessibilityIdentifier("terminal-font-apply")
            }
        }.padding(16).frame(width: 490, height: 480)
            .background(Color(nsColor: .windowBackgroundColor))
            .onAppear(perform: picker.refresh)
    }
}
