package com.hanxi.ghosterm

import android.app.Application
import android.content.ClipData
import android.content.ClipboardManager
import android.view.KeyEvent
import androidx.test.espresso.Espresso.onView
import androidx.test.espresso.matcher.ViewMatchers.withText
import androidx.test.espresso.matcher.ViewMatchers.withContentDescription
import androidx.test.espresso.action.ViewActions.click
import androidx.test.espresso.action.GeneralClickAction
import androidx.test.espresso.action.Tap
import androidx.test.espresso.action.Press
import androidx.test.espresso.action.CoordinatesProvider
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
        ui.runOnIdle { terminal().commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HUI_SHELL_READY'\r") }
        ui.waitUntil(10_000) { terminal().currentFrame?.line(0) == "UI_SHELL_READY" }

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

    private fun clickSystemText(label: String, click: Boolean = true) {
        val automation = InstrumentationRegistry.getInstrumentation().uiAutomation
        automation.serviceInfo = automation.serviceInfo.apply {
            flags = flags or android.accessibilityservice.AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
        }
        val deadline = android.os.SystemClock.uptimeMillis() + 5_000
        var labels = emptyList<String>()
        while (android.os.SystemClock.uptimeMillis() < deadline) {
            val nodes = mutableListOf<android.view.accessibility.AccessibilityNodeInfo>()
            fun visit(node: android.view.accessibility.AccessibilityNodeInfo?) {
                if (node == null) return
                nodes.add(node)
                for (i in 0 until node.childCount) visit(node.getChild(i))
            }
            automation.windows.forEach { visit(it.root) }
            labels = nodes.map { "${it.text}/${it.contentDescription}" }
            nodes.firstOrNull { it.isVisibleToUser && (it.text?.toString() == label || it.contentDescription?.toString() == label) }?.let { match ->
                if (!click) return
                var node: android.view.accessibility.AccessibilityNodeInfo? = match
                while (node != null) {
                    if (node.isClickable && node.performAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_CLICK)) return
                    node = node.parent
                }
            }
            android.os.SystemClock.sleep(50)
        }
        var diagnostic = ""
        ui.runOnIdle {
            val view = terminal()
            diagnostic = listOf("actionMode", "selecting", "dragging").joinToString { name ->
                val field = view.javaClass.getDeclaredField(name).apply { isAccessible = true }
                "$name=${field.get(view)}"
            }
        }
        fail("System text action $label missing ($diagnostic): $labels")
    }
    private fun longPressCell(x: Int, y: Int) {
        onView(withContentDescription("终端")).perform(GeneralClickAction(Tap.LONG, CoordinatesProvider { view ->
            val terminal = view as TerminalView; val frame = terminal.currentFrame!!
            val xy = IntArray(2); view.getLocationOnScreen(xy)
            floatArrayOf(xy[0] + (x + 0.5f) * (view.width / frame.cols), xy[1] + (y + 0.5f) * (view.height / frame.rows))
        }, Press.FINGER))
    }
    private fun clipboard(text: String) = ui.runOnIdle {
        ui.activity.getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("fixture", text))
    }
    private fun pasteKey() = ui.runOnIdle {
        terminal().onKeyDown(KeyEvent.KEYCODE_V, KeyEvent(0, 0, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_V, 0,
            KeyEvent.META_CTRL_ON or KeyEvent.META_SHIFT_ON))
    }

    @Test fun longPressPaintsSelectionAndCopiesRealTextToSystemClipboard() {
        clipboard("clipboard-before-copy")
        send("stty -echo; printf '\\033[2J\\033[HCOPY_THIS second'; sleep 30\r")
        waitFor("COPY_THIS")
        longPressCell(1, 0)
        ui.waitUntil { terminal().currentFrame!!.cells.any { it.flags and 256 != 0 } }
        ui.runOnIdle {
            val view = terminal(); val image = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(image)); var selected = 0
            for (y in 0 until image.height step 2) for (x in 0 until image.width step 2)
                if (image.getPixel(x, y) == Color.rgb(68, 91, 125)) selected++
            image.recycle(); assertTrue("selected cells must be visibly painted", selected > 50)
        }
        clickSystemText("复制")
        ui.waitUntil {
            ui.activity.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString() == "COPY_THIS"
        }
    }

    @Test fun pasteConfirmationUsesCapturedTextAndCancelSendsNothing() {
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004lCLIP_READY\\r\\n'; dd bs=1 count=3 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nCLIP_DONE\\r\\n'\r")
        waitFor("CLIP_READY")
        clipboard("X\n"); pasteKey()
        clickSystemText("粘贴可能执行命令", false)
        onView(withText("取消")).perform(click())
        clipboard("a\nb"); pasteKey()
        clickSystemText("粘贴可能执行命令", false)
        onView(withText("粘贴可能执行命令")).check(androidx.test.espresso.assertion.ViewAssertions.matches(androidx.test.espresso.matcher.ViewMatchers.isDisplayed()))
        clipboard("changed")
        onView(withText("仍然粘贴")).perform(click())
        waitFor("CLIP_DONE")
        ui.runOnIdle { assertTrue(text(), text().contains("61 0d 62")); assertFalse(text(), text().contains("58")) }
    }

    @Test fun fileClipboardIsNotCoercedIntoTerminalInput() {
        send("stty raw -echo; printf '\\033[2J\\033[HFILE_READY\\r\\n'; dd bs=1 count=1 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nFILE_DONE\\r\\n'\r")
        waitFor("FILE_READY")
        ui.runOnIdle { ui.activity.getSystemService(ClipboardManager::class.java).setPrimaryClip(
            ClipData.newRawUri("file", android.net.Uri.parse("content://fixture/private-file"))) }
        pasteKey()
        ui.onNodeWithText("剪贴板中没有可粘贴的文本").assertIsDisplayed()
        send("Z"); waitFor("FILE_DONE")
        ui.runOnIdle { assertTrue(text(), text().contains("5a")) }
    }

    @Test fun clearMenuConfirmsLocalScopeAndKeepsRunningCommand() {
        send("stty raw -echo; printf '\\033[2J\\033[HLOCAL_CLEAR_READY\\r\\n'; dd bs=1 count=1 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nLOCAL_CLEAR_DONE\\r\\n'\r")
        waitFor("LOCAL_CLEAR_READY")
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithText("清除本地显示").performClick()
        ui.onNodeWithText("取消").performClick()
        ui.runOnIdle { assertTrue(text().contains("LOCAL_CLEAR_READY")) }
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithText("清除本地显示").performClick()
        ui.onNodeWithText("清除").performClick()
        ui.waitUntil { terminal().currentFrame!!.cells.all { it.text.isEmpty() } }
        send("Z"); waitFor("LOCAL_CLEAR_DONE")
        ui.runOnIdle { assertTrue(text(), text().contains("5a")); assertTrue(sessions.active!!.state.connected) }
    }

    @Test fun selectionEndHandleChangesCopiedRangeThroughRealTouch() {
        clipboard("selection-before-copy")
        send("stty -echo; printf '\\033[2J\\033[Halpha beta'; sleep 30\r")
        waitFor("alpha beta"); longPressCell(1, 0)
        ui.waitUntil { terminal().currentFrame!!.cells[4].flags and 256 != 0 }
        var downX = 0f; var downY = 0f; var endX = 0f; var endY = 0f
        ui.runOnIdle {
            val view = terminal(); val f = view.currentFrame!!; val pos = IntArray(2); view.getLocationOnScreen(pos)
            val cw = view.width / f.cols; val ch = view.height / f.rows
            downX = pos[0] + cw * 5f; downY = pos[1] + ch.toFloat()
            endX = pos[0] + cw * 8.5f; endY = pos[1] + ch * 0.5f
        }
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val now = android.os.SystemClock.uptimeMillis()
        fun touch(action: Int, x: Float, y: Float) {
            val event = android.view.MotionEvent.obtain(now, android.os.SystemClock.uptimeMillis(), action, x, y, 0)
            instrumentation.sendPointerSync(event); event.recycle()
        }
        touch(android.view.MotionEvent.ACTION_DOWN, downX, downY)
        for (i in 1..8) {
            touch(android.view.MotionEvent.ACTION_MOVE, downX + (endX - downX) * i / 8, downY + (endY - downY) * i / 8)
            android.os.SystemClock.sleep(20)
        }
        touch(android.view.MotionEvent.ACTION_UP, endX, endY)
        ui.waitUntil { terminal().currentFrame!!.cells[8].flags and 256 != 0 }
        clickSystemText("复制")
        ui.waitUntil { ui.activity.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString() == "alpha bet" }
    }

    @Test fun inputConnectionContextPasteUsesBracketedEncoding() {
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004hCONTEXT_READY\\r\\n'; dd bs=1 count=15 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nCONTEXT_DONE\\r\\n'\r")
        waitFor("CONTEXT_READY"); clipboard("abc")
        ui.runOnIdle { assertTrue(terminal().onCreateInputConnection(EditorInfo()).performContextMenuAction(android.R.id.paste)) }
        waitFor("CONTEXT_DONE")
        ui.runOnIdle { assertTrue(text(), text().replace(Regex("\\s+"), " ").contains("1b 5b 32 30 30 7e 61 62 63 1b 5b 32 30 31 7e")) }
    }

    @Test fun accessibilityLongClickSelectsAndContextCopyUsesTheSelection() {
        send("stty -echo; printf '\\033[2J\\033[HACCESSIBLE\\033[1;2H'; sleep 30\r")
        waitFor("ACCESSIBLE"); clipboard("before-accessibility-copy")
        ui.runOnIdle {
            assertTrue("advertised accessibility action must actually work", terminal().performAccessibilityAction(
                android.view.accessibility.AccessibilityNodeInfo.ACTION_LONG_CLICK, null))
        }
        ui.waitUntil { terminal().currentFrame!!.cells[0].flags and 256 != 0 }
        ui.runOnIdle { assertTrue(terminal().onCreateInputConnection(EditorInfo()).performContextMenuAction(android.R.id.copy)) }
        ui.waitUntil { ui.activity.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString() == "ACCESSIBLE" }
    }
}
