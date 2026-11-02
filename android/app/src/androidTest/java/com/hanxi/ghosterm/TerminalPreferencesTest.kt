package com.hanxi.ghosterm

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.os.SystemClock
import android.util.TypedValue
import android.view.View
import android.view.ViewGroup
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import com.hanxi.ghosterm.bridge.NativeTerminalSession
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.terminal.*
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import kotlin.math.ceil

@RunWith(AndroidJUnit4::class)
class TerminalPreferencesTest {
    @get:Rule val ui = createAndroidComposeRule<ComponentActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun <T> main(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(message: String = "Terminal preference result did not arrive", predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 10_000
        while (SystemClock.elapsedRealtime() < end) { if (predicate()) return; SystemClock.sleep(15) }
        fail(message)
    }
    private fun frame(session: NativeTerminalSession, predicate: (TerminalFrame) -> Boolean): TerminalFrame {
        var value: TerminalFrame? = null
        await { session.frame(0)?.let { value = it }; value?.let(predicate) == true }
        return checkNotNull(value)
    }
    private fun output(session: NativeTerminalSession, sequence: String, marker: String): TerminalFrame {
        session.commit("printf '$sequence$marker\\r\\n'\r")
        return frame(session) { (0 until it.rows).any { row -> it.line(row) == marker } }
    }
    private fun configure(session: NativeTerminalSession, appearance: TerminalAppearance): TerminalFrame {
        val previous = frame(session) { true }.revision
        runBlocking { withTimeout(5_000) { session.configure(appearance) } }
        return frame(session) { it.revision > previous }
    }
    private fun terminal(test: (LocalSession) -> Unit) {
        val root = File(instrumentation.targetContext.cacheDir, "appearance-native-${UUID.randomUUID()}").apply { mkdirs() }
        val session = LocalSession(root.absolutePath)
        try {
            await("Fixture: native terminal did not connect") { session.state().connected }
            session.commit("stty -echo; PS1=''; printf '\\033[0m\\033[2J\\033[HSTYLE_READY\\r\\n'\r")
            frame(session) { it.line(0) == "STYLE_READY" }
            test(session)
        } finally { session.close(); root.deleteRecursively() }
    }
    private class Storage : PreferencesStorage {
        @Volatile var bytes: ByteArray? = null
        @Volatile var reject = false
        override fun read() = bytes?.copyOf()
        override fun write(expected: ByteArray?, bytes: ByteArray): PreferenceWrite {
            if (reject) return PreferenceWrite.Unchanged
            this.bytes = bytes.copyOf(); return PreferenceWrite.Saved
        }
    }
    private fun withModel(test: (File, Storage, AppSettings, SessionsViewModel) -> Unit) {
        val root = File(instrumentation.targetContext.cacheDir, "appearance-model-${UUID.randomUUID()}").apply { mkdirs() }
        val application = object : Application() { init { attachBaseContext(instrumentation.targetContext) }; override fun getFilesDir() = root }
        val storage = Storage(); val store = ViewModelStore()
        lateinit var owner: AppSettings; lateinit var model: SessionsViewModel
        main { owner = AppSettings(PreferencesRepository(storage)); model = SessionsViewModel(application, settings = owner); store.put("sessions", model) }
        try {
            await("Fixture: preferences did not load") { owner.state.value.storage is PreferencesState.Ready }
            test(root, storage, owner, model)
        } finally {
            main { model.home(); store.clear(); owner.close() }
            runBlocking { withTimeout(5_000) { owner.awaitClosed() } }
            root.deleteRecursively()
        }
    }
    private fun save(owner: AppSettings, vararg edits: PreferenceEdit) = runBlocking {
        withTimeout(5_000) { owner.save(UUID.randomUUID(), edits.toList()) }
    }
    private fun add(model: SessionsViewModel): TerminalSession {
        val count = main { model.sessions.size }
        main { model.newLocal() }
        await("Fixture: model terminal did not connect") { main { model.sessions.size == count + 1 && !model.creating && model.active?.state?.connected == true } }
        return main { checkNotNull(model.active) }
    }
    private fun marker(session: TerminalSession, home: File, id: String): String {
        val file = File(home, id)
        session.connection.commit("stty -echo; PS1=''; printf '%s' \"\$\$\" > $id; printf '\\033[0m\\033[2J\\033[HMODEL_READY\\r\\n'\r")
        frame(session.connection) { it.line(0) == "MODEL_READY" }
        await("Fixture: shell PID was not written") { file.isFile && file.length() > 0 }
        return file.readText().also { assertTrue(it.toInt() > 0) }
    }
    private fun terminalView(): TerminalView = main {
        fun find(view: View): TerminalView? = (view as? TerminalView) ?: (view as? ViewGroup)?.let { group ->
            (0 until group.childCount).firstNotNullOfOrNull { find(group.getChildAt(it)) }
        }
        checkNotNull(find(ui.activity.window.decorView))
    }
    private fun bitmap(view: TerminalView, blink: Boolean = false): Bitmap = main {
        instrumentation.setInTouchMode(false)
        view.requestFocus()
        TerminalView::class.java.getDeclaredField("blink").apply { isAccessible = true }.setBoolean(view, blink)
        Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888).also { view.draw(Canvas(it)) }
    }

