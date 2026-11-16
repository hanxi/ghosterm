package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.AndroidComposeTestRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.rules.ActivityScenarioRule
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.Assert.*
import java.io.File
import java.util.UUID

/** Uses real PTYs, window insets, the platform IME and OS key-event injection. */
internal class PluginTerminalReturnProbe(
    private val ui: AndroidComposeTestRule<ActivityScenarioRule<MainActivity>, MainActivity>
) {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    val sessions get() = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private val owners = mutableListOf<UUID>()
    private val proofs = mutableListOf<File>()
    private fun find(view: View): TerminalView? = (view as? TerminalView) ?: (view as? ViewGroup)?.let { group ->
        (0 until group.childCount).firstNotNullOfOrNull { find(group.getChildAt(it)) }
    }
    fun terminal() = checkNotNull(find(ui.activity.window.decorView))
    fun await(message: String, condition: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 10000
        while (SystemClock.elapsedRealtime() < deadline) {
            ui.mainClock.advanceTimeByFrame()
            if (condition()) return
            SystemClock.sleep(25)
        }
        assertTrue(message, condition())
    }
    fun settle() { repeat(20) { ui.mainClock.advanceTimeByFrame(); SystemClock.sleep(40) }; ui.waitForIdle() }
    private fun shell(command: String) = instrumentation.uiAutomation.executeShellCommand(command).use {
        ParcelFileDescriptor.AutoCloseInputStream(it).bufferedReader().use { reader -> reader.readText() }
    }
    private fun type(value: String) {
        // UiAutomation uses Runtime.exec, so shell quoting would be typed literally.
        value.chunked(8).forEach { shell("input text " + it.replace(" ", "%s")) }
        shell("input keyevent 66")
    }
    fun own(session: TerminalSession) { owners += session.id }
    fun begin(): TerminalSession {
        ui.onNodeWithText("本地终端").performClick()
        await("real local terminal did not attach") { ui.runOnIdle {
            sessions.active?.let { it.state.connected && find(ui.activity.window.decorView)?.surface === it.surface } == true
        } }
        val original = ui.runOnIdle { sessions.active!! }; own(original)
        ui.runOnIdle { assertTrue(terminal().commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HRETURN_READY'\r")) }
        return original
    }
    fun keyboard(shown: Boolean) {
        if (shown) ui.showTerminalKeyboard() else {
            ui.runOnIdle { terminal().hideKeyboard() }
            await("actual IME did not close") { !terminalImeShown() }
        }
        settle(); assertEquals("precondition: actual IME visibility", shown, terminalImeShown())
    }
    fun leaveToPlugins(shown: Boolean) {
        keyboard(shown)
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithTag("terminal-menu-home").performScrollTo().performClick()
        ui.onNodeWithContentDescription("设置").performClick()
        ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
        ui.onNodeWithTag("plugin-versions-disclosure").assertExists()
    }
    fun returnFromPlugins(original: TerminalSession, shown: Boolean, systemBack: Boolean = false) {
        repeat(2) { if (systemBack) ui.terminalBack() else ui.onNodeWithContentDescription("返回").performClick() }
        ui.onNodeWithText("${if (original.state.terminal) "查看" else "继续"} ${original.title}").performClick()
        assertReturned(original, shown)
    }
    fun assertReturned(original: TerminalSession, shown: Boolean) {
        await("return must attach the original PTY and native input focus") { ui.runOnIdle {
            sessions.visible && sessions.active === original && terminal().surface === original.surface &&
                terminal().hasFocus() && terminal().hasWindowFocus()
        } }
        if (shown) ui.awaitTerminalKeyboard() else {
            settle(); assertFalse("return must not summon a previously hidden IME", terminalImeShown())
        }
    }
    fun inputProof(original: TerminalSession) {
        val home = File(instrumentation.targetContext.filesDir, "home")
        val expected = File(home, "owner-${UUID.randomUUID()}.txt"); proofs += expected
        val actual = File(home, "return-${UUID.randomUUID()}.txt"); proofs += actual
        ui.runOnIdle { original.connection.commit("echo \u0024\u0024 > '${expected.absolutePath}'\r") }
        await("original shell did not record its PID") { expected.isFile && expected.readText().trim().toIntOrNull() != null }
        type("echo \u0024\u0024 > ${actual.name}")
        await("OS input events did not reach the original shell") { actual.isFile && actual.readText() == expected.readText() }
        assertSame(original, ui.runOnIdle { sessions.active }); assertTrue(original.connection.state().connected)
    }
    fun close() {
        ui.runOnIdle { find(ui.activity.window.decorView)?.hideKeyboard(); owners.forEach(sessions::close) }
        proofs.forEach { it.delete() }
    }
}
