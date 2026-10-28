import Combine
import GhostermAppModel
import SwiftUI

struct TabSearchView: View {
    @ObservedObject var tabs: TabManager
    let select: (UUID) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var query = ""
    @State private var selected: UUID?
    @State private var titleRevision = 0
    @FocusState private var searchFocused: Bool

    private var matches: [TerminalTab] {
        tabs.tabs.filter { ConnectionSearch.matches(query, text: $0.title + " " + $0.subtitle) }
    }
    private var canSelect: Bool { matches.contains { $0.id == selected } }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("搜索标签页").font(DS.Font.titleLarge)
            TextField("搜索标题、主机或目录", text: $query)
                .textFieldStyle(.roundedBorder).focused($searchFocused).onSubmit(activate)
            List(selection: $selected) {
                ForEach(matches) { tab in
                    HStack {
                        Image(systemName: tab.kind.symbolName)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(tab.title).lineLimit(1)
                            Text(tab.subtitle).font(DS.Font.caption).foregroundStyle(.secondary).lineLimit(1)
                        }
                        Spacer()
                        if tab.id == tabs.activeTabID { Image(systemName: "checkmark") }
                    }.tag(tab.id)
                }
            }.frame(height: 260)
            if matches.isEmpty { Text("没有匹配的标签页").foregroundStyle(.secondary) }
            HStack {
                Spacer()
                Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("切换", action: activate).keyboardShortcut(.defaultAction).disabled(!canSelect)
            }
        }
        .padding(20).frame(width: 500)
        .onAppear { selected = tabs.activeTabID; searchFocused = true; reconcileSelection() }
        .onChange(of: query) { _ in reconcileSelection() }
        .onChange(of: tabs.tabs.map(\.id)) { _ in reconcileSelection() }
        .onReceive(Publishers.MergeMany(tabs.tabs.map(\.objectWillChange))) { _ in titleRevision &+= 1 }
        .onChange(of: titleRevision) { _ in reconcileSelection() }
    }

    private func reconcileSelection() {
        if !canSelect { selected = matches.first?.id }
    }
    private func activate() {
        guard canSelect, let selected else { return }
        select(selected)
    }
}
