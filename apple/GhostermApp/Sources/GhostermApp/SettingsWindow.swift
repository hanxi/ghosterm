// SettingsWindow — 设置**窗口**（不是 sheet）。
//
// 取法参照 iGhostVT 的 Mac 设置（`SettingsSceneDelegate` +
// `SettingsContainerController`）：窗口**顶部一排带图标的页签**，高度
// **跟着当前页的内容走**，所以既不需要右下角的「完成」按钮，也不会留
// 一大片空白。
//
// 为什么不用 `.sheet`：sheet 的高度由呈现它的窗口决定，内容变高变矮时
// 那个窗口自己不跟着动（实测：先渲染 96pt 的短内容，切到长内容后 sheet
// 仍是 96pt 高，内容被压扁）。设置是「打开—看着调—关掉」的常驻面板，
// 用真窗口更贴 macOS 的惯例。
//
// **窗口高度必须我们自己去改**（踩过的坑）：把 `NSHostingView` 直接设成
// `window.contentView`、再给它 `sizingOptions = .preferredContentSize`，
// **窗口不会跟着变**（实测：短页 / 长页窗口都是同一个高度）。于是内容
// 被**垂直居中**在那个固定高度里——每一页内容多高，页签条离窗口顶就多远。
// 用户看到的就是「切页时页签图标的位置一直在变」。
// 正确做法（也就是这里做的）：容器控制器把子视图**钉在顶部**，
// 高度由我们自己按内容算，再连同「保持顶边不动」一起设给窗口。
import AppKit
import GhostermAppModel
import SwiftUI

@MainActor
enum SettingsWindow {
    /// 当前那张设置窗口（只允许一张；再开就把它带到前面）。
    /// 强引用：`isReleasedWhenClosed = false`，关掉后再 ⌘, 复用同一个。
    /// 非 private：`SettingsUITests` 要拿它量「内容有没有贴顶」。
    private(set) static var controller: SettingsWindowController?

    /// 开设置窗口，或把已有的带到前面（并切到 `pane` 那一页）。
    static func open(model: AppModel, pane: SettingsView.Pane = .general) {
        if let controller {
            controller.show(pane: pane)
            if let window = controller.window { WindowPresentation.present(window) }
            return
        }
        let made = SettingsWindowController(model: model, pane: pane)
        controller = made
        WindowPresentation.prepare(made.window)
        made.showWindow(nil)
    }

    static func close() { controller?.close() }
}

/// 设置窗口。只干两件事：装上内容、把窗口尺寸**贴着内容**改。
@MainActor
final class SettingsWindowController: NSWindowController {
    let selection: SettingsSelection
    private let container: SettingsContainerController

    init(model: AppModel, pane: SettingsView.Pane) {
        self.selection = SettingsSelection(pane: pane)
        self.container = SettingsContainerController(model: model, selection: selection)
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: DS.settingsWidth, height: 480),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered, defer: false
        )
        window.title = "设置"
        // 关掉后再 ⌘, 要能复用同一张窗口（`open` 里靠 controller 是否存活判断）。
        window.isReleasedWhenClosed = false
        super.init(window: window)
        // 先挂回调再设 contentViewController：`viewDidLoad` 里就会量一次高度。
        container.onSizeChange = { [weak self] in self?.fit() }
        window.contentViewController = container
        window.center()
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) 未实现") }

    /// 切到某一页（幂等：`SettingsSelection` 自己去重）。
    func show(pane: SettingsView.Pane) {
        selection.pane = pane
        fit()
    }

    // MARK: - 贴内容改尺寸

    /// 一次等待下一轮主队列的贴尺寸。
    private var isFitPending = false

    /// 把窗口改成「内容的高度」，**顶边不动**（往下长，而不是从中间张开）。
    ///
    /// 两条纪律：
    ///   · 绝不在布局过程中改窗口——`onSizeChange` 就是从布局里来的，
    ///     在 AppKit 自己的布局里改窗口会再次布局，AppKit 直接抛循环错误。
    ///     所以推迟到下一轮主队列，且只做一次。
    ///   · 最小/最大**同时**设成目标尺寸：设置窗口就是「所见即所得」的大小，
    ///     不许拖动改（iGhostVT 的 `sizeRestrictions` 同理）。
    private func fit() {
        guard !isFitPending else { return }
        isFitPending = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.isFitPending = false
            self.applyFit()
        }
    }

    private func applyFit() {
        guard let window, let height = container.paneHeight, height > 0 else { return }
        let size = NSSize(width: DS.settingsWidth, height: height)
        let topEdge = window.frame.maxY

        // 两个边界要**先各自让开**，否则 setContentSize 会被旧的上限夹住
        // （iGhostVT 的 `applyFit()` 就是先让路再设值）。
        if size.height > window.contentMaxSize.height {
            window.contentMaxSize = size
            window.contentMinSize = size
        } else {
            window.contentMinSize = size
            window.contentMaxSize = size
        }
        window.setContentSize(size)
        // 顶边钉住：高度变了只往下（或往上）长，窗口不会在屏幕上跳。
        window.setFrameOrigin(NSPoint(x: window.frame.origin.x,
                                      y: topEdge - window.frame.height))
    }
}

