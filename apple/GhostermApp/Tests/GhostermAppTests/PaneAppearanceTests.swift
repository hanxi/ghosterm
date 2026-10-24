// PaneAppearanceTests — 窗格外观的**纯规则**与**标题取值**。
//
// 这些都是「错了不报错、只在屏幕上不对」的判断：间距越界会悄悄回到默认、
// 标题会在命令跑完后卡住不动、侧栏开着时标签列两处。规则抽成纯函数后
// 正面穷举，视图只照它画（同 SplitDim / PaneStatus.worst 的口径）。
import GhostermTerminal
import XCTest
@testable import GhostermApp
@testable import GhostermAppModel

final class PaneAppearanceTests: XCTestCase {

    // MARK: - 间距

    /// 越界值收回默认档，合法档原样保留。
    ///
    /// 「0 也是合法档」是这里的要害：不能拿「值为 0」当「没设置过」，
    /// 否则选了「无间距」的用户每次启动都会被改回标准。
    func testGapResolvesOnlyToKnownSteps() {
        for step in PaneGap.steps {
            XCTAssertEqual(PaneGap.resolved(step), step, "合法档 \(step) 不该被改")
        }
        XCTAssertEqual(PaneGap.resolved(0), 0, "0 是「无间距」这一档，不是「没设置」")
        XCTAssertEqual(PaneGap.resolved(7), PaneGap.standard, "越界值回到默认档")
        XCTAssertEqual(PaneGap.resolved(-3), PaneGap.standard)
        XCTAssertEqual(PaneGap.resolved(999), PaneGap.standard)
    }

    /// 四档各有自己的名字（设置页的弹出菜单照它画）。
    func testGapTitlesAreDistinct() {
        let titles = PaneGap.steps.map(PaneGap.title(of:))
        XCTAssertEqual(titles, ["无", "紧凑", "标准", "宽松"])
        XCTAssertEqual(Set(titles).count, 4, "四档的名字不能重样")
    }

    /// 间距为 0 = 贴边形态：不画圆角（贴边的圆角会在四角露出背景色缺口）。
    func testZeroGapMeansNoCornerRadius() {
        XCTAssertEqual(PaneGap.cornerRadius(gap: 0), 0, accuracy: 1e-9)
        for step in PaneGap.steps where step > 0 {
            XCTAssertGreaterThan(PaneGap.cornerRadius(gap: step), 0,
                                 "有间距时该有圆角（档位 \(step)）")
        }
    }

    // MARK: - 标题栏按钮

    /// 两态穷举：按钮只画聚焦那格（曾经的「每一格都显示」偏好已弃用——
    /// 见 PaneTitleBar.showsButtons。四分屏时同一组动作画四遍，多的那份
    /// 不是信息，是图标墙）。
    func testTitleBarButtonVisibility() {
        XCTAssertTrue(PaneTitleBar.showsButtons(isFocused: true))
        XCTAssertFalse(PaneTitleBar.showsButtons(isFocused: false),
                       "非聚焦那格不画按钮")
    }

    // MARK: - 顶部标签条的显隐

    /// 两态穷举（规则写死，曾经的「侧栏展开时隐藏」偏好已弃用——见
    /// TabStripVisibility）。要害是「侧栏收起时顶栏一定在」——顶栏没了
    /// 标签列表就没有落点，红绿灯也住在那条横带上。
    func testTabStripVisibilityMatrix() {
        XCTAssertTrue(TabStripVisibility.shows(sidebarVisible: false),
                      "侧栏收起时顶栏必须在（标签清单只剩这一个落点）")
        XCTAssertFalse(TabStripVisibility.shows(sidebarVisible: true),
                       "侧栏展开时顶栏让位（标签已经列在侧栏里）")
    }

    // MARK: - 标题取值：进程 → 目录 → 标签标题

