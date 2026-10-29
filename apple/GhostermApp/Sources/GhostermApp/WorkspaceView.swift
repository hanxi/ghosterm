import AppKit
import GhostermAppModel
import GhostermTerminal
import SwiftUI

let workspacePaneType = "com.ghosterm.workspace-pane"

@MainActor func workspaceDragProvider(tab: TerminalTab, paneID: UUID) -> NSItemProvider {
    let data = (try? JSONEncoder().encode(WorkspacePaneDrag(tabID: tab.id, paneID: paneID))) ?? Data()
    return NSItemProvider(item: data as NSData, typeIdentifier: workspacePaneType)
}

struct WorkspaceView: NSViewRepresentable {
    @ObservedObject var model: AppModel
    @ObservedObject var tab: TerminalTab
    let isActive: Bool
    let opts: PaneChromeOptions
    func makeNSView(context: Context) -> WorkspaceHostView { WorkspaceHostView() }
    func updateNSView(_ view: WorkspaceHostView, context: Context) {
        view.update(model: model, tab: tab, isActive: isActive, opts: opts)
    }
}

/// 窗格内容的宿主。`mouseDownCanMoveWindow = false` 与 `WorkspaceHostView`
/// 同理：窗格上沿会顶进系统标题栏带（侧栏展开时右列没有顶栏），不让 AppKit
/// 把带内窗格上的拖动劫持成「移动窗口」——否则窗格标题栏里的拖动把手、
/// 双击、划选统统收不到 mouseDragged。
@MainActor final class WorkspacePaneHostingView: NSHostingView<WorkspacePaneView> {
    override var mouseDownCanMoveWindow: Bool { false }
}