    @Test fun acknowledgedAppearanceUpdatesTheExistingGridAndDefaultCursor() = terminal { session ->
        val result = configure(session, TerminalAppearance(0xfaf8f2, 0x283b33, CursorShape.UNDERLINE, false))
        assertEquals(0xfffaf8f2.toInt(), result.background); assertEquals(0xff283b33.toInt(), result.foreground)
        assertEquals(2, result.cursorStyle); assertFalse(result.cursorBlink)
        assertEquals("STYLE_READY", result.line(0))
        assertTrue(session.state().connected)
        for ((shape, code) in listOf(CursorShape.BAR to 0, CursorShape.BLOCK to 1, CursorShape.UNDERLINE to 2)) {
            for (blink in listOf(false, true)) {
                val changed = configure(session, TerminalAppearance(0xfaf8f2, 0x283b33, shape, blink))
                assertEquals(code, changed.cursorStyle); assertEquals(blink, changed.cursorBlink)
                assertEquals("STYLE_READY", changed.line(0))
            }
        }
    }

    @Test fun malformedNativeAppearancePacketsAreRejectedBeforeChangingTheGrid() = terminal { session ->
        configure(session, TerminalAppearance(0xfaf8f2, 0x283b33, CursorShape.UNDERLINE, false))
        val type = Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
        val instance = type.getField("INSTANCE").get(null)
        val begin = type.getMethod("beginAction", Long::class.javaPrimitiveType, Int::class.javaPrimitiveType,
            Int::class.javaPrimitiveType, Int::class.javaPrimitiveType, ByteArray::class.java)
        val read = type.getMethod("readAction", Long::class.javaPrimitiveType, Long::class.javaPrimitiveType)
        val end = type.getMethod("endAction", Long::class.javaPrimitiveType, Long::class.javaPrimitiveType)
        val handle = NativeTerminalProbe.handle(session)
        for ((shape, blink, bytes) in listOf(Triple(0, 2, ByteArray(6)), Triple(0, -1, ByteArray(6)),
            Triple(3, 0, ByteArray(6)), Triple(-1, 0, ByteArray(6)), Triple(0, 0, ByteArray(7)), Triple(0, 0, ByteArray(5)))) {
            val revision = frame(session) { true }.revision
            val request = begin.invoke(instance, handle, 12, shape, blink, bytes) as Long
            assertNotEquals("Fixture: appearance request could not be queued", 0L, request)
            try {
                var response: ByteArray? = null
                await { response = read.invoke(instance, handle, request) as ByteArray?; response != null }
                assertEquals("Malformed appearance must return an explicit error", 3, response!![0].toInt())
            } finally { end.invoke(instance, handle, request) }
            val result = frame(session) { it.revision > revision }
            assertEquals("A rejected packet cannot partially change the palette", 0xfffaf8f2.toInt(), result.background)
            assertEquals(0xff283b33.toInt(), result.foreground); assertEquals(2, result.cursorStyle); assertFalse(result.cursorBlink)
        }
    }

