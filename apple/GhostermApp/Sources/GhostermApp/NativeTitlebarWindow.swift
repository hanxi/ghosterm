import AppKit
import GhostermAppModel
import ObjectiveC

private func appKitTabItem(_ object: AnyObject, ivar name: String) -> AnyObject? {
    guard let cls = object_getClass(object), let ivar = class_getInstanceVariable(cls, name),
          let encoding = ivar_getTypeEncoding(ivar), encoding.pointee == 64 else { return nil }
    return object_getIvar(object, ivar) as AnyObject?
}

/// Places AppKit's native tab bar in the titlebar alongside the window controls.
@MainActor
final class NativeTitlebarWindow: NSWindow, NSToolbarDelegate {
    private var tabConstraints: [NSLayoutConstraint] = []
    private weak var constrainedClip: NSView?
    private var closeRouters: [NativeTabCloseRouter] = []
    private var menuDelegates: [NativeTabContextMenu] = []
    private var contextMonitor: Any?
    weak var appModel: AppModel?
    var representedTabID: UUID?

    override init(contentRect: NSRect, styleMask: NSWindow.StyleMask,
                  backing bufferingType: NSWindow.BackingStoreType, defer flag: Bool) {
        super.init(contentRect: contentRect, styleMask: styleMask, backing: bufferingType, defer: flag)
        let toolbar = NSToolbar(identifier: "GhostermNativeTabs")
        toolbar.delegate = self
        self.toolbar = toolbar
        toolbarStyle = .unifiedCompact
        contextMonitor = NSEvent.addLocalMonitorForEvents(matching: .rightMouseDown) { [weak self] event in
            guard let self, let (menu, view, point) = self.nativeContextMenu(for: event) else { return event }
            menu.popUp(positioning: nil, at: point, in: view)
            return nil
        }
    }

    deinit { if let contextMonitor { NSEvent.removeMonitor(contextMonitor) } }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { [.flexibleSpace] }
    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { [.flexibleSpace] }

    override func addTitlebarAccessoryViewController(_ child: NSTitlebarAccessoryViewController) {
        if child.identifier == nil,
           descendant(child.view, named: "NSTabBar") != nil ||
            (child.layoutAttribute == .bottom && child.view.subviews.isEmpty) {
            child.layoutAttribute = .right
        }
        super.addTitlebarAccessoryViewController(child)
        scheduleTabLayout()
    }

    override func becomeMain() {
        super.becomeMain()
        clearTabLayout()
        refreshNativeTabs()
    }

