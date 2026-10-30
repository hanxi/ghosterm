package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.KnownHostsBridge
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class KnownHostsBridgeTest {
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