/// 叶子的 NSHostingView 始终是同一父节点的子视图。重排仅修改矩形，不能重新挂终端。
@MainActor final class WorkspaceHostView: NSView {
    override var isFlipped: Bool { true }
    /// 工作区**永不**移动窗口：移窗只归顶带（WindowDragArea 的显式
    /// performDrag）。系统标题栏带（顶部约 28pt）内的 mouseDown 会被 AppKit
    /// 按命中视图的 `mouseDownCanMoveWindow`（非不透明视图默认 YES）劫持成
    /// 窗口拖动，工作区里任何可拖的东西（窗格把手/分隔条/终端划选）都会
    /// 因此失效。缝隙区域（本视图自身）同样拒绝。
    override var mouseDownCanMoveWindow: Bool { false }
    private(set) var hosts: [UUID: NSHostingView<WorkspacePaneView>] = [:]
    private var tab: TerminalTab?
    private var opts = PaneChromeOptions()
    private var isActive = false
    private var mouseMonitor: Any?
    private var previewObservers: [NSObjectProtocol] = []
    let splitPreview = SplitPreview()
    private var placement: GhostermAppModel.TabPlacement?
    private var zoomedID: UUID?
    private var isDraggingPane = false
    private var workspaceLayout: WorkspaceLayout?
    private var focusedPaneID: UUID?
    private var dividers: [WorkspaceDividerView] = []
    private var dividerGeometry: [WorkspaceLayout.Divider] = []
    private var normalFrames: [UUID: CGRect] = [:]
    private var preview: (WorkspacePaneDrag, UUID, WorkspaceLayout.Edge, CGRect)?
    private let previewLayer = CAShapeLayer()
    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([NSPasteboard.PasteboardType(workspacePaneType)])
        wantsLayer = true
        previewLayer.fillColor = NSColor.controlAccentColor.withAlphaComponent(0.18).cgColor
        previewLayer.strokeColor = NSColor.controlAccentColor.cgColor
        previewLayer.lineWidth = 2
        previewLayer.zPosition = 1000
        layer?.addSublayer(previewLayer)
        mouseMonitor = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .keyDown]) { [weak self] event in
            guard let self, self.isActive, let window = self.window, event.window === window, let tab = self.tab else { return event }
            if event.type != .keyDown || event.keyCode == 53 { self.splitPreview.clear() }
            guard event.type == .leftMouseDown else { return event }
            let point = self.convert(event.locationInWindow, from: nil)
            if let id = self.hosts.first(where: { !$0.value.isHidden && $0.value.frame.contains(point)
                && (tab.zoomedPaneID == nil || tab.zoomedPaneID == $0.key) })?.key {
                tab.activeWorkspacePaneID = id
            }
            return event
        }
        // Esc 取消由 AppKit 终止拖动会话；结束回调统一清预览，不依赖
        // `draggingExited`（取消时不保证送达）。
        for name in [Notification.Name.workspacePaneDragBegan, .workspacePaneDragEnded, NSWindow.didResignKeyNotification] {
            previewObservers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] note in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    if name == NSWindow.didResignKeyNotification, (note.object as? NSWindow) !== self.window { return }
                    self.splitPreview.clear()
                    if name == .workspacePaneDragBegan { self.isDraggingPane = true }
                    if name == .workspacePaneDragEnded { self.isDraggingPane = false }
                    if name == .workspacePaneDragEnded { self.clearDropPreview() }
                }
            })
        }
    }
    deinit {
        if let mouseMonitor { NSEvent.removeMonitor(mouseMonitor) }
        previewObservers.forEach(NotificationCenter.default.removeObserver)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) 未实现") }
    func update(model: AppModel, tab: TerminalTab, isActive: Bool, opts: PaneChromeOptions) {
        let focusMoved = self.tab !== tab || focusedPaneID != tab.activeWorkspacePaneID
        // 浏览按钮时保留键盘；命令真正切换窗格后让新终端 / 文件列表接手。
        if isActive, focusMoved, let button = window?.firstResponder as? PaneActionButton,
           button.isDescendant(of: self) {
            window?.makeFirstResponder(nil)
        }
        if self.tab !== tab || !isActive || !opts.showsTitleBar || tab.isLocked ||
            placement != model.preferences.tabPlacement || zoomedID != tab.zoomedPaneID ||
            workspaceLayout != tab.workspaceLayout || focusedPaneID != tab.activeWorkspacePaneID {
            splitPreview.clear()
        }
        placement = model.preferences.tabPlacement
        zoomedID = tab.zoomedPaneID
        workspaceLayout = tab.workspaceLayout; focusedPaneID = tab.activeWorkspacePaneID
        self.tab = tab; self.opts = opts; self.isActive = isActive
        let live = Set(tab.workspacePanes.map(\.id))
        for id in Array(hosts.keys) where !live.contains(id) {
            hosts.removeValue(forKey: id)?.removeFromSuperview()
        }
        let visible = Set(tab.visibleWorkspaceLayout?.paneIDs ?? [])
        for pane in tab.workspacePanes {
            let focused = isActive && tab.activeWorkspacePaneID == pane.id
            let showing = isActive && visible.contains(pane.id) && (tab.zoomedPaneID == nil || tab.zoomedPaneID == pane.id)
            let root = WorkspacePaneView(model: model, tab: tab, pane: pane, isFocused: focused,
                                         isVisible: showing, opts: opts,
                                         fileChromeChanged: { [weak self] in self?.needsLayout = true },
                                         splitPreview: { [weak self] axis in self?.showSplitPreview(paneID: pane.id, axis: axis) })
            if let host = hosts[pane.id] { host.rootView = root }
            else {
                let host = WorkspacePaneHostingView(rootView: root)
                if #available(macOS 13.3, *) { host.safeAreaRegions = [] }
                hosts[pane.id] = host
                if let frame = tab.visibleWorkspaceLayout?.geometry(
                    in: bounds.insetBy(dx: CGFloat(opts.gap)/2, dy: CGFloat(opts.gap)/2), minimum: minimum).panes[pane.id],
                   frame.width > 0, frame.height > 0 { host.frame = frame }
                else { host.frame = CGRect(origin: .zero, size: minimum(pane.id)) }
                addSubview(host)
            }
        }
        needsLayout = true
        claimFilePaneFocus()
    }
    /// 当前窗格是文件时把键盘交给它的列表。
    ///
    /// 首帧的列表由 SwiftUI 在下一轮 runloop 才建出来，同步查不到；此时
    /// 再补一次（不轮询：补完仍没有列表，就说明这格确实没有可聚焦的列表）。
    private func claimFilePaneFocus() {
        guard let host = focusedFileHost(window: window) else { return }
        if let list = fileResponder(in: host) {
            window?.makeFirstResponder(list)
            return
        }
        DispatchQueue.main.async { [weak self, weak host] in
            guard let self, let host, host === self.focusedFileHost(window: host.window) else { return }
            host.layoutSubtreeIfNeeded()
            guard self.focusedFileHost(window: host.window) === host,
                  let list = self.fileResponder(in: host) else { return }
            host.window?.makeFirstResponder(list)
        }
    }
    /// 窗口自身（不是 NSView）也可能是 firstResponder，纯文件标签页
    /// 首次显示就是这一档；此时同样要把键盘交给列表。
    private func focusedFileHost(window: NSWindow?) -> NSHostingView<WorkspacePaneView>? {
        guard isActive, let window, window.isKeyWindow, !(window.firstResponder is PaneActionButton), let tab,
              let id = tab.activeWorkspacePaneID, tab.activeWorkspacePane?.files != nil,
              let host = hosts[id],
              (window.firstResponder as? NSView)?.isDescendant(of: host) != true
        else { return nil }
        return host
    }
    private func fileResponder(in view: NSView) -> NSView? {
        if view is NSTableView || view is NSOutlineView { return view }
        return view.subviews.lazy.compactMap(fileResponder).first
    }
    /// 文件窗格最小高度 = 标题条/拖动把手 + 实测头部 + 64pt 内容区。
    /// 头部随工具条折行与搜索栏显隐变化，不能写死（窄面板会挤掉内容区）。
    static let fileContentMinimum: CGFloat = 64
    /// 首帧还没测到头部时的保守值（旧版固定 210 的等价高度减去标题条与内容区）。
    static let fallbackFileChrome: CGFloat = 120
    func minimum(_ id: UUID) -> CGSize {
        let gap = CGFloat(opts.gap)
        if let file = tab?.workspacePanes.first(where: { $0.id == id })?.files {
            let title = opts.showsTitleBar ? PaneTitleBar.height : 6
            let chrome = file.chromeHeight ?? Self.fallbackFileChrome
            return CGSize(width: 260 + gap,
                          height: title + chrome + Self.fileContentMinimum + gap)
        }
        return CGSize(width: 120 + gap, height: 80 + gap)
    }
    override func layout() {
        super.layout()
        previewLayer.frame = bounds
        splitPreview.frame = bounds
        guard let tab, let tree = tab.visibleWorkspaceLayout, bounds.width > 0, bounds.height > 0 else { splitPreview.clear(); return }
        let inset = CGFloat(opts.gap) / 2
        let rect = bounds.insetBy(dx: inset, dy: inset)
        let geometry = tree.geometry(in: rect, minimum: minimum)
        let visible = Set(tree.paneIDs)
        if tab.zoomedPaneID == nil { normalFrames = geometry.panes }
        for (id, host) in hosts {
            host.isHidden = !visible.contains(id)
            host.alphaValue = tab.zoomedPaneID != nil && tab.zoomedPaneID != id ? 0 : 1
            if let frame = normalFrames[id] ?? geometry.panes[id], frame.width > 0, frame.height > 0 {
                host.frame = frame
            }
        }
        if let zoom = tab.zoomedPaneID, let host = hosts[zoom] {
            host.frame = rect
            addSubview(host, positioned: .above, relativeTo: nil)
        }
        dividerGeometry = tab.zoomedPaneID == nil ? geometry.dividers : []
        while dividers.count < dividerGeometry.count {
            let view = WorkspaceDividerView(); dividers.append(view); addSubview(view)
        }
        for (i, view) in dividers.enumerated() {
            view.isHidden = i >= dividerGeometry.count
            guard i < dividerGeometry.count else { continue }
            view.frame = dividerGeometry[i].rect
            view.axis = dividerGeometry[i].axis
            view.onDrag = { [weak self] point in self?.resizeDivider(i, at: point) }
            addSubview(view, positioned: .above, relativeTo: nil)
        }
        addSubview(splitPreview, positioned: .above, relativeTo: nil)
        if let id = splitPreview.paneID {
            if let content = previewContent(for: id) { splitPreview.update(content: content) }
            else { splitPreview.clear() }
        }
        let min = tree.minimumSize(minimum)
        if let window, window.contentView != nil, !isHidden,
           tab.id == (hosts.values.first?.rootView.model.tabs.activeTabID) {
            // 标题带和侧栏也占空间；不把窗口压到树的最小尺寸以下。
            let ownRect = convert(bounds, to: nil)
            let required = NSSize(width: max(720, min.width + CGFloat(opts.gap) + max(0, window.contentLayoutRect.width - ownRect.width)),
                                  height: max(420, min.height + CGFloat(opts.gap) + max(0, window.contentLayoutRect.height - ownRect.height)))
            window.contentMinSize = required
            if let content = window.contentView, content.bounds.width < required.width || content.bounds.height < required.height {
                window.setContentSize(NSSize(width: max(content.bounds.width, required.width), height: max(content.bounds.height, required.height)))
            }
        }
    }
    func showSplitPreview(paneID: UUID, axis: PaneLayout.Axis?) {
        guard let axis else {
            if splitPreview.paneID == paneID { splitPreview.clear() }
            return
        }
        guard isActive, opts.showsTitleBar, window?.isKeyWindow == true, tab?.isLocked == false, !isDraggingPane,
              preview == nil, let content = previewContent(for: paneID) else { splitPreview.clear(); return }
        splitPreview.show(paneID: paneID, axis: axis, content: content)
    }
    private func previewContent(for id: UUID) -> CGRect? {
        guard let tab, tab.visibleWorkspaceLayout?.paneIDs.contains(id) == true,
              tab.zoomedPaneID == nil || tab.zoomedPaneID == id,
              let host = hosts[id], !host.isHidden else { return nil }
        let card = host.frame.insetBy(dx: CGFloat(opts.gap) / 2, dy: CGFloat(opts.gap) / 2)
        return CGRect(x: card.minX, y: card.minY + PaneTitleBar.height,
                      width: card.width, height: max(0, card.height - PaneTitleBar.height))
    }
    var dividerViews: [NSView] { dividers.filter { !$0.isHidden } }
    func setPosition(_ position: CGFloat, ofDividerAt index: Int) {
        guard index < dividerGeometry.count else { return }
        let d = dividerGeometry[index]
        let point = d.axis == .horizontal
            ? NSPoint(x: d.container.minX + position, y: d.container.midY)
            : NSPoint(x: d.container.midX, y: d.container.minY + position)
        resizeDivider(index, at: point)
    }
    private func resizeDivider(_ index: Int, at point: NSPoint) {
        guard index < dividerGeometry.count, let tab else { return }
        let d = dividerGeometry[index]
        let pos = d.axis == .horizontal ? point.x - d.container.minX : point.y - d.container.minY
        var lengths = d.lengths
        let start = lengths.prefix(d.index).reduce(0, +)
        let pair = lengths[d.index] + lengths[d.index + 1]
        lengths[d.index] = min(max(pos - start, d.minimums[d.index]), pair - d.minimums[d.index + 1])
        lengths[d.index + 1] = pair - lengths[d.index]
        tab.workspaceLayout = tab.workspaceLayout?.settingWeights(node: d.node, weights: lengths.map(Double.init))
        tab.layoutCustomized = true
    }
    /// 预览状态（测试与 `draggingUpdated` 共用同一条路径）。
    var hasDropPreview: Bool { preview != nil }
    func clearDropPreview() {
        preview = nil
        previewLayer.path = nil
    }
    /// 给定拖动载荷与工作区坐标，更新高亮。返回是否是可停靠的落点。
    @discardableResult
    func updateDropPreview(_ drag: WorkspacePaneDrag, at point: NSPoint) -> Bool {
        guard let value = drop(drag, at: point) else { clearDropPreview(); return false }
        splitPreview.clear()
        preview = value
        previewLayer.path = CGPath(roundedRect: value.3, cornerWidth: 8, cornerHeight: 8,
                                   transform: nil)
        return true
    }
    private func paneDrag(from sender: NSDraggingInfo) -> WorkspacePaneDrag? {
        guard let data = sender.draggingPasteboard.data(forType: .init(workspacePaneType)),
              let drag = try? JSONDecoder().decode(WorkspacePaneDrag.self, from: data),
              drag.tabID == tab?.id,
              tab?.workspacePanes.contains(where: { $0.id == drag.paneID }) == true else { return nil }
        return drag
    }
    private func drop(_ drag: WorkspacePaneDrag, at point: NSPoint)
        -> (WorkspacePaneDrag, UUID, WorkspaceLayout.Edge, CGRect)? {
        guard let tab, drag.tabID == tab.id,
              tab.workspacePanes.contains(where: { $0.id == drag.paneID }) else { return nil }
        let frames = normalFrames
            .filter { $0.key != drag.paneID && hosts[$0.key]?.isHidden == false }
            .map { ($0.key, $0.value) }
        guard let hit = WorkspaceDockingGeometry.resolve(point: point, frames: frames,
                                                         excluding: drag.paneID),
              let next = tab.workspaceLayout?.docking(source: drag.paneID, onto: hit.target,
                                                      edge: hit.edge) else { return nil }
        let size = next.minimumSize(minimum)
        let available = bounds.insetBy(dx: CGFloat(opts.gap)/2, dy: CGFloat(opts.gap)/2)
        guard size.width <= available.width, size.height <= available.height else { return nil }
        return (drag, hit.target, hit.edge, hit.highlight)
    }
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { draggingUpdated(sender) }
    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        guard let drag = paneDrag(from: sender) else { clearDropPreview(); return [] }
        let point = convert(sender.draggingLocation, from: nil)
        return updateDropPreview(drag, at: point) ? .move : []
    }
    override func draggingExited(_ sender: NSDraggingInfo?) { clearDropPreview() }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        defer { clearDropPreview() }
        guard let drag = paneDrag(from: sender), let value = drop(drag, at: convert(sender.draggingLocation, from: nil)),
              let tab else { return false }
        return tab.dock(value.0, onto: value.1, edge: value.2,
                        available: bounds.insetBy(dx: CGFloat(opts.gap)/2, dy: CGFloat(opts.gap)/2).size,
                        minimum: minimum)
    }
}

