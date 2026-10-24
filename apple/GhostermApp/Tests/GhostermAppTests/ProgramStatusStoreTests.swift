// ProgramStatusStoreTests —— OSC 7501 记录面的规范规则（离线，不碰引擎）。
//
// 这里测的是**规范行为**，不是我们的实现口味：
//   整条替换 / clear 删子树 / app 继承 / 上限淘汰 / working·blocked 清理 /
//   摘要优先级。每条都能在规范里找到出处（见 ProgramStatusStore 顶部注释）。
@testable import GhostermAppModel
import GhostermTerminal
import XCTest

final class ProgramStatusStoreTests: XCTestCase {
    private func report(_ state: ProgramStatusReport.State,
                        id: String = "", app: String = "",
                        kind: ProgramStatusReport.Kind = .none,
                        progress: Int = -1,
                        title: String = "", message: String = "") -> ProgramStatusReport {
        ProgramStatusReport(state: state, kind: kind, progress: progress,
                            id: id, app: app, title: title, message: message)
    }

    func testRootReportReplacesWholeRecord() {
        var s = ProgramStatusStore()
        s.apply(report(.working, app: "cargo", progress: 40, message: "编译"))
        XCTAssertEqual(s.records.count, 1)
        XCTAssertEqual(s.records[0].app, "cargo")
        XCTAssertEqual(s.records[0].progress, 40)

        // 整条替换：这一条没带 app/progress/message → 旧值**不留**。
        s.apply(report(.done))
        XCTAssertEqual(s.records.count, 1)
        XCTAssertEqual(s.records[0].state, .done)
        XCTAssertEqual(s.records[0].app, "", "报告没发的字段不得保留旧值")
        XCTAssertEqual(s.records[0].progress, -1)
        XCTAssertEqual(s.records[0].message, "")
    }

    func testRecordsAreKeyedByID() {
        var s = ProgramStatusStore()
        s.apply(report(.working, id: "build", app: "deploy"))
        s.apply(report(.working, id: "build/test", app: "deploy"))
        s.apply(report(.done, id: "build"))
        XCTAssertEqual(s.records.count, 2, "不同 id 各占一条记录")
        XCTAssertEqual(s.records.first { $0.id == "build" }?.state, .done)
        XCTAssertEqual(s.records.first { $0.id == "build/test" }?.state, .working)
    }

    func testClearRemovesRecordAndItsDescendants() {
        var s = ProgramStatusStore()
        s.apply(report(.working, id: "build"))
        s.apply(report(.working, id: "build/test"))
        s.apply(report(.working, id: "build/test/unit"))
        s.apply(report(.working, id: "deploy"))

        s.apply(report(.clear, id: "build"))
        XCTAssertEqual(Set(s.records.map(\.id)), ["deploy"],
                       "clear('build') 必须连 build/test、build/test/unit 一起删")

        s.apply(report(.clear, id: ""))
        XCTAssertTrue(s.isEmpty, "clear 空 id = 清空全部")
    }

    func testRecordWithoutAppInheritsFromNearestAncestor() {
        var s = ProgramStatusStore()
        s.apply(report(.working, id: "build", app: "deploy"))
        s.apply(report(.working, id: "build/test"))       // 没带 app
        XCTAssertEqual(s.records.first { $0.id == "build/test" }?.app, "deploy",
                       "app 应从最近的有 app 的祖先继承")
    }

    func testExplicitAppIsNotOverriddenByAncestor() {
        var s = ProgramStatusStore()
        s.apply(report(.working, id: "build", app: "deploy"))
        s.apply(report(.working, id: "build/test", app: "cargo"))
        XCTAssertEqual(s.records.first { $0.id == "build/test" }?.app, "cargo",
                       "自己带了 app 就不继承")
    }

