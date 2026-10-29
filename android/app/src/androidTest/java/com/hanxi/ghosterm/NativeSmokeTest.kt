package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.CoreLibrary
import com.hanxi.ghosterm.terminal.TerminalEngine
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class NativeSmokeTest {
    @Test fun corePreservesUtf8AndLineEndings() {
        val config = "# 主机 🔑\r\nHost demo\r\n  HostName example.test\r\n  X-Future untouched\r\n".toByteArray()
        assertArrayEquals(config, CoreLibrary.roundTrip(config))
        assertTrue(CoreLibrary.version.isNotBlank())
    }
    @Test fun configurationRejectsNul() {
        assertThrows(IllegalStateException::class.java) { CoreLibrary.roundTrip(byteArrayOf(65, 0, 66)) }
    }
    @Test fun vtExecutesCursorMovement() {
        assertTrue(TerminalEngine.version.isNotBlank())
        // This is the real VT parser, not a returned build constant or echo fixture.
        assertArrayEquals(intArrayOf(9, 4), TerminalEngine.probeCursor("\u001b[5;10H".toByteArray()))
    }
}
