import GhostermAppModel
import SwiftUI

/// Standard read-only comparison contribution, independent of command/provider.
struct PluginDiffView: View {
    @ObservedObject var model: PluginDiffModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(model.descriptor?.title ?? "差异").font(.headline).accessibilityAddTraits(.isHeader)
            if let issue = model.issue { Text(issue).foregroundStyle(.red).textSelection(.enabled).accessibilityIdentifier("plugin-diff-issue") }
            if model.busy { ProgressView("正在读取差异…").accessibilityIdentifier("plugin-diff-loading") }
            if let d = model.descriptor, let page = model.page {
                Text("\(d.beforeLabel) → \(d.afterLabel)").textSelection(.enabled).accessibilityIdentifier("plugin-diff-labels")
                Text("新增 \(d.added) 行 · 删除 \(d.removed) 行").accessibilityIdentifier("plugin-diff-summary")
                if d.total == 0 { Text("两份文本均为空").accessibilityIdentifier("plugin-diff-empty") }
                else {
                    if d.added == 0 && d.removed == 0 { Text("两份文本相同").accessibilityIdentifier("plugin-diff-identical") }
                    Text("第 \(page.offset + 1)–\(page.offset + page.rows.count) 行，共 \(d.total) 行").accessibilityIdentifier("plugin-diff-range")
                    GeometryReader { geometry in
                        ScrollView([.horizontal, .vertical]) {
                            VStack(alignment: .leading, spacing: 12) {
                                ForEach(Array(page.rows.enumerated()), id: \.offset) { index, row in
                                    HStack(alignment: .firstTextBaseline, spacing: 12) {
                                        Text(row.kind == .insert ? "+ 新增" : row.kind == .delete ? "− 删除" : "= 相同")
                                            .foregroundStyle(row.kind == .insert ? Color.green : row.kind == .delete ? Color.red : Color.secondary)
                                        Text("\(row.beforeLine == 0 ? "—" : String(row.beforeLine)) / \(row.afterLine == 0 ? "—" : String(row.afterLine))")
                                            .monospacedDigit().accessibilityLabel("原 / 新行号")
                                        Text(row.text).monospaced().textSelection(.enabled)
                                            .fixedSize(horizontal: true, vertical: true)
                                            .accessibilityLabel("\(row.beforeLine == 0 ? "原文无此行" : "原第 \(row.beforeLine) 行")，\(row.afterLine == 0 ? "新文无此行" : "新第 \(row.afterLine) 行")")
                                            .accessibilityValue(row.text).accessibilityIdentifier("plugin-diff-text-\(page.offset + index)")
                                        Text(row.ending.label).monospaced().foregroundStyle(.secondary).accessibilityLabel("换行：\(row.ending.label)")
                                            .accessibilityIdentifier("plugin-diff-ending-\(page.offset + index)")
                                    }.accessibilityElement(children: .contain)
                                }
                            }.padding(8).frame(minWidth: geometry.size.width, minHeight: geometry.size.height, alignment: .topLeading)
                        }
                    }.frame(minHeight: 160, idealHeight: 280, maxHeight: 480).accessibilityIdentifier("plugin-diff-rows")
                }
            }
            ViewThatFits(in: .horizontal) { HStack { controls }; VStack(alignment: .leading) { controls } }
        }.accessibilityElement(children: .contain).accessibilityIdentifier("plugin-diff")
            .task { if model.descriptor == nil && model.issue == nil { await model.load() } }
    }
    @ViewBuilder private var controls: some View {
        Button("重新读取") { Task { await model.load() } }
            .disabled(!model.available || model.busy).accessibilityIdentifier("plugin-diff-reload")
        Button("上一页") { Task { await model.showPage(offset: max(0, model.selectedOffset - (model.descriptor?.pageSize ?? 3))) } }
            .disabled(!model.canPrevious).accessibilityIdentifier("plugin-diff-previous")
        Button("下一页") { Task { await model.showPage(offset: model.selectedOffset + (model.descriptor?.pageSize ?? 3)) } }
            .disabled(!model.canNext).accessibilityIdentifier("plugin-diff-next")
    }
}
