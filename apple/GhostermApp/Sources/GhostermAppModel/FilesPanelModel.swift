// FilesPanelModel — 文件浏览器面板的 UI 状态与驱动。
//
// 与后端 FileBackend 解耦：本地 shell 标签页后端=LocalFileBackend，SSH 标签页
// 后端=SFTPFileBackend，UI 零分叉。阻塞调用全在私有串行队列上跑（远程后端
// 因此单线程用 libssh2）；结果 hop 回主线程发布 @Published。
import Foundation
import GhostermCore

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
    private var treeLoading: Set<String> = []

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
    public enum SearchMode: Equatable { case none, running(usedExec: Bool), done(usedExec: Bool, hits: Int) }
    @Published public private(set) var searchMode: SearchMode = .none
    @Published public private(set) var searchResults: [FileEntry] = []
    @Published public private(set) var scanned = 0

    private let queue = DispatchQueue(label: "dev.ghosterm.files-panel")
    private var backStack: [String] = []
    private var forwardStack: [String] = []
    private var searchToken = 0

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
                  !cwd.isEmpty, cwd != self.currentPath else { return }
            self.navigate(to: cwd)
        }
        RunLoop.main.add(t, forMode: .common)
        followTimer = t
    }

    /// 供测试直接驱动一次跟随检查（不必等 Timer）。
    public func followTick() {
        guard followsTerminalCwd, let cwd = cwdProvider?(), !cwd.isEmpty,
              cwd != currentPath else { return }
        navigate(to: cwd)
    }

    deinit { followTimer?.invalidate() }

    public func navigate(to path: String, record: Bool = true) {
        let target = path.isEmpty ? "/" : path
        isLoading = true
        queue.async { [weak self] in
            guard let self else { return }
            do {
                let list = try self.backend.list(target)
                DispatchQueue.main.async {
                    if record, self.currentPath != target {
                        self.backStack.append(self.currentPath)
                        self.forwardStack.removeAll()
                    }
                    self.currentPath = target
                    self.entries = list
                    self.errorText = nil
                    self.isLoading = false
                    self.selection.removeAll()
                }
            } catch {
                DispatchQueue.main.async {
                    self.errorText = "\(error)"
                    self.isLoading = false
                }
            }
        }
    }

    public func refresh() { navigate(to: currentPath, record: false) }

    public func goBack() {
        guard let prev = backStack.popLast() else { return }
        forwardStack.append(currentPath)
        navigate(to: prev, record: false)
    }

    public func goForward() {
        guard let next = forwardStack.popLast() else { return }
        backStack.append(currentPath)
        navigate(to: next, record: false)
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
            try b.chmod(entry.path, mode: mode)
            guard recursive, entry.isDirectory else { return }
            // 递归应用到子项：walk 只报条目，逐个 chmod；单个失败不中断
            // （已改的成功，失败的在 errorText 里如实反映）。
            var firstError: Error?
            try? b.walk(root: entry.path, maxDepth: -1) { child in
                do { try b.chmod(child.path, mode: mode) }
                catch { if firstError == nil { firstError = error } }
                return true
            }
            if let e = firstError { throw e }
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
        treeLoading.insert(path)
        queue.async { [weak self] in
            guard let self else { return }
            let subs = (try? self.backend.list(path)) ?? []
            let dirs = subs.filter(\.isDirectory).sorted {
                $0.name.localizedStandardCompare($1.name) == .orderedAscending
            }
            DispatchQueue.main.async {
                self.treeChildren[path] = dirs
                self.treeLoading.remove(path)
            }
        }
    }

    /// 树模式可见的子目录（隐藏文件规则同列表）。
    public func treeChildrenVisible(_ path: String) -> [FileEntry] {
        let kids = treeChildren[path] ?? []
        return showHidden ? kids : kids.filter { !$0.isHidden }
    }

    // MARK: - 搜索

    public func search(_ query: String, maxDepth: Int = -1) {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { cancelSearch(); return }
        guard let tb = backend as? TransferBackend else {
            // 本地后端：用 walk 做客户端匹配。
            runLocalSearch(q, maxDepth: maxDepth)
            return
        }
        searchToken += 1
        let token = searchToken
        searchResults = []
        scanned = 0
        searchMode = .running(usedExec: true)
        queue.async { [weak self] in
            guard let self else { return }
            var hits: [FileEntry] = []
            var usedExec = true
            let lock = NSLock()
            do {
                usedExec = try tb.search(root: self.currentPath, pattern: q, maxDepth: maxDepth,
                    onHit: { e in
                        lock.lock(); hits.append(e); lock.unlock()
                        DispatchQueue.main.async {
                            guard token == self.searchToken else { return }
                            self.searchResults = hits
                        }
                        return true
                    },
                    onScanned: { n in
                        DispatchQueue.main.async {
                            guard token == self.searchToken else { return }
                            self.scanned = n
                        }
                    })
            } catch {
                DispatchQueue.main.async {
                    guard token == self.searchToken else { return }
                    self.searchMode = .done(usedExec: usedExec, hits: hits.count)
                    self.errorText = "搜索失败：\(error)"
                }
                return
            }
            DispatchQueue.main.async {
                guard token == self.searchToken else { return }
                self.searchResults = hits
                self.searchMode = .done(usedExec: usedExec, hits: hits.count)
            }
        }
    }

    private func runLocalSearch(_ q: String, maxDepth: Int) {
        searchToken += 1
        let token = searchToken
        searchResults = []; scanned = 0
        searchMode = .running(usedExec: false)
        queue.async { [weak self] in
            guard let self else { return }
            var hits: [FileEntry] = []
            var n = 0
            try? self.backend.walk(root: self.currentPath, maxDepth: maxDepth) { e in
                n += 1
                if Self.matches(e.name, q) { hits.append(e) }
                if n % 200 == 0 {
                    DispatchQueue.main.async {
                        guard token == self.searchToken else { return }
                        self.searchResults = hits; self.scanned = n
                    }
                }
                return true
            }
            DispatchQueue.main.async {
                guard token == self.searchToken else { return }
                self.searchResults = hits
                self.scanned = n
                self.searchMode = .done(usedExec: false, hits: hits.count)
            }
        }
    }

    public func cancelSearch() {
        searchToken += 1
        searchMode = .none
        searchResults = []
        scanned = 0
    }

    // MARK: - 匹配（与 SFTPFileBackend.matches 同口径）

    static func matches(_ name: String, _ pattern: String) -> Bool {
        SFTPFileBackend.matches(name, pattern)
    }
}
