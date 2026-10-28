// AppKit window tabs and the application's tab model share one source of truth.
import AppKit
import Combine
import GhostermAppModel

@MainActor
final class NativeTabWindows: NSObject, NSWindowDelegate {
    private let model: AppModel
    private var anchor: NSWindow
    private var windows: [UUID: NSWindow] = [:]
    private var titles: [UUID: AnyCancellable] = [:]
    private var subscriptions: Set<AnyCancellable> = []
    private var observedGroup: NSWindowTabGroup?
    private var selectionObservation: NSKeyValueObservation?
    private var orderObservation: NSKeyValueObservation?
    private var syncing = false

    init(model: AppModel, anchor: NSWindow) {
        self.model = model
        self.anchor = anchor
        super.init()
        anchor.tabbingMode = .preferred
        anchor.delegate = self
        // @Published emits before the property changes. Reconcile on the next
        // main-queue turn, after both the list and active ID have settled.
        model.tabs.$tabs.merge(with: model.tabs.$activeTabID.map { _ in [] })
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.reconcile() }
            .store(in: &subscriptions)
        reconcile()
    }

    private func configure(_ window: NSWindow, tab: TerminalTab) {
        if let native = window as? NativeTitlebarWindow {
            native.appModel = model
            native.representedTabID = tab.id
        }
        window.tabbingMode = .preferred
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.contentView = makeRootHostingView(model: model, windowTabID: tab.id, nativeTabs: true)
        WindowChrome.apply(to: window)
        window.title = tab.title
        titles[tab.id] = tab.$title.receive(on: DispatchQueue.main).sink { [weak window] title in
            window?.title = title
        }
    }

    private func reconcile() {
        guard !syncing else { return }
        syncing = true
        defer { syncing = false }
        let tabs = model.tabs.tabs
        let ids = Set(tabs.map(\.id))

        // Keep one window alive as the empty workspace when the last tab closes.
        // If another tab survives, its window becomes the anchor instead.
        let survivor = windows.first(where: { ids.contains($0.key) })?.value
        if let survivor {
            anchor = survivor
        }
        for (id, window) in windows where !ids.contains(id) {
            titles[id] = nil
            windows[id] = nil
            if survivor == nil && window === anchor {
                window.contentView = makeRootHostingView(model: model, nativeTabs: true)
                window.title = ""
            } else {
                window.delegate = nil
                window.close()
            }
        }

        for tab in tabs where windows[tab.id] == nil {
            let window: NSWindow
            if !windows.values.contains(where: { $0 === anchor }) {
                window = anchor
            } else {
                window = NativeTitlebarWindow(contentRect: anchor.frame,
                                  styleMask: anchor.styleMask,
                                  backing: .buffered, defer: false)
                configure(window, tab: tab)
                anchor.addTabbedWindow(window, ordered: .above)
                observeSelection(anchor.tabGroup)
            }
            if window === anchor { configure(window, tab: tab) }
            windows[tab.id] = window
        }
        if let group = anchor.tabGroup {
            for (index, tab) in tabs.enumerated() {
                guard let window = windows[tab.id], group.windows.indices.contains(index) else { continue }
                if group.windows[index] !== window { group.insertWindow(window, at: index) }
            }
        }
        if let id = model.tabs.activeTabID, let window = windows[id],
           window.tabGroup?.selectedWindow !== window {
            window.tabGroup?.selectedWindow = window
        }
        if let id = model.tabs.activeTabID, let window = windows[id] as? NativeTitlebarWindow {
            window.refreshNativeTabs()
        }
    }

    private func observeSelection(_ group: NSWindowTabGroup?) {
        guard observedGroup !== group else { return }
        selectionObservation = nil
        orderObservation = nil
        observedGroup = group
        selectionObservation = group?.observe(\.selectedWindow, options: [.new]) { [weak self, weak group] _, _ in
            // AppKit sends KVO synchronously while we build/select the group.
            // Those changes came from the model and must not be replayed later
            // as if the user had selected the previous window.
            if Thread.isMainThread && MainActor.assumeIsolated({ self?.syncing ?? true }) { return }
            DispatchQueue.main.async {
                guard let self, let group, self.observedGroup === group,
                      let window = group.selectedWindow else { return }
                self.activateWindow(window)
            }
        }
        orderObservation = group?.observe(\.windows, options: [.new]) { [weak self, weak group] _, _ in
            if Thread.isMainThread && MainActor.assumeIsolated({ self?.syncing ?? true }) { return }
            DispatchQueue.main.async {
                guard let self, let group, self.observedGroup === group else { return }
                self.acceptWindowOrder(group.windows)
            }
        }
    }

    private func acceptWindowOrder(_ orderedWindows: [NSWindow]) {
        guard !syncing, orderedWindows.count == model.tabs.tabs.count else { return }
        let ids = orderedWindows.compactMap { window in
            windows.first(where: { $0.value === window })?.key
        }
        guard ids.count == orderedWindows.count, Set(ids) == Set(model.tabs.tabs.map(\.id)) else { return }
        for (index, id) in ids.enumerated() {
            guard let tab = model.tabs.tabs.first(where: { $0.id == id }) else { continue }
            model.tabs.move(tab, to: index)
        }
    }

    private func activateWindow(_ window: NSWindow) {
        guard !syncing, let id = windows.first(where: { $0.value === window })?.key,
              let tab = model.tabs.tabs.first(where: { $0.id == id }),
              model.tabs.activeTabID != id else { return }
        model.tabs.activate(tab)
    }

    func windowDidBecomeKey(_ notification: Notification) {
        guard let window = notification.object as? NSWindow else { return }
        activateWindow(window)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard let id = windows.first(where: { $0.value === sender })?.key,
              let tab = model.tabs.tabs.first(where: { $0.id == id }) else { return true }
        model.tabs.requestClose(tab)
        return false
    }

    func windowDidResize(_ notification: Notification) {
        guard let window = notification.object as? NSWindow else { return }
        let title = window.title
        WindowChrome.redress(window)
        window.title = title
    }
}
