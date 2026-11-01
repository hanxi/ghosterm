package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.terminal.TerminalView
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.io.FileInputStream
import java.util.UUID

/** Real UI and PTY. Ctrl-U is checked through normal shell editing, without stty raw. */
@RunWith(AndroidJUnit4::class)
class TerminalControlsTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private lateinit var root: File
    private lateinit var owner: AppSettings
    private lateinit var model: SessionsViewModel
    private val store = ViewModelStore()
    private val contentVisible = mutableStateOf(true)
    private fun <T> main(block: () -> T): T {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) return block()
        var result: T? = null
        instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(message: String = "Terminal controls did not settle", predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 10_000
        while (SystemClock.elapsedRealtime() < end) { if (predicate()) return; SystemClock.sleep(25) }
        fail(message)
    }
    private fun shell(command: String) = instrumentation.uiAutomation.executeShellCommand(command).use {
        FileInputStream(it.fileDescriptor).bufferedReader().use { reader -> reader.readText() }
    }
    private fun imeShown() = "mInputShown=true" in shell("dumpsys input_method")
    private fun view(): TerminalView = main {
        fun find(item: View): TerminalView? = (item as? TerminalView) ?: (item as? ViewGroup)?.let { group ->
            (0 until group.childCount).firstNotNullOfOrNull { find(group.getChildAt(it)) }
        }
        checkNotNull(find(ui.activity.window.decorView))
    }
    private fun save(vararg edits: PreferenceEdit) {
        assertTrue(runBlocking { withTimeout(5_000) { owner.save(UUID.randomUUID(), edits.toList()) } } is PreferenceUpdate.Saved)
        ui.waitForIdle()
    }
    @Before fun setup() {
        val context = instrumentation.targetContext
        root = File(context.cacheDir, "terminal-controls-${UUID.randomUUID()}").apply { mkdirs() }
        val app = object : Application() { init { attachBaseContext(context) }; override fun getFilesDir() = root }
        main {
            owner = AppSettings(PreferencesRepository(PreferencesFile(File(root, "preferences"))))
            model = SessionsViewModel(app, settings = owner); store.put("sessions", model)
        }
        await("Fixture: preferences did not load") { owner.state.value.storage is PreferencesState.Ready }
        ui.setContent { MaterialTheme { if (contentVisible.value && model.visible) TerminalScreen(model) {} } }
    }
    @After fun cleanup() {
        if (::model.isInitialized) main { model.home(); store.clear(); owner.close() }
        if (::owner.isInitialized) runBlocking { withTimeout(5_000) { owner.awaitClosed() } }
        if (::root.isInitialized) root.deleteRecursively()
    }
    private fun add(): TerminalSession {
        val count = main { model.sessions.size }
        main { model.newLocal() }
        await("Fixture: local terminal did not become ready") { main { model.sessions.size == count + 1 && !model.creating && model.active?.state?.connected == true } }
        ui.waitForIdle(); await("Fixture: terminal view did not join its window") { main { view().isAttachedToWindow && view().hasWindowFocus() } }
        return main { checkNotNull(model.active) }
    }
    private fun prepare(session: TerminalSession) {
        session.connection.commit("PS1=''; printf '\\033[3J\\033[2J\\033[HCTRL_READY\\r\\n'\r")
        output(session, "CTRL_READY")
    }
    private fun output(session: TerminalSession, marker: String) {
        await("Real shell did not produce $marker") {
            session.connection.frame(0)?.let { f -> (0 until f.rows).any { f.line(it) == marker } } == true
        }
    }
    private fun command(view: TerminalView, marker: String) = main { view.commitText("printf '$marker\\r\\n'\r") }

    @Test fun basicExtendedAndHiddenKeysUpdateTheSameViewAndPty() {
        val session = add(); val original = view()
        ui.onNodeWithTag("terminal-auxiliary-keys").assertIsDisplayed()
        ui.onNodeWithText("Home").assertDoesNotExist()
        save(PreferenceEdit.KeySet(AuxiliaryKeySet.EXTENDED))
        ui.onNodeWithText("Home").performScrollTo().assertIsDisplayed()
        val bounds = ui.onNodeWithText("Home").fetchSemanticsNode().touchBoundsInRoot
        assertTrue(bounds.height >= 48 * ui.activity.resources.displayMetrics.density - 1)
        save(PreferenceEdit.AuxKeys(false))
        ui.onNodeWithTag("terminal-auxiliary-keys").assertDoesNotExist()
        assertSame(original, view()); assertSame(session.surface, main { view().surface })
        command(original, "HIDDEN_KEYS_OK"); output(session, "HIDDEN_KEYS_OK")
    }

    @Test fun onceCtrlClearsTheActualShellLineAndReleasesBeforeTyping() {
        val session = add(); prepare(session); val terminal = view()
        session.connection.commit("invalid-command")
        ui.onNodeWithText("Ctrl").performScrollTo().performClick()
        ui.onNodeWithText("Ctrl ✓").assertExists()
        main { assertEquals(1, terminal.ctrl); assertTrue(terminal.sendKey(KeyEvent.KEYCODE_U, text = "u", unshiftedCodepoint = 'u'.code)); assertEquals(0, terminal.ctrl) }
        command(terminal, "ONCE_CTRL_OK"); output(session, "ONCE_CTRL_OK")
    }

    @Test fun lockedCtrlAppliesToTwoShellEditsUntilExplicitlyReleased() {
        save(PreferenceEdit.CtrlMode(CtrlBehavior.LOCKED)); val session = add(); prepare(session); val terminal = view()
        session.connection.commit("first-invalid")
        ui.onNodeWithText("Ctrl").performScrollTo().performClick()
        ui.onNodeWithText("Ctrl 锁定").assertExists()
        main { assertEquals(2, terminal.ctrl); assertTrue(terminal.sendKey(KeyEvent.KEYCODE_U, text = "u", unshiftedCodepoint = 'u'.code)); assertEquals(2, terminal.ctrl) }
        session.connection.commit("second-invalid")
        main { assertTrue(terminal.sendKey(KeyEvent.KEYCODE_U, text = "u", unshiftedCodepoint = 'u'.code)); assertEquals(2, terminal.ctrl) }
        ui.onNodeWithText("Ctrl 锁定").performClick()
        main { assertEquals(0, terminal.ctrl) }
        command(terminal, "LOCKED_CTRL_OK"); output(session, "LOCKED_CTRL_OK")
    }

    @Test fun modeChangesAndHidingKeysReleasePendingCtrl() {
        val session = add(); prepare(session); val terminal = view()
        ui.onNodeWithText("Ctrl").performScrollTo().performClick()
        save(PreferenceEdit.CtrlMode(CtrlBehavior.LOCKED))
        main { assertEquals(0, terminal.ctrl) }
        ui.onNodeWithText("Ctrl").performClick()
        main { assertEquals(2, terminal.ctrl) }
        save(PreferenceEdit.AuxKeys(false))
        main { assertEquals(0, terminal.ctrl); terminal.toggleCtrl(); assertEquals(0, terminal.ctrl) }
        command(terminal, "POLICY_RELEASE_OK"); output(session, "POLICY_RELEASE_OK")
    }

    @Test fun keepAwakeTracksWindowFocusAndStopsWhenTheTerminalLeaves() {
        save(PreferenceEdit.KeepAwake(true)); val session = add(); val terminal = view()
        await { main { terminal.keepScreenOn } }
        val windows = shell("dumpsys window windows")
        val block = Regex("(?s)Window #\\d+ Window\\{[^\\n]*com\\.hanxi\\.ghosterm/androidx\\.activity\\.ComponentActivity.*?(?=\\n\\s*Window #|\\z)").find(windows)?.value
        assertNotNull("Fixture: could not locate the actual terminal window", block)
        assertTrue("The actual window must carry KEEP_SCREEN_ON", block!!.contains("KEEP_SCREEN_ON"))
        val dialog = main { android.app.AlertDialog.Builder(ui.activity).setMessage("常亮焦点对照").setPositiveButton("返回", null).show() }
        try { await { main { !terminal.hasWindowFocus() && !terminal.keepScreenOn } } }
        finally { main { dialog.dismiss() } }
        await { main { terminal.keepScreenOn } }
        main { model.home() }; ui.waitForIdle()
        assertFalse(main { terminal.keepScreenOn }); assertFalse(main { terminal.isAttachedToWindow })
        main { model.show(session.id) }; ui.waitForIdle()
        await { main { view().keepScreenOn } }
        save(PreferenceEdit.KeepAwake(false)); await { main { !view().keepScreenOn } }
        assertSame(session, main { model.active })
    }

    @Test fun automaticKeyboardShowsForNewConnectionsAndNotExistingSelections() {
        save(PreferenceEdit.AutoKeyboard(true)); val first = add()
        await("A new ready connection must show the system keyboard") { imeShown() }
        main { view().hideKeyboard() }; await { !imeShown() }
        main { model.home() }; ui.waitForIdle(); main { model.show(first.id) }; ui.waitForIdle()
        SystemClock.sleep(1_200)
        assertFalse("Selecting an existing session must not summon the keyboard", imeShown())
        val second = add(); assertNotEquals(first.id, second.id)
        await { imeShown() }
        main { view().hideKeyboard() }; await { !imeShown() }
    }

    @Test fun enablingAutomaticKeyboardDoesNotAffectTheAlreadyReadyConnection() {
        val session = add()
        assertFalse("Default setting leaves the keyboard hidden", imeShown())
        save(PreferenceEdit.AutoKeyboard(true))
        SystemClock.sleep(1_200)
        assertFalse("Only a new connection may request automatic keyboard", imeShown())
        assertSame(session, main { model.active })
    }

    @Test fun manualHideCancelsAnAutomaticRequestWaitingForWindowFocus() {
        add(); val terminal = view()
        val dialog = main { android.app.AlertDialog.Builder(ui.activity).setMessage("等待焦点").setPositiveButton("返回", null).show() }
        try {
            await { main { !terminal.hasWindowFocus() } }
            main { terminal.requestAutomaticKeyboard(); terminal.hideKeyboard() }
        } finally { main { dialog.dismiss() } }
        SystemClock.sleep(1_200)
        assertFalse("A manual hide wins over a pending automatic request", imeShown())
    }

    @Test fun anAutomaticRequestExpiresWhileAnotherWindowOwnsFocus() {
        add(); val terminal = view()
        val dialog = main { android.app.AlertDialog.Builder(ui.activity).setMessage("自动请求期限对照").setPositiveButton("返回", null).show() }
        try {
            await { main { !terminal.hasWindowFocus() } }
            main { terminal.requestAutomaticKeyboard() }
            SystemClock.sleep(1_200)
        } finally { main { dialog.dismiss() } }
        SystemClock.sleep(250)
        assertFalse("An expired request cannot summon the keyboard on a later focus", imeShown())
    }
}
