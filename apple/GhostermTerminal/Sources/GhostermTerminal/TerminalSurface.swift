// TerminalSurface — 字节流 ⇄ 屏幕。这是「接缝」的 Apple 实现。
//
// 唯一职责：
//   transport 读到的字节  → surface.processOutput（进 libghostty 网格）
//   终端编码的输入        → onInput 回调（由调用方写回 transport）
//   网格尺寸变化          → onResize 回调（由调用方转成 SSH window-change）
//
// 它**不**知道背后是 SSH 还是本地 shell（铁律 #1），也**不**搬运业务逻辑。
import AppKit
import CGhostty
import Foundation
import QuartzCore

public enum TerminalSurfaceError: Error, CustomStringConvertible {
    case appInit(String)
    case surfaceInit(String)

    public var description: String {
        switch self {
        case .appInit(let m): return "libghostty app 初始化失败: \(m)"
        case .surfaceInit(let m): return "libghostty surface 初始化失败: \(m)"
        }
    }
}

/// libghostty app 的薄包装。
///
/// **进程内单例**：`shared` 拿，别自己 new。
///
/// 为什么必须单例（实测教训，不是洁癖）：libghostty 的 app 释放会连带
/// 停掉它的全局状态（分配器、xev 后端、字体后端），而
/// `gt_ghostty_app_new` 里的一次性闸门保证 `ghostty_init` 只跑一次——
/// 于是「建 app → 释放 app → 再建 app」会拿到一个**没有重新初始化过全局
/// 状态**的 app。实测症状：测试进程里先后建几个 app，第二个之后的创建
/// 崩在 xev 的 mach 异步通知上（`panic: attempt to unwrap error:
/// MachMsgFailed`），而单独跑那组测试永远不复现。
///
/// 所以：一个进程一个 app，活到进程退出。配置变化走 `reloadConfig`
/// （libghostty 会遍历所有 surface 下发），不重建 app。
public final class TerminalApp {
    /// 进程内共享实例。首次访问时创建；失败时**抛错**（不缓存失败——
    /// 让调用方能重试，也避免把一次失败固化成永久状态）。
    public static func shared(configPath: String? = nil) throws -> TerminalApp {
        if let existing = sharedInstance { return existing }
        let made = try TerminalApp(configPath: configPath)
        sharedInstance = made
        return made
    }

    private static var sharedInstance: TerminalApp?

    /// 测试辅助：清掉共享实例（**只给进程结束前的收尾用**——清掉之后再建
    /// 会走进上面说的坑，所以生产路径没有这个入口）。
    public static func resetSharedForTesting() {
        guard ProcessInfo.processInfo.environment["GHOSTERM_ALLOW_APP_RESET"] != nil
        else { return }
        sharedInstance = nil
    }

    let raw: OpaquePointer
    private var eventLoop: Timer?
    /// `configPath` 指向 Ghostty 语法的配置文件（设置页产出的那份）。
    /// nil / 空 = 默认配置。配置无效时**抛错**（不静默回落成默认值——
    /// 用户改了字体却看到默认大小，比报错更难查）。
    ///
    /// 一般不要直接调它：用 `TerminalApp.shared(configPath:)`。
    public init(configPath: String? = nil) throws {
        let app: OpaquePointer?
        if let path = configPath, !path.isEmpty {
            app = path.withCString { gt_ghostty_app_new_with_config($0) }
        } else {
            app = gt_ghostty_app_new()
        }
        guard let app else {
            throw TerminalSurfaceError.appInit(Self.lastError())
        }
        self.raw = app
    }

    deinit { eventLoop?.invalidate(); gt_ghostty_app_free(raw) }

