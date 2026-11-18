// FilesPanelModel — 文件浏览器面板的 UI 状态与驱动。
//
// 与后端 FileBackend 解耦：本地 shell 标签页后端=LocalFileBackend，SSH 标签页
// 后端=SFTPFileBackend，UI 零分叉。阻塞调用全在私有串行队列上跑（远程后端
// 因此单线程用 libssh2）；结果 hop 回主线程发布 @Published。
import Foundation
import GhostermCore

private final class FilePanelReadRequest: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }
    func cancel() { lock.lock(); cancelled = true; lock.unlock() }
}

@MainActor
public final class FilesPanelModel: ObservableObject {
    public enum SortKey: String, CaseIterable, Identifiable {
        case name, size, date
        public var id: String { rawValue }
        public var label: String {
            switch self { case .name: return "名称"; case .size: return "大小"; case .date: return "修改时间" }
        }
    }

    public func reportError(_ message: String) { errorText = message }

    public let backend: FileBackend

    @Published public private(set) var currentPath: String
    @Published public private(set) var entries: [FileEntry] = []
    @Published public private(set) var errorText: String?
    @Published public private(set) var isLoading = false

    /// 展示形态：列表（当前目录）或目录树（懒展开）。
    public enum ViewMode: String, CaseIterable, Identifiable {
        case list, tree
        public var id: String { rawValue }
        public var label: String { self == .list ? "列表" : "树" }
    }
    @Published public var viewMode: ViewMode = .list
    /// 树模式：已加载的子目录（path → 子项，仅目录）。
    @Published public private(set) var treeChildren: [String: [FileEntry]] = [:]
    /// 树模式：展开集合。
    @Published public var expandedDirs: Set<String> = []
    /// 树模式的加载状态（避免重复请求）。
    @Published public private(set) var treeLoading: Set<String> = []
    @Published public private(set) var treeErrors: [String: String] = [:]
    private var treeEpoch: UInt64 = 0

    /// 「跟随终端目录」：本地 shell 标签页默认开——终端 `cd` 到哪，面板跟到哪。
    /// cwd 是内核查询（非 @Published），故用**轮询**比对。
    @Published public var followsTerminalCwd = false
    /// 提供当前终端 cwd（由 AppModel 注入 tab.currentDirectory）。nil = 不跟随。
    public var cwdProvider: (() -> String?)?

    @Published public var showHidden = false
    @Published public var filter = ""
    @Published public var sortKey: SortKey = .name
    @Published public var ascending = true
    @Published public var selection: Set<String> = []

    // 搜索（⌘F）：优先远端 exec 快路径，禁 exec 显式回退 walk。
    public enum SearchMode: Equatable { case none, running, done(usedExec: Bool, hits: Int), failed(hits: Int) }
    @Published public private(set) var searchMode: SearchMode = .none
    @Published public private(set) var searchResults: [FileEntry] = []
    @Published public private(set) var scanned = 0

    private let queue = DispatchQueue(label: "dev.ghosterm.files-panel")
    private var backStack: [String] = []
    private var forwardStack: [String] = []
    private var searchRequest: FilePanelReadRequest?
    private var searchError: String?
    private var lastSearch: (query: String, depth: Int)?
    var searchLimits = FileSearchLimits()
    @Published public private(set) var searchDepth = 64
    private var navigationRequest: FilePanelReadRequest?
    private var pendingNavigationPath: Data?
    private enum NavigationHistory { case record, unchanged, back, forward }

    public init(backend: FileBackend, initialPath: String? = nil) {
        self.backend = backend
        self.currentPath = initialPath ?? backend.initialPath
    }

    // MARK: - 派生视图

    /// 过滤 + 排序后的可见条目（目录优先，再按 sortKey）。
    public var visibleEntries: [FileEntry] {
        let f = filter.trimmingCharacters(in: .whitespaces)
        var list = entries
        if !showHidden { list = list.filter { !$0.isHidden } }
        if !f.isEmpty {
            list = list.filter { Self.matches($0.name, f) }
        }
        list.sort { a, b in
            if a.isDirectory != b.isDirectory { return a.isDirectory }
            switch sortKey {
            case .name:
                let r = a.name.localizedStandardCompare(b.name)
                return ascending ? r == .orderedAscending : r == .orderedDescending
            case .size:
                return ascending ? a.size < b.size : a.size > b.size
            case .date:
                let am = a.mtime ?? .distantPast, bm = b.mtime ?? .distantPast
                return ascending ? am < bm : am > bm
            }
        }
        return list
    }

