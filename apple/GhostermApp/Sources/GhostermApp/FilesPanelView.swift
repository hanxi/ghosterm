// FilesPanelView — 终端标签页右侧的内嵌文件面板（单栏）。
//
// 后端无关：本地 shell 标签页后端=LocalFileBackend，SSH 标签页=SFTPFileBackend。
// 面板只认 FilesPanelModel，故两侧零分叉。
import GhostermAppModel
import SwiftUI
import UniformTypeIdentifiers

struct FilesPanelView: View {
    @ObservedObject var panel: FilesPanelModel
    @ObservedObject var transfers: TransferManager = .shared
    /// 下载/上传的本地落点（面板不弹保存框时用它；测试可注入）。
    var downloadTo: (FileEntry) -> Void = { _ in }
    var uploadFrom: () -> Void = {}
    /// 本栏在双栏里的身份（拖放路由用）。nil = 单栏面板（不接受栏间拖放）。
    var side: DragItem.Side? = nil
    /// 拖放落点决策交给调用方（DropRouter 已算好动作）。
    var onDropAction: ((DropAction) -> Void)? = nil
    /// 空格 Quick Look（远端条目先下临时副本）。nil = 不定（单栏面板）。
    var quickLook: ((FileEntry) -> Void)? = nil
    /// 关闭面板（内嵌面板传；双栏 SFTP 标签页的每一栏不传——它没有「关掉一栏」的语义）。
    var onClose: (() -> Void)? = nil
    var workspaceTab: TerminalTab? = nil
    var workspaceFile: WorkspaceFilePane? = nil
    /// 标题条以下、内容区以上的实测高度上报（工作区据此算窗格最小高度）。
    var onChromeHeightChange: ((CGFloat) -> Void)? = nil

    @State private var newFolderName = ""
    @State private var showingNewFolder = false
    @State private var renameTarget: FileEntry?
    @State private var renameName = ""
    @State private var permTarget: FileEntry?
    @State private var permText = ""
    @State private var permRecursive = false
    @State private var infoTarget: FileEntry?
    @State private var deleteTargets: [FileEntry] = []
    @State private var searchText = ""
    @State private var showingSearch = false

    var body: some View {
        VStack(spacing: 0) {
            chrome
                .background(GeometryReader { proxy in
                    Color.clear.preference(key: FilesPanelChromeHeightKey.self,
                                           value: proxy.size.height)
                })
            content
        }
        .frame(minWidth: 260)
        .onPreferenceChange(FilesPanelChromeHeightKey.self) { onChromeHeightChange?($0) }
        .alert("删除 \(deleteTargets.count) 项？", isPresented: deleteConfirmBinding) {
            Button("删除", role: .destructive) {
                panel.delete(deleteTargets)
                deleteTargets = []
            }
            Button("取消", role: .cancel) { deleteTargets = [] }
        } message: {
            Text(deleteMessage)
        }
        .sheet(isPresented: $showingNewFolder) { newFolderSheet }
        .sheet(item: $renameTarget) { renamingSheet($0) }
        .sheet(item: $permTarget) { permissionSheet($0) }
        .sheet(item: $infoTarget) { infoSheet($0) }
    }

    // MARK: - 头部：导航 + 工具

