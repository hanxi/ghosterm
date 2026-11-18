import GhostermAppModel
import SwiftUI

/// Standard Native contribution, shared by every command declaring table.
struct PluginTableView: View {
    @ObservedObject var model: PluginTableModel
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(model.descriptor?.title ?? "表格").font(.headline).accessibilityAddTraits(.isHeader)
            if let issue = model.issue { Text(issue).foregroundStyle(.red).textSelection(.enabled).accessibilityIdentifier("plugin-table-issue") }
            if model.busy { ProgressView("正在读取表格…").accessibilityIdentifier("plugin-table-loading") }
            if let descriptor = model.descriptor, let page = model.page {
                if descriptor.total == 0 {
                    Text("空表").accessibilityIdentifier("plugin-table-empty")
                } else {
                    Text("第 \(page.offset + 1)–\(page.offset + page.rows.count) 行，共 \(descriptor.total) 行")
                        .accessibilityIdentifier("plugin-table-range")
                    ScrollView([.horizontal, .vertical]) {
                        Grid(alignment: .topLeading, horizontalSpacing: 16, verticalSpacing: 12) {
                            GridRow {
                                ForEach(descriptor.columns) { column in
                                    Text(column.title).bold().accessibilityAddTraits(.isHeader)
                                        .accessibilityIdentifier("plugin-table-column-" + column.id)
                                }
                            }
                            ForEach(Array(page.rows.enumerated()), id: \.element.id) { index, row in
                                GridRow {
                                    ForEach(descriptor.columns.indices, id: \.self) { column in
                                        Text(row.cells[column]).textSelection(.enabled)
                                            .frame(minWidth: 90, maxWidth: 240, alignment: .leading)
                                            .fixedSize(horizontal: false, vertical: true)
                                            .accessibilityLabel("\(descriptor.columns[column].title)，第 \(page.offset + index + 1) 行")
                                            .accessibilityValue(row.cells[column])
                                            .accessibilityIdentifier("plugin-table-cell-\(row.id)-\(descriptor.columns[column].id)")
                                    }
                                }
                            }
                        }.padding(8)
                    }.frame(minHeight: 160, idealHeight: 320, maxHeight: 480)
                }
            }
            ViewThatFits(in: .horizontal) {
                HStack { controls }
                VStack(alignment: .leading) { controls }
            }
        }.accessibilityElement(children: .contain).accessibilityIdentifier("plugin-table")
            .announcePluginStatus(model.issue)
            .task { if model.descriptor == nil && model.issue == nil { await model.load() } }
    }
    @ViewBuilder private var controls: some View {
        Button(model.descriptor == nil ? "读取表格" : "重新读取") { Task { await model.load() } }
            .disabled(!model.available || model.busy).accessibilityIdentifier("plugin-table-reload")
        Button("上一页") { Task { await model.showPage(offset: max(0, model.selectedOffset - (model.descriptor?.pageSize ?? 16))) } }
            .disabled(!model.canPrevious).accessibilityIdentifier("plugin-table-previous")
        Button("下一页") { Task { await model.showPage(offset: model.selectedOffset + (model.descriptor?.pageSize ?? 16)) } }
            .disabled(!model.canNext).accessibilityIdentifier("plugin-table-next")
        Button("末页") { Task { await model.showPage(offset: model.lastOffset) } }
            .disabled(!model.canNext).accessibilityIdentifier("plugin-table-last")
    }
}
