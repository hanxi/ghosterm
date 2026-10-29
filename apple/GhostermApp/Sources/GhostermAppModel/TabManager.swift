// TabManager — 一个窗口的标签页清单（结构的唯一写入者）。
//
// 模仿 iGhostVT 的 TabManager：标签页的**增删改一律经这里**，视图只借阅
// （`@ObservedObject`），不允许自己改 tabs 数组。这样「关一个标签页」在
// 侧边栏、标签条、终端区三处看到的是同一个事件，不会各猜各的。
//
// 与 iGhostVT 的差别（诚实标注）：它把插入/删除包在 `withAnimation` 里让
// 各处动画一致；我们 v0 不做显式过渡动画（Metal 面 + NSViewRepresentable
// 的过渡容易画坏帧），先求行为正确，动画留作后续。
import Foundation
import SwiftUI

@MainActor
public final class TabManager: ObservableObject {
    @Published public private(set) var tabs: [TerminalTab] = []
    /// 当前显示的标签页。nil = 没有任何标签页（空态）。
    @Published public var activeTabID: UUID?

    /// 一个等待用户确认的关闭请求（关闭有东西会被打断的标签页时）。
    /// 由 UI 弹确认框；确认后调 `confirmClose()`。
    @Published public var closeRequest: TerminalTab?

    /// 关闭一个标签页**之前**的钩子。AppModel 用它取消该标签页的在途连接
    /// （工作线程可能正卡在弹凭据框上——不取消就会「标签页没了还弹口令框」）。
    /// 必须是「关之前」而不是「关之后」：关之后工作线程已经在弹框了。
    public var willClose: ((TerminalTab) -> Void)?

    public init() {}

    public var activeTab: TerminalTab? {
        guard let id = activeTabID else { return tabs.first }
        return tabs.first { $0.id == id } ?? tabs.first
    }

    public var activeIndex: Int? {
        guard let active = activeTab else { return nil }
        return tabs.firstIndex { $0.id == active.id }
    }

    /// 追加并激活（新建标签页）。
    public func add(_ tab: TerminalTab) {
        tabs.append(tab)
        activeTabID = tab.id
    }

    /// 在指定位置插入（拖拽重排 / 「移到新窗口」用）。
    public func insert(_ tab: TerminalTab, at index: Int) {
        let clamped = max(0, min(index, tabs.count))
        tabs.insert(tab, at: clamped)
    }

    public func activate(_ tab: TerminalTab) {
        guard tabs.contains(where: { $0.id == tab.id }) else { return }
        activeTabID = tab.id
    }

    /// 按索引激活（⌘1…⌘9）。
    public func activate(index: Int) {
        guard tabs.indices.contains(index) else { return }
        activeTabID = tabs[index].id
    }

    /// 按当前显示顺序循环切换，首尾相接；空清单不操作。
    public func cycleTab(backwards: Bool = false) {
        guard let index = activeIndex else { return }
        activate(index: (index + (backwards ? tabs.count - 1 : 1)) % tabs.count)
    }

    /// 激活相邻标签页（关闭后接管）。
    /// 关闭一个标签页后要激活谁：优先右边的，没有就左边的（Safari 习惯）。
    public func activateNeighbor(of tab: TerminalTab) {
        guard let idx = tabs.firstIndex(where: { $0.id == tab.id }) else { return }
        if tabs.count <= 1 { activeTabID = nil; return }
        let next = idx + 1 < tabs.count ? tabs[idx + 1] : tabs[idx - 1]
        activeTabID = next.id
    }

    /// 立即移除（不弹确认，不停止会话——调用方负责先 stop）。
    public func remove(_ tab: TerminalTab) {
        guard let idx = tabs.firstIndex(where: { $0.id == tab.id }) else { return }
        let wasActive = activeTabID == tab.id || (activeTabID == nil && idx == 0)
        tabs.remove(at: idx)
        if wasActive {
            if tabs.isEmpty { activeTabID = nil }
            else if idx < tabs.count { activeTabID = tabs[idx].id }
            else { activeTabID = tabs[tabs.count - 1].id }
        }
    }

    /// 请求关闭：**只在有东西会被打断时才留给 UI 确认**；否则直接关
    /// （空闲提示符 / 已退出 / 失败 / 连接中都没什么可丢——不拿弹框烦人）。
    ///
    /// 判据在 `TerminalTab.closeNeedsConfirmation`（铁律 #6：只有能证实
    /// 「没什么可丢」才不问，查不出来一律问）。
    public func requestClose(_ tab: TerminalTab) {
        if tab.closeNeedsConfirmation {
            closeRequest = tab
        } else {
            close(tab)
        }
    }

    /// 用户确认关闭：停会话 + 移除。返回被关掉的那个（UI 记录日志用）。
    @discardableResult
    public func confirmClose() -> TerminalTab? {
        guard let tab = closeRequest else { return nil }
        closeRequest = nil
        close(tab)
        return tab
    }

    public func cancelClose() { closeRequest = nil }

    /// 关闭一个标签页：**先停会话再移除**（顺序不能反——移除后没人撑住
    /// transport/surface，读循环会踩已释放内存）。
    public func close(_ tab: TerminalTab) {
        willClose?(tab)   // 取消在途连接（见 willClose 说明），必须在 stop 之前
        tab.stop()
        remove(tab)
    }

    /// 按索引关闭（⌘W 的默认目标 = 当前标签页）。
    public func closeActive() {
        guard let tab = activeTab else { return }
        close(tab)
    }

    /// 拖拽重排：把 `tab` 移到 `index`（index 是**移动后的最终位置**）。
    public func move(_ tab: TerminalTab, to index: Int) {
        guard let from = tabs.firstIndex(where: { $0.id == tab.id }) else { return }
        let to = max(0, min(index, tabs.count - 1))
        guard from != to else { return }
        let t = tabs.remove(at: from)
        // 摘掉之后数组短了一位，插入位置按**新**数组长度收敛——
        // 这里不做 from/to 的相对调整：`to` 已经是最终下标（拖到尾部
        // 就是插在末尾），调整反而会差一位。
        tabs.insert(t, at: min(to, tabs.count))
    }

    /// 有没有**任何一个**标签页关掉会丢东西——关窗口 / 退出前的判据
    /// （口径同单页的 `closeNeedsConfirmation`，只是问的是「整窗」）。
    public var hasRunningPrograms: Bool {
        tabs.contains { $0.closeNeedsConfirmation }
    }

    /// 全部停掉（窗口关闭 / 应用退出）。
    public func stopAll() {
        for tab in tabs { tab.stop() }
    }

    /// 关闭全部并清空（测试收尾用；生产路径走 stopAll + 窗口销毁）。
    public func removeAll() {
        for tab in tabs { tab.stop() }
        tabs.removeAll()
        activeTabID = nil
        closeRequest = nil
    }
}