    /// 标题条以下、内容区以上的全部固定部分。必须整体实测：工具条会按宽度
    /// 折行、搜索栏可显隐，任何写死的高度在窄面板或搜索打开时都会把内容区
    /// 挤掉（工作区最小高度用这个值，见 `WorkspaceHostView.minimum`）。
    private var chrome: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if showingSearch || panel.searchMode != .none {
                searchBar
                Divider()
            }
        }
    }

    private var header: some View {
        VStack(spacing: DS.Padding.xs) {
            // 工具条用 FlowLayout **折行**：面板可以窄到 260pt，一行硬排会
            // 把按钮画到框外（用户实测「顶部按钮超框了」）。关闭按钮钉在最右。
            HStack(spacing: DS.Padding.xs) {
                FlowLayout(spacing: DS.Padding.xs) {
                    GlassIconButton(systemName: "chevron.left", help: "后退") { panel.goBack() }
                    GlassIconButton(systemName: "chevron.right", help: "前进") { panel.goForward() }
                    GlassIconButton(systemName: "arrow.up", help: "上级目录") { panel.up() }
                    GlassIconButton(systemName: "arrow.clockwise", help: "刷新") { panel.refresh() }
                    GlassIconButton(systemName: showingSearch ? "xmark" : "magnifyingglass",
                                    help: "搜索") {
                        showingSearch.toggle()
                        if !showingSearch { panel.cancelSearch() }
                    }
                    if panel.cwdProvider != nil {
                        GlassIconButton(systemName: "arrow.triangle.2.circlepath",
                                        help: "跟随终端目录",
                                        isActive: panel.followsTerminalCwd) {
                            panel.followsTerminalCwd.toggle()
                        }
                    }
                    GlassIconButton(systemName: panel.showHidden ? "eye" : "eye.slash",
                                    help: "显示/隐藏隐藏文件") {
                        panel.showHidden.toggle()
                    }
                    Picker("", selection: $panel.viewMode) {
                        ForEach(FilesPanelModel.ViewMode.allCases) { m in
                            Text(m.label).tag(m)
                        }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .frame(width: 96)
                    .help("列表 / 目录树")
                    GlassIconButton(systemName: "folder.badge.plus", help: "新建文件夹") {
                        newFolderName = ""
                        showingNewFolder = true
                    }
                    if panel.backend is TransferBackend {
                        GlassIconButton(systemName: "square.and.arrow.up", help: "上传…") {
                            uploadFrom()
                        }
                    }
                    GlassIconButton(systemName: "arrow.up.arrow.down.circle",
                                    help: "传输队列") {
                        TransferWindow.open(manager: transfers)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                if let onClose {
                    GlassIconButton(systemName: "xmark", help: "关闭文件面板（⇧⌘E）",
                                    action: onClose)
                }
            }
            HStack(spacing: DS.Padding.xs) {
                Text(panel.backend.displayName)
                    .font(DS.Font.caption).foregroundStyle(.secondary)
                Text(shortPath).font(DS.Font.code).lineLimit(1)
                    .truncationMode(.head)
                    .help(panel.currentPath)
                Spacer()
                if panel.isLoading { ProgressView().controlSize(.small) }
            }
        }
        .padding(.horizontal, DS.Padding.s)
        .padding(.vertical, DS.Padding.xs)
    }

    private var shortPath: String { panel.currentPath.isEmpty ? "/" : panel.currentPath }

    private var deleteConfirmBinding: Binding<Bool> {
        Binding(get: { !deleteTargets.isEmpty },
                set: { if !$0 { deleteTargets = [] } })
    }

    private var deleteMessage: String {
        let dirs = deleteTargets.filter(\.isDirectory).count
        if dirs > 0 {
            return "含 \(dirs) 个目录，其内容会**一并递归删除**（不可撤销）。"
        }
        return "删除后不可撤销。"
    }

    private var searchBar: some View {
        HStack(spacing: DS.Padding.xs) {
            Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                .font(DS.Font.caption)
            TextField("搜索当前目录（递归）", text: $searchText)
                .textFieldStyle(.plain)
                .font(DS.Font.callout)
                .onSubmit { runSearch() }
            if case .running = panel.searchMode {
                Button("取消") { panel.cancelSearch() }.buttonStyle(.link)
            } else if case .done(let usedExec, let hits) = panel.searchMode {
                Text("\(hits) 项 · \(usedExec ? "远端快路径" : "本地回退") 扫描 \(panel.scanned)")
                    .font(DS.Font.captionSmall).foregroundStyle(.secondary)
            }
        }
        .padding(.horizontal, DS.Padding.s)
        .padding(.vertical, DS.Padding.xs)
    }

    private func runSearch() {
        let q = searchText
        panel.search(q)
    }

    // MARK: - 内容

    @ViewBuilder private var content: some View {
        if case .none = panel.searchMode {
            if panel.viewMode == .tree { treeList } else { directoryList }
        } else {
            resultsList
        }
    }

    /// 目录树（懒展开：展开某目录才去列它；只显示目录）。
    private var treeList: some View {
        List {
            ForEach(panel.treeChildrenVisible(panel.currentPath)) { d in
                DirRow(panel: panel, entry: d)
            }
        }
        .listStyle(.sidebar)
        .onAppear { panel.loadTreeChildren(panel.currentPath) }
        .onChange(of: panel.currentPath) { _ in panel.loadTreeChildren(panel.currentPath) }
    }

    private var directoryList: some View {
        List(selection: $panel.selection) {
            if let err = panel.errorText {
                Text(err).font(DS.Font.callout).foregroundStyle(.red)
            }
            ForEach(visibleEntries) { e in
                row(e).tag(e.path)
                    .contextMenu { contextMenu(e) }
                    .onDrag { dragProvider(for: e) }
                    .modifier(WorkspaceFileDropModifier(tab: e.isDirectory ? workspaceTab : nil,
                                                       file: workspaceFile, directory: e.path))
            }
        }
        .listStyle(.inset)
        .environment(\.defaultMinListRowHeight, 20)
        .modifier(WorkspaceFileDropModifier(tab: workspaceTab, file: workspaceFile, directory: panel.currentPath))
        .modifier(PanelKeyHandler(
            onSpace: quickLook != nil && !visibleEntries.isEmpty
                ? { if let e = selectedOrFirstEntry() { quickLook?(e) } } : nil,
            onNewFolder: { newFolderName = ""; showingNewFolder = true },
            onRename: { if let e = selectedOrFirstEntry() { renameTarget = e; renameName = e.name } },
            onInfo: { if let e = selectedOrFirstEntry() { infoTarget = e } },
            onDelete: {
                let targets = panel.selection.isEmpty
                    ? (selectedOrFirstEntry().map { [$0] } ?? [])
                    : visibleEntries.filter { panel.selection.contains($0.path) }
                if !targets.isEmpty { deleteTargets = targets }
            },
            onSearch: { showingSearch = true }))
        // 双击打开走 AppKit 通道（见 ListDoubleClick）。挂在 background：
        // 钩子视图从这里向上逐层找本 List 底层的 NSTableView。
        .background(ListDoubleClick { clickedRow in
            if let idx = Self.entryIndex(clickedRow: clickedRow,
                                         errorVisible: panel.errorText != nil,
                                         count: visibleEntries.count) {
                openEntry(visibleEntries[idx])
            }
        })
    }

    /// 空格预览的目标：优先选中项，否则第一条。
    private func selectedOrFirstEntry() -> FileEntry? {
        if let path = panel.selection.first,
           let e = visibleEntries.first(where: { $0.path == path }) { return e }
        return visibleEntries.first
    }

    /// 拖动源载荷：条目编码成 DragItem（应用内栏间拖）；**本地**条目额外带
    /// 一份 fileURL，故能直接拖出到 Finder。远端条目拖出到 Finder 需要懒下载
    /// 的文件承诺（异步），v1 未做（诚实标注）。
    private func dragProvider(for e: FileEntry) -> NSItemProvider {
        if let file = workspaceFile {
            let entries = targets(for: e).map { WorkspaceFileDrag.Entry(path: $0.path, name: $0.name, isDirectory: $0.isDirectory) }
            let drag = WorkspaceFileDrag(paneID: file.id, endpointID: file.endpointID, entries: entries)
            let data = (try? JSONEncoder().encode(drag)) ?? Data()
            let provider = NSItemProvider(item: data as NSData,
                                          typeIdentifier: file.remote ? workspaceRemoteFileType : workspaceLocalFileType)
            provider.suggestedName = file.id.uuidString + ":" + file.endpointID.uuidString
            if !file.remote {
                provider.registerDataRepresentation(forTypeIdentifier: UTType.fileURL.identifier, visibility: .all) { completion in
                    completion(URL(fileURLWithPath: e.path).dataRepresentation, nil); return nil
                }
            }
            return provider
        }
        if side == .local {
            return NSItemProvider(contentsOf: URL(fileURLWithPath: e.path))
                ?? NSItemProvider()
        }
        let item = DragItem(side: side ?? .local, path: e.path, name: e.name,
                            isDirectory: e.isDirectory)
        return NSItemProvider(object: item.encoded() as NSString)
    }

    /// 落点：区分「应用内部 DragItem」与「Finder 文件 URL」，交给 DropRouter。
    private func handleDrop(_ providers: [NSItemProvider]) {
        guard let target = side, let act = onDropAction else { return }
        for p in providers {
            if p.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
                _ = p.loadItem(forTypeIdentifier: UTType.fileURL.identifier) { data, _ in
                    var url: URL?
                    if let d = data as? Data { url = URL(dataRepresentation: d, relativeTo: nil) }
                    else if let u = data as? URL { url = u }
                    guard let u = url else { return }
                    var isDir: ObjCBool = false
                    FileManager.default.fileExists(atPath: u.path, isDirectory: &isDir)
                    let action = DropRouter.route(finderPath: u.path,
                                                  name: u.lastPathComponent,
                                                  isDirectory: isDir.boolValue, onto: target)
                    DispatchQueue.main.async { act(action) }
                }
            } else if p.canLoadObject(ofClass: NSString.self) {
                _ = p.loadObject(ofClass: NSString.self) { obj, _ in
                    guard let str = obj as? String, let item = DragItem.decode(str) else { return }
                    let action = DropRouter.route(item: item, onto: target)
                    DispatchQueue.main.async { act(action) }
                }
            }
        }
    }

    private var visibleEntries: [FileEntry] { panel.visibleEntries }

    private var resultsList: some View {
        List {
            ForEach(panel.searchResults) { e in
                VStack(alignment: .leading, spacing: 0) {
                    HStack(spacing: DS.Padding.xs) {
                        icon(e)
                        Text(e.name).font(DS.Font.callout).lineLimit(1)
                    }
                    Text(e.path).font(DS.Font.codeCaption)
                        .foregroundStyle(.secondary).lineLimit(1)
                        .truncationMode(.head)
                }
                .contextMenu { contextMenu(e) }
            }
        }
        .listStyle(.inset)
    }

    private func row(_ e: FileEntry) -> some View {
        HStack(spacing: DS.Padding.xs) {
            icon(e)
            Text(e.name).font(DS.Font.callout).lineLimit(1)
                .foregroundStyle(e.isSymlink ? Color.secondary : Color.primary)
            Spacer()
            Text(e.sizeText).font(DS.Font.caption).foregroundStyle(.secondary)
            Text(e.modeText).font(DS.Font.codeCaption).foregroundStyle(.secondary)
        }
        // 注意：这里不能加 .contentShape/.onTapGesture——行级 tap 手势会吞掉
        // List 处理选中所需的点击（macOS SwiftUI 文档化问题：给行加 tap
        // 手势后单击选中高亮失效），症状就是「点文件没高亮」。双击打开走
        // AppKit 通道（ListDoubleClick→doubleAction），单击选中留给表原生。
    }

    /// 双击「打开」语义：目录进入；远端文件走下载；本地文件交给系统打开。
    private func openEntry(_ e: FileEntry) {
        if e.isDirectory { panel.open(e) }
        else if panel.backend is TransferBackend { downloadTo(e) }
        else { NSWorkspace.shared.open(URL(fileURLWithPath: e.path)) }
    }

    /// clickedRow → visibleEntries 下标。List 第 0 行可能是错误提示文本
    /// （errorText 非空时），条目行号整体后移一位；越界/负行号一律 nil。
    static func entryIndex(clickedRow: Int, errorVisible: Bool, count: Int) -> Int? {
        let idx = clickedRow - (errorVisible ? 1 : 0)
        guard idx >= 0, idx < count else { return nil }
        return idx
    }

    @ViewBuilder private func icon(_ e: FileEntry) -> some View {
        switch e.kind {
        case .directory: Image(systemName: "folder").foregroundStyle(.secondary)
        case .symlink: Image(systemName: "link").foregroundStyle(.secondary)
        case .file: Image(systemName: "doc").foregroundStyle(.secondary)
        case .other: Image(systemName: "questionmark.square").foregroundStyle(.secondary)
        }
    }

    /// 右键目标集：右键项在选中集里且多于一项 → 对整批操作；否则只此一项。
    private func targets(for e: FileEntry) -> [FileEntry] {
        guard panel.selection.count > 1, panel.selection.contains(e.path) else { return [e] }
        return visibleEntries.filter { panel.selection.contains($0.path) }
    }

    @ViewBuilder private func contextMenu(_ e: FileEntry) -> some View {
        let many = targets(for: e)
        if many.count > 1 {
            // 批量：下载 / 删除整批（右键项在选中集里且多于一项）。
            if panel.backend is TransferBackend {
                Button("下载 \(many.count) 项…") { many.forEach { downloadTo($0) } }
            }
            Button("删除 \(many.count) 项", role: .destructive) { deleteTargets = many }
                .modifier(PanelMenuShortcut(.delete, modifiers: []))
        } else {
            singleContextMenu(e)
        }
    }

    @ViewBuilder private func singleContextMenu(_ e: FileEntry) -> some View {
        if e.isDirectory {
            Button("打开") { panel.open(e) }
        }
        if panel.backend is TransferBackend, !e.isDirectory {
            Button("下载…") { downloadTo(e) }
        }
        Button("重命名…") { renameTarget = e; renameName = e.name }
            .modifier(PanelMenuShortcut("r", modifiers: .command))
        Button("权限…") { permTarget = e; permText = String(e.mode & 0o777, radix: 8) }
        Button("信息…") { infoTarget = e }
            .modifier(PanelMenuShortcut("i", modifiers: .command))
        if quickLook != nil, !e.isDirectory {
            Button("快速查看") { quickLook?(e) }
                .modifier(PanelMenuShortcut(.space, modifiers: []))
        }
        Divider()
        Button("拷贝路径") { copyToPasteboard(e.path) }
        Divider()
        Button("删除", role: .destructive) {
            deleteTargets = [e]
        }
        .modifier(PanelMenuShortcut(.delete, modifiers: []))
    }

    private func copyToPasteboard(_ s: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(s, forType: .string)
    }

    // MARK: - 弹窗

    private var newFolderSheet: some View {
        formSheet(title: "新建文件夹") {
            TextField("名称", text: $newFolderName)
            HStack {
                Button("取消") { showingNewFolder = false }
                Button("创建") {
                    panel.newFolder(name: newFolderName)
                    showingNewFolder = false
                }.keyboardShortcut(.defaultAction).disabled(newFolderName.isEmpty)
            }
        }
    }

    private func renamingSheet(_ e: FileEntry) -> some View {
        formSheet(title: "重命名「\(e.name)」") {
            TextField("新名称", text: $renameName)
            HStack {
                Button("取消") { renameTarget = nil }
                Button("重命名") {
                    panel.rename(e, to: renameName)
                    renameTarget = nil
                }.keyboardShortcut(.defaultAction).disabled(renameName.isEmpty)
            }
        }
    }

    private func permissionSheet(_ e: FileEntry) -> some View {
        formSheet(title: "权限「\(e.name)」") {
            Text("八进制模式（如 644 / 755）").font(DS.Font.caption)
            TextField("模式", text: $permText)
                .font(DS.Font.code)
                .frame(width: 120)
            if e.isDirectory {
                Toggle("应用到子项（递归）", isOn: $permRecursive)
                    .font(DS.Font.callout)
            }
            HStack {
                Button("取消") { permTarget = nil }
                Button("应用") {
                    if let m = UInt32(permText, radix: 8) {
                        panel.setPermissions(e, mode: m & 0o7777,
                                             recursive: permRecursive)
                    }
                    permTarget = nil
                }.keyboardShortcut(.defaultAction)
            }
        }
    }

    private func infoSheet(_ e: FileEntry) -> some View {
        formSheet(title: "信息") {
            infoRow("名称", e.name)
            infoRow("路径", e.path)
            infoRow("类型", kindText(e.kind))
            infoRow("大小", e.sizeText)
            infoRow("权限", e.modeText)
            if let m = e.mtime {
                infoRow("修改时间", DateFormatter.localizedString(
                    from: m, dateStyle: .medium, timeStyle: .medium))
            }
            if e.hasOwner { infoRow("uid/gid", "\(e.uid)/\(e.gid)") }
            HStack { Spacer(); Button("关闭") { infoTarget = nil } }
        }
    }

    private func kindText(_ k: FileEntry.Kind) -> String {
        switch k {
        case .directory: return "目录"
        case .file: return "文件"
        case .symlink: return "符号链接"
        case .other: return "其它"
        }
    }

    private func infoRow(_ label: String, _ value: String) -> some View {
        HStack(alignment: .top) {
            Text(label).font(DS.Font.caption).foregroundStyle(.secondary)
                .frame(width: 72, alignment: .leading)
            Text(value).font(DS.Font.callout).textSelection(.enabled)
        }
    }

    @ViewBuilder
    private func formSheet<C: View>(title: String, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: DS.Padding.m) {
            Text(title).font(DS.Font.title)
            content()
        }
        .padding(DS.Padding.l)
        .frame(minWidth: 320)
    }
}


private struct FilesPanelChromeHeightKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = max(value, nextValue())
    }
}

/// 右键菜单上显示面板的快捷键。**只在 macOS 14+ 显示**：键位由
/// `PanelKeyHandler` 的 `onKeyPress` 接，13 上根本没人接（见下），菜单
/// 不能承诺一个按不动的键。键位本身仍是 `PanelKeymap` 那一份
/// （`FilesPanelTests` 把菜单上的键倒推回 `PanelKeymap` 对拍）。
private struct PanelMenuShortcut: ViewModifier {
    let key: KeyEquivalent
    let modifiers: EventModifiers

    init(_ key: KeyEquivalent, modifiers: EventModifiers) {
        self.key = key
        self.modifiers = modifiers
    }

    func body(content: Content) -> some View {
        if #available(macOS 14.0, *) {
            content.keyboardShortcut(key, modifiers: modifiers)
        } else {
            content
        }
    }
}

/// 面板键盘动作（macOS 14+ 的 onKeyPress；13 上是 no-op——诚实降级到右键菜单）。
/// 刻意只挂在**文件列表**上，不挂窗口级，免得把 ⌫/空格 从终端里偷走。
private struct PanelKeyHandler: ViewModifier {
    let onSpace: (() -> Void)?
    let onNewFolder: () -> Void
    let onRename: () -> Void
    let onInfo: () -> Void
    let onDelete: () -> Void
    let onSearch: () -> Void