    func testCapacityEvictsLeastRecentlyUpdated() {
        var s = ProgramStatusStore()
        for i in 0..<(ProgramStatusStore.capacity + 3) {
            s.apply(report(.working, id: "rec\(i)"))
        }
        XCTAssertEqual(s.records.count, ProgramStatusStore.capacity, "不得超过上限")
        // 最早写入的 3 条应被淘汰。
        XCTAssertNil(s.records.first { $0.id == "rec0" })
        XCTAssertNil(s.records.first { $0.id == "rec2" })
        XCTAssertNotNil(s.records.first { $0.id == "rec3" })
        XCTAssertNotNil(s.records.first { $0.id == "rec\(ProgramStatusStore.capacity + 2)" })
    }

    func testEphemeralStatesAreClearedOnPromptOrExit() {
        var s = ProgramStatusStore()
        s.apply(report(.working, id: "a"))
        s.apply(report(.blocked, id: "b", kind: .permission))
        s.apply(report(.done, id: "c"))
        s.apply(report(.error, id: "d"))
        s.removeEphemeral()
        XCTAssertEqual(Set(s.records.map(\.id)), ["c", "d"],
                       "只清 working/blocked，done/error 保留到用户看过")
    }

    func testSummaryPrefersBlockedThenErrorThenWorking() {
        var s = ProgramStatusStore()
        s.apply(report(.working, id: "w", app: "cargo"))
        s.apply(report(.done, id: "d"))
        XCTAssertEqual(s.summary?.id, "w", "working 比 done 更该被注意")

        s.apply(report(.error, id: "e"))
        XCTAssertEqual(s.summary?.id, "e", "error 比 working 更该被注意")

        s.apply(report(.blocked, id: "b", kind: .question))
        XCTAssertEqual(s.summary?.id, "b", "blocked（等用户）优先级最高")
    }
}

// MARK: - 角标（UI 决策的可测部分）

final class ProgramStatusBadgeTests: XCTestCase {
    private func record(_ state: ProgramStatusReport.State,
                        id: String = "", app: String = "",
                        kind: ProgramStatusReport.Kind = .none,
                        progress: Int = -1,
                        title: String = "", message: String = "") -> ProgramStatusStore.Record {
        ProgramStatusStore.Record(id: id, app: app, state: state, kind: kind,
                                  progress: progress, title: title,
                                  message: message, updatedAt: 1)
    }

    func testIdleAndMissingProduceNoBadge() {
        XCTAssertNil(ProgramStatusBadge(record: nil))
        XCTAssertNil(ProgramStatusBadge(record: record(.idle, app: "cargo")),
                     "idle 不打扰用户：终端里不显示角标")
    }

    func testStatesMapToDistinctSymbolsAndProgress() {
        let working = ProgramStatusBadge(record: record(.working, app: "cargo", progress: 40))
        XCTAssertEqual(working?.kind, .working)
        XCTAssertEqual(working?.progress, 40, "working 带百分比时角标带进度")

        let blocked = ProgramStatusBadge(record: record(.blocked, app: "terraform", kind: .permission))
        XCTAssertEqual(blocked?.kind, .blocked)
        XCTAssertNil(blocked?.progress, "没给进度就不编一个")

        XCTAssertEqual(ProgramStatusBadge(record: record(.done, app: "cargo"))?.kind, .done)
        XCTAssertEqual(ProgramStatusBadge(record: record(.error, app: "cargo"))?.kind, .error)

        let symbols = Set([working, blocked,
                           ProgramStatusBadge(record: record(.done)),
                           ProgramStatusBadge(record: record(.error))].compactMap(\.?.symbol))
        XCTAssertEqual(symbols.count, 4, "四档状态要用四个不同的图标（不能折叠）")
    }

    func testLabelFallsBackAndNamesTheTerminal() {
        let b = ProgramStatusBadge(record: record(.blocked, app: "terraform", kind: .permission,
                                                  message: "Apply?"),
                                   terminalName: "prod-1")
        XCTAssertEqual(b?.label, "prod-1 · terraform：Apply?",
                       "文案要标明文本来自哪个终端（规范要求）")
    }