    /// Native host event loop. A shared engine is driven once, independently
    /// of how many windows or Transports consume it. Terminal bytes stay on IO.
    @MainActor public func startEventLoop() {
        guard eventLoop == nil else { return }
        let timer = Timer(timeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        eventLoop = timer; RunLoop.main.add(timer, forMode: .common)
    }

    /// 由宿主事件循环周期调用（libghostty 的 wakeup 语义）。
    public func tick() { gt_ghostty_app_tick(raw) }

    /// 把界面的深浅告诉了 libghostty（`theme = light:X,dark:Y` 靠它选边）。
    /// 幂等：同一档重复调用由 libghostty 自己短路。
    public func setColorScheme(dark: Bool) {
        gt_ghostty_app_set_color_scheme(raw, dark)
    }

    /// 告诉 libghostty「本 App 是不是活动 App」（**app 级**焦点）。
    ///
    /// 与下面的 surface 级焦点是两回事：这一档管 App 范围的语义（如今后可能
    /// 接的全局快捷键）。上游 macOS 应用在 App 激活 / 失活时各报一次；
    /// 见 CGhostty 头里 `gt_ghostty_app_set_focus` 的说明。
    public func setFocused(_ focused: Bool) {
        gt_ghostty_app_set_focus(raw, focused)
    }

    /// 重新加载配置文件（设置页「保存」后调用）。失败返回错误描述，
    /// **旧配置保持有效**（不半途换配置）。
    ///
    /// 已存在的 surface 不用另外通知：libghostty 的 app 级更新会遍历它们
    /// 逐个下发（见 CGhostty 头里 gt_ghostty_app_reload_config 的说明）。
    @discardableResult
    public func reloadConfig(path: String?) -> String? {
        let rc: Int32 = (path ?? "").withCString { gt_ghostty_app_reload_config(raw, $0) }
        return rc == 0 ? nil : Self.lastError()
    }

    /// 读一个数值配置项（设置页回显 / 测试探针）。键用 Ghostty 语法
    /// （`font-size`、`background-opacity`…）。键不存在或类型不符返回 nil。
    public func float(_ key: String) -> Float? {
        // 显式分配对齐的缓冲：往 UnsafeMutableRawPointer 传 `&x` 会经过
        // 对齐不保证的临时缓冲，而 libghostty 对 out 做 @alignCast。
        let buf = UnsafeMutablePointer<Float>.allocate(capacity: 1)
        defer { buf.deallocate() }
        buf.initialize(to: 0)
        let ok = key.withCString {
            gt_ghostty_config_get_float(raw, $0, buf)
        }
        return ok ? buf.pointee : nil
    }

    /// 读一个颜色配置项（`background`、`unfocused-split-fill` 这类），返回
    /// 0~255 的 RGB。键不存在 / 类型不符 / 值为 null 返回 nil（不写 out）。
    public func color(_ key: String) -> (r: UInt8, g: UInt8, b: UInt8)? {
        var r: UInt8 = 0, g: UInt8 = 0, b: UInt8 = 0
        let ok = key.withCString { gt_ghostty_config_get_color(raw, $0, &r, &g, &b) }
        return ok ? (r, g, b) : nil
    }

    /// 读一个 f64 配置项（`background-opacity` 这类）。
    public func double(_ key: String) -> Double? {
        let buf = UnsafeMutablePointer<Double>.allocate(capacity: 1)
        defer { buf.deallocate() }
        buf.initialize(to: 0)
        let ok = key.withCString {
            gt_ghostty_config_get_double(raw, $0, buf)
        }
        return ok ? buf.pointee : nil
    }

    /// 读一个布尔配置项。
    public func bool(_ key: String) -> Bool? {
        let buf = UnsafeMutablePointer<Bool>.allocate(capacity: 1)
        defer { buf.deallocate() }
        buf.initialize(to: false)
        let ok = key.withCString {
            gt_ghostty_config_get_bool(raw, $0, buf)
        }
        return ok ? buf.pointee : nil
    }

    /// 配置诊断（0 条 = 干净）。每条是人可读的错误描述。
    public var configDiagnostics: [String] {
        let n = gt_ghostty_config_diagnostics_count(raw)
        guard n > 0 else { return [] }
        return (0..<n).compactMap { i in
            gt_ghostty_config_diagnostic(raw, i).map { String(cString: $0) }
        }
    }

    static func lastError() -> String {
        guard let p = gt_ghostty_last_error() else { return "" }
        return String(cString: p)
    }
}

/// 程序状态报告（OSC 7501, Program Status Protocol）。
///
/// 程序用它汇报「我在干什么」（idle/working/done/blocked/error），好让终端在
/// 标签页等 UI 上显示。**terminal 只搬运**：按 id 记录、clear 删子树、上限、
/// 提示符开始/程序退出时清理等「记录管理」都是 App 层的事（见 `ProgramStatusStore`）。
///
/// C 侧的字符串只在回调期间有效，所以这里**立刻复制**成 Swift 值。
public struct ProgramStatusReport: Sendable, Equatable {
    /// 序数与上游 C 枚举 `GHOSTTY_PROGRAM_STATUS_STATE_*` 一致。
    public enum State: Int, Sendable {
        case idle = 0, working = 1, done = 2, blocked = 3, error = 4, clear = 5
    }
    /// 序数与 `GHOSTTY_PROGRAM_STATUS_KIND_*` 一致。
    public enum Kind: Int, Sendable {
        case none = 0, permission = 1, question = 2, auth = 3
    }
    public var state: State
    public var kind: Kind
    /// 0…100，或 -1（不定 / 不适用）。
    public var progress: Int
    /// 记录标识；根记录为空串。`/` 表示父子（`build/test` 是 `build` 的子）。
    public var id: String
    /// 机器可匹配的程序名（`cargo` / `terraform`…），不是标签。
    public var app: String
    /// 给人看的短标签。
    public var title: String
    /// 一行说明；程序没发就是空串。
    public var message: String

