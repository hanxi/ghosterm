package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ConfigBridge
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ConfigDirectiveTest {
    @Test fun nativeRowsPreserveLexicalMeaningAndCopyOwnership() {
        val text = "Host\tdemo sibling\r\n\tHostName = \"host name\" # keep\r\n IdentityFile \"a,b 中文\"\r\n IdentityFile next\r\n User first\r\n User second\r\n"
        val bytes = text.toByteArray()
        val rows = ConfigBridge.directives(bytes, "DEMO")
        assertEquals(listOf("hostname", "identityfile", "identityfile", "user", "user"), rows.map { it.keyword })
        assertEquals(listOf(listOf("host name"), listOf("a,b 中文"), listOf("next"), listOf("first"), listOf("second")), rows.map { it.arguments })
        assertEquals(listOf(2, 3, 4, 5, 6), rows.map { it.sourceLine })
        assertEquals("\tHostName = \"host name\" # keep\r\n", rows[0].raw)
        assertTrue(rows[1].renderPerLine)
        assertTrue(rows.all { !it.included && it.sourcePath.isEmpty() })
        assertEquals(rows, ConfigBridge.directives(bytes, "sibling"))
        bytes.fill(0)
        assertEquals(listOf("a,b 中文"), rows[1].arguments)
    }

    @Test fun invalidAmbiguousAndMissingBlocksAreErrors() {
        fun rejected(text: String, alias: String = "demo") {
            try { ConfigBridge.directives(text.toByteArray(), alias); fail("Expected explicit error") }
            catch (_: IllegalStateException) { }
        }
        rejected("Host demo\n User \"bad\n")
        rejected("Host demo\n User first\nHost demo\n User second\n")
        rejected("Host demo\n User first\nMatch all\n User conditional\n")
        rejected("Host demo\n", "missing")
        rejected("Host demo\n", "demo\u0000suffix")
        assertTrue(ConfigBridge.directives("Host demo\n".toByteArray(), "demo").isEmpty())
    }

    @Test fun longDecodedTokenIsComplete() {
        val value = "字".repeat(4096)
        assertEquals(listOf(value), ConfigBridge.directives("Host demo\n IdentityFile $value\n".toByteArray(), "demo")[0].arguments)
    }
}
