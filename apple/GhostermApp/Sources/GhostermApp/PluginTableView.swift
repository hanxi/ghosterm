import GhostermAppModel
import SwiftUI

/// Standard Native contribution, shared by every command declaring table.
struct PluginTableView: View {
    @ObservedObject var model: PluginTableModel
    @State private var query = ""
    var body: some View {
        let loading = "正在读取表格…"
        let searching = model.searchQuery != nil
        let rows = searching ? model.searchPage?.rows : model.page?.rows
        let range = searching ? model.searchPage.map { page in
            if page.rows.isEmpty {
                return page.cursor < page.latest ? "本页没有匹配项，可继续搜索" : "搜索结束，本页没有匹配项"
            }
            return "本页有 \(page.rows.count) 个匹配项"
        } : model.page.flatMap { page in
            model.descriptor.flatMap { descriptor in
                descriptor.total > 0
                    ? "第 \(page.offset + 1)–\(page.offset + page.rows.count) 行，共 \(descriptor.total) 行"
                    : nil
            }
        }
        VStack(alignment: .leading, spacing: 12) {
            Text(model.descriptor?.title ?? "表格").font(.headline).accessibilityAddTraits(.isHeader)
            if model.supportsSearch {
                ViewThatFits(in: .horizontal) {
                    HStack { searchControls }
                    VStack(alignment: .leading) { searchControls }
                }
            }
            if let issue = model.issue { Text(issue).foregroundStyle(.red).textSelection(.enabled).accessibilityIdentifier("plugin-table-issue") }
            if model.busy { ProgressView(loading).accessibilityIdentifier("plugin-table-loading") }
            if let descriptor = model.descriptor, let rows {
                if let range { Text(verbatim: range).accessibilityIdentifier("plugin-table-range") }
                if model.searchTruncated { Text("部分历史记录已超出保留范围") }
                if !searching && descriptor.total == 0 {
                    Text("空表").accessibilityIdentifier("plugin-table-empty")
                } else if !rows.isEmpty {
                    ScrollView([.horizontal, .vertical]) {
                        Grid(alignment: .topLeading, horizontalSpacing: 16, verticalSpacing: 12) {
                            GridRow {
                                ForEach(descriptor.columns) { column in
                                    Text(column.title).bold().accessibilityAddTraits(.isHeader)
                                        .accessibilityIdentifier("plugin-table-column-" + column.id)
                                }
                            }
                            ForEach(Array(rows.enumerated()), id: \.element.id) { index, row in
                                GridRow {
                                    ForEach(descriptor.columns.indices, id: \.self) { column in
                                        let value = row.cells[column]
                                        Text(value).textSelection(.enabled)
                                            .frame(minWidth: 90, maxWidth: 240, alignment: .leading)
                                            .fixedSize(horizontal: false, vertical: true)
                                            .accessibilityLabel("\(descriptor.columns[column].title)，第 \((searching ? 0 : model.selectedOffset) + index + 1) 行")
                                            .accessibilityValue(value.isEmpty ? "空白" : value)
                                            .accessibilityIdentifier("plugin-table-cell-\(row.id)-\(descriptor.columns[column].id)")
                                    }
                                }
                            }
                        }.padding(8)
                    }.frame(minHeight: 160, idealHeight: 320, maxHeight: 480)
                }
            }
            if !searching {
                ViewThatFits(in: .horizontal) {
                    HStack { controls }
                    VStack(alignment: .leading) { controls }
                }
            }
        }.accessibilityElement(children: .contain).accessibilityIdentifier("plugin-table")
            .announcePluginStatus(model.issue ?? (model.busy ? loading : range))
            .task { if model.descriptor == nil && model.issue == nil { await model.load() } }
    }
    private var canSearch: Bool {
        model.available && !model.busy && model.descriptor != nil && !query.isEmpty && query.unicodeScalars.count <= 64
    }
    @ViewBuilder private var searchControls: some View {
        TextField("搜索表格", text: $query)
            .onSubmit { if canSearch { Task { await model.search(query) } } }
        Button("搜索") { Task { await model.search(query) } }.disabled(!canSearch)
            .accessibilityIdentifier("plugin-table-search")
        if let active = model.searchQuery {
            Text("搜索：\(active)")
            if let page = model.searchPage, page.cursor < page.latest {
                Button("继续搜索") { Task { await model.continueSearch() } }.disabled(!model.available || model.busy)
                    .accessibilityIdentifier("plugin-table-search-more")
            }
            Button("返回表格") { Task { await model.load() } }.disabled(!model.available || model.busy)
                .accessibilityIdentifier("plugin-table-search-clear")
        }
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