    /// 直接构造（上层造样本 / 测试用）。显式声明以暴露为 public
    /// （否则只有 internal 的 synthesized memberwise，跨模块用不了）。
    public init(state: State, kind: Kind = .none, progress: Int = -1,
                id: String = "", app: String = "",
                title: String = "", message: String = "") {
        self.state = state
        self.kind = kind
        self.progress = progress
        self.id = id
        self.app = app
        self.title = title
        self.message = message
    }
}

/// shell 集成事件（OSC 133, semantic prompt）。
///
/// 典型来源是 shell 的集成脚本（bash/zsh/fish 的 `vte`/`finalterm` 钩子）。
/// `promptStart` 表示 shell **开始画提示符** = 上一条命令在显示层面结束了。
/// C 侧字符串只在回调期间有效，故这里立刻复制。
public struct SemanticPromptEvent: Sendable, Equatable {
    /// 序数与上游 C 枚举 `GHOSTTY_SEMANTIC_PROMPT_*` 一致。
    public enum Kind: Int, Sendable {
        case invalid = 0, promptStart = 1, inputStart = 2, outputStart = 3, commandEnd = 4
    }
    /// 序数与 `GHOSTTY_SEMANTIC_PROMPT_PROMPT_*` 一致。
    public enum PromptKind: Int, Sendable {
        case primary = 0, right = 1, continuation = 2, secondary = 3
    }
    public var kind: Kind
    public var promptKind: PromptKind
    /// 仅 commandEnd 且 shell 报了退出码时为 true。
    public var hasExitCode: Bool
    public var exitCode: Int32
    /// 仅 outputStart：解码后的命令行（shell 没发则为空）。
    public var command: String
    /// 仅 commandEnd：shell 给的原因说明（很少见，可为空）。
    public var err: String

