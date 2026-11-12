package com.hanxi.ghosterm

import android.content.ClipData
import android.content.ClipboardManager
import android.os.SystemClock
import android.util.Base64
import android.view.View
import android.view.ViewGroup
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.action.ViewActions.click
import androidx.test.espresso.matcher.ViewMatchers.withText
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.After
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class TerminalOSC52ProductTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private fun model() = ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun find(view: View): TerminalView? {
        if (view is TerminalView) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun terminal() = checkNotNull(find(ui.activity.window.decorView))
    private fun clipboard() = ui.activity.getSystemService(ClipboardManager::class.java)
    private fun open() {
        ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10_000) { model().active?.state?.connected == true }
        ui.waitUntil { find(ui.activity.window.decorView)?.currentFrame != null }
        ui.runOnIdle { model().active!!.connection.commit(NativePtyFixture.initialize()+"stty raw -echo; PS1=''; printf '\\033[2J\\033[HOSC_UI_READY'\r") }
        ui.waitUntil(10_000) { terminal().currentFrame?.line(0) == "OSC_UI_READY" }
    }
    @After fun close() { ui.runOnIdle { model().sessions.map { it.id }.forEach(model()::close) } }
    @Test fun programWriteReachesSystemClipboard() {
        open()
        val text = "OSC UI 中文 café\nsecond line"
        val encoded = Base64.encodeToString(text.toByteArray(), Base64.NO_WRAP)
        ui.runOnIdle {
            clipboard().setPrimaryClip(ClipData.newPlainText("test", "before"))
            model().active!!.connection.commit("printf '\\033]52;c;$encoded\\007'\r")
        }
        ui.waitUntil(10_000) { ui.runOnIdle { clipboard().primaryClip?.getItemAt(0)?.text?.toString() == text } }
    }
    private fun read(allow: Boolean) {
        open()
        val text = "OSC UI read 中文"
        val expected = "\u001b]52;c;" + (if (allow) Base64.encodeToString(text.toByteArray(), Base64.NO_WRAP) else "") + "\u0007"
        val output = File(ui.activity.cacheDir, "osc-ui-reply-${System.nanoTime()}")
        try {
            ui.runOnIdle {
                clipboard().setPrimaryClip(ClipData.newPlainText("test", text))
                model().active!!.connection.commit("printf '\\033]52;c;?\\007'; dd bs=1 count=${expected.toByteArray().size} of='${output.path}' 2>/dev/null; printf OSC_UI_DONE\r")
            }
            val deadline = SystemClock.uptimeMillis() + 10_000
            while (true) {
                try {
                    onView(withText("允许终端程序读取剪贴板？")).check(androidx.test.espresso.assertion.ViewAssertions.matches(androidx.test.espresso.matcher.ViewMatchers.isDisplayed()))
                    break
                } catch (missing: androidx.test.espresso.NoMatchingViewException) {
                    if (SystemClock.uptimeMillis() >= deadline) throw missing
                    SystemClock.sleep(20)
                }
            }
            assertEquals(0L, output.length())
            onView(withText(if (allow) "允许一次" else "拒绝")).perform(click())
            ui.waitUntil(10_000) { output.exists() && output.length() == expected.toByteArray().size.toLong() }
            assertArrayEquals(expected.toByteArray(), output.readBytes())
        } finally { output.delete() }
    }
    @Test fun readRequiresOneTimeApproval() = read(true)
    @Test fun rejectedReadDoesNotDiscloseText() = read(false)
}
