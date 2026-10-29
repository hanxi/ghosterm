package com.hanxi.ghosterm

import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.After
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** Production Activity and its real ViewModelStore, not a retained test-owned model. */
@RunWith(AndroidJUnit4::class)
class TerminalLifecycleTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
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
        ui.runOnIdle { fail("等待 $value: ${text()}") }
    }
    private fun open() {
        ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10_000) { model().active?.state?.connected == true }
        ui.waitUntil { find(ui.activity.window.decorView)?.currentFrame != null }
    }
    @After fun close() { ui.runOnIdle { model().sessions.map { it.id }.forEach(model()::close) } }

    @Test fun activityRecreationRetainsOwnerPidAndKeyboardResizesTheSamePty() {
        open()
        lateinit var original: SessionsViewModel
        var pid = ""; var rows = 0
        ui.runOnIdle {
            original = model()
            terminal().commitText("stty -echo; printf '\\033[2J\\033[HPID:%s\\r\\n' \"\u0024\u0024\"\r")
        }
        waitFor("PID:")
        ui.runOnIdle { pid = terminal().currentFrame!!.line(0); rows = terminal().currentFrame!!.rows }
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
        ui.onNodeWithText("收起键盘").assertIsDisplayed()
        var smaller = 0; var cols = 0
        ui.runOnIdle { val f = terminal().currentFrame!!; smaller = f.rows; cols = f.cols
            terminal().commitText("printf '\\033[2J\\033[H'; stty size; printf 'KEYBOARD_SIZE\\r\\n'\r") }
        waitFor("KEYBOARD_SIZE")
        ui.runOnIdle { assertTrue(text(), text().contains("$smaller $cols")) }
        ui.onNodeWithTag("terminal-keyboard").performClick()
        ui.waitUntil(10_000) { terminal().currentFrame!!.rows == rows }
        ui.onNodeWithText("显示键盘").assertIsDisplayed()
        ui.runOnIdle { assertSame(original, model()) }
    }

    @Test fun newTerminalReceivesPhysicalKeysAndTopBackHidesImeBeforeLeaving() {
        open()
        val original = model().active!!.id
        // No terminal tap or showKeyboard call before physical input.
        ui.runOnIdle { assertTrue("new terminal owns keyboard focus", terminal().hasFocus()) }
        val instrumentation = androidx.test.platform.app.InstrumentationRegistry.getInstrumentation()
        for (key in listOf(android.view.KeyEvent.KEYCODE_P, android.view.KeyEvent.KEYCODE_W,
            android.view.KeyEvent.KEYCODE_D, android.view.KeyEvent.KEYCODE_ENTER)) instrumentation.sendKeyDownUpSync(key)
        waitFor("/files/home")
        if (ui.onAllNodesWithText("显示键盘").fetchSemanticsNodes().isNotEmpty())
            ui.onNodeWithTag("terminal-keyboard").performClick()
        ui.waitUntil(10_000) { ui.onAllNodesWithText("收起键盘").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("terminal-back").performClick()
        ui.waitUntil(10_000) { ui.onAllNodesWithText("显示键盘").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
        assertEquals(original, model().active!!.id)
        ui.onNodeWithTag("terminal-back").performClick()
        ui.onNodeWithText("继续本地终端").assertIsDisplayed()
        assertEquals(original, model().active!!.id)
    }

    @Test fun returningFromBackgroundConsumesOutputWithoutReplacingTheSession() {
        open()
        val original = model().active!!.id
        // A separate foreground process remains JOB until every output byte is
        // written. A builtin shell loop would misleadingly report IDLE while busy.
        ui.runOnIdle { terminal().commitText("stty -echo; printf '\\033[2J\\033[H'; sh -c 'read token; seq 1 100000'; printf 'BACKGROUND_EXIT:%d\\r\\nBACKGROUND_DONE\\r\\n' \"\u0024?\"\r") }
        ui.waitUntil(10_000) { model().active?.state?.child == 3 }
        ui.onNodeWithText("主机", useUnmergedTree = true).performClick()
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
