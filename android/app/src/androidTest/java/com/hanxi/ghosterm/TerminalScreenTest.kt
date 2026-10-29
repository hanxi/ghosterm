package com.hanxi.ghosterm

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import androidx.activity.ComponentActivity
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class TerminalScreenTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private lateinit var directory: File
    private lateinit var sessions: SessionsViewModel
    @Before fun setup() {
        directory = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "terminal-ui-${UUID.randomUUID()}").apply { mkdirs() }
        val app = object : Application() { override fun getFilesDir() = directory }
        val hosts = HostsViewModel(app)
        sessions = SessionsViewModel(app)
        ui.setContent { GhostermApp(hosts, RuntimeInfoViewModel(), sessions) }
        ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10_000) { sessions.active?.state?.connected == true }
        ui.waitUntil(10_000) { viewOrNull()?.currentFrame != null }
    }
    @After fun cleanup() {
        ui.runOnIdle { sessions.sessions.map { it.id }.forEach(sessions::close) }
        directory.deleteRecursively()
    }
    private fun find(view: View): TerminalView? {
        if (view is TerminalView) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun viewOrNull(): TerminalView? {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) return find(ui.activity.window.decorView)
        var terminal: TerminalView? = null
        InstrumentationRegistry.getInstrumentation().runOnMainSync { terminal = find(ui.activity.window.decorView) }
        return terminal
    }
    private fun terminal() = checkNotNull(viewOrNull())
    private fun text(): String = terminal().currentFrame!!.let { frame -> (0 until frame.rows).joinToString("\n", transform = frame::line) }
    private fun send(command: String) { ui.runOnIdle { assertTrue(terminal().commitText(command)) } }
    private fun waitFor(value: String) {
        val end = android.os.SystemClock.uptimeMillis() + 10_000
        while (android.os.SystemClock.uptimeMillis() < end) {
            if (text().contains(value)) return
            android.os.SystemClock.sleep(20)
        }
        throw AssertionError("等待 $value: ${text()}; state=${sessions.active?.connection?.state()}")
    }

    @Test fun rendersRealAnsiPixelsAndHasOneKeyboardControl() {
        send("stty -echo; printf '\\033[41m\\033[2J\\033[HPIXEL_READY'\r")
        waitFor("PIXEL_READY")
        ui.onAllNodesWithTag("terminal-keyboard").assertCountEquals(1)
        ui.onNodeWithText("新建本地会话").assertDoesNotExist()
        var red = 0; var count = 0
        ui.runOnIdle {
            val view = terminal()
            val image = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(image))
            for (y in 0 until image.height step 4) for (x in 0 until image.width step 4) {
                val color = image.getPixel(x, y); count++
                if (Color.red(color) > Color.green(color) + 40 && Color.red(color) > Color.blue(color) + 40) red++
            }
            image.recycle()
        }
        assertTrue("VT red background must be painted ($red/$count)", red > count / 2)
        send("printf '\\033[42m\\033[2J\\033[HGREEN_READY'\r")
        waitFor("GREEN_READY")
        ui.runOnIdle {
            val view = terminal(); val image = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(image)); var redAfter = 0
            for (y in 0 until image.height step 4) for (x in 0 until image.width step 4) {
                val color = image.getPixel(x, y)
                if (Color.red(color) > Color.green(color) + 40 && Color.red(color) > Color.blue(color) + 40) redAfter++
            }
            image.recycle(); assertEquals(0, redAfter)
        }
    }

    @Test fun canvasPaintingIsClippedToTerminalBounds() {
        send("stty -echo; printf '\\033[41m\\033[2J\\033[HCLIP_READY'\r")
        waitFor("CLIP_READY")
        ui.runOnIdle {
            val view = terminal()
            val image = Bitmap.createBitmap(view.width + 40, view.height + 40, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(image)
            canvas.drawColor(Color.GREEN)
            canvas.translate(20f, 20f)
            view.draw(canvas)
            for (x in listOf(1, image.width / 2, image.width - 2)) {
                assertEquals("terminal must not paint toolbar/status margins", Color.GREEN, image.getPixel(x, 1))
                assertEquals("terminal must not paint navigation margin", Color.GREEN, image.getPixel(x, image.height - 2))
            }
            assertNotEquals("fixture must draw terminal content", Color.GREEN, image.getPixel(30, 30))
            image.recycle()
        }
    }

    @Test fun imeCompositionCancellationAndDeleteReachRealPtyCorrectly() {
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004hIME_READY\\r\\n'; dd bs=1 count=12 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_DONE\\r\\n'\r")
        waitFor("IME_READY")
        ui.runOnIdle {
            val view = terminal(); val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("草稿😀", 1))
            assertEquals("草稿😀", view.composition)
            assertTrue(connection.deleteSurroundingTextInCodePoints(1, 0))
            assertEquals("草稿", view.composition)
            assertTrue(connection.finishComposingText())
            assertEquals("", view.composition)
            assertTrue(connection.setComposingText("中wen", 1))
            assertTrue(connection.commitText("中文", 1))
            assertEquals("", view.composition)
            assertTrue(connection.deleteSurroundingText(1, 1))
            assertTrue(connection.sendKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, android.view.KeyEvent.KEYCODE_TAB)))
            connection.closeConnection()
            assertFalse(connection.commitText("不可发送", 1))
        }
        waitFor("IME_DONE")
        assertTrue(text(), text().replace(Regex("\\s+"), " ").contains("e4 b8 ad e6 96 87 7f 1b 5b 33 7e 09"))
    }

    @Test fun returningHomePreservesShellAndViewUsesActualSize() {
        val id = sessions.active!!.id
        send("stty -echo; printf '\\033[2J\\033[HPID:%s\\r\\n' \"\u0024\u0024\"; stty size\r")
        waitFor("PID:")
        val first = terminal().currentFrame!!
        val pid = first.line(0)
        waitFor("${first.rows} ${first.cols}")
        ui.onNodeWithText("主机", useUnmergedTree = true).performClick()
        ui.onNodeWithText("继续本地终端").assertIsDisplayed()
        ui.onNodeWithTag("terminal-surface").assertDoesNotExist()
        assertEquals(id, sessions.active!!.id)
        ui.onNodeWithText("继续本地终端").performClick()
        ui.waitUntil(10_000) { viewOrNull()?.currentFrame != null }
        send("printf '\\033[2J\\033[HPID:%s\\r\\n' \"\u0024\u0024\"\r")
        waitFor(pid)
        assertEquals(id, sessions.active!!.id)
        assertEquals(pid, terminal().currentFrame!!.line(0))
    }

    @Test fun closeRequiresConfirmationAndSessionPanelCreatesIndependentSession() {
        val original = sessions.active!!.id
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithText("关闭会话").performClick()
        ui.onNodeWithText("取消").performClick()
        assertEquals(original, sessions.active!!.id)
        ui.onNodeWithText("会话 · 1").performClick()
        ui.onNodeWithText("新建本地会话").performClick()
        ui.waitUntil(10_000) { sessions.sessions.size == 2 && sessions.active?.state?.connected == true }
        assertNotEquals(original, sessions.active!!.id)
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithText("关闭会话").performClick()
        ui.onNodeWithText("关闭会话", useUnmergedTree = true).performClick()
        ui.waitUntil { sessions.sessions.size == 1 }
        assertEquals(original, sessions.active!!.id)
    }
}