    public var canGoBack: Bool { !backStack.isEmpty }
    public var canGoForward: Bool { !forwardStack.isEmpty }

    // MARK: - 导航

    public func start() {
        navigate(to: currentPath, record: false)
        startFollowing()
    }

    // MARK: - 跟随终端目录

    private var followTimer: Timer?

    /// 开始轮询终端 cwd（1s 一次；cwd 变了就导航过去）。幂等。
    public func startFollowing() {
        guard cwdProvider != nil, followTimer == nil else { return }
        let t = Timer(timeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            guard self.followsTerminalCwd, let cwd = self.cwdProvider?(),
                  self.shouldFollow(cwd) else { return }
            self.navigate(to: cwd)
        }
        RunLoop.main.add(t, forMode: .common)
        followTimer = t
    }

    /// 供测试直接驱动一次跟随检查（不必等 Timer）。
    public func followTick() {
        guard followsTerminalCwd, let cwd = cwdProvider?(), shouldFollow(cwd) else { return }
        navigate(to: cwd)
    }

    private func shouldFollow(_ directory: String) -> Bool {
        let path = Data(directory.utf8)
        return !directory.isEmpty && path != Data(currentPath.utf8) && pendingNavigationPath != path
    }

    deinit { followTimer?.invalidate(); navigationRequest?.cancel(); searchRequest?.cancel() }

    public func navigate(to path: String, record: Bool = true) {
        requestNavigation(path, history: record ? .record : .unchanged)
    }