    @Test fun programCursorOverridesSurviveDefaultsUntilDecscusrReset() = terminal { session ->
        output(session, "\\033[6 q", "EXPLICIT_CURSOR")
        val result = configure(session, TerminalAppearance(cursor = CursorShape.UNDERLINE, blink = true))
        assertEquals(0, result.cursorStyle); assertFalse(result.cursorBlink)
        output(session, "\\033[0 q", "RESET_CURSOR")
        val reset = frame(session) { it.cursorStyle == 2 && it.cursorBlink }
        assertEquals(2, reset.cursorStyle)
    }

    @Test fun oscOverridesSurviveNewDefaultsAndResetToTheNewPalette() = terminal { session ->
        output(session, "\\033]11;#112233\\007", "OSC_COLOR")
        val result = configure(session, TerminalAppearance(0xfaf8f2, 0x283b33))
        assertEquals(0xff112233.toInt(), result.background)
        output(session, "\\033]111\\007", "OSC_RESET")
        val reset = frame(session) { it.background == 0xfffaf8f2.toInt() }
        assertEquals(0xfffaf8f2.toInt(), reset.background)
    }

    @Test fun appearanceChangesNeverEnterTheShellInputStream() = terminal { session ->
        session.commit("printf '\\033[2J\\033[HREAD_PREF_INPUT\\r\\n'; IFS= read -r line; printf '%s' \"\$line\" | od -An -tx1; printf 'PREF_INPUT_DONE\\r\\n'\r")
        frame(session) { it.line(0) == "READ_PREF_INPUT" }
        configure(session, TerminalAppearance(0x14241c, 0xe2ecdf, CursorShape.BAR, false))
        session.commit("STYLE_INPUT\r")
        val result = frame(session) { (0 until it.rows).any { row -> it.line(row) == "PREF_INPUT_DONE" } }
        // Interactive mksh may echo its read input; inspect od's actual byte output.
        val hex = (1 until result.rows).map(result::line).takeWhile { it != "PREF_INPUT_DONE" }
            .map { it.trim().replace(Regex(" +"), " ") }.filter { it.matches(Regex("[0-9a-f]{2}( [0-9a-f]{2})*")) }.joinToString(" ")
        assertEquals("53 54 59 4c 45 5f 49 4e 50 55 54", hex)
    }

    @Test fun committedPreferencesReachExistingAndNewOwnersWithoutReplacingShells() = withModel { root, _, owner, model ->
        val first = add(model); val second = add(model)
        val home = File(root, "home")
        val pids = listOf(first, second).associateWith { marker(it, home, "before-${it.id}") }
        val handles = listOf(first, second).associateWith { NativeTerminalProbe.handle(it.connection) }
        assertTrue(save(owner, PreferenceEdit.Theme(TerminalTheme.PAPER), PreferenceEdit.Cursor(TerminalCursor.UNDERLINE), PreferenceEdit.Blink(false)) is PreferenceUpdate.Saved)
        for (session in listOf(first, second)) {
            frame(session.connection) { it.background == 0xfffaf8f2.toInt() && it.cursorStyle == 2 && !it.cursorBlink }
            assertEquals(handles[session], NativeTerminalProbe.handle(session.connection))
            assertSame(session, main { model.sessions.single { it.id == session.id } })
            assertEquals(pids[session], marker(session, home, "after-${session.id}"))
        }
        val third = add(model)
        val latest = frame(third.connection) { it.background == 0xfffaf8f2.toInt() && it.cursorStyle == 2 && !it.cursorBlink }
        assertEquals(0xff283b33.toInt(), latest.foreground)
        for ((theme, background, foreground) in listOf(
            Triple(TerminalTheme.FOREST, 0xff14241c.toInt(), 0xffe2ecdf.toInt()),
            Triple(TerminalTheme.GRAPHITE, 0xff171b20.toInt(), 0xffdce3ec.toInt()),
            Triple(TerminalTheme.PAPER, 0xfffaf8f2.toInt(), 0xff283b33.toInt()))) {
            assertTrue(save(owner, PreferenceEdit.Theme(theme)) is PreferenceUpdate.Saved)
            for (session in listOf(first, second, third)) {
                val painted = frame(session.connection) { it.background == background }
                assertEquals(foreground, painted.foreground)
            }
        }
    }