    public init(kind: Kind, promptKind: PromptKind = .primary,
                hasExitCode: Bool = false, exitCode: Int32 = 0,
                command: String = "", err: String = "") {
        self.kind = kind; self.promptKind = promptKind
        self.hasExitCode = hasExitCode; self.exitCode = exitCode
        self.command = command; self.err = err
    }
}

/// 渲染锚点视图。显式拒绝「拖动=移窗」：终端上沿可能顶进系统标题栏带
/// （侧栏展开时右列没有顶栏；窗格标题栏关闭时上沿只有 6pt 把手），带内
/// AppKit 会按**命中视图**的 `mouseDownCanMoveWindow`（非不透明视图默认
/// YES）把拖动劫持成「移动窗口」——从终端第一行开始划选文字会变成拖着
/// 整个窗口跑。锚点是终端区域最深层的命中视图，必须它自己拒绝。
private final class SurfaceAnchorView: NSView {
    override var mouseDownCanMoveWindow: Bool { false }
}

/// 一个终端面。用 NSView 作为渲染锚点（Metal 层由 libghostty 自己挂）。
public final class TerminalSurface {
    /// 终端编码后的用户输入，调用方应写进自己的 transport。
    public var onInput: ((Data) -> Void)?
    /// 网格尺寸变化（列 / 行），调用方应转成 transport 的 resize。
    public var onResize: ((UInt16, UInt16) -> Void)?
    /// 终端标题变化（OSC 0/2）。
    public var onTitle: ((String) -> Void)?
    /// 程序状态报告（OSC 7501）。与 onInput 同一线程（libghostty 的 termio
    /// 线程）回调，UI 侧要自己 hop 回主线程。
    public var onProgramStatus: ((ProgramStatusReport) -> Void)?
    /// shell 集成事件（OSC 133）。同样是 termio 线程回调。
    public var onSemanticPrompt: ((SemanticPromptEvent) -> Void)?
    /// 终端工作目录变化（OSC 7）。**由 shell 集成发出**——没有集成就不会来。
    ///
    /// **现状（诚实标注）**：在我们钉的 embedded host-IO 下这条**不会到达**
    /// ——实测喂 OSC 7（含本机 host）时 `cb_action` 收不到 `GHOSTTY_ACTION_PWD`
    /// （`apprt/embedded.zig` 没有 surface message 的 pwd 通路）；libghostty
    /// 还额外要求 OSC 7 的 host `isLocal`。这里保留这条接线作为 seam：本地 cwd
    /// 另有可靠来源（core `gt_transport_child_cwd` 的内核查询），远端 cwd 待
    /// 上游支持（roadmap R2）。见 `docs/reference/plan-tabs-close-and-newtab.md`。
    public var onPwd: ((String) -> Void)?

    public let view: NSView
    private let raw: OpaquePointer

    /// 回调里引用的 C 结构必须存活到 surface 释放为止。
    private final class CallbackBox {
        var onInput: ((Data) -> Void)?
        var onResize: ((UInt16, UInt16) -> Void)?
        var onTitle: ((String) -> Void)?
        var onProgramStatus: ((ProgramStatusReport) -> Void)?
        var onSemanticPrompt: ((SemanticPromptEvent) -> Void)?
        var onPwd: ((String) -> Void)?
    }
    private let box = CallbackBox()

