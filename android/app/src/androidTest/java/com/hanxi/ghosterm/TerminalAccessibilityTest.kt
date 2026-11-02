package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.terminal.TerminalCell
import com.hanxi.ghosterm.terminal.TerminalFrame
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class TerminalAccessibilityTest {
    private fun frame(cols: Int, rows: Int, cells: List<TerminalCell>) =
        TerminalFrame(1, cols, rows, 0, 0, 0, 0, false, false, 0, cells)

    @Test fun visibleTextOmitsConcealedContentAndWideContinuation() {
        val cells = listOf(
            TerminalCell(0, 0, 0, 2, "你"),
            TerminalCell(0, 0, 0, 0, "DUPLICATE"),
            TerminalCell(0, 0, 8, 1, "HIDDEN_SECRET"),
            TerminalCell(0, 0, 0, 1, "e\u0301"),
            TerminalCell(0, 0, 0, 2, "😀"),
            TerminalCell(0, 0, 0, 0, ""),
            TerminalCell(0, 0, 0, 1, String(Character.toChars(0x10eeee))),
        )
        assertEquals("终端屏幕文字\n你 e\u0301😀", frame(7, 1, cells).accessibilityText())
    }

    @Test fun blankAndOversizedViewportsReportTheirActualScope() {
        val blank = TerminalCell(0, 0, 0, 1, "")
        assertEquals("终端屏幕为空", frame(256, 256, List(65536) { blank }).accessibilityText())
        val text = TerminalCell(0, 0, 0, 1, "A")
        val result = frame(256, 256, List(65536) { text }).accessibilityText()
        assertTrue("Readout must remain bounded", result.length <= 32768)
        assertTrue("Truncation must be explicit", result.endsWith("（内容过长，后续文字未提供）"))
        assertTrue("The available text must still be readable", result.contains("AAAA"))
    }
}