    @Test fun failedPreferenceWritesDoNotReconfigureTheTerminal() = withModel { _, storage, owner, model ->
        val session = add(model)
        frame(session.connection) { it.foreground == 0xffdce3ec.toInt() }
        storage.reject = true
        assertTrue(save(owner, PreferenceEdit.Theme(TerminalTheme.PAPER), PreferenceEdit.FontSize(20)) is PreferenceUpdate.Failed)
        val value = frame(session.connection) { true }
        assertEquals(0xff171b20.toInt(), value.background)
        assertEquals(12f, main { model.terminalStyle.fontSize })
    }

    @Test fun liveCanvasStyleResizesTheSamePtyAndPaintsTheNewPalette() = withModel { root, _, owner, model ->
        val session = add(model)
        ui.setContent { MaterialTheme { if (model.visible) TerminalScreen(model) {} } }
        ui.waitForIdle(); val view = terminalView()
        val density = view.resources.displayMetrics
        val defaultFont = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 12f, density)
        val defaultHeight = ceil(defaultFont * 1.85f).toInt()
        val defaultWidth = ceil(Paint().apply { typeface = Typeface.MONOSPACE; textSize = defaultFont }.measureText("M")).toInt()
        frame(session.connection) { it.rows == view.height / defaultHeight && it.cols == view.width / defaultWidth }
        val pid = marker(session, File(root, "home"), "before-view")
        assertTrue(save(owner, PreferenceEdit.Theme(TerminalTheme.PAPER), PreferenceEdit.FontSize(18), PreferenceEdit.LineHeight(TerminalLineHeight.RELAXED)) is PreferenceUpdate.Saved)
        await { main { view.style.fontSize == 18f && view.style.lineHeight == 2.15f } }
        val expectedHeight = ceil(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 18f, density) * 2.15f).toInt()
        val resized = frame(session.connection) { it.rows == view.height / expectedHeight && it.background == 0xfffaf8f2.toInt() }
        output(session.connection, "\\033[0m\\033[2J\\033[H", "STYLE_PIXELS")
        await { main { view.currentFrame?.line(0) == "STYLE_PIXELS" && view.currentFrame?.background == 0xfffaf8f2.toInt() } }
        val image = bitmap(view)
        assertEquals(0xfffaf8f2.toInt(), image.getPixel(image.width - 1, image.height - 1))
        var ink = 0
        for (y in 0 until expectedHeight) for (x in 0 until image.width) if (image.getPixel(x, y) == 0xff283b33.toInt()) ink++
        assertTrue("The updated foreground must reach real glyph pixels", ink > 20); image.recycle()
        session.connection.commit("printf '\\033[2J\\033[H'; stty size; printf 'STYLE_SIZE_DONE\\r\\n'\r")
        val size = frame(session.connection) { it.line(1) == "STYLE_SIZE_DONE" }
        assertEquals("${resized.rows} ${resized.cols}", size.line(0).trim())
        assertSame(view, terminalView()); assertSame(session.surface, main { view.surface })
        assertEquals(pid, marker(session, File(root, "home"), "after-view"))
    }

    @Test fun disablingCursorBlinkKeepsProgramBlinkStaticWithoutDisablingAnsiBlinkText() = withModel { _, _, owner, model ->
        val session = add(model)
        ui.setContent { MaterialTheme { if (model.visible) TerminalScreen(model) {} } }
        ui.waitForIdle(); val view = terminalView()
        assertTrue(save(owner, PreferenceEdit.Blink(false)) is PreferenceUpdate.Saved)
        session.connection.commit("stty -echo; PS1=''; printf '\\033[0m\\033[2J\\033[H\\033[5mBLINK_TEXT\\033[0m\\r\\n\\033[1 q'\r")
        frame(session.connection) { it.line(0) == "BLINK_TEXT" && it.cursorBlink }
        await { main { view.currentFrame?.line(0) == "BLINK_TEXT" && view.currentFrame?.cursorBlink == true && !view.style.appearance.blink } }
        val frame = main { checkNotNull(view.currentFrame) }
        assertTrue(frame.hasBlinkingText)
        val cw = main { TerminalView::class.java.getDeclaredField("cellWidth").apply { isAccessible = true }.getInt(view) }
        val ch = main { TerminalView::class.java.getDeclaredField("cellHeight").apply { isAccessible = true }.getInt(view) }
        val image = bitmap(view, false)
        val center = image.getPixel(frame.cursorX * cw + cw / 2, frame.cursorY * ch + ch / 2)
        assertNotEquals("A disabled cursor blink must stay painted in its off phase", frame.background, center)
        for (y in 0 until ch) for (x in 0 until cw * 10) assertEquals("ANSI blink text still follows its own phase", frame.background, image.getPixel(x, y))
        image.recycle()
    }

    @Test fun anUnfocusedWindowShowsAStaticCursorEvenWhenTheViewKeepsFocus() = withModel { _, _, _, model ->
        val session = add(model)
        ui.setContent { MaterialTheme { if (model.visible) TerminalScreen(model) {} } }
        ui.waitForIdle(); val view = terminalView()
        session.connection.commit("stty -echo; PS1=''; printf '\\033[0m\\033[2J\\033[HWINDOW_FOCUS\\r\\n\\033[1 q'\r")
        await { main { view.currentFrame?.line(0) == "WINDOW_FOCUS" && view.currentFrame?.cursorBlink == true && view.hasWindowFocus() } }
        val frame = main { checkNotNull(view.currentFrame) }
        val cw = main { TerminalView::class.java.getDeclaredField("cellWidth").apply { isAccessible = true }.getInt(view) }
        val ch = main { TerminalView::class.java.getDeclaredField("cellHeight").apply { isAccessible = true }.getInt(view) }
        val dialog = main { android.app.AlertDialog.Builder(ui.activity).setMessage("窗口焦点对照").setPositiveButton("返回", null).show() }
        try {
            await("Fixture: dialog did not take window focus") { main { !view.hasWindowFocus() } }
            assertTrue("Fixture: underlying view must retain local focus", main { view.hasFocus() })
            val image = bitmap(view, false)
            assertNotEquals("A visible terminal without window focus must draw its static outline", frame.background,
                image.getPixel(frame.cursorX * cw, frame.cursorY * ch + ch / 2))
            assertEquals("An unfocused cursor uses an outline", frame.background,
                image.getPixel(frame.cursorX * cw + cw / 2, frame.cursorY * ch + ch / 2))
            image.recycle()
        } finally { main { dialog.dismiss() } }
    }

    @Test fun oneUnavailableSurfaceReportsAnErrorWithoutBlockingOtherOwners() = withModel { _, _, owner, model ->
        val first = add(model); val second = add(model)
        first.connection.close()
        assertTrue(save(owner, PreferenceEdit.Theme(TerminalTheme.PAPER)) is PreferenceUpdate.Saved)
        frame(second.connection) { it.background == 0xfffaf8f2.toInt() }
        val error = main { model.error }
        assertNotNull("An unavailable surface must not be reported as applied", error)
        assertTrue(error!!, error.startsWith("终端设置未能应用："))
        assertEquals(TerminalTheme.PAPER, owner.state.value.committed.terminalTheme)
    }
}
