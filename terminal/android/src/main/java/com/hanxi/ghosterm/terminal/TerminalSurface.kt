package com.hanxi.ghosterm.terminal

/** The renderer knows only this seam, never connection kinds or native handles. */
interface TerminalSurface {
    fun frame(after: Long): TerminalFrame?
    fun resize(cols: Int, rows: Int, cellWidth: Int, cellHeight: Int)
    fun commit(text: String)
    fun key(code: Int, modifiers: Int = 0, text: String = "")
    fun scroll(lines: Int)
    /** False requests confirmation; no bytes were written in that case. */
    suspend fun paste(text: String, allowUnsafe: Boolean = false): Boolean
    suspend fun select(operation: SelectionOperation, x: Int = 0, y: Int = 0)
    /** Scroll and extend one endpoint atomically on the VT owner. */
    suspend fun extendSelection(operation: SelectionOperation, x: Int, y: Int, scrollLines: Int)
    suspend fun copySelection(): String
    suspend fun clearLocal()
}

enum class SelectionOperation(val code: Int) { WORD(2), START(3), END(4), ALL(5), CLEAR(6), ORDER(9) }

data class TerminalCell(val background: Int, val foreground: Int, val flags: Int, val width: Int, val text: String)

data class TerminalFrame(
    val revision: Long, val cols: Int, val rows: Int, val background: Int, val foreground: Int,
    val cursorX: Int, val cursorY: Int, val cursorVisible: Boolean, val cursorBlink: Boolean,
    val cursorStyle: Int, val cells: List<TerminalCell>,
) {
    fun line(row: Int): String = cells.subList(row * cols, (row + 1) * cols)
        .joinToString("") { if (it.width == 0) "" else it.text.ifEmpty { " " } }.trimEnd()

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