/// 设置窗口的内容容器：把 SwiftUI 那一页**钉在顶部**，并按内容量高度。
///
/// 钉顶是这件事的关键——把 hosting view 用 Auto Layout 顶到容器上边，
/// 它就不会被居中；高度写进一个约束，由 `preferredContentSize` 回调更新。
/// （直接把它当 `window.contentView` 就没有这层锚点，于是被居中——见文件头。）
@MainActor
final class SettingsContainerController: NSViewController {
    /// 每一页都按这个宽度排版（窗口宽度也钉在它上）。
    static let width = DS.settingsWidth

    /// 本页的高度或页签的高度变了；窗口该跟着改。
    var onSizeChange: (() -> Void)?

    /// 当前页的高度（还没量出来时为 nil）。
    private(set) var paneHeight: CGFloat?

    private let model: AppModel
    private let selection: SettingsSelection
    private var host: NSHostingController<SettingsView>?
    private var heightConstraint: NSLayoutConstraint?

    init(model: AppModel, selection: SettingsSelection) {
        self.model = model
        self.selection = selection
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) 未实现") }

    override func loadView() { view = SettingsBackgroundView() }

    override func viewDidLoad() {
        super.viewDidLoad()
        // 窗口自己的底色（不要用 iOS 的分组灰，那会让设置窗口比同机其它
        // 应用的设置面板暗一档）。**底色由 `SettingsBackgroundView` 在绘制时
        // 解，不要在这里 `NSColor.windowBackgroundColor.cgColor`**——见那个类。

        let host = NSHostingController(
            rootView: SettingsView(model: model, selection: selection))
        // 让 hosting 控制器把内容的理想高度报出来（macOS 13+）。
        host.sizingOptions = .preferredContentSize
        addChild(host)
        host.view.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(host.view)
        // 高度先给 0，等量出来再写实——**顶边**是与容器对齐的，
        // 所以窗口比内容高的那段时间内容是**贴着顶**的，不会被居中。
        let height = host.view.heightAnchor.constraint(equalToConstant: 0)
        NSLayoutConstraint.activate([
            host.view.topAnchor.constraint(equalTo: view.topAnchor),
            host.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            host.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            height,
        ])
        self.host = host
        self.heightConstraint = height
        // 先按当前能拿到的理想高度量一次；内容稳定后 UIKit 还会再报一次。
        update(height: host.view.fittingSize.height)
    }

    /// 子控制器的理想尺寸变了（macOS 上这个回调在 `NSViewController` 里叫
    /// `preferredContentSizeDidChange(for:)`）。
    override func preferredContentSizeDidChange(for childContentContainer: NSViewController) {
        super.preferredContentSizeDidChange(for: childContentContainer)
        guard childContentContainer === host else { return }
        update(height: childContentContainer.preferredContentSize.height)
    }

    /// 布局完之后再量一次：`preferredContentSizeDidChange` 是**子控制器主动
    /// 通知**时才来的，而 SwiftUI 的 hosting 控制器并不保证每次都通知
    /// （实测：窗口一直停在初始高度）。布局后的 frame 是**已经算好的事实**，
    /// 拿它当兜底最稳。
    override func viewDidLayout() {
        super.viewDidLayout()
        guard let host else { return }
        update(height: host.view.frame.height)
    }

    private func update(height: CGFloat) {
        let h = height.rounded(.up)
        guard h > 0, h != paneHeight else { return }
        paneHeight = h
        heightConstraint?.constant = h
        onSizeChange?()
    }
}

// MARK: - 设置窗口的底色

/// 设置窗口容器的底色。**必须在绘制时解 `NSColor`、且跟着外观重画**。
///
/// 踩过的坑（用户实测「重启后设置界面看不清了」）：早先在这里写
/// `view.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor`。
/// 动态 `NSColor` 的 `.cgColor` 是按 **`NSAppearance.current`** 解的——那是
/// 「当前绘制上下文」的外观，**不在一次绘制里时它并不等于窗口的
/// `effectiveAppearance`**（本机系统浅色、用户选深色时，实测
/// `.cgColor` 解出的是**白色**）。于是窗口底是白的，而 SwiftUI 的文字按
/// 窗口的深色外观渲染成浅色 —— 白底浅字，什么都看不清。
///
/// 正确做法是让 AppKit 在**绘制期间**（此时 `NSAppearance.current` 就是
/// 本视图的 `effectiveAppearance`）来问颜色：`wantsUpdateLayer` +
/// `updateLayer`。再在 `viewDidChangeEffectiveAppearance` 里标脏，外观
/// 一变（用户在设置里切换 / 系统切换）就重画。断言见
/// `SettingsUITests.testSettingsBackgroundTracksAppearance`。
final class SettingsBackgroundView: NSView {
    override var wantsUpdateLayer: Bool { true }

    override func updateLayer() {
        layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        // 外观变了（切换偏好 / 系统切换）→ 重画底色。
        needsDisplay = true
    }
}