    public init(app: TerminalApp, frame: NSRect = NSRect(x: 0, y: 0, width: 800, height: 600)) throws {
        self.view = SurfaceAnchorView(frame: frame)
        // 不要在这里设 wantsLayer：libghostty 的 Metal 渲染器要把 view 做成
        // "layer-hosting"——必须先给它自己的 layer，再置 wantsLayer（见
        // vendor/ghostty/src/renderer/Metal.zig）。若我们提前把 view 变成
        // layer-backed，它的赋值路径会走进另一条分支，画面全黑。

        let box = self.box
        var cbs = gt_host_callbacks_t()
        cbs.userdata = Unmanaged.passUnretained(box).toOpaque()
        cbs.write = { ud, data, len in
            guard let ud = ud, let data = data else { return }
            let box = Unmanaged<CallbackBox>.fromOpaque(ud).takeUnretainedValue()
            box.onInput?(Data(bytes: data, count: len))
        }
        cbs.resize = { ud, cols, rows in
            guard let ud = ud else { return }
            let box = Unmanaged<CallbackBox>.fromOpaque(ud).takeUnretainedValue()
            box.onResize?(cols, rows)
        }
        cbs.title = { ud, title in
            guard let ud = ud, let title = title else { return }
            let box = Unmanaged<CallbackBox>.fromOpaque(ud).takeUnretainedValue()
            box.onTitle?(String(cString: title))
        }

        cbs.program_status = { ud, report in
            guard let ud = ud, let report = report else { return }
            let box = Unmanaged<CallbackBox>.fromOpaque(ud).takeUnretainedValue()
            box.onProgramStatus?(ProgramStatusReport(c: report.pointee))
        }

        cbs.semantic_prompt = { ud, event in
            guard let ud = ud, let event = event else { return }
            let box = Unmanaged<CallbackBox>.fromOpaque(ud).takeUnretainedValue()
            box.onSemanticPrompt?(SemanticPromptEvent(c: event.pointee))
        }

        cbs.pwd = { ud, pwd in
            guard let ud = ud, let pwd = pwd else { return }
            let box = Unmanaged<CallbackBox>.fromOpaque(ud).takeUnretainedValue()
            box.onPwd?(String(cString: pwd))
        }

        let scale = view.window?.backingScaleFactor
            ?? NSScreen.main?.backingScaleFactor ?? 2.0
        let made = gt_ghostty_surface_new(app.raw, Unmanaged.passUnretained(view).toOpaque(), scale, &cbs)
        guard let s = made else {
            throw TerminalSurfaceError.surfaceInit(TerminalApp.lastError())
        }
        self.raw = s
        // C 侧回调经 box 转发，故这里只设 box 上的闭包。
        self.box.onInput = { [weak self] d in self?.onInput?(d) }
        self.box.onResize = { [weak self] c, r in self?.onResize?(c, r) }
        self.box.onTitle = { [weak self] t in self?.onTitle?(t) }
        self.box.onProgramStatus = { [weak self] r in self?.onProgramStatus?(r) }
        self.box.onSemanticPrompt = { [weak self] e in self?.onSemanticPrompt?(e) }
        self.box.onPwd = { [weak self] p in self?.onPwd?(p) }
    }

    /// 释放 C surface。**必须在主线程**——见下面的说明。
    deinit {
        // libghostty 的 `Surface.deinit` 会 `join` 它自己的 io / renderer 线程，
        // 而回调正是在 io 线程上派发的。若最后一个强引用恰好是在 io 线程的
        // 一次回调里被释放的（ARC 的临时引用，实测 1/20 复现），deinit 就会
        // 在 io 线程上执行 → `io_thr.join()` = join 自己 → EDEADLK → libghostty
        // 直接 panic（"two threads tried to join each other"）。
        //
        // 修法：把释放搬到主线程（这本来也是 libghostty 的线程契约）。并把
        // `box` 一起带过去——C 侧的 userdata 是 `passUnretained(box)`，free 完成
        // 前必须让 box 一直活着，否则 io 线程若再回调就是 use-after-free。
        // （各回调闭包都是 `[weak self]`，self 已在销毁中 → 回调即 no-op。）
        let raw = self.raw
        let box = self.box
        if Thread.isMainThread {
            gt_ghostty_surface_free(raw)
        } else {
            DispatchQueue.main.async {
                withExtendedLifetime(box) { gt_ghostty_surface_free(raw) }
            }
        }
    }

    // MARK: - transport → surface（字节流直连，不过事件队列）

    /// 把 transport 读到的字节喂进终端。任意线程可调。
    public func feed(_ data: Data) {
        guard !data.isEmpty else { return }
        data.withUnsafeBytes { buf in
            guard let base = buf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            gt_ghostty_surface_process_output(raw, base, data.count)
        }
    }

    // MARK: - 视图尺寸 → 网格

    /// 换屏时必须同时更新字体 DPI 与 Metal 层的合成比例。
    /// layer 由 libghostty 持有；只改比例，不替换 layer 或 wantsLayer。
    func setContentScale(_ scale: CGFloat) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        view.layer?.contentsScale = scale
        CATransaction.commit()
        gt_ghostty_surface_set_content_scale(raw, Double(scale))
    }

    /// 视图（像素）尺寸变化；libghostty 会重算网格并回调 onResize。
    public func setSize(width: UInt32, height: UInt32) {
        gt_ghostty_surface_set_size(raw, width, height)
    }

    public func draw() { gt_ghostty_surface_draw(raw) }

    // MARK: - 焦点

