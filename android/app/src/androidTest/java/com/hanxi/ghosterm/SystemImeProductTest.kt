package com.hanxi.ghosterm

import android.app.Application
import android.app.Dialog
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.provider.Settings
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.terminal.TerminalView
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Exercise the installed system IME instead of calling InputConnection by hand. */
@RunWith(AndroidJUnit4::class)
class SystemImeProductTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private lateinit var directory: File
    private lateinit var sessions: SessionsViewModel
    private var stockDialog: Dialog? = null
    private var originalSubtypeSetting: String? = null
    private var originalSubtypeHash: Int? = null

    @Before fun setup() {
        directory = File(instrumentation.targetContext.cacheDir, "system-ime-${UUID.randomUUID()}")
            .apply { check(mkdirs()) }
        val app = object : Application() { override fun getFilesDir() = directory }
        val hosts = HostsViewModel(app)
        val runtime = RuntimeInfoViewModel()
        sessions = SessionsViewModel(app)
        instrumentation.runOnMainSync {
            ui.activity.enableEdgeToEdge()
            ui.activity.window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        }
        ui.setContent { GhostermApp(hosts, runtime, sessions) }
        ui.onNodeWithText("本地终端").performClick()
        ui.waitUntil(10_000) { ui.runOnIdle { sessions.active?.state?.connected == true } }
        ui.waitUntil(10_000) { ui.runOnIdle { terminal()?.currentFrame != null } }
        ui.runOnIdle {
            assertTrue(checkNotNull(terminal()).commitText("stty -echo; PS1=''; printf '\\033[2J\\033[HIME_PRODUCT_READY'\r"))
        }
        ui.waitUntil(10_000) { ui.runOnIdle { terminal()?.currentFrame?.line(0) == "IME_PRODUCT_READY" } }
    }

    @After fun cleanup() {
        try {
            if (::sessions.isInitialized) ui.runOnIdle {
                stockDialog?.dismiss()
                stockDialog = null
                terminal()?.hideKeyboard()
                sessions.sessions.map { it.id }.forEach(sessions::close)
            }
        } finally {
            originalSubtypeSetting?.let { previous ->
                if (previous == "null") shell("settings delete secure selected_input_method_subtype")
                else shell("settings put secure selected_input_method_subtype $previous")
                assertEquals("Restore the original IME subtype preference", previous,
                    shell("settings get secure selected_input_method_subtype").trim())
                originalSubtypeHash?.let { hash ->
                    ui.waitUntil(10_000) {
                        ui.activity.getSystemService(InputMethodManager::class.java)
                            .currentInputMethodSubtype?.hashCode() == hash
                    }
                }
            }
            if (::directory.isInitialized) directory.deleteRecursively()
        }
    }

    private fun terminal(): TerminalView? {
        fun find(view: View): TerminalView? {
            if (view is TerminalView) return view
            if (view is ViewGroup) for (index in 0 until view.childCount) {
                find(view.getChildAt(index))?.let { return it }
            }
            return null
        }
        return find(ui.activity.window.decorView)
    }

    private fun shell(command: String): String =
        instrumentation.uiAutomation.executeShellCommand(command).use {
            ParcelFileDescriptor.AutoCloseInputStream(it).bufferedReader().readText()
        }

    private val command = "printf ghosterm-ime-flow-proof > ime-proof.txt"
    private val nextCommand = "printf ghosterm-ime-next-proof > ime-next.txt"
    private fun typeCommand(value: String = command) {
        // UiAutomation uses Runtime.exec rather than a shell: quotes would be
        // typed literally. Encoded spaces keep the fixed payload in one argv.
        shell("input text " + value.replace(" ", "%s"))
    }

    private fun awaitShellCompletion(marker: String) {
        ui.runOnIdle {
            assertTrue(checkNotNull(terminal()).commitText("printf '\\r\\n$marker\\r\\n'\r"))
        }
        fun completed(): Boolean = ui.runOnIdle {
            terminal()?.currentFrame?.let { frame ->
                (0 until frame.rows).joinToString("\n", transform = frame::line).contains(marker)
            } == true
        }
        val deadline = SystemClock.uptimeMillis() + 10_000
        while (!completed() && SystemClock.uptimeMillis() < deadline) SystemClock.sleep(20)
        assertTrue("Shell must execute the completion barrier after the IME command", completed())
    }

    private fun useEnglishSubtypeForShellCommand(ime: String) {
        val manager = ui.activity.getSystemService(InputMethodManager::class.java)
        val current = manager.currentInputMethodSubtype
        if (current?.mode == "keyboard" &&
            (current.languageTag.startsWith("en") || current.locale.startsWith("en"))) return
        val info = checkNotNull(manager.enabledInputMethodList.firstOrNull { it.id == ime })
        val english = manager.getEnabledInputMethodSubtypeList(info, true).firstOrNull {
            it.mode == "keyboard" &&
                (it.languageTag.startsWith("en") || it.locale.startsWith("en"))
        } ?: return // Old IMEs may expose no locale; the stock editor must still prove exact ASCII input.
        if (current?.hashCode() == english.hashCode()) return
        originalSubtypeSetting = shell("settings get secure selected_input_method_subtype").trim()
        originalSubtypeHash = current?.hashCode()
        // Test-only setup through the shell; keep the IME and app input type.
        // Restore both the stored preference and effective subtype in cleanup.
        shell("settings put secure selected_input_method_subtype ${english.hashCode()}")
        ui.waitUntil(10_000) { manager.currentInputMethodSubtype?.hashCode() == english.hashCode() }
    }

    private fun stockEditorControl() {
        lateinit var editor: EditText
        ui.runOnIdle {
            editor = EditText(ui.activity).apply {
                inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or
                    InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING or
                    EditorInfo.IME_ACTION_NONE
            }
            stockDialog = Dialog(ui.activity).apply {
                setContentView(editor)
                show()
                window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
            }
            editor.requestFocus()
        }
        ui.waitUntil(10_000) { ui.runOnIdle { editor.hasWindowFocus() } }
        ui.runOnIdle { ui.activity.getSystemService(InputMethodManager::class.java)
            .showSoftInput(editor, InputMethodManager.SHOW_IMPLICIT) }
        typeCommand()
        shell("input keyevent 66")
        val deadline = SystemClock.uptimeMillis() + 2_000
        while (ui.runOnIdle { editor.text.toString() } != command + "\n" &&
            SystemClock.uptimeMillis() < deadline) SystemClock.sleep(20)
        assertEquals("System IME command driver must preserve text in an independent stock editor",
            command + "\n", ui.runOnIdle { editor.text.toString() })
        typeCommand(nextCommand)
        shell("input keyevent 66")
        val nextDeadline = SystemClock.uptimeMillis() + 2_000
        while (ui.runOnIdle { editor.text.toString() } != command + "\n" + nextCommand + "\n" &&
            SystemClock.uptimeMillis() < nextDeadline) SystemClock.sleep(20)
        assertEquals("Stock editor control also preserves the second line", command + "\n" + nextCommand + "\n",
            ui.runOnIdle { editor.text.toString() })
        ui.runOnIdle {
            ui.activity.getSystemService(InputMethodManager::class.java)
                .hideSoftInputFromWindow(editor.windowToken, 0)
            stockDialog?.dismiss()
            stockDialog = null
        }
        ui.waitUntil(10_000) { ui.runOnIdle { terminal()?.hasWindowFocus() == true } }
    }

    @Test fun systemKeyboardProducesTheExactCommandFile() {
        val ime = Settings.Secure.getString(instrumentation.targetContext.contentResolver,
            Settings.Secure.DEFAULT_INPUT_METHOD)
        assertTrue("A real enabled system IME is required", !ime.isNullOrBlank())
        useEnglishSubtypeForShellCommand(checkNotNull(ime))
        stockEditorControl()
        ui.onNodeWithTag("terminal-keyboard").performTouchInput { click() }
        val keyboardDeadline = SystemClock.uptimeMillis() + 10_000
        var keyboardState = shell("dumpsys input_method")
        while (!keyboardState.contains("mInputShown=true") && SystemClock.uptimeMillis() < keyboardDeadline) {
            SystemClock.sleep(20)
            keyboardState = shell("dumpsys input_method")
        }
        assertTrue("System keyboard display precondition: $keyboardState",
            keyboardState.contains("mInputShown=true"))
        // These are real virtual-key events routed through the device IME. The
        // command contains only this test's fixed ASCII fixture, never user input.
        typeCommand()
        shell("input keyevent 66")
        awaitShellCompletion("IME_FIRST_COMMAND_DONE")
        val result = File(directory, "home/ime-proof.txt")
        val screen = ui.runOnIdle { terminal()?.currentFrame?.let { frame ->
            (0 until frame.rows).joinToString("\n", transform = frame::line) } }
        assertTrue("System IME command must create the exact target file ($ime); actual names: " +
            File(directory, "home").listFiles().orEmpty().map { it.name } + "; screen: $screen", result.isFile)
        assertEquals("System IME must preserve the exact command bytes ($ime)",
            "ghosterm-ime-flow-proof", result.readText(Charsets.UTF_8))
        typeCommand(nextCommand)
        shell("input keyevent 66")
        awaitShellCompletion("IME_SECOND_COMMAND_DONE")
        val nextResult = File(directory, "home/ime-next.txt")
        assertTrue("System IME must preserve command entry after Return", nextResult.isFile)
        assertEquals("Second command contents", "ghosterm-ime-next-proof", nextResult.readText(Charsets.UTF_8))
    }
}