    func body(content: Content) -> some View {
        if #available(macOS 14.0, *) {
            content.onKeyPress(.space) {
                guard let a = onSpace else { return .ignored }
                a(); return .handled
            }
            .onKeyPress { press in
                if press.key == .delete { onDelete(); return .handled }
                let key = press.characters.lowercased()
                switch PanelKeymap.action(key: key,
                                          command: press.modifiers.contains(.command),
                                          shift: press.modifiers.contains(.shift)) {
                case .newFolder: onNewFolder(); return .handled
                case .rename: onRename(); return .handled
                case .info: onInfo(); return .handled
                case .search: onSearch(); return .handled
                case .delete: onDelete(); return .handled
                case .none: return .ignored
                }
            }
        } else {
            content
        }
    }
}


/// 目录树的一行（懒展开）。双击导航到该目录（列表模式同步过去）。
private struct DirRow: View {
    @ObservedObject var panel: FilesPanelModel
    let entry: FileEntry

    var body: some View {
        DisclosureGroup(
            isExpanded: Binding(
                get: { panel.expandedDirs.contains(entry.path) },
                set: { _ in panel.toggleTree(entry.path) })
        ) {
            ForEach(panel.treeChildrenVisible(entry.path)) { child in
                DirRow(panel: panel, entry: child)
            }
        } label: {
            Label(entry.name, systemImage: "folder")
                .font(DS.Font.callout)
                .lineLimit(1)
                .contentShape(Rectangle())
                .onTapGesture(count: 2) { panel.navigate(to: entry.path) }
        }
    }
}

