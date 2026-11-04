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
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.CtrlBehavior
import kotlinx.coroutines.runBlocking

@RunWith(AndroidJUnit4::class)
class TerminalScreenTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private lateinit var directory: File
    private lateinit var sessions: SessionsViewModel
    private lateinit var preferences: AppSettings
    @Before fun setup() {
        directory = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "terminal-ui-${UUID.randomUUID()}").apply { mkdirs() }
        val app = object : Application() { init { attachBaseContext(InstrumentationRegistry.getInstrumentation().targetContext) }; override fun getFilesDir() = directory }
        val hosts = HostsViewModel(app)
        val runtime = RuntimeInfoViewModel()
        preferences = AppSettings(PreferencesRepository(PreferencesFile(File(directory, "preferences"))))
        sessions = SessionsViewModel(app, settings = preferences)
        ui.setContent { GhostermApp(hosts, runtime, sessions) }
        ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10_000) { ui.runOnIdle { sessions.active?.state?.connected == true } }
        ui.waitUntil(10_000) { viewOrNull()?.currentFrame != null }
        awaitReadyViewport()
        ui.runOnIdle { terminal().commitText(NativePtyFixture.initialize()+"stty -echo; PS1=''; printf '\\033[2J\\033[HUI_SHELL_READY'\r") }
        ui.waitUntil(10_000) { terminal().currentFrame?.line(0) == "UI_SHELL_READY" }

    }
    @After fun cleanup() {
        ui.runOnIdle { sessions.sessions.map { it.id }.forEach(sessions::close) }
        preferences.close()
        runBlocking { preferences.awaitClosed() }
        directory.deleteRecursively()
    }
    private fun lockCtrlMode() {
        val result = runBlocking { preferences.save(UUID.randomUUID(), listOf(PreferenceEdit.CtrlMode(CtrlBehavior.LOCKED))) }
        assertTrue(result is PreferenceUpdate.Saved)
        ui.waitUntil { ui.runOnIdle { sessions.terminalControls.lockedCtrl } }
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

    private fun hideMouseTestKeyboard() {
        ui.runOnIdle { terminal().hideKeyboard() }
        ui.waitUntil(10_000) {
            androidx.core.view.ViewCompat.getRootWindowInsets(terminal())
                ?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime()) == false
        }
        awaitReadyViewport()
    }
    private fun awaitReadyViewport() {
        // The initial VT snapshot may still have its default 24 rows while
        // Android is applying the full-screen window's final geometry.
        ui.waitUntil(10_000) { ui.runOnIdle {
            val v=terminal();val style=sessions.terminalStyle
            val line=kotlin.math.ceil(android.util.TypedValue.applyDimension(
                android.util.TypedValue.COMPLEX_UNIT_SP,style.fontSize,v.resources.displayMetrics)*style.lineHeight).toInt()
            val safeTop=androidx.core.view.ViewCompat.getRootWindowInsets(v)
                ?.getInsets(androidx.core.view.WindowInsetsCompat.Type.captionBar())?.top ?: 0
            val top=IntArray(2).also(v::getLocationOnScreen)[1]
            top==safeTop && v.currentFrame?.rows == (v.height / line).coerceAtMost(256)
        } }
    }

    private fun clickMouseCell(column: Int, row: Int, long: Boolean = false) {
        val coordinates = CoordinatesProvider { view ->
            val t = view as TerminalView
            val frame = t.currentFrame!!
            val location = IntArray(2); t.getLocationOnScreen(location)
            floatArrayOf(location[0] + (column + .5f) * (t.width / frame.cols),
                location[1] + (row + .5f) * (t.height / frame.rows))
        }
        onView(androidx.test.espresso.matcher.ViewMatchers.isAssignableFrom(TerminalView::class.java))
            .perform(GeneralClickAction(if (long) Tap.LONG else Tap.SINGLE, coordinates, Press.FINGER, android.view.InputDevice.SOURCE_TOUCHSCREEN, 0))
    }
    @Test fun mouseModeFingerTapSendsRealCoordinatesAndOpensKeyboard() {
        hideMouseTestKeyboard()
        val packet = File(directory, "mouse-packet")
        val expected = "\u001b[<0;3;4M\u001b[<0;3;4m".toByteArray()
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?1000h\\033[?1006hMOUSE_TAP_READY'; dd bs=1 count=${expected.size} of='${packet.path}' 2>/dev/null; printf '\\033[?1000l\\033[?1006l\\r\\nMOUSE_TAP_DONE'; stty sane\r")
        waitFor("MOUSE_TAP_READY")
        val owner = sessions.active!!.connection
        clickMouseCell(2, 3)
        waitFor("MOUSE_TAP_DONE")
        assertArrayEquals(expected, packet.readBytes())
        try { ui.waitUntil(10_000) { terminalImeShown() } }
        catch (failure: ComposeTimeoutException) { fail("A touchscreen tap in mouse mode must show the actual system IME") }
        ui.runOnIdle { assertSame(owner, sessions.active!!.connection) }
    }

    @Test fun menuShowsKeyboardInMouseModeWithoutSendingTerminalInput() {
        hideMouseTestKeyboard()
        val packet = File(directory, "keyboard-menu-packet")
        val owner = sessions.active!!.connection
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?1000h\\033[?1006hMENU_KEYBOARD_READY'; dd bs=1 count=1 of='${packet.path}' 2>/dev/null; printf '\\033[?1000l\\033[?1006l\\r\\nMENU_KEYBOARD_DONE'; stty sane\r")
        waitFor("MENU_KEYBOARD_READY")
        ui.showTerminalKeyboard()
        androidx.test.espresso.Espresso.pressBack()
        ui.waitUntil(10_000) { !terminalImeShown() }
        ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
        ui.runOnIdle { assertSame(owner, sessions.active!!.connection); assertTrue(terminal().commitText("Z")) }
        waitFor("MENU_KEYBOARD_DONE")
        assertArrayEquals(byteArrayOf(90), packet.readBytes())
    }

    @Test fun wheelReachesMouseProgramWithCoordinatesAndNoReleasePacket() {
        hideMouseTestKeyboard()
        val packet = File(directory, "wheel-packet")
        val expected = "\u001b[<64;3;4M\u001b[<64;3;4M\u001b[<65;3;4M".toByteArray()
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?1000h\\033[?1006hWHEEL_READY'; dd bs=1 count=${expected.size} of='${packet.path}' 2>/dev/null; printf '\\033[?1000l\\033[?1006l\\r\\nWHEEL_DONE'; stty sane\r")
        waitFor("WHEEL_READY")
        ui.runOnIdle {
            val view = terminal(); val frame = view.currentFrame!!
            val x = (2.5f * view.width / frame.cols).toInt()
            val y = (3.5f * view.height / frame.rows).toInt()
            sessions.active!!.surface.wheel(-2, x, y)
            sessions.active!!.surface.wheel(1, x, y)
        }
        waitFor("WHEEL_DONE")
        assertArrayEquals(expected, packet.readBytes())
        assertFalse(terminalImeShown())
    }

    @Test fun fingerSwipeMovesOrdinaryShellHistoryWithoutOpeningKeyboard() {
        hideMouseTestKeyboard()
        send("seq 1 300; printf 'FINGER_SCROLL_DONE'; sleep 30\r")
        waitFor("FINGER_SCROLL_DONE")
        val initial = ui.runOnIdle { terminal().currentFrame!! };val before = initial.line(0)
        // Full-screen edges belong to system bars; scroll inside the viewport.
        ui.onNodeWithTag("terminal-surface").performTouchInput { swipeDown(startY = height * .2f, endY = height * .8f, durationMillis = 400) }
        try { ui.waitUntil(10_000) { terminal().currentFrame!!.line(0) != before } }
        catch (failure: ComposeTimeoutException) { throw AssertionError("Interior swipe must move real shell history",failure) }
        val historical = ui.runOnIdle { terminal().currentFrame!!.line(0) }
        assertFalse(terminalImeShown())
        ui.onNodeWithTag("terminal-surface").performTouchInput { swipeUp(startY = height * .8f, endY = height * .2f, durationMillis = 400) }
        ui.waitUntil(10_000) { terminal().currentFrame!!.line(0) != historical }
        repeat(3) {
            if (ui.runOnIdle { terminal().currentFrame!!.line(0) } != before)
                ui.onNodeWithTag("terminal-surface").performTouchInput { swipeUp(startY = height * .8f, endY = height * .2f, durationMillis = 400) }
        }
        try { ui.waitUntil(10_000) { terminal().currentFrame!!.line(0) == before } }
        catch (failure: ComposeTimeoutException) {
            val diagnostic=ui.runOnIdle { val v=terminal();val f=v.currentFrame!!
                "before=$before rows=${initial.rows} after=${f.line(0)} rows=${f.rows} view=${v.width}x${v.height} cursor=${f.cursorY} visible=${f.cursorVisible} last=${f.line(f.rows-1)} error=${sessions.error} background=${sessions.backgroundError}" }
            throw AssertionError(diagnostic,failure)
        }
    }

    @Test fun fingerSwipeRoutesWheelToTheSameMouseSurface() {
        hideMouseTestKeyboard()
        send("printf '\\033[?1000h\\033[?1006hFINGER_MOUSE_READY'; sleep 30\r")
        waitFor("FINGER_MOUSE_READY")
        val original = sessions.active!!.surface
        val deltas = mutableListOf<Int>()
        ui.runOnIdle {
            terminal().surface = object : com.hanxi.ghosterm.terminal.TerminalSurface by original {
                override fun wheel(lines: Int, x: Int, y: Int, modifiers: Int) {
                    assertTrue(x >= 0 && y >= 0)
                    deltas.add(lines)
                    original.wheel(lines, x, y, modifiers)
                }
            }
        }
        ui.onNodeWithTag("terminal-surface").performTouchInput { swipeDown(startY = height * .2f, endY = height * .8f, durationMillis = 400) }
        ui.runOnIdle { assertTrue(deltas.isNotEmpty()); assertTrue(deltas.all { it < 0 }); deltas.clear() }
        ui.onNodeWithTag("terminal-surface").performTouchInput { swipeUp(startY = height * .8f, endY = height * .2f, durationMillis = 400) }
        ui.runOnIdle { assertTrue(deltas.isNotEmpty()); assertTrue(deltas.all { it > 0 }) }
        assertFalse(terminalImeShown())
    }

    @Test fun stoppedLocalOffersANewTerminalAndPreservesItsOldViewport() {
        val old = sessions.active!!
        send("printf '\\r\\nSTOPPED_LOCAL_PROOF\\r\\n'; exit 7\r")
        ui.waitUntil(10_000) { old.state.terminal }
        ui.onNodeWithTag("terminal-stopped").assertIsDisplayed()
        ui.runOnIdle {
            assertFalse(terminal().commitText("must-not-be-sent"))
            assertFalse(terminal().sendKey(KeyEvent.KEYCODE_ENTER))
        }
        ui.onNodeWithTag("terminal-surface").performTouchInput { click() }
        assertFalse(terminalImeShown())
        ui.onNodeWithTag("terminal-recover").assertTextEquals("新建终端").performClick()
        ui.waitUntil(10_000) { sessions.active?.id != old.id && sessions.active?.state?.connected == true }
        ui.runOnIdle {
            assertTrue(sessions.sessions.any { it === old })
            assertEquals(7, old.state.exitCode)
            assertTrue(old.connection.frame(-1)!!.let { f -> (0 until f.rows).any { f.line(it).contains("STOPPED_LOCAL_PROOF") } })
            sessions.show(old.id)
        }
        ui.onNodeWithTag("terminal-stopped").assertIsDisplayed()
        ui.terminalBack()
        ui.onNodeWithText("查看 ${old.title}").assertIsDisplayed()
        ui.onNodeWithText("已停止 · 旧画面保留").assertIsDisplayed()
        ui.onNodeWithText("继续本地终端").performClick()
        ui.runOnIdle { assertNotEquals(old.id, sessions.active?.id); assertEquals(2, sessions.sessions.size) }
    }

    @Test fun cleanupRemovesOnlyConfirmedStoppedIdsAndKeepsLiveAndLaterStoppedSessions() {
        val old = sessions.active!!
        send("exit 7\r")
        ui.waitUntil(10_000) { old.state.terminal }
        ui.runOnIdle { sessions.newLocal() }
        ui.waitUntil(10_000) { sessions.active?.id != old.id && sessions.active?.state?.connected == true }
        val live = sessions.active!!
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-session-list").performScrollToNode(hasTestTag("terminal-clean-stopped"))
        ui.onNodeWithTag("terminal-clean-stopped").assertTextEquals("清理").performClick()
        ui.onNodeWithText("保留").performClick()
        ui.runOnIdle { assertTrue(sessions.sessions.any { it === old }) }
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithTag("terminal-clean-stopped").performClick()
        ui.runOnIdle { sessions.newLocal() }
        ui.waitUntil(10_000) { sessions.active?.id != live.id && sessions.active?.state?.connected == true }
        val late = sessions.active!!
        ui.runOnIdle { late.connection.commit("exit 8\r") }
        ui.waitUntil(10_000) { late.state.terminal }
        ui.onNodeWithTag("terminal-clean-stopped-confirm").performClick()
        ui.runOnIdle {
            assertFalse(sessions.sessions.any { it.id == old.id })
            assertTrue(sessions.sessions.any { it === live })
            assertTrue(sessions.sessions.any { it === late })
            // Even an explicitly supplied live id cannot bypass the native-state check.
            sessions.clearStopped(listOf(live.id))
            assertTrue(sessions.sessions.any { it === live })
            assertTrue(live.connection.state().connected)
        }
    }

    @Test fun plainTerminalTapStillOpensKeyboardWithoutWritingMouseBytes() {
        hideMouseTestKeyboard()
        val packet = File(directory, "plain-tap-packet")
        send("stty raw -echo; printf '\\033[2J\\033[HPLAIN_TAP_READY'; dd bs=1 count=1 of='${packet.path}' 2>/dev/null; printf '\\r\\nPLAIN_TAP_DONE'; stty sane\r")
        waitFor("PLAIN_TAP_READY")
        clickMouseCell(2, 3)
        ui.waitUntil(10_000) { androidx.core.view.ViewCompat.getRootWindowInsets(terminal())
            ?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime()) == true }
        ui.runOnIdle { assertTrue(terminal().commitText("Z")) }
        waitFor("PLAIN_TAP_DONE")
        assertArrayEquals(byteArrayOf(90), packet.readBytes())
    }
    @Test fun longPressInMouseModeSelectsTextWithoutSendingAClick() {
        hideMouseTestKeyboard()
        val packet = File(directory, "long-press-packet")
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?1000h\\033[?1006hSELECT_THIS word'; dd bs=1 count=1 of='${packet.path}' 2>/dev/null; printf '\\033[?1000l\\033[?1006l\\r\\nLONG_PRESS_DONE'; stty sane\r")
        waitFor("SELECT_THIS")
        clickMouseCell(2, 0, long = true)
        ui.waitUntil(10_000) { terminal().currentFrame?.cells?.any { it.flags and 256 != 0 } == true }
        ui.runOnIdle { assertTrue(terminal().dismissSelection()); assertTrue(terminal().commitText("Z")) }
        waitFor("LONG_PRESS_DONE")
        assertArrayEquals(byteArrayOf(90), packet.readBytes())
    }

    @Test fun accessibilityReadsVisibleViewportOnQueryAndFocus() {
        send("printf '\\033[2J\\033[HA11Y_VISIBLE \\033[8mA11Y_HIDDEN\\033[0m\\r\\n'\r")
        waitFor("A11Y_VISIBLE")
        ui.runOnIdle {
            val pending = TerminalView(ui.activity).createAccessibilityNodeInfo()
            assertEquals("终端屏幕尚未就绪", pending.text?.toString())
            pending.recycle()
            val view = terminal()
            val info = view.createAccessibilityNodeInfo()
            assertEquals("android.widget.TextView", info.className?.toString())
            assertFalse("The output is not an editable document", info.isEditable)
            assertNull("The generic label must not mask output text", info.contentDescription)
            assertTrue("Actual VT output must be exposed", info.text?.contains("A11Y_VISIBLE") == true)
            assertFalse("Concealed text must not be exposed", info.text?.contains("A11Y_HIDDEN") == true)
            info.recycle()
        }
        val automation = InstrumentationRegistry.getInstrumentation().uiAutomation
        val manager = ui.activity.getSystemService(android.view.accessibility.AccessibilityManager::class.java)
        val service = automation.serviceInfo
        val originalFlags = service.flags
        val originalExploration = manager.isTouchExplorationEnabled
        try {
            service.flags = originalFlags or
                android.accessibilityservice.AccessibilityServiceInfo.FLAG_REQUEST_TOUCH_EXPLORATION_MODE or
                android.accessibilityservice.AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
            automation.serviceInfo = service
            ui.waitUntil(10_000) { manager.isTouchExplorationEnabled }
            fun findOutput(node: android.view.accessibility.AccessibilityNodeInfo?): android.view.accessibility.AccessibilityNodeInfo? {
                if (node == null) return null
                if (node.text?.contains("A11Y_VISIBLE") == true) return node
                for (i in 0 until node.childCount) findOutput(node.getChild(i))?.let { return it }
                return null
            }
            var output: android.view.accessibility.AccessibilityNodeInfo? = null
            ui.waitUntil(10_000) {
                output = automation.windows.firstNotNullOfOrNull { findOutput(it.root) }
                output != null
            }
            val node = checkNotNull(output)
            val event = automation.executeAndWaitForEvent({
                assertTrue("The accessibility service must focus the output", node.performAction(
                    android.view.accessibility.AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS))
            }, { event ->
                event.eventType == android.view.accessibility.AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED &&
                    event.packageName?.toString() == ui.activity.packageName
            }, 5_000)
            try {
                assertEquals("android.widget.TextView", event.className?.toString())
                assertNull(event.contentDescription)
                assertTrue("Focus must read the current viewport", event.text.joinToString().contains("A11Y_VISIBLE"))
                assertFalse(event.text.joinToString().contains("A11Y_HIDDEN"))
            } finally { event.recycle(); node.recycle() }
        } finally {
            service.flags = originalFlags
            automation.serviceInfo = service
            ui.waitUntil(10_000) { manager.isTouchExplorationEnabled == originalExploration }
        }
    }

    @Test fun rendersRealAnsiPixelsWithoutPersistentKeyboardControl() {
        send("stty -echo; printf '\\033[41m\\033[2J\\033[HPIXEL_READY'\r")
        waitFor("PIXEL_READY")
        ui.onAllNodesWithTag("terminal-keyboard").assertCountEquals(0)
        ui.onNodeWithTag("terminal-new-local").assertDoesNotExist()
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

    @Test fun blinkingTextRepaintsWithoutCursorOrNewOutput() {
        send("printf '\\033[0m\\033[40;97m\\033[2J\\033[H\\033[?25l\\033[2 q\\033[5mFLASH\\033[0m\\033[40;97m\\r\\nSTEADY'\r")
        // Wait for complete output, not the same word in an echoed printf command.
        ui.waitUntil(10_000) {
            val frame = terminal().currentFrame
            frame?.line(0) == "FLASH" && frame.line(1) == "STEADY"
        }
        ui.runOnIdle {
            val frame = terminal().currentFrame!!
            assertFalse(frame.cursorVisible)
            assertFalse(frame.cursorBlink)
            assertTrue(frame.cells[0].flags and 128 != 0)
        }
        // Copy the actual window buffer: calling View.draw ourselves would hide
        // a missing invalidation and make a non-animating terminal pass.
        val samples = mutableListOf<Pair<Int, Int>>()
        repeat(24) {
            var image: Bitmap? = null
            var rowHeight = 0
            var status = -1
            val ready = java.util.concurrent.CountDownLatch(1)
            InstrumentationRegistry.getInstrumentation().runOnMainSync {
                val view = terminal()
                val frame = view.currentFrame!!
                val position = IntArray(2); view.getLocationInWindow(position)
                rowHeight = view.height / frame.rows
                val width = (view.width / frame.cols) * 7
                val region = android.graphics.Rect(position[0], position[1], position[0] + width, position[1] + rowHeight * 2)
                image = Bitmap.createBitmap(width, rowHeight * 2, Bitmap.Config.ARGB_8888)
                android.view.PixelCopy.request(ui.activity.window, region, image!!, { result ->
                    status = result; ready.countDown()
                }, android.os.Handler(android.os.Looper.getMainLooper()))
            }
            assertTrue("window capture timed out", ready.await(3, java.util.concurrent.TimeUnit.SECONDS))
            assertEquals(android.view.PixelCopy.SUCCESS, status)
            val bitmap = image!!
            fun white(row: Int): Int {
                var count = 0
                for (y in row * rowHeight until (row + 1) * rowHeight) for (x in 0 until bitmap.width) {
                    val c = bitmap.getPixel(x, y)
                    if (Color.red(c) > 180 && Color.green(c) > 180 && Color.blue(c) > 180) count++
                }
                return count
            }
            samples += white(0) to white(1)
            bitmap.recycle()
            android.os.SystemClock.sleep(80)
        }
        assertTrue("blinking text never appeared: $samples", samples.maxOf { it.first } > 20)
        assertTrue("blinking text never disappeared: $samples", samples.minOf { it.first } < 5)
        assertTrue("ordinary text must stay visible: $samples", samples.minOf { it.second } > 20)
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

    @Test fun imeSurroundingDeletePreservesReversedSelection() {
        send("stty raw -echo; printf '\\033[2J\\033[HIME_DELETE_READY\\r\\n'; dd bs=1 count=5 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_DELETE_DONE\\r\\n'\r")
        waitFor("IME_DELETE_READY")
        ui.runOnIdle {
            val view = terminal()
            val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("abcdef", 1))
            assertTrue(connection.setSelection(3, 1))
            assertTrue(connection.deleteSurroundingText(1, 1))
            assertEquals("surrounding deletion preserves selected text", "bc", connection.getSelectedText(0)?.toString())
            assertEquals("bcef", view.composition)
            val editable = checkNotNull((connection as android.view.inputmethod.BaseInputConnection).editable)
            assertEquals("selection direction is retained", 2, android.text.Selection.getSelectionStart(editable))
            assertEquals(0, android.text.Selection.getSelectionEnd(editable))
            if (android.os.Build.VERSION.SDK_INT >= 31) {
                val window = checkNotNull(connection.getSurroundingText(10, 10, 0))
                // BaseInputConnection normalizes the query range, while the
                // editable itself retains the original selection direction.
                assertEquals(0, window.selectionStart)
                assertEquals(2, window.selectionEnd)
            }
            assertTrue(connection.finishComposingText())
            connection.closeConnection()
            assertTrue(view.onCreateInputConnection(EditorInfo()).commitText("!1234", 1))
        }
        waitFor("IME_DELETE_DONE")
        assertTrue("surrounding deletion preserves the selected bytes: ${text()}",
            text().replace(Regex("\\s+"), " ").contains("62 63 65 66 21"))
    }

    @Test fun terminalKeysReleaseReadOnlyImeContextForFurtherInput() {
        send("stty raw -echo; printf '\\033[>4;0m\\033[<u\\033[?1l\\033[2J\\033[HIME_NAV_READY\\r\\n'; dd bs=1 count=16 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_NAV_DONE\\r\\n'\r")
        waitFor("IME_NAV_READY")
        ui.runOnIdle {
            val view = terminal()
            val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("abcd", 1))
            assertTrue(connection.setSelection(1, 3))
            assertTrue(connection.finishComposingText())
            assertTrue(view.sendKey(KeyEvent.KEYCODE_DPAD_LEFT))
            assertTrue("auxiliary terminal key releases read-only context", connection.commitText("Z", 1))
            assertTrue(connection.setComposingText("efgh", 1))
            assertTrue(connection.setSelection(6, 8))
            assertTrue(connection.finishComposingText())
            assertTrue(view.onKeyDown(KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT)))
            assertTrue(view.onKeyUp(KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DPAD_RIGHT)))
            assertTrue("hardware terminal key releases read-only context", connection.commitText("Q", 1))
        }
        waitFor("IME_NAV_DONE")
        assertTrue("terminal navigation neither replays accepted text nor blocks subsequent input: ${text()}",
            text().replace(Regex("\\s+"), "").contains("616263641b5b445a656667681b5b4351"))
    }

    @Test fun imeCursorPositionsKeepAnEditableTailUntilAcceptance() {
        send("stty raw -echo; printf '\\033[2J\\033[HIME_CURSOR_READY\\r\\n'; dd bs=1 count=6 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_CURSOR_DONE\\r\\n'\r")
        waitFor("IME_CURSOR_READY")
        ui.runOnIdle {
            val connection = terminal().onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("abcd", 1))
            assertTrue(connection.setComposingRegion(1, 3))
            assertTrue(connection.commitText("XY", 0))
            assertEquals("cursor zero retains replacement and suffix", "XYd", connection.getTextAfterCursor(10, 0).toString())
            assertTrue(connection.setSelection(1, 1))
            assertTrue(connection.setComposingRegion(1, 3))
            assertTrue(connection.setComposingText("文", 1))
            assertTrue(connection.commitText("草", 1))
            assertEquals("cursor one retains the uncommitted suffix", "d", connection.getTextAfterCursor(10, 0).toString())
            assertTrue(connection.finishComposingText())
            assertEquals("finish retains the cursor before the accepted suffix", "d", connection.getTextAfterCursor(10, 0).toString())
            assertTrue(connection.setSelection(3, 3))
            assertTrue(connection.commitText("!12", 1))
        }
        waitFor("IME_CURSOR_DONE")
        assertTrue("cursor-relative edits send only final accepted bytes: ${text()}",
            text().replace(Regex("\\s+"), " ").contains("61 e8 8d 89 64 21"))
    }

    @Test fun imeRejectedCommitKeepsTheOriginalEditableAndRetry() {
        send("stty raw -echo; printf '\\033[2J\\033[HIME_REJECT_READY\\r\\n'; dd bs=1 count=6 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_REJECT_DONE\\r\\n'\r")
        waitFor("IME_REJECT_READY")
        ui.runOnIdle {
            val view = terminal()
            val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("a中b", 1))
            assertTrue(connection.setComposingRegion(1, 2))
            // The invalid unit lies AFTER the requested cursor: validating only
            // the emitted prefix would incorrectly accept and send the 'a'.
            assertFalse("invalid commit is rejected", connection.commitText(String(charArrayOf(0xD800.toChar())), 0))
            assertFalse("direct typed input rejects an orphan low surrogate",
                view.commitText(String(charArrayOf(0xDC00.toChar()))))
            assertEquals("rejected commit keeps the visible pending text", "a中b", view.composition)
            assertEquals("a中b", connection.getTextBeforeCursor(10, 0).toString())
            assertTrue(connection.commitText("文", 2))
            assertTrue(connection.commitText("!12", 1))
        }
        waitFor("IME_REJECT_DONE")
        assertTrue("retry uses the original composing region without a partial failed write: ${text()}",
            text().replace(Regex("\\s+"), " ").contains("61 e6 96 87 62 21"))
    }

    @Test fun imePlainSuffixIsDeliveredOnceWhenTheConnectionCloses() {
        send("stty raw -echo; printf '\\033[2J\\033[HIME_TAIL_READY\\r\\n'; dd bs=1 count=6 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_TAIL_DONE\\r\\n'\r")
        waitFor("IME_TAIL_READY")
        ui.runOnIdle {
            val view = terminal()
            val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("a中b", 1))
            assertTrue(connection.setComposingRegion(1, 2))
            assertTrue(connection.commitText("文", 1))
            assertEquals("b", connection.getTextAfterCursor(10, 0).toString())
            connection.closeConnection()
            connection.closeConnection()
            assertTrue(view.onCreateInputConnection(EditorInfo()).commitText("!12", 1))
        }
        waitFor("IME_TAIL_DONE")
        assertTrue("connection closure preserves a plain committed suffix once: ${text()}",
            text().replace(Regex("\\s+"), " ").contains("61 e6 96 87 62 21"))
    }

    @Test fun imeFinalizedSelectionCannotReplayOrRewriteAcceptedBytes() {
        send("stty raw -echo; printf '\\033[2J\\033[HIME_FINAL_READY\\r\\n'; dd bs=1 count=5 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_FINAL_DONE\\r\\n'\r")
        waitFor("IME_FINAL_READY")
        ui.runOnIdle {
            val view = terminal()
            val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("abcd", 1))
            assertTrue(connection.setSelection(1, 3))
            assertTrue(connection.finishComposingText())
            assertTrue(connection.finishComposingText())
            assertFalse("accepted text is explicitly read-only", connection.commitText("bad", 1))
            assertEquals("failed rewrite keeps the accepted selection", "bc", connection.getSelectedText(0).toString())
            assertTrue(connection.finishComposingText())
            connection.closeConnection()
            assertTrue(view.onCreateInputConnection(EditorInfo()).commitText("!1234", 1))
        }
        waitFor("IME_FINAL_DONE")
        assertTrue("finalized bytes are neither rewritten nor replayed: ${text()}",
            text().replace(Regex("\\s+"), " ").contains("61 62 63 64 21"))
    }

    @Test fun imeFinishPreservesSelectionInsideTheAcceptedText() {
        ui.runOnIdle {
            val connection = terminal().onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("abcd", 1))
            assertTrue(connection.setSelection(1, 3))
            assertEquals("bc", connection.getSelectedText(0).toString())
            val before = if (android.os.Build.VERSION.SDK_INT >= 31)
                checkNotNull(connection.getSurroundingText(10, 10, 0)) else null
            assertTrue(connection.finishComposingText())
            assertEquals("finish removes composing status without changing the selection", "bc",
                connection.getSelectedText(0)?.toString())
            assertEquals("finish removes the composing marker", -1,
                android.view.inputmethod.BaseInputConnection.getComposingSpanStart(
                    checkNotNull((connection as android.view.inputmethod.BaseInputConnection).editable)))
            if (android.os.Build.VERSION.SDK_INT >= 31) {
                val after = checkNotNull(connection.getSurroundingText(10, 10, 0))
                assertEquals(before!!.offset + before.selectionStart, after.offset + after.selectionStart)
                assertEquals(before.offset + before.selectionEnd, after.offset + after.selectionEnd)
            }
            assertTrue(connection.finishComposingText())
            connection.closeConnection()
        }
    }

    @Test fun imePartialCommitPreservesTextOutsideTheComposingRegion() {
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004hIME_PARTIAL_READY\\r\\n'; dd bs=1 count=6 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_PARTIAL_DONE\\r\\n'\r")
        waitFor("IME_PARTIAL_READY")
        ui.runOnIdle {
            val connection = terminal().onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("a中b", 1))
            assertTrue(connection.setComposingRegion(1, 2))
            // Position 2 places the cursor after the retained trailing 'b'.
            assertTrue(connection.commitText("文", 2))
            // Both the old short write and the correct write finish dd. The
            // verdict must be the bytes themselves rather than a reader timeout.
            assertTrue(connection.commitText("!12", 1))
        }
        waitFor("IME_PARTIAL_DONE")
        assertTrue("partial IME replacement keeps both uncommitted sides: ${text()}",
            text().replace(Regex("\\s+"), " ").contains("61 e6 96 87 62 21"))
    }

    @Test fun imeWindowUsesAcceptedUtf16OffsetsForSelectionAndRegions() {
        ui.runOnIdle {
            val view = terminal()
            val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.commitText("😀中", 1))
            assertTrue(connection.beginBatchEdit())
            assertTrue(connection.beginBatchEdit())
            assertTrue(connection.setComposingText("草稿", 1))
            assertTrue("selection uses the accepted UTF-16 prefix", connection.setSelection(4, 5))
            assertEquals("稿", connection.getSelectedText(0).toString())
            assertTrue(connection.setComposingRegion(5, 4))
            assertEquals("region marking preserves selection", "稿", connection.getSelectedText(0).toString())
            assertTrue(connection.setSelection(3, 3))
            assertTrue(connection.setComposingText("文", 1))
            assertEquals("草文", view.composition)
            assertTrue("one nested batch remains", connection.endBatchEdit())
            assertFalse("the last batch ends", connection.endBatchEdit())
            if (android.os.Build.VERSION.SDK_INT >= 31) {
                val window = checkNotNull(connection.getSurroundingText(100, 100, 0))
                assertEquals("surrounding text keeps its document offset", 3, window.offset)
                assertEquals("草文", window.text.toString())
                assertEquals(2, window.selectionStart)
                assertEquals(2, window.selectionEnd)
            }
            assertTrue("regions clip without overflowing", connection.setComposingRegion(Int.MAX_VALUE, Int.MIN_VALUE))
            assertTrue(connection.finishComposingText())
            assertTrue(connection.setComposingText("😀", 1))
            assertTrue(connection.setSelection(5, 7))
            assertEquals("😀", connection.getSelectedText(0).toString())
            connection.closeConnection()
            assertNull(connection.getTextBeforeCursor(10, 0))
            assertNull(connection.getTextAfterCursor(10, 0))
            assertNull(connection.getSelectedText(0))
            assertFalse(connection.setSelection(0, 0))
            if (android.os.Build.VERSION.SDK_INT >= 31) assertNull(connection.getSurroundingText(10, 10, 0))
        }
    }

    @Test fun finishingImeCompositionPreservesBytesOnceAndClosingStillCancels() {
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004hIME_FINISH_READY\\r\\n'; dd bs=1 count=7 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_FINISH_DONE\\r\\n'\r")
        waitFor("IME_FINISH_READY")
        ui.runOnIdle {
            val view = terminal(); val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.finishComposingText())
            assertTrue(connection.setComposingText("草稿", 1))
            assertEquals("草稿", view.composition)
            assertTrue(connection.finishComposingText())
            assertEquals("", view.composition)
            assertTrue(connection.finishComposingText())
            assertTrue(connection.setComposingText("丢弃", 1))
            connection.closeConnection()
            assertEquals("", view.composition)
            assertFalse(connection.finishComposingText())
            assertTrue(view.onCreateInputConnection(EditorInfo()).commitText("!", 1))
        }
        waitFor("IME_FINISH_DONE")
        assertTrue("finish must preserve accepted bytes once without bracketed paste: ${text()}",
            text().replace(Regex("\\s+"), " ").contains("e8 8d 89 e7 a8 bf 21"))
    }

    @Test fun imeEnterAndEditorActionAcceptPendingTextBeforeTheReturn() {
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004hIME_ENTER_READY\\r\\n'; dd bs=1 count=21 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_ENTER_DONE\\r\\n'\r")
        waitFor("IME_ENTER_READY")
        ui.runOnIdle {
            val view = terminal(); val connection = view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("草稿", 1))
            assertTrue(connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER)))
            assertTrue(connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER)))
            assertEquals("IME Enter accepts composition before the key", "", view.composition)
            assertTrue(connection.finishComposingText())
            assertTrue(connection.setComposingText("中文", 1))
            assertTrue(connection.performEditorAction(EditorInfo.IME_ACTION_DONE))
            assertEquals("editor action accepts composition before Return", "", view.composition)
            assertTrue(connection.finishComposingText())
            assertTrue(connection.setComposingText("硬键", 1))
            assertTrue(view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER)))
            assertTrue(view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER)))
            assertEquals("physical Return accepts preedit before the key", "", view.composition)
            assertTrue(connection.finishComposingText())
        }
        waitFor("IME_ENTER_DONE")
        assertTrue("IME acceptance must precede each Return without duplication: ${text()}",
            text().replace(Regex("\\s+"), "").contains("e88d89e7a8bf0de4b8ade696870de7a1ace994ae0d"))
    }

    @Test fun imeLineFeedAcceptsPendingCompositionBeforeReturn() {
        send("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004hIME_LF_READY\\r\\n'; dd bs=1 count=7 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nIME_LF_DONE\\r\\n'\r")
        waitFor("IME_LF_READY")
        ui.runOnIdle {
            val view=terminal();val connection=view.onCreateInputConnection(EditorInfo())
            assertTrue(connection.setComposingText("草稿",1))
            assertTrue(connection.commitText("\n",1))
            assertEquals("single IME LF accepts composition before Return","",view.composition)
            assertTrue(connection.finishComposingText())
            // A dropped composition still gives dd seven bytes and a normal
            // exit. The mutation must fail on byte content, not a hung reader.
            assertTrue(connection.commitText("123456",1))
        }
        waitFor("IME_LF_DONE")
        assertTrue("single IME LF must preserve typed bytes before CR: ${text()}",
            text().replace(Regex("\\s+"),"").contains("e88d89e7a8bf0d"))
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
            // Explicit cancellation replaces the composing text with nothing.
            // finishComposingText itself accepts the remaining text.
            assertTrue(connection.setComposingText("", 1))
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
        // The interactive shell can echo the command containing "PID:" before
        // executing its clear/printf. Capture only the actual complete PID row,
        // never the previous frame plus a partially echoed command.
        ui.waitUntil(10_000) {
            viewOrNull()?.currentFrame?.line(0)?.matches(Regex("PID:[0-9]+")) == true
        }
        val first = terminal().currentFrame!!
        val pid = first.line(0)
        waitFor("${first.rows} ${first.cols}")
        ui.terminalBack()
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
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-new-local").performClick()
        ui.waitUntil(10_000) { !sessions.creating && sessions.sessions.size == 2 && sessions.active?.id != original && sessions.active?.state?.connected == true }
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
            labels = nodes.map { node ->
                val bounds=android.graphics.Rect();node.getBoundsInScreen(bounds)
                "${node.text}/${node.contentDescription} visible=${node.isVisibleToUser} enabled=${node.isEnabled} clickable=${node.isClickable} bounds=$bounds"
            }
            nodes.firstOrNull { node ->
                val named=node.text?.toString()==label || node.contentDescription?.toString()==label
                val bounds=android.graphics.Rect();node.getBoundsInScreen(bounds)
                val screen=ui.activity.resources.displayMetrics
                // Android 10's floating toolbar can draw an action while its
                // accessibility node incorrectly reports visible=false.
                val legacy=android.os.Build.VERSION.SDK_INT<=29 && node.isEnabled && node.isClickable &&
                    node.packageName?.toString()==ui.activity.packageName && !bounds.isEmpty &&
                    bounds.left>=0 && bounds.top>=0 && bounds.right<=screen.widthPixels && bounds.bottom<=screen.heightPixels
                named && (node.isVisibleToUser || legacy)
            }?.let { match ->
                if (!click) return
                var node: android.view.accessibility.AccessibilityNodeInfo? = match
                while (node != null) {
                    if (node.isEnabled && node.performAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_CLICK)) return
                    node = node.parent
                }
                if(android.os.Build.VERSION.SDK_INT<=29 && match.isEnabled) {
                    // Exercise the actual bounded touch target when the old
                    // floating toolbar cannot dispatch an accessibility click.
                    val bounds=android.graphics.Rect();match.getBoundsInScreen(bounds)
                    if(!bounds.isEmpty) {
                        val now=android.os.SystemClock.uptimeMillis()
                        fun touch(action:Int):Boolean {
                            val event=android.view.MotionEvent.obtain(now,android.os.SystemClock.uptimeMillis(),action,bounds.exactCenterX(),bounds.exactCenterY(),0)
                            event.source=android.view.InputDevice.SOURCE_TOUCHSCREEN
                            return try { automation.injectInputEvent(event,true) } finally { event.recycle() }
                        }
                        if(touch(android.view.MotionEvent.ACTION_DOWN) && touch(android.view.MotionEvent.ACTION_UP)) return
                    }
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
        ui.runOnIdle {
            val view=terminal();val visible=android.graphics.Rect();view.getGlobalVisibleRect(visible)
            val handles=view.javaClass.getDeclaredMethod("selectionHandles").apply { isAccessible=true }.invoke(view)
            diagnostic+=" visible=$visible handles=$handles touch=${view.isInTouchMode}"
        }
        automation.takeScreenshot()?.let { image ->
            java.io.File(ui.activity.cacheDir,"terminal-text-action-failed.png").outputStream().use { image.compress(Bitmap.CompressFormat.PNG,100,it) };image.recycle()
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
        try {
            ui.waitUntil {
                ui.activity.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString() == "COPY_THIS"
            }
        } catch (failure: ComposeTimeoutException) {
            val actual = ui.runOnIdle {
                ui.activity.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString()
            }
            throw AssertionError("复制应为 COPY_THIS，实际剪贴板为 <$actual>", failure)
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

    @Test fun ctrlSingleUseAndLockEncodeRealPtyInput() {
        send("stty raw -echo; printf '\\033[2J\\033[HCTRL_READY\\r\\n'; dd bs=1 count=5 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nCTRL_DONE\\r\\n'\r")
        waitFor("CTRL_READY")
        ui.onNodeWithText("Ctrl").performScrollTo().performClick()
        ui.onNodeWithText("Ctrl ✓").performScrollTo().assertIsDisplayed()
        ui.runOnIdle { assertTrue(terminal().onCreateInputConnection(EditorInfo()).commitText("a", 1)) }
        ui.onNodeWithText("Ctrl").performScrollTo().assertIsDisplayed()
        send("b")
        lockCtrlMode()
        ui.onNodeWithText("Ctrl").performScrollTo().performClick()
        ui.onNodeWithText("Ctrl 锁定").performScrollTo().assertIsDisplayed()
        ui.runOnIdle {
            val ime = terminal().onCreateInputConnection(EditorInfo())
            assertTrue(ime.commitText("c", 1)); assertTrue(ime.commitText("d", 1))
        }
        ui.onNodeWithText("Ctrl 锁定").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText("Ctrl 锁定").performScrollTo().performClick()
        send("e"); waitFor("CTRL_DONE")
        assertTrue(text(), text().replace(Regex("\\s+"), " ").contains("01 62 03 04 65"))
    }

    /** Capture the program's bytes, independent of wrapping or VT escape interpretation. */
    private fun captureKeys(mode: String, expected: ByteArray, input: (File) -> Unit) {
        val token = UUID.randomUUID().toString().take(8)
        val result = File(directory, "home/keys-$token")
        send("stty raw -echo; printf '\\033[2J\\033[H${mode}KEY_READY_$token\\r\\n'; " +
            "dd bs=1 count=${expected.size} of='${result.absolutePath}' 2>/dev/null; " +
            "stty sane; printf '\\033[<u\\033[>4;0m\\r\\nKEY_DONE_$token\\r\\n'\r")
        ui.waitUntil(10_000) { terminal().currentFrame?.line(0) == "KEY_READY_$token" }
        input(result)
        try {
            ui.waitUntil(10_000) { terminal().currentFrame?.let { frame ->
                (0 until frame.rows).any { frame.line(it) == "KEY_DONE_$token" }
            } == true }
        } catch (failure: androidx.compose.ui.test.ComposeTimeoutException) {
            throw AssertionError("mode=$mode expected=${expected.joinToString { "%02x".format(it) }} actual=${result.takeIf { it.exists() }?.readBytes()?.joinToString { "%02x".format(it) }}", failure)
        }
        assertArrayEquals("raw PTY input (${result.readBytes().joinToString { "%02x".format(it) }})", expected, result.readBytes())
    }

    @Test fun ctrlPunctuationUsesVtAndLeavesUnicodeAndBulkCommitsIntact() {
        // The pinned VT deliberately disambiguates Ctrl+[ as CSI 91;5u.
        val controls = byteArrayOf(31, 0) + "\u001b[91;5u".toByteArray(Charsets.UTF_8) +
            byteArrayOf(28, 29, 30, 31, 127, 0, 27, 28, 29, 30, 31, 127, 0)
        captureKeys("\\033[?2004h", controls + "K中文😀ab/".toByteArray(Charsets.UTF_8)) {
            ui.onNodeWithText("Ctrl").performScrollTo().performClick()
            ui.onNodeWithText("/").performScrollTo().performClick()
            ui.onNodeWithText("Ctrl").performScrollTo().assertIsDisplayed()
            lockCtrlMode()
            ui.onNodeWithText("Ctrl").performScrollTo().performClick()
            ui.runOnIdle {
                val ime = terminal().onCreateInputConnection(EditorInfo())
                "@[\\]^_?2345678 ".forEach { assertTrue(ime.commitText(it.toString(), 1)) }
                // Kelvin sign lowercases to ASCII k; it must still remain Unicode text.
                listOf("K", "中文", "😀", "ab").forEach { assertTrue(ime.commitText(it, 1)) }
            }
            ui.onNodeWithText("Ctrl 锁定").performScrollTo().assertIsDisplayed()
            ui.onNodeWithText("Ctrl 锁定").performScrollTo().performClick()
            ui.onNodeWithText("/").performScrollTo().performClick()
        }
    }

    @Test fun softCtrlUsesCurrentKittyAndModifyOtherKeysModes() {
        listOf("\\033[>1u" to "\u001b[47;5u", "\\033[>4;2m" to "\u001b[27;5;47~").forEach { (mode, encoded) ->
            captureKeys(mode, encoded.toByteArray(Charsets.UTF_8)) {
                ui.onNodeWithText("Ctrl").performScrollTo().performClick()
                ui.onNodeWithText("/").performScrollTo().performClick()
                ui.onNodeWithText("Ctrl").performScrollTo().assertIsDisplayed()
            }
        }
    }

    @Test fun hardwareModifiersKeepLayoutCodepointsForKittyEncoding() {
        val expected = "\u001b[47;6u\u001b[50;7u\u001b[97;6u".toByteArray(Charsets.UTF_8)
        captureKeys("\\033[>1u", expected) {
            ui.runOnIdle {
                val view = terminal(); view.requestFocus()
                listOf(KeyEvent.KEYCODE_SLASH to KeyEvent.META_SHIFT_ON,
                    KeyEvent.KEYCODE_2 to KeyEvent.META_ALT_ON, KeyEvent.KEYCODE_A to KeyEvent.META_SHIFT_ON).forEach { (key, modifier) ->
                    val event = KeyEvent(0, 0, KeyEvent.ACTION_DOWN, key, 0, KeyEvent.META_CTRL_ON or modifier)
                    assertTrue(ui.activity.window.decorView.dispatchKeyEvent(event))
                    assertTrue(ui.activity.window.decorView.dispatchKeyEvent(KeyEvent.changeAction(event, KeyEvent.ACTION_UP)))
                }
            }
        }
    }

    @Test fun hardwarePressRepeatAndReleaseFollowCurrentVtMode() {
        val kitty = "\u001b[97u\u001b[97;1:2u\u001b[97;1:3u" +
            "\u001b[13u\u001b[13;1:2u\u001b[13;1:3uEND"
        listOf("" to "aa\r\rEND", "\\033[>11u" to kitty).forEach { (mode, expected) ->
            captureKeys(mode, expected.toByteArray(Charsets.UTF_8)) {
                ui.runOnIdle {
                    val view = terminal(); view.requestFocus()
                    val ime = view.onCreateInputConnection(EditorInfo())
                    for (code in listOf(KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_ENTER)) {
                        val down = KeyEvent(0, 0, KeyEvent.ACTION_DOWN, code, 0)
                        assertTrue(ime.sendKeyEvent(down))
                        assertTrue(ime.sendKeyEvent(KeyEvent.changeTimeRepeat(down, 1, 1)))
                        assertTrue(ime.sendKeyEvent(KeyEvent.changeAction(down, KeyEvent.ACTION_UP)))
                        // No matching down: no duplicate release or typed character.
                        assertTrue(ime.sendKeyEvent(KeyEvent.changeAction(down, KeyEvent.ACTION_UP)))
                    }
                    assertTrue(view.commitText("END"))
                }
            }
        }
    }

    @Test fun oneShotCtrlStaysWithHeldKeyUntilRelease() {
        val expected = "\u001b[97;5u\u001b[97;5:2u\u001b[97;5:3u\u001b[98u\u001b[98;1:3uEND"
        captureKeys("\\033[>11u", expected.toByteArray(Charsets.UTF_8)) {
            ui.onNodeWithText("Ctrl").performScrollTo().performClick()
            ui.runOnIdle {
                val view = terminal(); view.requestFocus()
                val a = KeyEvent(0, 0, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A, 0)
                assertTrue(ui.activity.window.decorView.dispatchKeyEvent(a))
                assertEquals(0, view.ctrl)
                assertTrue(ui.activity.window.decorView.dispatchKeyEvent(KeyEvent.changeTimeRepeat(a, 1, 1)))
                assertTrue(ui.activity.window.decorView.dispatchKeyEvent(KeyEvent.changeAction(a, KeyEvent.ACTION_UP)))
                val b = KeyEvent(0, 2, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_B, 0)
                assertTrue(ui.activity.window.decorView.dispatchKeyEvent(b))
                assertTrue(ui.activity.window.decorView.dispatchKeyEvent(KeyEvent.changeAction(b, KeyEvent.ACTION_UP)))
                assertTrue(view.commitText("END"))
            }
        }
    }

    @Test fun heldKeysReleaseOnFocusLossAndBeforeSurfaceChanges() {
        val pair = "\u001b[97u\u001b[97;1:3u"
        captureKeys("\\033[>11u", (pair.repeat(3) + "END").toByteArray(Charsets.UTF_8)) { result ->
            val event = KeyEvent(0, 0, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_A, 0)
            lateinit var edit: android.widget.EditText
            for (phase in 0..2) {
                ui.runOnIdle {
                    val view = terminal(); view.requestFocus()
                    assertTrue(view.onKeyDown(event.keyCode, event))
                    when (phase) {
                        0 -> {
                            edit = android.widget.EditText(ui.activity)
                            ui.activity.findViewById<ViewGroup>(android.R.id.content).addView(edit, ViewGroup.LayoutParams(120, 80))
                            edit.requestFocus(); assertTrue(edit.hasFocus()); assertFalse(view.hasFocus())
                        }
                        1 -> view.onWindowFocusChanged(false)
                        else -> {
                            val original = view.surface
                            val other = com.hanxi.ghosterm.bridge.LocalSession(File(directory, "other-home").apply { mkdirs() }.absolutePath)
                            try {
                                view.surface = other
                                assertTrue(view.onKeyUp(event.keyCode, KeyEvent.changeAction(event, KeyEvent.ACTION_UP)))
                                view.surface = original
                            } finally { view.surface = original; other.close() }
                        }
                    }
                }
                // Observe the release BEFORE allowing the delayed physical key-up.
                // Otherwise a broken focus hook could be masked by the later key-up.
                val prefix = pair.repeat(phase + 1).toByteArray(Charsets.UTF_8)
                ui.waitUntil(5_000) { result.exists() && result.length() == prefix.size.toLong() }
                assertArrayEquals(prefix, result.readBytes())
                ui.runOnIdle {
                    val view = terminal()
                    assertTrue(view.onKeyUp(event.keyCode, KeyEvent.changeAction(event, KeyEvent.ACTION_UP)))
                    if (phase == 0) (edit.parent as ViewGroup).removeView(edit)
                    if (phase == 1) view.onWindowFocusChanged(true)
                    view.requestFocus()
                }
            }
            ui.runOnIdle { assertTrue(terminal().commitText("END")) }
        }
    }

    @Test fun auxiliaryAndImeKeyTapsIncludeReleaseWhenRequested() {
        val expected = "\u001b[9u\u001b[9;1:3u\u001b[13u\u001b[13;1:3u\u001b[47;5u\u001b[47;5:3uEND"
        captureKeys("\\033[>11u", expected.toByteArray(Charsets.UTF_8)) {
            ui.onNodeWithText("Tab").performScrollTo().performClick()
            ui.runOnIdle { assertTrue(terminal().onCreateInputConnection(EditorInfo()).performEditorAction(EditorInfo.IME_ACTION_NONE)) }
            ui.onNodeWithText("Ctrl").performScrollTo().performClick(); ui.onNodeWithText("/").performScrollTo().performClick()
            ui.runOnIdle { assertTrue(terminal().commitText("END")) }
        }
    }

    private fun hardwareTap(code: Int, meta: Int = 0, releaseMeta: Int = meta, device: Int = -1) {
        val down = KeyEvent(0, 0, KeyEvent.ACTION_DOWN, code, 0, meta, device, 0, 0)
        assertTrue(terminal().onKeyDown(code, down))
        assertTrue(terminal().onKeyUp(code, KeyEvent(0, 1, KeyEvent.ACTION_UP, code, 0, releaseMeta, device, 0, 0)))
    }

    @Test fun modifierKeysLocksAndSoftCtrlKeepSeparateState() {
        val modifiers = listOf(
            Triple(KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_LEFT_ON, 57441 to 2),
            Triple(KeyEvent.KEYCODE_SHIFT_RIGHT, KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_RIGHT_ON, 57447 to 2),
            Triple(KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_LEFT_ON, 57442 to 5),
            Triple(KeyEvent.KEYCODE_CTRL_RIGHT, KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_RIGHT_ON, 57448 to 5),
            Triple(KeyEvent.KEYCODE_ALT_LEFT, KeyEvent.META_ALT_ON or KeyEvent.META_ALT_LEFT_ON, 57443 to 3),
            Triple(KeyEvent.KEYCODE_ALT_RIGHT, KeyEvent.META_ALT_ON or KeyEvent.META_ALT_RIGHT_ON, 57449 to 3),
            Triple(KeyEvent.KEYCODE_META_LEFT, KeyEvent.META_META_ON or KeyEvent.META_META_LEFT_ON, 57444 to 9),
            Triple(KeyEvent.KEYCODE_META_RIGHT, KeyEvent.META_META_ON or KeyEvent.META_META_RIGHT_ON, 57450 to 9),
        )
        val keys = modifiers.joinToString("") { (_, _, protocol) -> "\u001b[${protocol.first};${protocol.second}u\u001b[${protocol.first};1:3u" }
        val locks = "\u001b[57358;65u\u001b[57358;65:3u\u001b[57360;129u\u001b[57360;129:3u\u001b[97;193u\u001b[97;193:3u"
        captureKeys("\\033[>11u", (keys + locks + "\u001b[97;5u\u001b[97;5:3uEND").toByteArray()) {
            ui.runOnIdle {
                terminal().ctrl = 1
                modifiers.forEach { (code, meta, _) -> hardwareTap(code, meta, 0); assertEquals(1, terminal().ctrl) }
                hardwareTap(KeyEvent.KEYCODE_CAPS_LOCK, KeyEvent.META_CAPS_LOCK_ON)
                hardwareTap(KeyEvent.KEYCODE_NUM_LOCK, KeyEvent.META_NUM_LOCK_ON)
                assertEquals(1, terminal().ctrl)
                // Use physical lock-state reports separately from the armed soft Ctrl.
                terminal().ctrl = 0
                hardwareTap(KeyEvent.KEYCODE_A, KeyEvent.META_CAPS_LOCK_ON or KeyEvent.META_NUM_LOCK_ON)
                terminal().ctrl = 1; hardwareTap(KeyEvent.KEYCODE_A)
                assertEquals(0, terminal().ctrl); assertTrue(terminal().commitText("END"))
            }
        }
        captureKeys("", "END".toByteArray()) {
            ui.runOnIdle {
                modifiers.forEach { (code, meta, _) -> hardwareTap(code, meta, 0) }
                assertTrue(terminal().commitText("END"))
            }
        }
    }

    @Test fun numericKeypadRespectsNumLockApplicationModeAndReleaseIdentity() {
        listOf("\\033>" to "1\r.\u001b[F\u001b[3~END",
            "\\033=" to "1\r.\u001b[F\u001b[3~END",
            "\\033[?1035l\\033=" to "\u001bOq\u001bOM\u001bOn\u001b[F\u001b[3~END").forEach { (mode, expected) ->
            captureKeys(mode, expected.toByteArray()) {
                ui.runOnIdle {
                    for (key in listOf(KeyEvent.KEYCODE_NUMPAD_1, KeyEvent.KEYCODE_NUMPAD_ENTER, KeyEvent.KEYCODE_NUMPAD_DOT))
                        hardwareTap(key, KeyEvent.META_NUM_LOCK_ON)
                    hardwareTap(KeyEvent.KEYCODE_NUMPAD_1); hardwareTap(KeyEvent.KEYCODE_NUMPAD_DOT)
                    assertTrue(terminal().commitText("END"))
                }
            }
        }
        captureKeys("\\033>\\033[>11u", "\u001b[57400;129u\u001b[57400;129:3u\u001b[57424u\u001b[57424;129:3uEND".toByteArray()) {
            ui.runOnIdle {
                hardwareTap(KeyEvent.KEYCODE_NUMPAD_1, KeyEvent.META_NUM_LOCK_ON)
                // Changing Num Lock while held must not turn an End release into a digit release.
                hardwareTap(KeyEvent.KEYCODE_NUMPAD_1, 0, KeyEvent.META_NUM_LOCK_ON)
                assertTrue(terminal().commitText("END"))
            }
        }
    }

    private fun deadAcute(device: Int = -1) {
        val dead = KeyEvent(0, 0, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_E, 0, KeyEvent.META_ALT_ON, device, 0, 0)
        assertTrue("real Virtual.kcm must provide an acute dead key", dead.getUnicodeChar(dead.metaState) and android.view.KeyCharacterMap.COMBINING_ACCENT != 0)
        hardwareTap(KeyEvent.KEYCODE_E, KeyEvent.META_ALT_ON, device = device)
    }

    @Test fun deadKeysComposeCancelAndPreserveLayoutText() {
        captureKeys("\\033[?1036h", "é´´q´ç\u001bqeEND".toByteArray()) { bytes ->
            ui.runOnIdle {
                deadAcute(); assertTrue(terminal().composition.isNotEmpty()); assertEquals(0, bytes.length())
                hardwareTap(KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.META_SHIFT_ON, 0)
                assertTrue(terminal().composition.isNotEmpty())
                hardwareTap(KeyEvent.KEYCODE_E); assertEquals("", terminal().composition)
                deadAcute(); hardwareTap(KeyEvent.KEYCODE_SPACE)
                deadAcute(); hardwareTap(KeyEvent.KEYCODE_Q)
                deadAcute(); deadAcute()
                hardwareTap(KeyEvent.KEYCODE_C, KeyEvent.META_ALT_ON)
                hardwareTap(KeyEvent.KEYCODE_Q, KeyEvent.META_ALT_ON)
                deadAcute(); hardwareTap(KeyEvent.KEYCODE_DEL); assertEquals("", terminal().composition)
                deadAcute(); hardwareTap(KeyEvent.KEYCODE_ESCAPE); assertEquals("", terminal().composition)
                hardwareTap(KeyEvent.KEYCODE_E); assertTrue(terminal().commitText("END"))
            }
        }
        captureKeys("\\033[>11u", "é´qEND".toByteArray()) {
            ui.runOnIdle {
                deadAcute(); hardwareTap(KeyEvent.KEYCODE_E)
                deadAcute(); hardwareTap(KeyEvent.KEYCODE_Q)
                assertTrue(terminal().commitText("END"))
            }
        }
    }

    @Test fun deadKeyStateDoesNotCrossFocusDevicesOrImeCommits() {
        captureKeys("", "eeé中文eEND".toByteArray()) { bytes ->
            ui.runOnIdle {
                deadAcute(); terminal().onWindowFocusChanged(false); terminal().onWindowFocusChanged(true)
                assertEquals("", terminal().composition); assertEquals(0, bytes.length())
                hardwareTap(KeyEvent.KEYCODE_E)
                deadAcute(); hardwareTap(KeyEvent.KEYCODE_E, device = 42)
                assertTrue(terminal().composition.isNotEmpty())
                hardwareTap(KeyEvent.KEYCODE_E); assertEquals("", terminal().composition)
                val ime = terminal().onCreateInputConnection(EditorInfo())
                deadAcute(); assertTrue(ime.commitText("中文", 1))
                hardwareTap(KeyEvent.KEYCODE_E); assertTrue(terminal().commitText("END"))
            }
        }
    }

    @Test fun auxiliaryKeysResolvePendingDeadCompositionBeforeSending() {
        captureKeys("", "´\teEND".toByteArray()) {
            ui.runOnIdle { deadAcute() }
            ui.onNodeWithText("Esc").performScrollTo().performClick()
            ui.runOnIdle { assertEquals("", terminal().composition); deadAcute() }
            ui.onNodeWithText("Tab").performScrollTo().performClick()
            ui.runOnIdle {
                assertEquals("", terminal().composition)
                val ime = terminal().onCreateInputConnection(EditorInfo())
                deadAcute(); assertTrue(ime.deleteSurroundingText(1, 0))
                assertEquals("", terminal().composition)
                hardwareTap(KeyEvent.KEYCODE_E); assertTrue(terminal().commitText("END"))
            }
        }
    }

    @Test fun imeSingleNewlineUsesEnterButMultilineCommitStaysText() {
        listOf("" to "\r", "\\033[>9u" to "\u001b[13u").forEach { (mode, enter) ->
            val block = "\n中文\n"
            captureKeys(mode, (enter + block).toByteArray(Charsets.UTF_8)) {
                ui.runOnIdle {
                    val ime = terminal().onCreateInputConnection(EditorInfo())
                    assertTrue(ime.commitText("\n", 1))
                    assertTrue(ime.commitText(block, 1))
                }
            }
        }
    }

    @Test fun ctrlAndCompositionClearWhenFocusMovesToAnotherView() {
        lateinit var old: android.view.inputmethod.InputConnection
        lateinit var edit: android.widget.EditText
        ui.runOnIdle {
            val view = terminal(); view.requestFocus(); assertTrue(view.hasFocus())
            old = view.onCreateInputConnection(EditorInfo())
            assertTrue(old.setComposingText("未提交", 1))
            view.ctrl = 2
            edit = android.widget.EditText(ui.activity)
            ui.activity.findViewById<ViewGroup>(android.R.id.content).addView(edit, ViewGroup.LayoutParams(120, 80))
            edit.requestFocus()
        }
        ui.runOnIdle {
            assertTrue(edit.hasFocus()); assertFalse(terminal().hasFocus())
            assertEquals(0, terminal().ctrl); assertEquals("", terminal().composition)
            assertFalse("stale IME must not input into an unfocused session", old.commitText("x", 1))
            (edit.parent as ViewGroup).removeView(edit)
            terminal().requestFocus()
        }
    }

    @Test fun ctrlDoesNotCrossPagesSessionsOrNormalExit() {
        val original = sessions.active!!.id
        ui.onNodeWithText("Ctrl").performScrollTo().performClick(); ui.onNodeWithText("Ctrl ✓").performScrollTo().performClick()
        ui.terminalBack()
        ui.onNodeWithText("继续本地终端").performClick()
        ui.waitUntil { viewOrNull()?.currentFrame != null }
        ui.onNodeWithText("Ctrl").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText("Ctrl").performScrollTo().performClick(); ui.onNodeWithText("Ctrl ✓").performScrollTo().performClick()
        val first = terminal()
        ui.openTerminalSessions(); ui.onNodeWithTag("terminal-new-local").performClick()
        ui.waitUntil { sessions.sessions.size == 2 && sessions.active!!.state.connected && viewOrNull()?.currentFrame != null }
        ui.runOnIdle { assertNotSame("each session needs its own input/selection state", first, terminal()); assertEquals(0, terminal().ctrl) }
        ui.openTerminalSessions(); ui.onNodeWithTag("terminal-session-select-$original").performClick()
        ui.waitUntil { sessions.active!!.id == original && viewOrNull()?.currentFrame != null }
        ui.onNodeWithText("Ctrl").performScrollTo().assertIsDisplayed()
        ui.onNodeWithText("Ctrl").performScrollTo().performClick(); ui.onNodeWithText("Ctrl ✓").performScrollTo().performClick()
        ui.runOnIdle { sessions.active!!.surface.commit("exit 0\r") }
        ui.waitUntil { sessions.active!!.state.terminal }
        ui.onNodeWithText("Ctrl").assertDoesNotExist()
        ui.onNodeWithTag("terminal-recover").assertExists()
        ui.runOnIdle { assertEquals(0, terminal().ctrl); assertFalse(terminal().commitText("x")) }
    }

    private inner class HandleDrag(start: Boolean) {
        private val instrumentation = InstrumentationRegistry.getInstrumentation()
        private val started = android.os.SystemClock.uptimeMillis()
        private var origin = IntArray(2)
        private var x = 0f
        private var y = 0f
        init {
            ui.runOnIdle {
                val view = terminal(); val frame = view.currentFrame!!
                val index = if (start) frame.cells.indexOfFirst { it.flags and 256 != 0 }
                    else frame.cells.indexOfLast { it.flags and 256 != 0 }
                assertTrue("handle requires a real visible selection", index >= 0)
                view.getLocationOnScreen(origin)
                val radius = 10f * view.resources.displayMetrics.density
                x = ((index % frame.cols + if (start) 0 else 1) * (view.width / frame.cols)).toFloat()
                    .coerceIn(radius, view.width - radius)
                y = ((index / frame.cols + 1) * (view.height / frame.rows)).toFloat()
                    .coerceIn(radius, view.height - radius)
            }
            touch(android.view.MotionEvent.ACTION_DOWN)
        }
        private fun touch(action: Int) {
            val event = android.view.MotionEvent.obtain(started, android.os.SystemClock.uptimeMillis(), action,
                origin[0] + x, origin[1] + y, 0)
            instrumentation.sendPointerSync(event); event.recycle()
        }
        fun move(toX: Float, toY: Float) {
            val fromX = x; val fromY = y
            for (i in 1..6) {
                x = fromX + (toX - fromX) * i / 6; y = fromY + (toY - fromY) * i / 6
                touch(android.view.MotionEvent.ACTION_MOVE)
                android.os.SystemClock.sleep(20)
            }
        }
        fun finish(cancel: Boolean = false) = touch(if (cancel) android.view.MotionEvent.ACTION_CANCEL else android.view.MotionEvent.ACTION_UP)
    }

    @Test fun handleEdgesScrollWhileHeldAndStopOnReleaseOrCancel() {
        send("printf '\\033[2J\\033[H'; seq 1 300; printf 'SCROLL_DONE'; sleep 30\r")
        waitFor("SCROLL_DONE")
        val before = terminal().currentFrame!!.line(0)
        ui.runOnIdle { sessions.active!!.surface.scroll(-80) }
        ui.waitUntil { terminal().currentFrame!!.line(0) != before }
        for (up in listOf(true, false)) {
            clipboard("before-edge-copy")
            longPressCell(1, 8)
            // A queued scroll can finish during the long press. Observe the
            // actual word selection, then capture viewport and anchor together.
            var selectedFrame=terminal().currentFrame!!
            try {
                ui.waitUntil {
                    val frame=terminal().currentFrame!!
                    val selected=frame.cells.indices.filter { frame.cells[it].flags and 256 != 0 }
                    if(selected.isNotEmpty() && selected.all { it/frame.cols==8 }) {
                        selectedFrame=frame;true
                    } else false
                }
            } catch(failure:androidx.compose.ui.test.ComposeTimeoutException) {
                val frame=terminal().currentFrame!!
                val selectedRows=frame.cells.indices.filter { frame.cells[it].flags and 256 != 0 }.map { it/frame.cols }.distinct()
                throw AssertionError("word selection must be acknowledged on viewport row 8; selectedRows=$selectedRows",failure)
            }
            val top=selectedFrame.line(0).toInt()
            val anchor=selectedFrame.line(8).toInt()
            val drag = HandleDrag(start = up)
            var x = 0f; var y = 0f
            ui.runOnIdle {
                val view = terminal(); val frame = view.currentFrame!!
                x = if (up) 1f else (frame.cols - 0.5f) * (view.width / frame.cols)
                y = if (up) 1f else view.height - 2f
            }
            drag.move(x, y)
            // No more MOVE events: holding at the edge must continue scrolling.
            ui.waitUntil(5_000) {
                val now = terminal().currentFrame!!.line(0).toIntOrNull() ?: return@waitUntil false
                if (up) now <= top - 7 else now >= top + 7
            }
            drag.finish(cancel = !up)
            android.os.SystemClock.sleep(250)
            val stopped = terminal().currentFrame!!
            android.os.SystemClock.sleep(400)
            assertEquals("release/cancel must stop auto-scroll", stopped.line(0), terminal().currentFrame!!.line(0))
            val first = if (up) stopped.line(0).toInt() else anchor
            val last = if (up) anchor else stopped.line(stopped.rows - 1).toInt()
            val expected = (first..last).joinToString("\n")
            clickSystemText("复制")
            try {
                ui.waitUntil {
                    ui.activity.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString() == expected
                }
            } catch (failure: androidx.compose.ui.test.ComposeTimeoutException) {
                val actual = ui.activity.getSystemService(ClipboardManager::class.java).primaryClip?.getItemAt(0)?.text?.toString()
                throw AssertionError("edge copy up=$up expected=<$expected> actual=<$actual> rows=${stopped.rows}", failure)
            }
        }
    }

    @Test fun crossingThenRegrabbingHandleKeepsTheCorrectAnchor() {
        send("printf '\\033[2J\\033[Halpha beta gamma'; sleep 30\r")
        waitFor("alpha beta gamma"); longPressCell(7, 0)
        ui.waitUntil { terminal().currentFrame!!.cells[9].flags and 256 != 0 }
        var cw = 0; var ch = 0
        ui.runOnIdle { val view = terminal(); cw = view.width / view.currentFrame!!.cols; ch = view.height / view.currentFrame!!.rows }
        val end = HandleDrag(start = false)
        end.move(cw * 3.5f, ch * 0.5f)
        end.move(cw * 1.5f, ch * 0.5f)
        end.finish()
        ui.waitUntil { terminal().currentFrame!!.cells[1].flags and 256 != 0 }
        kotlinx.coroutines.runBlocking { assertEquals("lpha b", sessions.active!!.surface.copySelection()) }
        val start = HandleDrag(start = true)
        start.move(cw * 0.5f, ch * 0.5f)
        start.finish()
        ui.waitUntil { terminal().currentFrame!!.cells[0].flags and 256 != 0 }
        kotlinx.coroutines.runBlocking { assertEquals("alpha b", sessions.active!!.surface.copySelection()) }
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