@MainActor private final class WorkspaceDividerView: NSView {
    var axis: PaneLayout.Axis = .horizontal
    var onDrag: ((NSPoint) -> Void)?
    /// 分隔条上沿可能进系统标题栏带（标题栏关闭形态 / gap=0）：拒绝窗口
    /// 拖动劫持，否则带内那截拖分隔条会变成移窗（mouseDown 的 nextEvent
    /// 循环根本收不到 dragged 事件）。
    override var mouseDownCanMoveWindow: Bool { false }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = nil
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) 未实现") }
    override func resetCursorRects() {
        addCursorRect(bounds, cursor: axis == .horizontal ? .resizeLeftRight : .resizeUpDown)
    }
    override func mouseDown(with event: NSEvent) {
        while let next = window?.nextEvent(matching: [.leftMouseDragged, .leftMouseUp]) {
            if next.type == .leftMouseUp { break }
            onDrag?(superview?.convert(next.locationInWindow, from: nil) ?? .zero)
        }
    }
}

struct WorkspacePaneView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var tab: TerminalTab
    let pane: WorkspacePane
    let isFocused: Bool
    let isVisible: Bool
    let opts: PaneChromeOptions
    /// 文件窗格头部实测高度变化后，让 AppKit 宿主重算最小尺寸与 frame。
    var fileChromeChanged: () -> Void = {}
    var splitPreview: (PaneLayout.Axis?) -> Void = { _ in }
    @State private var hovering = false
    var body: some View {
        Group {
            switch pane {
            case .terminal(let terminal):
                PaneView(model: model, tab: tab, pane: terminal, isFocused: isFocused,
                         isVisible: isVisible, dim: model.splitDim, opts: opts,
                         showsActions: hovering, preview: splitPreview)
            case .files(let file):
                WorkspaceFileView(model: model, tab: tab, file: file, isFocused: isFocused, opts: opts,
                                  chromeChanged: fileChromeChanged, isVisible: isVisible, showsActions: hovering, preview: splitPreview)
            }
        }
        .background(HoverTracking(hovering: $hovering, identifier: "pane-hover-\(pane.id)"))
        .onDisappear { splitPreview(nil) }
    }
}
