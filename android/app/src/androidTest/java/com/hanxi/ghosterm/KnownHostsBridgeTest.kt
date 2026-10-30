package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.KnownHostsBridge
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class KnownHostsBridgeTest {
    @Test fun removalKeepsExactBytesAndRejectsStaleRevision() {
        val directory = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,
            "known-hosts-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            val file = File(directory, "中文 🔑")
            val prefix = "# 保留\r\n"
            val first = " a,b ssh-rsa AAAAAAAAAAAAAAAA comment\r\n"
            val tail = "\t\nother ssh-rsa BBBBBBBBBBBBBBBB"
            val original = (prefix + first + tail).toByteArray()
            file.writeBytes(original)
            val result = KnownHostsBridge.remove(file.absolutePath, original, 0)
            assertTrue(result.succeeded); assertTrue(result.committed); assertFalse(result.conflict)
            assertEquals("", result.message)
            assertArrayEquals((prefix + tail).toByteArray(), file.readBytes())
            assertArrayEquals(original, directory.listFiles()!!.single { it.extension == "bak" }.readBytes())
            val stale = KnownHostsBridge.remove(file.absolutePath, original, 0)
            assertFalse(stale.succeeded); assertFalse(stale.committed); assertTrue(stale.conflict)
            assertTrue(stale.message.isNotBlank())
            assertArrayEquals((prefix + tail).toByteArray(), file.readBytes())
        } finally { directory.deleteRecursively() }
    }
    @Test fun removalCannotEraseReadonlyRulesOrHideIoFailures() {
        val directory = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,
            "known-hosts-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            val file = File(directory, "known_hosts")
            for (line in listOf("@revoked a ssh-rsa AAAAAAAAAAAAAAAA\n", "broken\n",
                "|1|salt|hash ssh-rsa AAAAAAAAAAAAAAAA\n")) {
                file.writeText(line)
                val result = KnownHostsBridge.remove(file.absolutePath, line.toByteArray(), 0)
                assertEquals(7, result.status); assertFalse(result.committed); assertFalse(result.conflict)
                assertEquals(line, file.readText())
            }
            val failed = KnownHostsBridge.remove(File(directory, "missing/known_hosts").absolutePath, byteArrayOf(), 0)
            assertEquals(3, failed.status); assertFalse(failed.committed); assertFalse(failed.conflict)
            assertTrue(failed.message.isNotBlank())
        } finally { directory.deleteRecursively() }
    }
    @Test fun sourceRecordsKeepMarkersMalformedRowsAndExactByteRanges() {
        val prefix = "# 中文\r\n\t\n"
        val revoked = " @revoked [host]:2222,other ssh-ed25519 AAAAAAAAAAAAAAAA\r\n"
        val text = prefix + revoked + "@cert-authority *.example ecdsa-sha2-nistp256 AAAA/AAAAAAAAAAA\n" +
            "|1|salt|hash ssh-rsa AAAAAAAAAAAAAAAA\n@future host ssh-new AAAAAAAAAAAAAAAA\nbroken"
        val input = text.toByteArray()
        val rows = KnownHostsBridge.records(input)
        input.fill(0)
        assertEquals(5, rows.size)
        val first = rows.first()
        assertEquals("@revoked", first.marker)
        assertEquals("[host]:2222,other", first.hosts)
        assertEquals("ssh-ed25519", first.keyType)
        assertEquals("AAAAAAAAAAAAAAAA", first.keyBase64)
        assertEquals(3, first.line)
        assertEquals(prefix.toByteArray().size, first.byteStart)
        assertEquals((prefix + revoked).toByteArray().size, first.byteEnd)
        assertTrue(first.recognized); assertFalse(first.hashed)
        assertEquals("@cert-authority", rows[1].marker)
        assertEquals("ecdsa-sha2-nistp256", rows[1].keyType)
        assertTrue(rows[2].hashed)
        assertEquals("@future", rows[3].marker)
        assertFalse(rows.last().recognized)
        assertEquals("broken", rows.last().hosts)
        assertEquals(text.toByteArray().size, rows.last().byteEnd)
    }
    @Test fun invalidAndOverBudgetInputNeverBecomeAnEmptyList() {
        assertTrue(KnownHostsBridge.records("# no records\n\t\r\n".toByteArray()).isEmpty())
        for (input in listOf(byteArrayOf(0), byteArrayOf(0xc0.toByte(),0xaf.toByte()),
            ByteArray(1024*1024+1) { 'x'.code.toByte() }, "bad\n".repeat(4097).toByteArray())) {
            try { KnownHostsBridge.records(input); fail("invalid/over-budget records must fail") }
            catch (_: Exception) { /* An input error is not an empty snapshot. */ }
        }
    }
}