    /// **surface 级**焦点：这个面是不是当前被操作的那一个。
    ///
    /// 只让当前窗格的游标闪：非聚焦的面由 libghostty 画成**空心块并停掉
    /// 闪烁**（见 CGhostty 头里的说明）。libghostty 的 surface 默认「已聚焦」，
    /// 所以多窗格时**每个非当前窗格都必须报 false**——漏报就会出现
    /// 「每个格子都在闪」。主线程调用。
    public func setFocused(_ focused: Bool) {
        gt_ghostty_surface_set_focus(raw, focused)
    }

    /// **可见性**：这个面是不是真的在屏幕上。
    ///
    /// 后台标签页的 surface 我们保持挂载（一释放就没网格、没连接），但把它报成
    /// 「不可见」，引擎就会停掉绘制（省 CPU/GPU）；切回来它会自己重画。见
    /// CGhostty 头里 `gt_ghostty_surface_set_occlusion` 的说明。主线程调用。
    public func setVisible(_ visible: Bool) {
        gt_ghostty_surface_set_occlusion(raw, visible)
    }

    /// 当前网格尺寸（列 × 行）。
    public var gridSize: (cols: UInt16, rows: UInt16) {
        var c: UInt16 = 0, r: UInt16 = 0
        gt_ghostty_surface_grid_size(raw, &c, &r)
        return (c, r)
    }

    /// 当前屏幕尺寸（像素）。
    public var size: (width: UInt32, height: UInt32) {
        var w: UInt32 = 0, h: UInt32 = 0
        gt_ghostty_surface_pixel_size(raw, &w, &h)
        return (w, h)
    }

    /// 读回屏幕文本（调试 / 断言用）。
    public func readText() -> String? {
        guard let p = gt_ghostty_surface_read_text(raw) else { return nil }
        defer { free(p) }
        return String(cString: p)
    }

    /// 当前引擎选区；无选区时不以整屏文本冒充复制内容。
    public func selectedText() -> String? {
        guard let p = gt_ghostty_surface_read_selection(raw) else { return nil }
        defer { free(p) }
        return String(cString: p)
    }

    /// 以**粘贴语义**注入文本（走终端输入编码路径）。App 层用于
    /// 「粘贴到会话」；测试用它验证输入方向真的通到 transport。
    ///
    /// 注意：粘贴语义**不带回车执行**——远端开启 bracketed paste 时，黏贴内容
    /// 里的换行只是字面换行（这正是 bracketed paste 的意义）。要「敲命令并回车」
    /// 须走 `sendKey`（键入语义），而不是这里。
    public func sendText(_ text: String) {
        guard !text.isEmpty else { return }
        text.withCString { p in
            gt_ghostty_surface_text(raw, p, strlen(p))
        }
    }

    /// 输入法的**预编辑**（组合中）文本——只做内联显示，**不发给程序**。
    ///
    /// 上游 macOS 的链路是：视图实现 `NSTextInputClient`，keyDown 里调
    /// `interpretKeyEvents`，输入法把组合串经 `setMarkedText` 回调上来。组合串
    /// 不是提交，程序不该看到它；它只让用户看到「我正在打什么」。空串 = 清除组合态。
    /// 主线程调用。
    public func setPreedit(_ text: String) {
        if text.isEmpty {
            gt_ghostty_surface_preedit(raw, nil, 0)
        } else {
            text.withCString { gt_ghostty_surface_preedit(raw, $0, strlen($0)) }
        }
    }

    /// 输入法候选窗的锚点（视图坐标，左上原点，单位 = 视图点）。
    /// `width` 为预编辑串宽度（无组合时为 0）。
    public var imePoint: (x: Double, y: Double, width: Double, height: Double) {
        var x = 0.0, y = 0.0, w = 0.0, h = 0.0
        gt_ghostty_surface_ime_point(raw, &x, &y, &w, &h)
        return (x, y, w, h)
    }

