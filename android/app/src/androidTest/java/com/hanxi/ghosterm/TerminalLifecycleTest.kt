package com.hanxi.ghosterm

import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.terminal.TerminalView
import com.hanxi.ghosterm.terminal.SelectionOperation
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** Production Activity and its real ViewModelStore, not a retained test-owned model. */
@RunWith(AndroidJUnit4::class)
class TerminalLifecycleTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private var restoreCtrl: com.hanxi.ghosterm.domain.CtrlBehavior? = null
    private fun model() = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun find(view: View): TerminalView? {
        if (view is TerminalView) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun terminal(): TerminalView = checkNotNull(find(ui.activity.window.decorView))
    private fun text(): String = terminal().currentFrame?.let { f -> (0 until f.rows).joinToString("\n", transform = f::line) } ?: ""
    private fun waitFor(value: String) {
        val end = SystemClock.uptimeMillis() + 10_000
        while (SystemClock.uptimeMillis() < end) {
            var found = false
            ui.runOnIdle { found = text().contains(value) }
            if (found) return
            SystemClock.sleep(20)
        }
        ui.runOnIdle { fail("等待 $value [focus=${terminal().hasFocus()} window=${terminal().hasWindowFocus()} composing=${terminal().composition} ctrl=${terminal().ctrl}]: ${text()}") }
    }
    private fun capturePid(): Pair<String, Int> {
        val end = SystemClock.uptimeMillis() + 10_000
        while (SystemClock.uptimeMillis() < end) {
            val captured = ui.runOnIdle {
                terminal().currentFrame?.let { frame ->
                    val line = frame.line(0)
                    if (line.matches(Regex("PID:[0-9]+"))) line to frame.rows else null
                }
            }
            if (captured != null) return captured
            SystemClock.sleep(20)
        }
        throw AssertionError("等待真实终端 PID: " + ui.runOnIdle { text() })
    }
    private fun waitForUnwrapped(value: String) {
        val surface = ui.runOnIdle { checkNotNull(model().active).surface }
        val end = SystemClock.uptimeMillis() + 10_000
        var copied = ""
        while (SystemClock.uptimeMillis() < end) {
            copied = runBlocking {
                surface.select(SelectionOperation.ALL)
                try { surface.copySelection() } finally { surface.select(SelectionOperation.CLEAR) }
            }
            if (copied.contains(value)) return
            SystemClock.sleep(20)
        }
        fail("等待真实 VT 解卷文本 $value: $copied")
    }
    private fun open() {
        ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10_000) { model().active?.state?.connected == true }
        ui.waitUntil { find(ui.activity.window.decorView)?.currentFrame != null }
        ui.runOnIdle {
            terminal().commitText(NativePtyFixture.initialize()+"stty -echo; PS1=''; printf '\\033[2J\\033[HLIFECYCLE_READY\\r\\n'\r")
        }
        ui.waitUntil(10_000) { terminal().currentFrame?.line(0)=="LIFECYCLE_READY" }
    }
    @After fun close() {
        ui.runOnIdle { model().sessions.map { it.id }.forEach(model()::close) }
        restoreCtrl?.let { previous ->
            val settings = (ui.activity.application as GhostermApplication).settings
            val result = runBlocking { settings.save(java.util.UUID.randomUUID(), listOf(PreferenceEdit.CtrlMode(previous))) }
            assertTrue(result is com.hanxi.ghosterm.data.PreferenceUpdate.Saved)
        }
    }

    @Test fun activityRecreationRetainsOwnerPidAndKeyboardResizesTheSamePty() {
        open()
        lateinit var original: SessionsViewModel
        var pid = ""; var rows = 0
        ui.runOnIdle {
            original = model()
            terminal().commitText("stty -echo; printf '\\033[2J\\033[HPID:%s\\r\\n' \"\u0024\u0024\"\r")
        }
        val captured = capturePid()
        pid = captured.first; rows = captured.second
        ui.activityRule.scenario.recreate()
        ui.runOnIdle { assertSame(original, model()) }
        ui.waitUntil { find(ui.activity.window.decorView)?.currentFrame != null }
        ui.runOnIdle {
            assertSame(original, model())
            terminal().commitText("printf '\\033[2J\\033[HPID:%s\\r\\n' \"\u0024\u0024\"\r")
        }
        waitFor(pid)
        ui.onNodeWithTag("terminal-keyboard").performClick()
        ui.waitUntil(10_000) { terminal().currentFrame!!.rows < rows }
        ui.onNodeWithContentDescription("收起键盘").assertIsDisplayed()
        var smaller = 0; var cols = 0
        ui.runOnIdle { val f = terminal().currentFrame!!; smaller = f.rows; cols = f.cols
            terminal().commitText("printf '\\033[2J\\033[H'; stty size; printf 'KEYBOARD_SIZE\\r\\n'\r") }
        waitFor("KEYBOARD_SIZE")
        ui.runOnIdle { assertTrue(text(), text().contains("$smaller $cols")) }
        ui.onNodeWithTag("terminal-keyboard").performClick()
        ui.waitUntil(10_000) { terminal().currentFrame!!.rows == rows }
        ui.onNodeWithContentDescription("显示键盘").assertIsDisplayed()
        ui.runOnIdle { assertSame(original, model()) }
    }

    @Test fun newTerminalReceivesPhysicalKeysAndTopBackHidesImeBeforeLeaving() {
        open()
        val original = model().active!!.id
        // Remove the initial prompt/path, so a stale prompt cannot satisfy pwd.
        ui.runOnIdle { model().active!!.connection.commit("stty echo; PS1=''; printf '\\033[3J\\033[2J\\033[HFOCUS_READY\\r\\n'\r") }
        waitFor("FOCUS_READY")
        // No terminal tap or showKeyboard call before physical input.
        ui.runOnIdle { assertTrue("new terminal owns keyboard focus", terminal().hasFocus()) }
        val instrumentation = androidx.test.platform.app.InstrumentationRegistry.getInstrumentation()
        for (key in listOf(android.view.KeyEvent.KEYCODE_P, android.view.KeyEvent.KEYCODE_W,
            android.view.KeyEvent.KEYCODE_D, android.view.KeyEvent.KEYCODE_ENTER)) instrumentation.sendKeyDownUpSync(key)
        // A real IME may consume the first Enter to commit its candidate (e.g.
        // Gboard Pinyin commits "pwd" without a newline). Do not require a
        // particular installed IME/subtype or execute a composition as a command.
        // The next Enter executes; direct single-Enter encoding has separate tests.
        ui.waitUntil(10_000) { ui.runOnIdle { text().contains("pwd") || terminal().composition=="pwd" } }
        instrumentation.sendKeyDownUpSync(android.view.KeyEvent.KEYCODE_ENTER)
        waitForUnwrapped("/files/home")
        if (ui.onAllNodesWithContentDescription("显示键盘").fetchSemanticsNodes().isNotEmpty())
            ui.onNodeWithTag("terminal-keyboard").performClick()
        ui.waitUntil(10_000) { ui.onAllNodesWithContentDescription("收起键盘").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("terminal-back").performClick()
        ui.waitUntil(10_000) { ui.onAllNodesWithContentDescription("显示键盘").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
        assertEquals(original, model().active!!.id)
        ui.onNodeWithTag("terminal-back").performClick()
        ui.onNodeWithText("继续本地终端").assertIsDisplayed()
        assertEquals(original, model().active!!.id)
    }

    @Test fun filesNavigationPreservesOwnerPidAndResetsTerminalInputState() {
        open()
        val original=model().active!!
        ui.runOnIdle { terminal().commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HFILES_PID:%s\\r\\n' \"\u0024\u0024\"\r") }
        ui.waitUntil(10_000) { ui.runOnIdle { terminal().currentFrame?.line(0)?.matches(Regex("FILES_PID:[0-9]+"))==true } }
        val pid=ui.runOnIdle { terminal().currentFrame!!.line(0) }
        ui.onAllNodesWithTag("terminal-sessions").assertCountEquals(1)
        ui.onAllNodesWithTag("terminal-keyboard").assertCountEquals(1)
        val settings = (ui.activity.application as GhostermApplication).settings
        restoreCtrl = settings.state.value.committed.ctrlMode
        val saved = runBlocking { settings.save(java.util.UUID.randomUUID(), listOf(PreferenceEdit.CtrlMode(com.hanxi.ghosterm.domain.CtrlBehavior.LOCKED))) }
        assertTrue(saved is com.hanxi.ghosterm.data.PreferenceUpdate.Saved)
        ui.waitUntil { ui.runOnIdle { model().terminalControls.lockedCtrl } }
        ui.onNodeWithText("Ctrl").performClick()
        ui.onNodeWithText("Ctrl 锁定").assertIsDisplayed()
        ui.onNodeWithTag("terminal-files").performClick()
        ui.onNodeWithTag("terminal-surface").assertDoesNotExist()
        ui.onNodeWithTag("file-location").assertIsDisplayed()
        ui.runOnIdle { assertSame(original,model().active);assertTrue(original.state.connected) }
        ui.activityRule.scenario.recreate()
        ui.onNodeWithTag("file-location").assertIsDisplayed()
        ui.onNodeWithText("返回").performClick()
        ui.waitUntil { ui.runOnIdle { find(ui.activity.window.decorView)?.currentFrame!=null } }
        ui.runOnIdle {
            assertSame(original,model().active);assertEquals(1,model().sessions.size)
            assertEquals(0,terminal().ctrl);assertTrue(terminal().hasFocus())
            terminal().commitText("printf '\\033[2J\\033[HFILES_PID:%s\\r\\n' \"\u0024\u0024\"\r")
        }
        ui.waitUntil(10_000) { ui.runOnIdle { terminal().currentFrame?.line(0)==pid } }
        ui.onNodeWithTag("terminal-back").performClick()
        ui.onNodeWithText("继续本地终端").assertIsDisplayed()
    }

    @Test fun returningFromBackgroundConsumesOutputWithoutReplacingTheSession() {
        open()
        val original = model().active!!.id
        // A separate foreground process remains JOB until every output byte is
        // written. A builtin shell loop would misleadingly report IDLE while busy.
        ui.runOnIdle { terminal().commitText("stty -echo; printf '\\033[2J\\033[H'; sh -c 'read token; seq 1 100000'; printf 'BACKGROUND_EXIT:%d\\r\\nBACKGROUND_DONE\\r\\n' \"\u0024?\"\r") }
        ui.waitUntil(10_000) { model().active?.state?.child == 3 }
        ui.onNodeWithTag("terminal-back").performClick()
        ui.onNodeWithTag("terminal-surface").assertDoesNotExist()
        // Release the child's input barrier only after the View is gone. Its
        // >400 KiB output exceeds the PTY buffer, so it cannot return without IO.
        ui.runOnIdle { model().active!!.surface.commit("start\n") }
        ui.waitUntil(10_000) { model().active?.state?.child == 2 }
        ui.onNodeWithText("继续本地终端").performClick()
        waitFor("BACKGROUND_DONE")
        ui.runOnIdle { assertTrue(text(), text().contains("BACKGROUND_EXIT:0")); assertTrue(text(), text().contains("100000")) }
        assertEquals(original, model().active!!.id)
    }
}