/// 给 SwiftUI List 底层的 NSTableView 挂 doubleAction：双击打开走 AppKit
/// 通道，不与单击选中打架。
///
/// 为什么不用行级 .onTapGesture(count:2)：tap 手势会吞掉 List 处理选中
/// 所需的点击（macOS SwiftUI 文档化问题——给行加 tap 手势后单击选中高亮
/// 失效），用户看到的就是「点文件没高亮」。doubleAction 与选中是两条
/// 通道：单击选中由表原生处理，双击时表额外向 target 发 action。
private struct ListDoubleClick: NSViewRepresentable {
    let onRow: (Int) -> Void

    final class Coordinator: NSObject {
        var onRow: (Int) -> Void
        init(_ onRow: @escaping (Int) -> Void) { self.onRow = onRow }
        // 避免与 NSObject.perform(_:) 重载冲突；#selector 可能解析为 performSelector:。
        @objc func openClickedRow(_ sender: NSTableView) { onRow(sender.clickedRow) }
    }

    func makeCoordinator() -> Coordinator { Coordinator(onRow) }
    func makeNSView(context: Context) -> NSView { NSView() }

    func updateNSView(_ nsView: NSView, context: Context) {
        context.coordinator.onRow = onRow
        // 表由 SwiftUI 在后续 runloop 才建出来（与 claimFilePaneFocus 同款
        // 时序问题），异步找；每次渲染都会补一遍，装好后幂等跳过。
        DispatchQueue.main.async {
            guard let table = Self.enclosingTable(of: nsView),
                  table.target !== context.coordinator else { return }
            table.doubleAction = #selector(Coordinator.openClickedRow(_:))
            table.target = context.coordinator
        }
    }

    /// 从钩子视图向上逐层、每层向下找表：第一个命中的层就是本 List 的表
    /// ——不再向上，避免把邻格窗格的表错认成自己的。
    static func enclosingTable(of view: NSView) -> NSTableView? {
        var node: NSView? = view
        while let cur = node {
            if let table = findTable(in: cur) { return table }
            node = cur.superview
        }
        return nil
    }

    private static func findTable(in v: NSView) -> NSTableView? {
        if let t = v as? NSTableView { return t }
        return v.subviews.lazy.compactMap(findTable(in:)).first
    }
}