    func testInvisibleFormatCharsAreStripped() {
        // 方向覆盖（U+202E）+ 零宽空格 + BOM 都是「不可见格式字符」——
        // 规范点名：显示在终端之外前必须去掉。
        let dirty = "安全\u{202E}文本\u{200B}\u{FEFF}"
        XCTAssertEqual(ProgramStatusBadge.clean(dirty), "安全文本",
                       "不可见格式字符必须被剥掉（防伪装）")

        let b = ProgramStatusBadge(record: record(.blocked, app: "a\u{202E}b", message: "x\u{200B}y"))
        XCTAssertFalse(b?.label.contains("\u{202E}") ?? true)
        XCTAssertFalse(b?.label.contains("\u{200B}") ?? true)
    }
}

// MARK: - 窗格接线（OSC 133 触发清理）

@MainActor
final class ProgramStatusPaneTests: XCTestCase {
    private func report(_ state: ProgramStatusReport.State,
                        id: String = "", app: String = "") -> ProgramStatusReport {
        ProgramStatusReport(state: state, id: id, app: app)
    }

    /// 提示符（重新）开始 → working/blocked 被清掉，done 保留。
    /// 这补的是「提示符开始」那一半（另一半是程序退出，见 TerminalTab.observeState）。
    func testPromptStartClearsEphemeralRecords() {
        let pane = TerminalPane()
        pane.applyProgramStatus(report(.working, id: "w", app: "cargo"))
        pane.applyProgramStatus(report(.blocked, id: "b", app: "deploy"))
        pane.applyProgramStatus(report(.done, id: "d", app: "cargo"))
        XCTAssertEqual(pane.programStatus.records.count, 3, "前置：三条记录")

        // 直接调接线处理函数，走与真实回调**同一条**路径。
        pane.handleSemanticPrompt(SemanticPromptEvent(kind: .promptStart))

        XCTAssertEqual(Set(pane.programStatus.records.map(\.id)), ["d"],
                       "提示符开始应清掉 working/blocked，done 保留")
    }

    /// 非 prompt_start 的事件不该动记录（只有命令行重新出现才算「上一条结束」）。
    func testNonPromptStartEventsDoNotClear() {
        let pane = TerminalPane()
        pane.applyProgramStatus(report(.working, id: "w", app: "cargo"))
        for kind in [SemanticPromptEvent.Kind.inputStart, .outputStart,
                     .commandEnd, .invalid] {
            pane.handleSemanticPrompt(SemanticPromptEvent(kind: kind))
        }
        XCTAssertEqual(pane.programStatus.records.count, 1,
                       "只有 prompt_start 才清 working/blocked")
    }
}

// MARK: - 上一条命令（OSC 133 的 output_start / command_end）

final class LastCommandTests: XCTestCase {
    private func ev(_ kind: SemanticPromptEvent.Kind,
                    command: String = "", hasExit: Bool = false,
                    code: Int32 = 0) -> SemanticPromptEvent {
        SemanticPromptEvent(kind: kind, hasExitCode: hasExit, exitCode: code,
                            command: command)
    }

    /// 正常一圈：命令行 → 结束（退出码 + 耗时）。
    func testCommandLineExitCodeAndDuration() {
        var s = LastCommand()
        s = LastCommand.advanced(s, event: ev(.outputStart, command: "ls -la"), now: 100)
        XCTAssertEqual(s.commandLine, "ls -la")
        XCTAssertNil(s.exitCode, "命令刚开始，退出码还没报")
        XCTAssertNil(s.duration, "还没有结束，谈不上耗时")

        s = LastCommand.advanced(s, event: ev(.commandEnd, hasExit: true, code: 0), now: 100.42)
        XCTAssertEqual(s.exitCode, 0)
        XCTAssertEqual(s.duration ?? -1, 0.42, accuracy: 0.001)
        XCTAssertFalse(s.failed, "退出码 0 不算失败")
        XCTAssertEqual(s.sidebarLine, "ls -la · 0.42s · 退出码 0")
    }

