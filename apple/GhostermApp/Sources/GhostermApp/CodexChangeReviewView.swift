import AppKit
import GhostermAppModel
import SwiftUI

struct CodexChangeSnapshotControl: View {
    @ObservedObject var model: AppModel
    let reference: CodexChangeReference
    @State private var expanded = false

    private var state: CodexChangeReviewState? {
        model.codexChangeReview?.reference == reference ? model.codexChangeReview : nil
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let state, state.loading {
                HStack {
                    ProgressView().controlSize(.small)
                    Text("读取完整补丁：\(state.receivedBytes) / \(reference.totalBytes) 字节")
                        .font(.caption)
                    Button("取消读取") { model.cancelCodexChangeReview() }
                }
            } else {
                Button(state?.document == nil ? "读取完整补丁" : expanded ? "收起完整补丁" : "展开完整补丁") {
                    if state?.document == nil { expanded = true; model.loadCodexChanges(reference) }
                    else { expanded.toggle() }
                }
                .disabled((state?.document == nil && !model.canReadCodexChanges) || model.codexChangeReview?.loading == true)
                .accessibilityIdentifier("plugin-codex-read-change-\(reference.snapshotID)")
                if state?.document == nil && !model.canReadCodexChanges {
                    Text("当前会话无法读取完整补丁。")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            if let issue = state?.issue {
                Text(issue).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
            if expanded, let document = state?.document {
                CodexCompleteChangesView(document: document)
                    .id(reference.snapshotID)
                    .accessibilityIdentifier("plugin-codex-complete-change-\(reference.snapshotID)")
            }
        }
    }
}

/// A single selectable file and a native, read-only text view keep every diff
/// line accessible without creating thousands of SwiftUI Text rows at once.
struct CodexCompleteChangesView: View {
    let document: CodexChangeDocument
    @State private var selection: Int? = 0

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("完整补丁 · \(document.changes.count) 个文件").font(.caption)
            if document.changes.isEmpty {
                Text("此快照没有文件变更。")
            } else {
                HSplitView {
                    fileList.frame(minWidth: 120, idealWidth: 200, maxWidth: 300)
                    selectedFile.frame(minWidth: 180, maxWidth: .infinity, maxHeight: .infinity)
                }
                .frame(height: 360)
            }
        }
    }

    private var fileList: some View {
        List(selection: $selection) {
            ForEach(Array(document.changes.enumerated()), id: \.offset) { index, file in
                Text(file.path).font(.system(size: 11, design: .monospaced))
                    .lineLimit(2).help(file.path).tag(index)
            }
        }
        .accessibilityIdentifier("plugin-codex-change-files")
    }

    @ViewBuilder
    private var selectedFile: some View {
        if let selection, document.changes.indices.contains(selection) {
            let file = document.changes[selection]
            VStack(alignment: .leading, spacing: 4) {
                Text(file.kind.type == "add" ? "新增" : file.kind.type == "delete" ? "删除" : "修改").font(.caption)
                path(file.path)
                if let destination = file.kind.move_path { path("→ " + destination) }
                CodexCompleteDiffText(text: file.diff)
            }
        } else {
            Text("选择要审阅的文件。")
        }
    }

    private func path(_ value: String) -> some View {
        ScrollView(.horizontal) {
            Text(value).font(.system(size: 11, design: .monospaced))
                .textSelection(.enabled).fixedSize(horizontal: true, vertical: false)
        }
        .frame(height: 24)
    }
}

struct CodexCompleteDiffText: NSViewRepresentable {
    let text: String
    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = true
        scroll.borderType = .bezelBorder
        let view = NSTextView(frame: NSRect(x: 0, y: 0, width: 300, height: 300))
        view.isEditable = false
        view.isSelectable = true
        view.isRichText = false
        view.isAutomaticLinkDetectionEnabled = false
        view.isContinuousSpellCheckingEnabled = false
        view.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        view.isHorizontallyResizable = true
        view.isVerticallyResizable = true
        view.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        view.textContainer?.containerSize = view.maxSize
        view.textContainer?.widthTracksTextView = false
        view.identifier = NSUserInterfaceItemIdentifier("plugin-codex-full-diff-text")
        view.setAccessibilityLabel("完整补丁内容")
        scroll.documentView = view
        return scroll
    }
    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let view = scroll.documentView as? NSTextView,
              !view.string.utf8.elementsEqual(text.utf8) else { return }
        view.string = text
        view.textColor = .labelColor
    }
}
