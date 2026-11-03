package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real PTY bytes, encoded by the current VT modes rather than a UI byte formatter. */
@RunWith(AndroidJUnit4::class)
class TerminalMouseTest {
    private fun await(test: () -> Boolean) {
        val until = SystemClock.uptimeMillis() + 10_000
        do { if (test()) return; SystemClock.sleep(10) } while (SystemClock.uptimeMillis() < until)
        fail("等待鼠标 PTY 字节或 VT 状态超时")
    }
    private fun terminal(block: (LocalSession, File) -> Unit) {
        val dir = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "mouse-${UUID.randomUUID()}").apply { mkdirs() }
        val owners = NativeTerminalProbe.owners()
        val session = LocalSession(dir.absolutePath)
        try {
            await { assertFalse(session.state().description, session.state().terminal); session.state().connected }
            session.resize(40, 12, 10, 20)
            session.commit(NativePtyFixture.initialize()+"stty raw -echo; PS1=''; printf '\\033[2J\\033[HBOOT_READY'\r")
            ready(session, "BOOT_READY")
            block(session, dir)
        } finally { session.close(); await { NativeTerminalProbe.owners() == owners }; dir.deleteRecursively() }
    }
    private fun ready(session: LocalSession, marker: String) {
        await { session.frame(0)?.let { f -> (0 until f.rows).any { f.line(it).contains(marker) } } == true }
    }
    private fun capture(session: LocalSession, dir: File, modes: String, expected: ByteArray, click: suspend () -> Unit) {
        val file = File(dir, UUID.randomUUID().toString())
        session.commit("printf '\\033[2J\\033[H$modes' ; printf MOUSE_READY; dd bs=1 count=${expected.size} of='${file.path}' 2>/dev/null; printf '\\033[?9l\\033[?1000l\\033[?1002l\\033[?1003l\\033[?1005l\\033[?1006l\\033[?1016l\\r\\nMOUSE_DONE'\r")
        ready(session, "MOUSE_READY")
        runBlocking { click() }
        await { file.isFile && file.length() >= expected.size }
        ready(session, "MOUSE_DONE")
        assertArrayEquals(expected, file.readBytes())
    }
    @Test fun sgrClickUsesCellsAndBalancedPressRelease() = terminal { s, dir ->
        capture(s, dir, "\\033[?1000h\\033[?1006h", "\u001b[<0;3;4M\u001b[<0;3;4m".toByteArray()) {
            assertTrue(s.click(25, 63))
        }
        assertFalse("禁用鼠标后仍发送点击", runBlocking { s.click(25, 63) })
    }
    @Test fun x10ClickDoesNotInventAReleasePacket() = terminal { s, dir ->
        capture(s, dir, "\\033[?9h", byteArrayOf(27, 91, 77, 32, 35, 36)) { assertTrue(s.click(25, 63)) }
    }
    @Test fun sgrPixelClickPreservesPixelCoordinates() = terminal { s, dir ->
        capture(s, dir, "\\033[?1000h\\033[?1016h", "\u001b[<0;25;63M\u001b[<0;25;63m".toByteArray()) { assertTrue(s.click(25, 63)) }
    }
    @Test fun rightAndMiddleButtonsModifiersAndRepeatedClicksStayOrdered() = terminal { s, dir ->
        val expected = "\u001b[<30;3;4M\u001b[<30;3;4m\u001b[<1;3;4M\u001b[<1;3;4m\u001b[<1;3;4M\u001b[<1;3;4m"
        capture(s, dir, "\\033[?1000h\\033[?1006h", expected.toByteArray()) {
            assertTrue(s.click(25, 63, 7, 2)); assertTrue(s.click(25, 63, 0, 3)); assertTrue(s.click(25, 63, 0, 3))
        }
    }
    @Test fun disabledMouseWritesNothingAndInvalidInputDoesNotPoisonNextInput() = terminal { s, dir ->
        capture(s, dir, "", byteArrayOf(90)) {
            assertFalse(s.click(25, 63))
            assertThrows(IllegalArgumentException::class.java) { runBlocking { s.click(-1, 0) } }
            assertThrows(IllegalArgumentException::class.java) { runBlocking { s.click(0, 0, 1024) } }
            assertThrows(IllegalArgumentException::class.java) { runBlocking { s.click(0, 0, 0, 4) } }
            s.commit("Z")
        }
    }
}
