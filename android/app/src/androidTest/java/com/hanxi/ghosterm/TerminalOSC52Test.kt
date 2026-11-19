package com.hanxi.ghosterm

import android.os.SystemClock
import android.util.Base64
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import com.hanxi.ghosterm.bridge.NativeTerminalSession.DiscoveryOffer
import com.hanxi.ghosterm.terminal.TerminalClipboardRequest
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real local PTY and VT parser, with no clipboard contents in test output. */
@RunWith(AndroidJUnit4::class)
class TerminalOSC52Test {
    private fun await(message: String = "等待 OSC 52 断言超时", test: () -> Boolean) {
        val until = SystemClock.uptimeMillis() + 10_000
        do { if (test()) return; SystemClock.sleep(10) } while (SystemClock.uptimeMillis() < until)
        fail(message)
    }
    private fun terminal(block: (LocalSession, File) -> Unit) {
        val dir = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "osc52-${UUID.randomUUID()}").apply { mkdirs() }
        val owners = NativeTerminalProbe.owners()
        val session = LocalSession(dir.absolutePath)
        try {
            await { session.state().connected }
            session.resize(60, 12, 10, 20); session.visible(true)
            session.commit(NativePtyFixture.initialize()+"stty raw -echo; PS1=''; printf '\\033[2J\\033[HOSC_READY'\r")
            // The initial command is echoed before stty disables echo. Require
            // its actual cleared-screen output, never a substring of that command.
            await { session.frame(0)?.line(0) == "OSC_READY" }
            block(session, dir)
        } finally { session.close(); await { NativeTerminalProbe.owners() == owners }; dir.deleteRecursively() }
    }
    private fun ready(session: LocalSession, marker: String) = await {
        session.frame(0)?.line(0) == marker
    }
    private fun request(session: LocalSession): TerminalClipboardRequest {
        var value: TerminalClipboardRequest? = null
        await { value = session.clipboardRequest(); value != null }
        return value!!
    }
    @Test fun discoveryIgnoresNestedOscAndPreservesLocalTerminalOutput() = terminal { session, _ ->
        val payload = "EREREREREREREREREREREaqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqAAsvd29yay9mcm9udA=="
        val offer = "\\033]777;ghosterm-discovery;1;$payload\\007"
        session.commit("printf '%b' '\\033P$offer\\033\\0134\\033[2J\\033[HPTY_CONTROL_MARKER'\r")
        ready(session, "PTY_CONTROL_MARKER")
        val nested = session.discovery()
        val discoveryStatus = if (BuildConfig.PLUGIN_JS_ENABLED) 0 else 7
        assertEquals(discoveryStatus, nested.status)
        assertNull("OSC nested in ordinary DCS must not become an offer", nested.offer)
        session.commit("printf '%b' '$offer\\033[2J\\033[HPTY_CANDIDATE_MARKER'\r")
        ready(session, "PTY_CANDIDATE_MARKER")
        if (!BuildConfig.PLUGIN_JS_ENABLED) {
            val disabled = session.discovery()
            assertEquals(discoveryStatus, disabled.status)
            assertNull(disabled.offer)
            return@terminal
        }
        var observed: DiscoveryOffer? = null
        await("顶层发现候选未到达，普通终端标记已绘制") {
            observed = session.discovery().offer; observed != null
        }
        assertEquals("/work/front", observed?.directory)
        assertEquals("1".repeat(32), observed?.instance)
        assertEquals("a".repeat(64), observed?.clientDigest)
    }

    @Test fun fragmentedUnicodeWriteAndEmptyWrite() = terminal { s, _ ->
        val text = "终端 café\nsecond line"
        val encoded = Base64.encodeToString(text.toByteArray(), Base64.NO_WRAP)
        s.commit("printf '\\033]52;c;${encoded.take(4)}'; sleep 0.1; printf '${encoded.drop(4)}\\007'; printf '\\033[2J\\033[HOSC_WRITTEN'\r")
        val write = request(s)
        assertFalse(write.read); assertEquals(text, write.text)
        s.replyClipboard(write.id, true); ready(s, "OSC_WRITTEN")
        s.commit("printf '\\033]52;c;\\033\\\\'; printf '\\033[2J\\033[HOSC_EMPTY'\r")
        val empty = request(s)
        assertFalse(empty.read); assertEquals("", empty.text)
        s.replyClipboard(empty.id, true); ready(s, "OSC_EMPTY")
    }
    @Test fun writeAndReadAboveOneMiBAreNotCappedByHost() = terminal { s, dir ->
        val text = "x".repeat(1024 * 1024 + 1) + "中文"
        val encoded = Base64.encodeToString(text.toByteArray(), Base64.NO_WRAP)
        val input = File(dir, "large-base64").apply { writeText(encoded) }
        s.commit("printf '\\033]52;c;'; cat '${input.path}'; printf '\\007'; printf '\\033[2J\\033[HOSC_LARGE_WRITE_DONE'\r")
        val write = request(s)
        assertFalse(write.read); assertTrue("Large write must preserve every byte", write.text == text)
        s.replyClipboard(write.id, true); ready(s, "OSC_LARGE_WRITE_DONE")
        val expected = ("\u001b]52;c;" + encoded + "\u0007").toByteArray()
        val output = File(dir, "large-reply")
        s.commit("printf '\\033]52;c;?\\007'; head -c ${expected.size} > '${output.path}' && printf '\\033[2J\\033[HOSC_LARGE_READ_DONE'\r")
        val read = request(s)
        assertTrue(read.read)
        s.replyClipboard(read.id, true, text)
        ready(s, "OSC_LARGE_READ_DONE")
        val actual = output.readBytes()
        val mismatch = (0 until minOf(expected.size, actual.size)).firstOrNull { expected[it] != actual[it] }
        assertTrue("Large reply must preserve every byte: expected=${expected.size}, actual=${actual.size}, mismatch=$mismatch", expected.contentEquals(actual))
    }

    private fun read(s: LocalSession, dir: File, allowed: Boolean) {
        val text = "中文 café\nreply"
        val expected = "\u001b]52;c;" + (if (allowed) Base64.encodeToString(text.toByteArray(), Base64.NO_WRAP) else "") + "\u001b\\"
        val output = File(dir, "reply")
        s.commit("printf '\\033]52;c;?\\033\\\\'; dd bs=1 count=${expected.toByteArray().size} of='${output.path}' 2>/dev/null; printf '\\033[2J\\033[HOSC_READ_DONE'\r")
        val request = request(s)
        assertTrue(request.read)
        // A wrong generation must leave the original request pending.
        s.replyClipboard(request.id + 1, true, "wrong")
        assertEquals(request.id, s.clipboardRequest()?.id)
        assertEquals(0L, output.length())
        s.replyClipboard(request.id, allowed, text)
        ready(s, "OSC_READ_DONE")
        assertArrayEquals(expected.toByteArray(), output.readBytes())
    }
    @Test fun approvedReadUsesEngineBase64Reply() = terminal { s, dir -> read(s, dir, true) }
    @Test fun deniedReadReturnsEmptyReply() = terminal { s, dir -> read(s, dir, false) }
    @Test fun hiddenReadRejectsLateApproval() = terminal { s, dir ->
        val output = File(dir, "hidden")
        val expected = "\u001b]52;c;\u0007".toByteArray()
        s.commit("printf '\\033]52;c;?\\007'; dd bs=1 count=${expected.size} of='${output.path}' 2>/dev/null; printf '\\033[2J\\033[HOSC_HIDDEN_DONE'\r")
        val request = request(s)
        s.visible(false); s.replyClipboard(request.id, true, "must not escape")
        ready(s, "OSC_HIDDEN_DONE")
        assertArrayEquals(expected, output.readBytes())
    }
    @Test fun unsupportedSelectionAndInvalidBase64LeaveGridUsable() = terminal { s, _ ->
        s.commit("printf '\\033]52;p;aGVsbG8=\\007\\033]52;c;%%%\\007'; printf '\\033[2J\\033[HOSC_UNSUPPORTED_DONE'\r")
        ready(s, "OSC_UNSUPPORTED_DONE")
        assertNull(s.clipboardRequest())
    }
    @Test fun closeWhileAwaitingPermissionReleasesOwner() = terminal { s, _ ->
        s.commit("printf '\\033]52;c;?\\007'\r")
        assertTrue(request(s).read)
        s.close()
    }
}
