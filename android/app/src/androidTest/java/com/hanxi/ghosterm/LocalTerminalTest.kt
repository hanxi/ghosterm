package com.hanxi.ghosterm

import android.os.SystemClock
import android.view.KeyEvent
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import com.hanxi.ghosterm.terminal.TerminalFrame
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import kotlinx.coroutines.*
import com.hanxi.ghosterm.terminal.SelectionOperation

@RunWith(AndroidJUnit4::class)
class LocalTerminalTest {
    private fun terminal(test: (LocalSession) -> Unit) {
        val dir = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "pty-${UUID.randomUUID()}").apply { mkdirs() }
        val session = LocalSession(dir.absolutePath)
        try {
            await { val state = session.state(); assertFalse(state.description, state.terminal); state.connected }
            session.commit("stty -echo; PS1=''; printf '\\033[2J\\033[HNATIVE_READY\\r\\n'\r")
            frame(session) { it.line(0) == "NATIVE_READY" }
            test(session)
        }
        finally { session.close(); dir.deleteRecursively() }
    }
    private fun await(test: () -> Boolean) {
        val end = SystemClock.uptimeMillis() + 10_000
        do { if (test()) return; SystemClock.sleep(20) } while (SystemClock.uptimeMillis() < end)
        fail("等待真实 PTY/VT 状态超时")
    }
    private fun frame(session: LocalSession, test: (TerminalFrame) -> Boolean): TerminalFrame {
        var current: TerminalFrame? = null
        try {
            await {
                session.frame(current?.revision ?: 0)?.let { current = it }
                current?.let(test) == true
            }
        } catch (error: AssertionError) { throw AssertionError("${session.state()}: ${current?.let(::text)}; row1=${current?.cells?.drop(current!!.cols)?.take(8)}", error) }
        return current!!
    }
    private fun text(frame: TerminalFrame) = (0 until frame.rows).joinToString("\n", transform = frame::line)

    @Test fun shellOutputRendersAnsiWideAndCombiningCells() = terminal { session ->
        session.commit("stty -echo\r")
        session.commit("printf '\\033[2J\\033[H\\033[41mPTY_READY\\033[0m\\r\\n中é\\360\\237\\230\\200\\r\\n'\r")
        val output = frame(session) { it.line(0) == "PTY_READY" && it.line(1).startsWith("中é😀") }
        assertEquals(2, output.cells[output.cols].width)
        assertEquals(0, output.cells[output.cols + 1].width)
        assertEquals("é", output.cells[output.cols + 2].text)
        assertEquals(2, output.cells[output.cols + 3].width)
        assertEquals(0, output.cells[output.cols + 4].width)
        assertNotEquals(output.background, output.cells[0].background)
        assertEquals(output.background, output.cells[9].background)
    }

    @Test fun nativeKeyEncoderUsesApplicationCursorAndTypedUtf8() = terminal { session ->
        session.commit("stty raw -echo; printf '\\033[2J\\033[H\\033[?1hRAW_READY'; dd bs=1 count=14 2>/dev/null | od -An -tx1; stty sane; printf 'RAW_DONE'\r")
        frame(session) { it.line(0).startsWith("RAW_READY") }
        session.key(KeyEvent.KEYCODE_DPAD_UP)
        session.key(KeyEvent.KEYCODE_B, 2, "b")
        session.commit("中文😀")
        val output = frame(session) { text(it).contains("RAW_DONE") }
        assertTrue(text(output), text(output).contains("1b 4f 41 02 e4 b8 ad e6 96 87 f0 9f 98 80"))
    }

    @Test fun resizeReachesPtyAndExitStatusIsReal() = terminal { session ->
        session.resize(61, 19, 8, 17)
        frame(session) { it.cols == 61 && it.rows == 19 }
        session.commit("stty -echo; printf '\\033[2J\\033[H'; stty size; printf 'SIZE_DONE'\r")
        val output = frame(session) { text(it).contains("SIZE_DONE") }
        assertTrue(text(output), text(output).contains("19 61"))
        session.commit("exit 7\r")
        await { session.state().terminal }
        assertEquals(session.state().toString(), 7, session.state().exitCode)
        assertThrows(IllegalStateException::class.java) { session.commit("must fail") }
    }

    @Test fun alternateScreenAndScrollbackComeFromVt() = terminal { session ->
        session.commit("stty -echo; printf '\\033[2J\\033[HMAIN_SCREEN\\033[?1049h\\033[2J\\033[HALT_SCREEN'\r")
        val alternate = frame(session) { it.line(0).startsWith("ALT_SCREEN") }
        assertFalse(text(alternate).contains("MAIN_SCREEN"))
        session.commit("printf '\\033[?1049l'\r")
        frame(session) { it.line(0).startsWith("MAIN_SCREEN") }
        session.commit("i=0; while [ \"\u0024i\" -lt 100 ]; do printf 'line-%03d\\r\\n' \"\u0024i\"; i=\u0024((i+1)); done; printf 'SCROLL_DONE'\r")
        frame(session) { text(it).contains("SCROLL_DONE") }
        session.scroll(-50)
        val history = frame(session) { !text(it).contains("SCROLL_DONE") && text(it).contains("line-") }
        assertFalse(text(history).contains("line-099"))
    }

    @Test fun sessionsAreIndependentAndCloseIsIdempotent() = terminal { first ->
        terminal { second ->
            first.commit("stty -echo; printf '\\033[2J\\033[HFIRST_SESSION'\r")
            second.commit("stty -echo; printf '\\033[2J\\033[HSECOND_SESSION'\r")
            assertFalse(text(frame(first) { it.line(0).startsWith("FIRST_SESSION") }).contains("SECOND_SESSION"))
            assertFalse(text(frame(second) { it.line(0).startsWith("SECOND_SESSION") }).contains("FIRST_SESSION"))
            val start = SystemClock.uptimeMillis()
            first.close(); first.close()
            assertTrue("close must not join IO owner", SystemClock.uptimeMillis() - start < 500)
            assertNull(first.frame(0))
            assertThrows(IllegalStateException::class.java) { first.commit("closed") }
            second.commit("printf '\\033[2J\\033[HSECOND_ALIVE'\r")
            frame(second) { it.line(0).startsWith("SECOND_ALIVE") }
        }
    }
    @Test fun staleNativeGenerationCannotAccessOrCloseReusedSlot() = terminal { original ->
        val stale = NativeTerminalProbe.handle(original)
        original.close()
        terminal { replacement ->
            val current = NativeTerminalProbe.handle(replacement)
            assertEquals("fixture must actually reuse the registry slot", stale % 64, current % 64)
            assertNotEquals(stale, current)
            assertThrows(IllegalStateException::class.java) { NativeTerminalProbe.readState(stale) }
            NativeTerminalProbe.dispose(stale)
            assertTrue("stale dispose must not close the replacement", runCatching { replacement.state().connected }.getOrDefault(false))
            replacement.commit("stty -echo; printf '\\033[2J\\033[HGENERATION_ALIVE'\r")
            frame(replacement) { it.line(0).startsWith("GENERATION_ALIVE") }
        }
    }

    @Test fun closeUnderBackpressureReleasesNativeOwner() {
        await { NativeTerminalProbe.owners() == 0 }
        terminal { session ->
            session.commit("stty raw -echo; printf '\\033[2J\\033[HNOT_READING'; sleep 30\r")
            frame(session) { it.line(0).startsWith("NOT_READING") }
            session.commit("x".repeat(768 * 1024))
            // The reader is asleep; this exceeds the PTY capacity and exercises the
            // native tail queue. Give the owner one bounded write grace to enter it.
            SystemClock.sleep(150)
            val calibrationStart = SystemClock.uptimeMillis()
            repeat(20) { SystemClock.sleep(10) }
            val budget = maxOf(500L, (SystemClock.uptimeMillis() - calibrationStart) * 3 / 2 + 100)
            val start = SystemClock.uptimeMillis()
            session.close()
            assertTrue("UI close must not wait for the shell", SystemClock.uptimeMillis() - start < budget)
            await { NativeTerminalProbe.owners() == 0 }
        }
    }

    @Test fun unsupportedWorkingDirectoryFailsExplicitlyAndReleasesOwner() {
        await { NativeTerminalProbe.owners() == 0 }
        val path = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "absent-${UUID.randomUUID()}/home")
        val session = LocalSession(path.absolutePath)
        try {
            await { session.state().terminal }
            assertEquals(3, session.state().phase)
            assertFalse(session.state().error.isNullOrBlank())
            assertNull(session.frame(0))
            await { NativeTerminalProbe.owners() == 0 }
        } finally { session.close() }
    }


    @Test fun pasteUsesVtBracketedModeAndPreservesUtf8() = terminal { session ->
        session.commit("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004hPASTE_READY\\r\\n'; dd bs=1 count=20 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nPASTE_DONE\\r\\n'\r")
        frame(session) { text(it).contains("PASTE_READY") }
        runBlocking { assertTrue(session.paste("中文\nx")) }
        val output = text(frame(session) { text(it).contains("PASTE_DONE") }).replace("\n", " ").replace(Regex(" +"), " ")
        assertTrue(output, output.contains("1b 5b 32 30 30 7e e4 b8 ad e6 96 87 0a 78 1b 5b 32 30 31 7e"))
    }

    @Test fun rejectedPasteWritesNothingAndLeavesConnectionUsable() = terminal { session ->
        session.commit("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004lREJECT_READY\\r\\n'; dd bs=1 count=1 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nREJECT_DONE\\r\\n'\r")
        frame(session) { text(it).contains("REJECT_READY") }
        runBlocking { assertFalse(session.paste("X\n")) }
        assertTrue(session.state().connected)
        session.commit("Z")
        val output = text(frame(session) { text(it).contains("REJECT_DONE") })
        assertTrue(output, output.contains("5a"))
        assertFalse(output, output.contains("58"))
    }

    @Test fun confirmedPasteConvertsNewlineUsingCurrentVtMode() = terminal { session ->
        session.commit("stty raw -echo; printf '\\033[2J\\033[H\\033[?2004lCONFIRM_READY\\r\\n'; dd bs=1 count=3 2>/dev/null | od -An -tx1; stty sane; printf '\\r\\nCONFIRM_DONE\\r\\n'\r")
        frame(session) { text(it).contains("CONFIRM_READY") }
        runBlocking { assertFalse(session.paste("a\nb")); assertTrue(session.paste("a\nb", true)) }
        val output = text(frame(session) { text(it).contains("CONFIRM_DONE") })
        assertTrue(output, output.contains("61 0d 62"))
    }

    @Test fun selectionCopiesGraphemesUnwrapsSoftLinesAndWorksAfterExit() = terminal { session ->
        session.resize(12, 10, 8, 17)
        frame(session) { it.cols == 12 }
        session.commit("stty -echo; PS1=''; printf '\\033[2J\\033[Habc中é0123456789  \\r\\nsecond'; exit 0\r")
        await { session.state().terminal }
        runBlocking {
            session.select(SelectionOperation.ALL)
            assertEquals("abc中é0123456789\nsecond", session.copySelection())
        }
        val selected = frame(session) { it.cells.any { cell -> cell.flags and 256 != 0 } }
        assertTrue(selected.cells.first().flags and 256 != 0)
        runBlocking {
            session.select(SelectionOperation.WORD, 1, 2)
            assertEquals("second", session.copySelection())
            session.select(SelectionOperation.START, 1, 2)
            session.select(SelectionOperation.END, 3, 2)
            assertEquals("eco", session.copySelection())
            session.select(SelectionOperation.WORD, 10, 5)
            assertEquals("blank word must clear the prior selection", "", session.copySelection())
            session.select(SelectionOperation.ALL)
            session.select(SelectionOperation.CLEAR)
            assertEquals("", session.copySelection())
        }
        frame(session) { it.cells.none { cell -> cell.flags and 256 != 0 } }
    }

    @Test fun draggingAcrossTheAnchorKeepsTheOtherEndpointUntilNextGesture() = terminal { session ->
        session.commit("printf '\\033[2J\\033[Halpha beta gamma'; sleep 30\r")
        frame(session) { it.line(0) == "alpha beta gamma" }
        runBlocking {
            session.select(SelectionOperation.WORD, 7, 0)
            session.select(SelectionOperation.ORDER)
            session.select(SelectionOperation.END, 3, 0)
            assertEquals("ha b", session.copySelection())
            session.select(SelectionOperation.END, 1, 0)
            assertEquals("lpha b", session.copySelection())
            session.select(SelectionOperation.ORDER)
            session.select(SelectionOperation.START, 0, 0)
            assertEquals("alpha b", session.copySelection())
            session.select(SelectionOperation.START, 8, 0)
            assertEquals("bet", session.copySelection())
            session.select(SelectionOperation.START, 11, 0)
            assertEquals("beta g", session.copySelection())
        }
    }

    @Test fun localClearPreservesParserModesCursorAndDoesNotWriteInput() = terminal { session ->
        session.commit("stty raw -echo; printf '\\033[?1h\\033[2J\\033[HCLEAR_READY\\033[5;7H\\033]2;partial'; value=\u0024(dd bs=1 count=3 2>/dev/null | od -An -tx1); printf 'complete\\007\\r\\n%s\\r\\nCLEAR_DONE\\r\\n' \"\u0024value\"; stty sane\r")
        val before = frame(session) { text(it).contains("CLEAR_READY") && it.cursorY == 4 }
        runBlocking { session.clearLocal() }
        val empty = frame(session) { it.cells.all { cell -> cell.text.isEmpty() } }
        assertEquals(before.cursorX, empty.cursorX); assertEquals(before.cursorY, empty.cursorY)
        session.key(KeyEvent.KEYCODE_DPAD_UP)
        val output = text(frame(session) { text(it).contains("CLEAR_DONE") })
        assertTrue(output, output.contains("1b 4f 41"))
        assertFalse("partial OSC must remain in the parser: $output", output.contains("complete"))
    }

    @Test fun localClearRemovesHistoryAndActiveSelection() = terminal { session ->
        session.commit("stty -echo; i=0; while [ \"\u0024i\" -lt 100 ]; do printf 'history-%s\\r\\n' \"\u0024i\"; i=\u0024((i+1)); done; printf 'HISTORY_DONE'; sleep 30\r")
        frame(session) { (0 until it.rows).any { row -> it.line(row) == "HISTORY_DONE" } }
        runBlocking {
            session.select(SelectionOperation.ALL)
            assertTrue(session.copySelection().contains("history-0"))
            session.clearLocal()
            assertEquals("", session.copySelection())
            session.select(SelectionOperation.ALL)
            assertEquals("", session.copySelection().trim())
        }
        session.scroll(-500)
        frame(session) { it.cells.all { cell -> cell.text.isEmpty() } }
    }

    @Test fun cancelledTextAwaitDoesNotBlockTheNextRequest() = terminal { session ->
        session.commit("stty -echo; printf '\\033[2J\\033[HCANCEL_TEXT_READY'; sleep 30\r")
        frame(session) { it.line(0) == "CANCEL_TEXT_READY" }
        runBlocking {
            val abandoned = launch(start = CoroutineStart.UNDISPATCHED) { session.copySelection() }
            abandoned.cancelAndJoin()
            val result = runCatching { session.select(SelectionOperation.ALL); session.copySelection() }
            assertTrue("cancelled request must release its native slot: $result", result.isSuccess)
            assertEquals("CANCEL_TEXT_READY", result.getOrNull())
        }
    }

    @Test fun clearingAlternateScreenDoesNotResetItsModeOrErasePrimary() = terminal { session ->
        session.commit("stty -echo; PS1=''; printf '\\033[2J\\033[HPRIMARY_KEPT\\033[?1049h\\033[2J\\033[HALTERNATE_READY'\r")
        frame(session) { it.line(0) == "ALTERNATE_READY" }
        runBlocking { session.clearLocal() }
        frame(session) { it.cells.all { cell -> cell.text.isEmpty() } }
        session.commit("printf '\\033[?1049l'\r")
        frame(session) { it.line(0) == "PRIMARY_KEPT" }
    }

    @Test fun selectionUsesScrolledViewportCoordinates() = terminal { session ->
        session.commit("stty -echo; i=0; while [ \"\u0024i\" -lt 100 ]; do printf 'row%03d\\r\\n' \"\u0024i\"; i=\u0024((i+1)); done; printf 'VIEWPORT_DONE'; sleep 30\r")
        frame(session) { (0 until it.rows).any { row -> it.line(row) == "VIEWPORT_DONE" } }
        session.scroll(-50)
        val scrolled = frame(session) { !text(it).contains("VIEWPORT_DONE") && it.line(0).startsWith("row") }
        runBlocking {
            session.select(SelectionOperation.WORD, 1, 0)
            assertEquals(scrolled.line(0), session.copySelection())
        }
    }
}