    func testTitleFallsBackThroughThreeLevels() {
        let home = NSHomeDirectory()
        let withProgram = PaneTitleRule.resolve(program: "npm",
                                                directory: home + "/code",
                                                tabTitle: "本地")
        XCTAssertEqual(withProgram.name, "npm", "有进程就以进程为主名")
        XCTAssertEqual(withProgram.detail, "~/code", "目录退到灰字那一段")

        let withDir = PaneTitleRule.resolve(program: nil, directory: home + "/code",
                                            tabTitle: "本地")
        XCTAssertEqual(withDir.name, "~/code", "没进程就用目录当主名")
        XCTAssertNil(withDir.detail, "退到目录这级就别把同一个目录画两遍")

        let bare = PaneTitleRule.resolve(program: nil, directory: nil, tabTitle: "web")
        XCTAssertEqual(bare.name, "web", "两样都没有才用标签页标题")
        XCTAssertNil(bare.detail)
    }

    /// 命令行取程序名：跳过前置环境赋值、取路径最后一段。
    func testProgramNameFromCommandLine() {
        XCTAssertEqual(PaneTitleRule.programName(commandLine: "npm run dev"), "npm")
        XCTAssertEqual(PaneTitleRule.programName(commandLine: "/usr/local/bin/npm run"),
                       "npm", "路径只取最后一段")
        XCTAssertEqual(PaneTitleRule.programName(commandLine: "FOO=bar RUST_LOG=x cargo b"),
                       "cargo", "前置的环境赋值不是程序名")
        XCTAssertNil(PaneTitleRule.programName(commandLine: "   "), "空命令行没得猜")
        XCTAssertNil(PaneTitleRule.programName(commandLine: ""))
    }

    /// 目录缩写：家目录变 `~`，别处原样，空的就不占位。
    func testDirectoryAbbreviation() {
        XCTAssertEqual(PaneTitleRule.abbreviate("/Users/x", home: "/Users/x"), "~")
        XCTAssertEqual(PaneTitleRule.abbreviate("/Users/x/code", home: "/Users/x"), "~/code")
        XCTAssertEqual(PaneTitleRule.abbreviate("/etc", home: "/Users/x"), "/etc")
        XCTAssertEqual(PaneTitleRule.abbreviate("/Users/xyz", home: "/Users/x"),
                       "/Users/xyz", "只认完整的一段，不能把 /Users/xyz 缩成 ~yz")
        XCTAssertNil(PaneTitleRule.abbreviate(nil, home: "/Users/x"))
        XCTAssertNil(PaneTitleRule.abbreviate("", home: "/Users/x"))
    }

    // MARK: - 「还在跑吗」只能看 OSC 133 的起点

    /// 走真的 OSC 133 事件：命令开始 → 标题是进程名；命令结束 → 退回目录。
    ///
    /// 最后一段是要害：shell **没报退出码**地结束（`hasExitCode: false`）时
    /// `exitCode` 仍是 nil，但命令确实已经完了。拿 `exitCode == nil` 当
    /// 「还在跑」会让标题永远卡在那个程序名上——铁律 #6 要防的折叠。
    @MainActor
    func testRunningProgramTracksCommandLifetime() {
        let pane = TerminalPane()
        XCTAssertNil(pane.runningProgram, "刚建好、还没有任何命令")

        pane.handleSemanticPrompt(SemanticPromptEvent(kind: .outputStart,
                                                      command: "cargo build"))
        XCTAssertTrue(pane.lastCommand.isRunning)
        XCTAssertEqual(pane.runningProgram, "cargo", "命令跑着时标题报进程名")

        pane.handleSemanticPrompt(SemanticPromptEvent(kind: .commandEnd,
                                                      hasExitCode: false))
        XCTAssertNil(pane.lastCommand.exitCode, "这条路径上 shell 确实没报退出码")
        XCTAssertFalse(pane.lastCommand.isRunning,
                       "没报退出码 ≠ 还在跑——命令已经结束了")
        XCTAssertNil(pane.runningProgram,
                     "命令结束后标题不该还卡在 cargo 上")
    }

    /// 没有 shell 集成的会话（一条 OSC 133 都没有）：标题老实退回标签页标题，
    /// 不去猜一个进程名。
    @MainActor
    func testTitleWithoutShellIntegrationFallsBackToTabTitle() {
        let pane = TerminalPane()
        let title = pane.displayTitle(fallback: "web")
        XCTAssertEqual(title.name, "web")
        XCTAssertNil(title.detail)
    }
}
