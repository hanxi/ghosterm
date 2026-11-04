package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import android.view.View
import android.view.ViewGroup
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real local owners/PTY input and system IME; remote labels below are presentation fixtures only. */
@RunWith(AndroidJUnit4::class)
class TerminalSessionPanelTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private lateinit var directory: File
    private lateinit var model: SessionsViewModel
    private lateinit var first: TerminalSession
    private val instrumentation get() = InstrumentationRegistry.getInstrumentation()
    private fun find(view: View): TerminalView? = (view as? TerminalView) ?: (view as? ViewGroup)?.let { group ->
        (0 until group.childCount).firstNotNullOfOrNull { find(group.getChildAt(it)) }
    }
    private fun terminal() = checkNotNull(find(ui.activity.window.decorView))
    private fun await(message: String, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 10_000
        while (SystemClock.elapsedRealtime() < end) {
            ui.mainClock.advanceTimeByFrame()
            if (predicate()) return
            SystemClock.sleep(25)
        }
        fail(message)
    }
    @Before fun prepare() {
        directory = File(instrumentation.targetContext.cacheDir, "session-panel-${UUID.randomUUID()}").apply { mkdirs() }
        val app = object : Application() {
            init { attachBaseContext(instrumentation.targetContext) }
            override fun getFilesDir() = directory
        }
        model = SessionsViewModel(app, settings = null)
        val hosts = HostsViewModel(app)
        val runtime = RuntimeInfoViewModel()
        ui.runOnUiThread {
            ui.activity.enableEdgeToEdge()
            // Match MainActivity's manifest; adjustPan cannot supply API 29 IME insets.
            ui.activity.window.setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        }
        ui.setContent { GhostermApp(hosts, runtime, model) }
        first = newLocal()
        await("initial keyboard must be closed") { !terminalImeShown() }
    }
    @After fun cleanup() {
        ui.runOnIdle { model.sessions.toList().forEach { model.close(it.id) } }
        directory.deleteRecursively()
    }
    private fun newLocal(): TerminalSession {
        val old = model.active?.id
        ui.runOnIdle { model.newLocal() }
        await("new owner must connect and attach") { ui.runOnIdle {
            model.active?.let { it.id != old && it.state.connected && find(ui.activity.window.decorView)?.surface === it.surface } == true
        } }
        return model.active!!
    }
    private fun hideKeyboard() {
        ui.runOnIdle { terminal().hideKeyboard() }
        await("actual IME must close") { !terminalImeShown() }
    }
    private fun settleInputFrames() {
        // A wall-clock sleep alone leaves withFrameNanos suspended on the
        // Compose test clock, hiding an incorrect pending keyboard request.
        repeat(20) { ui.mainClock.advanceTimeByFrame(); SystemClock.sleep(60) }
        ui.waitForIdle()
    }
    private fun select(id: UUID) {
        ui.onNodeWithTag("terminal-session-list").performScrollToNode(hasTestTag("terminal-session-select-$id"))
        ui.onNodeWithTag("terminal-session-select-$id").performClick()
        await("selected owner must attach") { ui.runOnIdle { model.active?.id == id && terminal().surface === model.active?.surface } }
    }
    private fun appendLabel(alias: String, number: Int, tmux: String? = null): TerminalSession {
        val owner = LocalSession(directory.absolutePath)
        val row = TerminalSession(UUID.randomUUID(), alias, owner, alias, tmux).also { it.switcherNumber = number }
        ui.runOnIdle { model.sessions.add(row) }
        await("presentation fixture must have a live native owner") { ui.runOnIdle { row.state.connected } }
        return row
    }

    @Test fun fixedEntryReplacesMenuAndRemainsReachableWithoutAuxiliaryKeys() {
        ui.onNodeWithTag("terminal-sessions").assertIsDisplayed()
        ui.onNodeWithTag("terminal-menu").performClick()
        ui.onNodeWithTag("terminal-menu-sessions").assertDoesNotExist()
        ui.onAllNodes(hasText("切换会话", substring = true)).assertCountEquals(0)
        ui.terminalBack()
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-new-local").assertIsDisplayed()
        ui.onNodeWithTag("terminal-new-ssh").assertIsDisplayed()
        val panel = ui.onNodeWithTag("terminal-session-panel").fetchSemanticsNode().boundsInRoot
        assertTrue("a single-session panel must not occupy most of a normal phone window",
            panel.height < ui.activity.resources.displayMetrics.heightPixels * .75f)
        ui.onNodeWithTag("terminal-sessions-dismiss").performClick()
        assertSame(first, model.active)
        assertTrue(first.connection.state().connected)
    }

    @Test fun previousSelectionKeepsOwnersOrderAndStableNumbers() {
        val second = newLocal()
        val before = model.sessions.map { it.id }
        assertEquals(1, first.switcherNumber)
        assertEquals(2, second.switcherNumber)
        assertSame(first, model.previousSession)
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-previous-session").performClick()
        assertSame(first, model.active)
        assertSame(second, model.previousSession)
        assertEquals(before, model.sessions.map { it.id })
        ui.runOnIdle { model.show(first.id); model.show(UUID.randomUUID()) }
        assertSame(second, model.previousSession)
        ui.runOnIdle { model.close(second.id) }
        assertNull(model.previousSession)
        assertTrue(first.connection.state().connected)
    }

    private fun capture(name: String) {
        val image = checkNotNull(instrumentation.uiAutomation.takeScreenshot())
        try {
            val folder = File(instrumentation.targetContext.cacheDir, "session-ime-review").apply { mkdirs() }
            File(folder, "$name.png").outputStream().use { image.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
        } finally { image.recycle() }
    }

    private fun keepImeAndViewport(checkHeight: Boolean = true, block: () -> Unit) {
        ui.waitForIdle()
        val height = ui.runOnIdle { terminal().height }
        val violations = mutableListOf<String>()
        val observer = android.view.ViewTreeObserver.OnPreDrawListener {
            val view = find(ui.activity.window.decorView)
            val visible = view?.let { androidx.core.view.ViewCompat.getRootWindowInsets(it)
                ?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime()) } == true
            if (!visible || (checkHeight && view?.height != height) || !view.hasWindowFocus())
                violations += "ime=$visible height=${view?.height}/$height focus=${view?.hasWindowFocus()}"
            true
        }
        ui.runOnIdle { ui.activity.window.decorView.viewTreeObserver.addOnPreDrawListener(observer) }
        try {
            block()
            settleInputFrames()
            ui.runOnIdle { assertTrue("chooser must retain the IME, window focus and viewport on every frame: $violations", violations.isEmpty()) }
        } finally {
            ui.runOnIdle { ui.activity.window.decorView.viewTreeObserver.removeOnPreDrawListener(observer) }
        }
    }

    @Test fun openingAndCancellingPanelKeepsImeVisibleOnEveryFrame() {
        ui.showTerminalKeyboard()
        settleInputFrames()
        val original = terminal()
        capture("keyboard-before")
        keepImeAndViewport {
            ui.openTerminalSessions()
            ui.onNodeWithTag("terminal-session-panel").assertIsDisplayed()
            settleInputFrames()
            assertTrue("opening the chooser must leave the actual IME visible", terminalImeShown())
            val bottom = ui.onNodeWithTag("terminal-session-panel").fetchSemanticsNode().boundsInRoot.bottom
            val window = ui.runOnIdle { android.graphics.Rect().also { ui.activity.window.decorView.getWindowVisibleDisplayFrame(it) } }
            assertTrue("chooser must stay above the actual IME", bottom <= window.bottom + 1)
            capture("keyboard-panel")
            val lateBack = ui.runOnIdle { checkNotNull(terminal().onBackBeforeIme) }
            ui.onNodeWithTag("terminal-sessions-dismiss").performClick()
            ui.onNodeWithTag("terminal-session-panel").assertDoesNotExist()
            ui.runOnIdle { lateBack() }
            ui.openTerminalSessions()
            ui.onNodeWithTag("terminal-sessions-scrim").performTouchInput { click(androidx.compose.ui.geometry.Offset(10f, 5f)) }
            ui.onNodeWithTag("terminal-session-panel").assertDoesNotExist()
        }
        assertSame(original, terminal())
        assertSame(first, model.active)
    }

    @Test fun searchingKeepsImeVisibleAndReturnsInputFocusToSameTerminal() {
        repeat(6) { newLocal() }
        val active = model.active!!
        ui.showTerminalKeyboard()
        settleInputFrames()
        keepImeAndViewport(checkHeight = false) {
            ui.openTerminalSessions()
            ui.onNodeWithTag("terminal-session-search").performClick().performTextInput("本地")
            settleInputFrames()
            capture("keyboard-search-many")
            ui.onNodeWithTag("terminal-sessions-dismiss").performClick()
            await("dismissing search must return native terminal input focus") { ui.runOnIdle { terminal().hasFocus() } }
            ui.openTerminalSessions()
            ui.onNodeWithTag("terminal-session-search").performClick().performTextInput("本地")
            ui.terminalBack()
            ui.onNodeWithTag("terminal-session-panel").assertDoesNotExist()
            await("Back from search must return native terminal input focus") { ui.runOnIdle { terminal().hasFocus() } }
        }
        assertSame(active, model.active)
        ui.terminalBack()
        await("normal Back after the chooser leaves must hide the actual IME") { !terminalImeShown() }
    }

    @Test fun switchAndDismissRestoreOpenImeAndInputGoesOnlyToSelectedPty() {
        val second = newLocal()
        val originalPid = File(directory, "original-pid")
        val otherPid = File(directory, "other-pid")
        ui.runOnIdle {
            first.connection.commit("printf '%s' \"\u0024\u0024\" > '${originalPid.path}'\r")
            second.connection.commit("printf '%s' \"\u0024\u0024\" > '${otherPid.path}'\r")
        }
        await("both real shells must record their own PID") {
            originalPid.isFile && originalPid.readText().trim().toIntOrNull() != null &&
                otherPid.isFile && otherPid.readText().trim().toIntOrNull() != null
        }
        ui.showTerminalKeyboard()
        settleInputFrames()
        // restartInput may update an IME's candidate strip when the owner
        // changes. It must stay visible; opening/cancelling is geometry-stable.
        keepImeAndViewport(checkHeight = false) {
            ui.openTerminalSessions()
            capture("keyboard-two-sessions")
            select(first.id)
            ui.awaitTerminalKeyboard()
        }
        capture("keyboard-after-switch")
        val proof = File(directory, "input-proof")
        val connection = ui.runOnIdle { terminal().onCreateInputConnection(android.view.inputmethod.EditorInfo())!! }
        ui.runOnIdle {
            assertTrue(terminal().hasFocus())
            assertTrue(connection.commitText("printf '%s' \"\u0024\u0024\" > '${proof.path}'\n", 1))
        }
        await("IME input must reach the selected PTY") { proof.isFile && proof.readText().trim().toIntOrNull() != null }
        assertEquals(originalPid.readText(), proof.readText())
        assertNotEquals(otherPid.readText(), proof.readText())
        assertTrue(second.connection.state().connected)
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-sessions-dismiss").performClick()
        ui.awaitTerminalKeyboard()
        ui.openTerminalSessions()
        ui.terminalBack()
        ui.onNodeWithTag("terminal-session-panel").assertDoesNotExist()
        ui.awaitTerminalKeyboard()
        assertSame(first, model.active)
    }

    @Test fun closedImeStaysClosedAndStoppedPreviousDoesNotReappear() {
        val second = newLocal()
        hideKeyboard()
        ui.openTerminalSessions()
        select(first.id)
        settleInputFrames()
        assertFalse("switching must not summon a closed IME", terminalImeShown())
        ui.runOnIdle { second.connection.commit("exit 7\r") }
        await("other owner must actually exit") { second.state.terminal }
        assertNull("a stopped owner cannot be the quick return target", model.previousSession)
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-previous-session").assertDoesNotExist()
        ui.onNodeWithTag("terminal-session-select-${second.id}").assertDoesNotExist()
        ui.onNodeWithTag("terminal-session-list").performScrollToNode(hasTestTag("terminal-stopped-toggle"))
        ui.onNodeWithTag("terminal-stopped-toggle").performClick()
        select(second.id)
        assertSame(second, model.active)
        assertFalse(terminalImeShown())
        ui.runOnIdle { assertFalse(terminal().commitText("must-not-be-sent")) }
    }

    @Test fun groupedSearchAndFooterKeepCurrentAndAllCreationActionsVisible() {
        val dev = appendLabel("dev-box", 1)
        appendLabel("dev-box", 2)
        appendLabel("production", 1, "deploy")
        repeat(4) { appendLabel("extra-$it", 1) }
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-session-row-${first.id}").assertIsSelected()
        ui.onNodeWithText("本机").assertIsDisplayed()
        ui.onNodeWithText("dev-box").assertIsDisplayed()
        ui.onNodeWithTag("terminal-session-search").performTextInput("dev-box")
        ui.onNodeWithTag("terminal-session-row-${first.id}").assertDoesNotExist()
        ui.onNodeWithTag("terminal-session-row-${dev.id}").assertIsNotSelected()
        ui.onNodeWithText("终端 1").assertIsDisplayed()
        ui.onNodeWithText("终端 2").assertIsDisplayed()
        ui.onNodeWithTag("terminal-new-local").assertIsDisplayed()
        ui.onNodeWithTag("terminal-new-ssh").assertIsDisplayed()
        ui.onNodeWithTag("terminal-session-search").performTextReplacement("no-such-host")
        ui.onNodeWithText("没有匹配的会话").assertIsDisplayed()
        assertSame(first, model.active)
        ui.onNodeWithTag("terminal-session-search").performTextReplacement("dev-box")
        select(dev.id)
        settleInputFrames()
        assertFalse("searching must not change the originally closed IME policy", terminalImeShown())
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-session-search").performTextReplacement("tmux")
        ui.onNodeWithText("deploy · 终端 1").assertIsDisplayed()
        ui.onNodeWithTag("terminal-new-ssh").performClick()
        await("new SSH must route to host selection") { model.selectingNewRemote && !model.visible }
        assertTrue(first.connection.state().connected)
    }

    @Test fun rowCloseConfirmationKeepsTheCapturedUuidAcrossSelectionChanges() {
        val second = newLocal()
        ui.openTerminalSessions()
        ui.onNodeWithTag("terminal-session-more-${first.id}").performClick()
        ui.onNodeWithTag("terminal-session-close-${first.id}").performClick()
        ui.onNodeWithText("取消").performClick()
        assertEquals(2, model.sessions.size)
        ui.onNodeWithTag("terminal-session-more-${first.id}").performClick()
        ui.onNodeWithTag("terminal-session-close-${first.id}").performClick()
        ui.runOnIdle { model.show(first.id); model.show(second.id) }
        ui.onNodeWithTag("terminal-close-confirm").performClick()
        assertFalse(model.sessions.any { it.id == first.id })
        assertSame(second, model.active)
        assertTrue(second.connection.state().connected)
        assertNull(model.previousSession)
    }
}
