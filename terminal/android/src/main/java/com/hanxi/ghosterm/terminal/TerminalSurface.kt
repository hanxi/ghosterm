package com.hanxi.ghosterm.terminal

/** The renderer knows only this seam, never connection kinds or native handles. */
interface TerminalSurface {
    /** Acknowledged on the VT owner; never injected into the terminal byte stream. */
    suspend fun configure(appearance: TerminalAppearance)
    fun frame(after: Long): TerminalFrame?
    fun graphics(after: Long): TerminalGraphics? = null
    fun visible(value: Boolean) {}
    fun resize(cols: Int, rows: Int, cellWidth: Int, cellHeight: Int)
    fun commit(text: String)
    /** Layout character without modifiers, for VT keyboard protocols; zero means unavailable. */
    fun key(code: Int, modifiers: Int = 0, text: String = "", unshiftedCodepoint: Int = 0, action: KeyAction = KeyAction.PRESS)
    fun scroll(lines: Int)
    /** Pixel position in this surface. False means mouse reporting is disabled. */
    suspend fun click(x: Int, y: Int, modifiers: Int = 0, button: Int = 1): Boolean
    /** False requests confirmation; no bytes were written in that case. */
    suspend fun paste(text: String, allowUnsafe: Boolean = false): Boolean
    suspend fun select(operation: SelectionOperation, x: Int = 0, y: Int = 0)
    /** Scroll and extend one endpoint atomically on the VT owner. */
    suspend fun extendSelection(operation: SelectionOperation, x: Int, y: Int, scrollLines: Int)
    suspend fun copySelection(): String
    suspend fun clearLocal()
}

/** Private VT event action, independent of Android KeyEvent numeric values. */
enum class KeyAction(val code: Int) { RELEASE(0), PRESS(1), REPEAT(2) }

/** Private surface code extension: preserve numpad navigation identity across key-up. */
object TerminalKeys { const val KEYPAD_NAVIGATION = 0x10000 }

enum class SelectionOperation(val code: Int) { WORD(2), START(3), END(4), ALL(5), CLEAR(6), ORDER(9) }

data class TerminalCell(val background: Int, val foreground: Int, val flags: Int, val width: Int, val text: String)

data class TerminalFrame(
    val revision: Long, val cols: Int, val rows: Int, val background: Int, val foreground: Int,
    val cursorX: Int, val cursorY: Int, val cursorVisible: Boolean, val cursorBlink: Boolean,
    val cursorStyle: Int, val cells: List<TerminalCell>,
) {
    val hasBlinkingText: Boolean = cells.any { it.width != 0 && it.flags and 128 != 0 && it.flags and 8 == 0 }
    fun line(row: Int): String = cells.subList(row * cols, (row + 1) * cols)
        .joinToString("") { if (it.width == 0) "" else it.text.ifEmpty { " " } }.trimEnd()

    /** Read-only visible text, requested by accessibility rather than output delivery. */
    fun accessibilityText(): String {
        val prefix = "终端屏幕文字\n"
        val omitted = "\n（内容过长，后续文字未提供）"
        val capacity = 32768 - prefix.length - omitted.length
        val body = StringBuilder()
        var emptyRows = 0
        var truncated = false
        for (row in 0 until rows) {
            val separators = if (body.isEmpty()) 0 else emptyRows + 1
            val available = capacity - body.length - separators
            val line = StringBuilder()
            var spaces = 0
            for (column in 0 until cols) {
                val cell = cells[row * cols + column]
                if (cell.width == 0) continue
                // Match Canvas concealment and suppress image placeholder glyphs.
                if (cell.flags and 8 != 0 || cell.text.isBlank() || cell.text.codePointAt(0) == 0x10eeee) {
                    spaces++
                    continue
                }
                if (line.length + spaces + cell.text.length > available) {
                    truncated = true
                    break
                }
                repeat(spaces) { line.append(' ') }
                spaces = 0
                line.append(cell.text)
            }
            if (line.isNotEmpty()) {
                repeat(separators) { body.append('\n') }
                body.append(line)
                emptyRows = 0
            } else emptyRows++
            if (truncated) break
        }
        if (body.isEmpty() && !truncated) return "终端屏幕为空"
        return prefix + body + if (truncated) omitted else ""
    }

    companion object {
        /** Private native snapshot format. Never used as a PTY output channel. */
        fun decode(data: IntArray): TerminalFrame {
            require(data.size >= 12 && data[2] == 1) { "终端画面格式无效" }
            val cols = data[3]; val rows = data[4]
            require(cols in 1..256 && rows in 1..256)
            var offset = 12
            val cells = ArrayList<TerminalCell>(cols * rows)
            repeat(cols * rows) {
                require(offset + 5 <= data.size)
                val count = data[offset + 4]
                require(count >= 0 && count <= data.size - offset - 5)
                val text = if (count == 0) "" else String(data, offset + 5, count)
                cells.add(TerminalCell(data[offset], data[offset + 1], data[offset + 2], data[offset + 3], text))
                offset += 5 + count
            }
            require(offset == data.size)
            return TerminalFrame((data[0].toLong() and 0xffffffffL) or (data[1].toLong() shl 32),
                cols, rows, data[5], data[6], data[7], data[8], data[9] != 0, data[10] != 0, data[11], cells)
        }
    }
}