    func refreshNativeTabs() {
        scheduleTabLayout()
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(100)) { [weak self] in
            self?.layoutNativeTabs()
        }
    }

    override func removeTitlebarAccessoryViewController(at index: Int) {
        super.removeTitlebarAccessoryViewController(at: index)
        clearTabLayout()
        scheduleTabLayout()
    }

    private func scheduleTabLayout() {
        DispatchQueue.main.async { [weak self] in self?.layoutNativeTabs() }
    }

    private func clearTabLayout() {
        NSLayoutConstraint.deactivate(tabConstraints)
        tabConstraints.removeAll()
        constrainedClip = nil
    }

    private func layoutNativeTabs() {
        guard let frame = contentView?.superview,
              let tabBar = descendant(frame, named: "NSTabBar"),
              let toolbarView = descendant(frame, named: "NSToolbarView") else { return }
        var parent = tabBar.superview
        while let view = parent, !["NSTitlebarAccessoryClipView", "NSTitlebarAccessoryContainerView"].contains(String(describing: type(of: view))) {
            parent = view.superview
        }
        guard let clip = parent, let accessory = tabBar.superview else { return }
        if constrainedClip === clip {
            wireTabCloseButtons(tabBar)
            return
        }
        clearTabLayout()
        constrainedClip = clip
        clip.translatesAutoresizingMaskIntoConstraints = false
        accessory.translatesAutoresizingMaskIntoConstraints = false
        tabConstraints = [
            // Reserve space for the toggle, new-tab menu and optional warning.
            clip.leftAnchor.constraint(equalTo: toolbarView.leftAnchor,
                                       constant: WindowChrome.trafficLightsWidth + 3 * DS.controlSize + 5 * DS.Padding.s),
            clip.rightAnchor.constraint(equalTo: toolbarView.rightAnchor),
            clip.topAnchor.constraint(equalTo: toolbarView.topAnchor),
            clip.heightAnchor.constraint(equalTo: toolbarView.heightAnchor),
            accessory.leftAnchor.constraint(equalTo: clip.leftAnchor),
            accessory.rightAnchor.constraint(equalTo: clip.rightAnchor),
            accessory.topAnchor.constraint(equalTo: clip.topAnchor),
            accessory.heightAnchor.constraint(equalTo: clip.heightAnchor),
        ]
        NSLayoutConstraint.activate(tabConstraints)
        wireTabCloseButtons(tabBar)
    }

    private func wireTabCloseButtons(_ tabBar: NSView) {
        guard tabGroup != nil else { return }
        func collect(_ view: NSView, named name: String) -> [NSView] {
            (String(describing: type(of: view)) == name ? [view] : []) +
                view.subviews.flatMap { collect($0, named: name) }
        }
        let tabs = collect(tabBar, named: "NSTabButton").sorted {
            $0.convert($0.bounds, to: tabBar).minX < $1.convert($1.bounds, to: tabBar).minX
        }
        var routers: [NativeTabCloseRouter] = []
        var delegates: [NativeTabContextMenu] = []
        // AppKit may animate the visual order after `group.windows` changes.
        // Pair the system's backing items by identity, never by title or position.
        func closeButton(_ view: NSView) -> NSButton? {
            if let button = view as? NSButton { return button }
            return view.subviews.lazy.compactMap { closeButton($0) }.first
        }
        var bindings: [(NSButton, NSView)] = []
        for tab in tabs {
            guard let button = tab.subviews.lazy.compactMap({ closeButton($0) }).first else { return }
            bindings.append((button, tab))
        }
        for (button, tab) in bindings {
            let router = NativeTabCloseRouter(owner: self, tab: tab)
            button.target = router
            button.action = #selector(NativeTabCloseRouter.close(_:))
            routers.append(router)
            if let model = appModel {
                let delegate = NativeTabContextMenu(owner: self, view: tab, model: model)
                let menu = NSMenu()
                menu.autoenablesItems = false
                menu.delegate = delegate
                tab.menu = menu
                delegates.append(delegate)
            }
        }
        closeRouters = routers
        menuDelegates = delegates
    }

    func tabID(for tabView: NSView) -> UUID? {
        guard let item = appKitTabItem(tabView, ivar: "_tabBarViewItem") else { return nil }
        return tabGroup?.windows.first(where: {
            appKitTabItem($0.tab, ivar: "_tabBarItem") === item
        }).flatMap { ($0 as? NativeTitlebarWindow)?.representedTabID }
    }

    func nativeContextMenu(for event: NSEvent) -> (NSMenu, NSView, NSPoint)? {
        guard let frame = contentView?.superview,
              let bar = descendant(frame, named: "NSTabBar"), event.window === bar.window,
              tabGroup?.selectedWindow === self else { return nil }
        func findTab(_ view: NSView) -> NSView? {
            if String(describing: type(of: view)) == "NSTabButton",
               view.bounds.contains(view.convert(event.locationInWindow, from: nil)) { return view }
            return view.subviews.lazy.compactMap { findTab($0) }.first
        }
        guard let tab = findTab(bar), let menu = tab.menu else { return nil }
        return (menu, tab, tab.convert(event.locationInWindow, from: nil))
    }

    private func descendant(_ root: NSView, named name: String) -> NSView? {
        if String(describing: type(of: root)) == name { return root }
        return root.subviews.lazy.compactMap { self.descendant($0, named: name) }.first
    }
}

@MainActor
private final class NativeTabCloseRouter: NSObject {
    private weak var owner: NSWindow?
    private weak var tab: NSView?
    init(owner: NSWindow, tab: NSView) { self.owner = owner; self.tab = tab }
    @objc func close(_ sender: Any?) {
        guard let owner, let tab, let item = appKitTabItem(tab, ivar: "_tabBarViewItem"),
              let window = owner.tabGroup?.windows.first(where: {
                  appKitTabItem($0.tab, ivar: "_tabBarItem") === item
              }) else {
            if let owner = self.owner {
                let alert = NSAlert()
                alert.messageText = "无法关闭标签页"
                alert.informativeText = "无法确定关闭按钮所属的标签页，请使用侧边栏或菜单关闭。"
                alert.beginSheetModal(for: owner)
            } else {
                NSSound.beep()
            }
            return
        }
        window.performClose(sender)
    }
}