    /// 非 0 退出码 = 失败（标签条红点靠它）。
    func testNonZeroExitCodeIsFailure() {
        var s = LastCommand()
        s = LastCommand.advanced(s, event: ev(.outputStart, command: "make"), now: 0)
        s = LastCommand.advanced(s, event: ev(.commandEnd, hasExit: true, code: 2), now: 1)
        XCTAssertTrue(s.failed)
        XCTAssertTrue(s.sidebarLine.contains("退出码 2"))
    }

    /// **三态不折叠**：没报退出码 ≠ 成功。`exitCode` 为 nil、`failed` 为 false，
    /// 显示层不明说成败。
    func testMissingExitCodeStaysUnknown() {
        var s = LastCommand()
        s = LastCommand.advanced(s, event: ev(.outputStart, command: "sleep 1"), now: 0)
        s = LastCommand.advanced(s, event: ev(.commandEnd, hasExit: false), now: 2)
        XCTAssertNil(s.exitCode, "shell 没报退出码 → 未知，不能当成 0")
        XCTAssertFalse(s.failed, "未知不等于失败")
        XCTAssertEqual(s.duration ?? -1, 2, accuracy: 0.001, "耗时仍然记得下")
        XCTAssertFalse(s.sidebarLine.contains("退出码"), "没报就不显示退出码")
    }

    /// 异常路径：只收到结束、没有起点 → 时长未知，但退出码照记（不编耗时）。
    func testCommandEndWithoutStartLeavesDurationUnknown() {
        var s = LastCommand()
        s = LastCommand.advanced(s, event: ev(.commandEnd, hasExit: true, code: 1), now: 5)
        XCTAssertEqual(s.exitCode, 1)
        XCTAssertNil(s.duration, "没有起点就不能编一个耗时出来")
    }

    /// 别的 OSC 133 事件（input_start / prompt_start / invalid）不动它。
    func testOtherEventsDoNotChange() {
        let base = LastCommand(commandLine: "keep", exitCode: 0, duration: 1, startedAt: nil)
        for kind in [SemanticPromptEvent.Kind.inputStart, .promptStart, .invalid] {
            XCTAssertEqual(LastCommand.advanced(base, event: ev(kind), now: 9), base)
        }
    }

    /// 新命令开始 → 清掉上一条的退出码/耗时（否则会显示成刚跑的这条）。
    func testNewCommandClearsPreviousResult() {
        var s = LastCommand()
        s = LastCommand.advanced(s, event: ev(.outputStart, command: "a"), now: 0)
        s = LastCommand.advanced(s, event: ev(.commandEnd, hasExit: true, code: 1), now: 1)
        XCTAssertEqual(s.exitCode, 1)
        s = LastCommand.advanced(s, event: ev(.outputStart, command: "b"), now: 5)
        XCTAssertEqual(s.commandLine, "b")
        XCTAssertNil(s.exitCode, "上一条的退出码不得留在这一条上")
        XCTAssertNil(s.duration)
    }

    /// 标签级摘要：多个窗格时，**失败**的那个优先。
    @MainActor
    func testTabSummaryPrefersFailedPane() {
        let ok = TerminalPane(); let bad = TerminalPane()
        ok.applyLastCommandForTesting(LastCommand(commandLine: "ok", exitCode: 0))
        bad.applyLastCommandForTesting(LastCommand(commandLine: "boom", exitCode: 3))
        let tab = TerminalTab(kind: .localShell, pane: ok)
        tab.split(axis: .horizontal, with: bad)
        XCTAssertEqual(tab.lastCommandSummary?.commandLine, "boom",
                       "有失败的就该显示失败的那条（不靠遍历顺序）")
    }
}