    /// 送一段**已提交**的文本（输入法提交 / 听写）：**键入语义**，不是粘贴。
    ///
    /// 与 `sendText` 的区别：`sendText` 是**粘贴**语义，对端开了 bracketed paste
    /// 时会被 `ESC[200~ … ESC[201~` 包裹——输入法提交的字不该被当成粘贴。这条走
    /// key 事件通路（keycode=0、无修饰键），libghostty 把文本原样编码。
    public func sendCommittedText(_ text: String) {
        guard !text.isEmpty else { return }
        text.withCString {
            gt_ghostty_surface_committed_text(raw, $0, strlen($0))
        }
    }

    /// 注入一次按键（**键入语义**）：libghostty 按物理键 + 当前终端模式编码，
    /// 与真人敲键等价。`key` 取 macOS 虚拟键码（`TerminalKey` 已列常用值）。
    public func sendKey(_ key: TerminalKey, codepoint: UInt32 = 0,
                        mods: TerminalModifiers = []) {
        gt_ghostty_surface_key_press(raw, key.rawValue, codepoint, mods.rawValue)
    }

    /// 「敲一行命令并回车」：文本按粘贴语义送入，回车走键入语义。
    /// 分两步正是为了绕开 bracketed paste 对换行的字面化处理。
    public func sendCommandLine(_ line: String) {
        sendText(line)
        sendKey(.return)
    }

    /// 转发一次**真实键盘事件**（App 层从 NSEvent 取字段调这里）。
    /// 与 sendKey 的区别：可打印字符走 text（libghostty 优先用 text），
    /// 这样 option 组合、死键、IME 提交的结果都按系统给的文本走。
    public func sendKeyEvent(nativeKeycode: UInt32, text: String,
                             codepoint: UInt32 = 0,
                             modifiers: NSEvent.ModifierFlags = []) {
        let mods = TerminalModifiers(modifiers)
        // NSEvent 用 U+F700…U+F8FF 表示方向键和功能键，并非可输入文本。
        // 与上游 NSEvent.ghosttyCharacters 一致，让物理键交给引擎按模式编码；
        // 否则 Kitty 模式会优先输出这些 PUA 字符（普通模式恰好先匹配功能键）。
        let hasText = !text.isEmpty && !text.unicodeScalars.contains {
            $0.value < 0x20 || $0.value == 0x7F || (0xF700...0xF8FF).contains($0.value)
        }
        if hasText {
            text.withCString { p in
                gt_ghostty_surface_key_text(raw, nativeKeycode, codepoint, p, mods.rawValue)
            }
        } else {
            gt_ghostty_surface_key_press(raw, nativeKeycode, codepoint, mods.rawValue)
        }
    }

    // MARK: - 鼠标事件

    /// 鼠标按键枚举（对齐 `ghostty_input_mouse_button_e`）。
    public enum MouseButton: Int32 {
        case unknown = 0, left = 1, right = 2, middle = 3
    }

    /// 鼠标按键状态（对齐 `ghostty_input_mouse_state_e`）。
    public enum MouseState: Int32 {
        case release = 0, press = 1
    }

    /// 发送鼠标按键事件（按下 / 释放）。返回 libghostty 是否消费了此事件
    /// （程序开启鼠标捕获时为 true，宿主可据此决定是否走系统默认行为）。
    @discardableResult
    public func sendMouseButton(_ state: MouseState, button: MouseButton,
                                modifiers: NSEvent.ModifierFlags = []) -> Bool {
        let mods = TerminalModifiers(modifiers)
        return gt_ghostty_surface_mouse_button(raw, state.rawValue,
                                               button.rawValue, mods.rawValue)
    }

    /// 发送鼠标位置事件（移动 / 拖拽）。坐标为视图坐标（左上原点，点单位）。
    public func sendMousePos(x: Double, y: Double,
                             modifiers: NSEvent.ModifierFlags = []) {
        let mods = TerminalModifiers(modifiers)
        gt_ghostty_surface_mouse_pos(raw, x, y, mods.rawValue)
    }