    private func requestNavigation(_ path: String, history: NavigationHistory) {
        let target = path.isEmpty ? "/" : path
        cancelSearch()
        navigationRequest?.cancel()
        let request = FilePanelReadRequest()
        navigationRequest = request
        pendingNavigationPath = Data(target.utf8)
        let origin = currentPath, back = backStack, forward = forwardStack
        let backend = self.backend
        isLoading = true
        queue.async { [weak self] in
            guard !request.isCancelled else { return }
            let result = Result { try backend.list(target) }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.navigationRequest === request else { return }
                self.isLoading = false
                self.pendingNavigationPath = nil
                switch result {
                case .success(let list):
                    let changed = Data(origin.utf8) != Data(target.utf8)
                    switch history {
                    case .record:
                        if changed { self.backStack = back + [origin]; self.forwardStack = [] }
                    case .unchanged: break
                    case .back:
                        self.backStack = Array(back.dropLast())
                        self.forwardStack = forward + [origin]
                    case .forward:
                        self.forwardStack = Array(forward.dropLast())
                        self.backStack = back + [origin]
                    }
                    if changed { self.cancelSearch(); self.invalidateTreeCache(preserveExpansion: false) }
                    self.currentPath = target
                    self.entries = list
                    self.errorText = nil
                    self.selection.removeAll()
                    if changed, self.viewMode == .tree { self.loadTreeChildren(target) }
                case .failure(let error):
                    // Keep the complete last successful directory/history snapshot.
                    self.errorText = "无法打开 \(target)：\(error)"
                }
            }
        }
    }

    public func refresh() {
        let reloadTree = viewMode == .tree || !treeChildren.isEmpty || !treeLoading.isEmpty || !treeErrors.isEmpty
        let prefix = Array((currentPath.hasSuffix("/") ? currentPath : currentPath + "/").utf8)
        let expanded = expandedDirs.filter { Array($0.utf8).starts(with: prefix) }
        invalidateTreeCache(preserveExpansion: true)
        navigate(to: currentPath, record: false)
        if reloadTree {
            loadTreeChildren(currentPath)
            for path in expanded { loadTreeChildren(path) }
        }
    }

    private func invalidateTreeCache(preserveExpansion: Bool) {
        treeEpoch &+= 1
        treeChildren.removeAll()
        treeErrors.removeAll()
        treeLoading.removeAll()
        if !preserveExpansion { expandedDirs.removeAll() }
    }

    public func goBack() {
        guard let previous = backStack.last else { return }
        requestNavigation(previous, history: .back)
    }

    public func goForward() {
        guard let next = forwardStack.last else { return }
        requestNavigation(next, history: .forward)
    }

    public func up() {
        guard currentPath != "/", !currentPath.isEmpty else { return }
        let parent = (currentPath as NSString).deletingLastPathComponent
        navigate(to: parent.isEmpty ? "/" : parent)
    }

    /// 双击：目录进入；文件交给调用方（下载/预览/打开）。
    public func open(_ entry: FileEntry) {
        if entry.isDirectory { navigate(to: entry.path) }
    }

    // MARK: - 写侧操作（写完刷新）

    private func run(_ label: String, _ op: @escaping (FileBackend) throws -> Void) {
        queue.async { [weak self] in
            guard let self else { return }
            do {
                try op(self.backend)
                DispatchQueue.main.async { self.refresh() }
            } catch {
                DispatchQueue.main.async { self.errorText = "\(label)失败：\(error)" }
            }
        }
    }

    public func newFolder(name: String) {
        let path = (currentPath as NSString).appendingPathComponent(name)
        run("新建文件夹") { try $0.mkdir(path, mode: 0o755) }
    }

    public func rename(_ entry: FileEntry, to newName: String) {
        let to = (currentPath as NSString).appendingPathComponent(newName)
        run("重命名") { try $0.rename(entry.path, to) }
    }

    public func delete(_ items: [FileEntry]) {
        run("删除") { b in
            for e in items { try b.remove(e.path, recursive: e.isDirectory) }
        }
    }

    public func setPermissions(_ entry: FileEntry, mode: UInt32,
                               recursive: Bool = false) {
        run("改权限") { b in
            guard !entry.isSymlink else {
                throw GhostermError.failed("符号链接不支持改权限")
            }
            guard recursive, entry.isDirectory else {
                try b.chmod(entry.path, mode: mode)
                return
            }
            // First finish the owner-held walk, then issue chmod requests. A
            // visitor must not re-enter the serialized SFTP owner queue.
            var children: [FileEntry] = []
            try b.walk(root: entry.path, maxDepth: -1) { child in
                children.append(child)
                return true
            }
            try b.chmod(entry.path, mode: mode)
            // 保留逐项结果：扫描失败不能伪装成部分成功，链接明确跳过。
            var failures: [String] = []
            var skippedLinks = 0
            for child in children {
                if child.isSymlink { skippedLinks += 1; continue }
                do { try b.chmod(child.path, mode: mode) }
                catch { failures.append("\(child.path): \(error)") }
            }
            guard failures.isEmpty, skippedLinks == 0 else {
                let details = failures.sorted().prefix(5).joined(separator: "；")
                throw GhostermError.failed("递归改权限未完全完成：失败 \(failures.count)，跳过符号链接 \(skippedLinks)\(details.isEmpty ? "" : "；\(details)")")
            }
        }
    }

    /// 多选批量：把一组条目一次性删除（目录递归）。
    public func deleteAll(_ items: [FileEntry]) { delete(items) }

    // MARK: - 目录树（懒加载：展开某个目录才去列它）

    /// 展开/收起某个目录（展开则按需加载它的子目录）。
    public func toggleTree(_ path: String) {
        if expandedDirs.contains(path) {
            expandedDirs.remove(path)
        } else {
            expandedDirs.insert(path)
            loadTreeChildren(path)
        }
    }

    /// 懒加载某目录的子项（只保留目录，避免树把整棵树塞进内存）。
    public func loadTreeChildren(_ path: String) {
        guard treeChildren[path] == nil, !treeLoading.contains(path) else { return }
        let epoch = treeEpoch, backend = self.backend
        treeErrors[path] = nil
        treeLoading.insert(path)
        queue.async { [weak self] in
            let result = Result { try backend.list(path).filter(\.isDirectory).sorted {
                $0.name.localizedStandardCompare($1.name) == .orderedAscending
            } }
            DispatchQueue.main.async { [weak self] in
                guard let self, epoch == self.treeEpoch else { return }
                self.treeLoading.remove(path)
                switch result {
                case .success(let directories):
                    self.treeChildren[path] = directories
                    self.treeErrors[path] = nil
                case .failure(let error):
                    // A failed read is retryable, never a cached empty directory.
                    self.treeErrors[path] = "读取 \(path) 失败：\(error)"
                }
            }
        }
    }

    public func retryTree(_ path: String) {
        guard !treeLoading.contains(path) else { return }
        treeChildren[path] = nil
        treeErrors[path] = nil
        loadTreeChildren(path)
    }

    /// 树模式可见的子目录（隐藏文件规则同列表）。
    public func treeChildrenVisible(_ path: String) -> [FileEntry] {
        let kids = treeChildren[path] ?? []
        return showHidden ? kids : kids.filter { !$0.isHidden }
    }

    // MARK: - 搜索

    public func search(_ query: String, maxDepth: Int = -1) {
        cancelSearch()
        lastSearch = (query, maxDepth)
        let pattern = query.trimmingCharacters(in: .whitespaces)
        guard !pattern.isEmpty else { return }
        let limits = searchLimits
        guard pattern.utf8.count <= 1024, !pattern.utf8.contains(0), maxDepth >= -1,
              maxDepth <= limits.depth else {
            searchMode = .failed(hits: 0)
            errorText = "搜索条件无效：名称最多 1024 字节，深度不能超过 \(limits.depth) 层"
            searchError = errorText
            return
        }
        let depth = maxDepth < 0 ? limits.depth : maxDepth
        searchDepth = depth
        let request = FilePanelReadRequest(), state = FileSearchState(limits: limits)
        searchRequest = request
        searchMode = .running
        errorText = nil
        let root = currentPath, backend = self.backend
        let publish: (FileSearchSnapshot) -> Void = { [weak self] snapshot in
            DispatchQueue.main.async { [weak self] in
                guard let self, self.searchRequest === request, !request.isCancelled else { return }
                self.searchResults = snapshot.entries
                self.scanned = snapshot.scanned
            }
        }
        queue.async { [weak self] in
            guard !request.isCancelled else { return }
            let result: Result<Bool, Error> = Result {
                let resolvedRoot = try backend.realpath(root)
                guard !request.isCancelled else { throw FileSearchError.cancelled }
                let usedExec: Bool
                if let remote = backend as? TransferBackend {
                    usedExec = try remote.search(root: resolvedRoot, pattern: pattern, maxDepth: depth,
                        isCancelled: { request.isCancelled || state.snapshot().stopped != nil },
                        onHit: { entry in
                            guard !request.isCancelled else { return false }
                            let more = state.append(entry)
                            if let snapshot = state.progress() { publish(snapshot) }
                            return more
                        }, onScanned: { count in
                            _ = state.scan(count)
                            if let snapshot = state.progress() { publish(snapshot) }
                        })
                } else {
                    usedExec = false
                    try backend.walk(root: resolvedRoot, maxDepth: depth) { entry in
                        guard !request.isCancelled, state.scan() else { return false }
                        let more = !Self.matches(entry.name, pattern) || state.append(entry)
                        if let snapshot = state.progress() { publish(snapshot) }
                        return more && !request.isCancelled
                    }
                }
                if let reason = state.snapshot().stopped { throw FileSearchError.incomplete(reason) }
                guard !request.isCancelled else { throw FileSearchError.cancelled }
                return usedExec
            }
            let snapshot = state.snapshot()
            DispatchQueue.main.async { [weak self] in
                guard let self, self.searchRequest === request, !request.isCancelled else { return }
                self.searchResults = snapshot.entries
                self.scanned = snapshot.scanned
                switch result {
                case .success(let usedExec):
                    self.searchMode = .done(usedExec: usedExec, hits: snapshot.entries.count)
                    self.searchError = nil
                case .failure(let error):
                    self.searchMode = .failed(hits: snapshot.entries.count)
                    self.errorText = "搜索未完成：\(snapshot.stopped ?? String(describing: error))（已保留 \(snapshot.entries.count) 项）"
                    self.searchError = self.errorText
                }
            }
        }
    }

    public func retrySearch() {
        if let lastSearch { search(lastSearch.query, maxDepth: lastSearch.depth) }
    }

    public func cancelSearch() {
        searchRequest?.cancel()
        searchRequest = nil
        searchMode = .none
        searchResults = []
        scanned = 0
        if let searchError, errorText == searchError { errorText = nil }
        searchError = nil
    }

    // MARK: - 匹配（与 SFTPFileBackend.matches 同口径）

    static func matches(_ name: String, _ pattern: String) -> Bool {
        SFTPFileBackend.matches(name, pattern)
    }
}