    /// 发送鼠标滚轮 / 触控板滚动事件。
    ///   - Parameters:
    ///     - dx: 水平滚动增量
    ///     - dy: 垂直滚动增量
    ///     - precision: 是否为高精度滚动（触控板 / Magic Mouse）
    ///     - momentum: 动量阶段（0=none, 1=began, …, 5=cancelled, 6=mayBegin）
    public func sendMouseScroll(dx: Double, dy: Double,
                                precision: Bool = false,
                                momentum: UInt8 = 0) {
        // 打包 scroll_mods：bit 0 = precision, bits 1-3 = momentum
        var scrollMods: Int32 = 0
        if precision { scrollMods |= 1 }
        scrollMods |= Int32(momentum & 0x07) << 1
        gt_ghostty_surface_mouse_scroll(raw, dx, dy, scrollMods)
    }

    /// 鼠标是否被终端程序捕获（如 vim / less 开启了鼠标报告）。
    public var isMouseCaptured: Bool {
        gt_ghostty_surface_mouse_captured(raw)
    }

    /// Force Touch 压力变化。stage 2 = force click。
    public func sendMousePressure(stage: UInt32, pressure: Double) {
        gt_ghostty_surface_mouse_pressure(raw, stage, pressure)
    }

    /// 仅供测试：语义同 sendText，保留独立名字以免测试依赖 App 层 API。
    public func injectTextForTesting(_ text: String) { sendText(text) }
}

/// macOS 虚拟键码（NSEvent.keyCode）。取值须与 vendor/ghostty 的
/// src/input/keycodes.zig 中 `native` 列一致（libghostty 用它反查物理键）。
public enum TerminalKey: UInt32 {
    case `return` = 36
    case tab = 48
    case backspace = 51
    case escape = 53
}

/// ghostty_input_mods_e 的位标志。
public struct TerminalModifiers: OptionSet, Sendable {
    public let rawValue: Int32
    public init(rawValue: Int32) { self.rawValue = rawValue }

    /// 从 AppKit 的修饰键标志转换（只取终端关心的四个）。
    public init(_ flags: NSEvent.ModifierFlags) {
        var v: Int32 = 0
        if flags.contains(.shift) { v |= 1 << 0 }
        if flags.contains(.control) { v |= 1 << 1 }
        if flags.contains(.option) { v |= 1 << 2 }
        if flags.contains(.command) { v |= 1 << 3 }
        self.rawValue = v
    }

    public static let shift = TerminalModifiers(rawValue: 1 << 0)
    public static let control = TerminalModifiers(rawValue: 1 << 1)
    public static let option = TerminalModifiers(rawValue: 1 << 2)
    public static let command = TerminalModifiers(rawValue: 1 << 3)
}

extension SemanticPromptEvent {
    /// 从 C 事件复制（指针只在回调期间有效）。
    init(c: ghostty_semantic_prompt_s) {
        func str(_ g: ghostty_string_s) -> String {
            guard let p = g.ptr, g.len > 0 else { return "" }
            return String(decoding: UnsafeRawBufferPointer(start: p, count: Int(g.len)),
                          as: UTF8.self)
        }
        self.kind = Kind(rawValue: Int(c.kind.rawValue)) ?? .invalid
        self.promptKind = PromptKind(rawValue: Int(c.prompt_kind.rawValue)) ?? .primary
        self.hasExitCode = c.has_exit_code
        self.exitCode = c.exit_code
        self.command = str(c.command)
        self.err = str(c.err)
    }
}

extension ProgramStatusReport {
    /// 从 C 报告复制。`ghostty_string_s` 的指针只在回调期间有效，故立刻取值。
    init(c: ghostty_program_status_s) {
        func str(_ g: ghostty_string_s) -> String {
            guard let p = g.ptr, g.len > 0 else { return "" }
            return String(decoding: UnsafeRawBufferPointer(start: p, count: Int(g.len)),
                          as: UTF8.self)
        }
        self.state = State(rawValue: Int(c.state.rawValue)) ?? .idle
        self.kind = Kind(rawValue: Int(c.kind.rawValue)) ?? .none
        self.progress = Int(c.progress)
        self.id = str(c.id)
        self.app = str(c.app)
        self.title = str(c.title)
        self.message = str(c.message)
    }
}
