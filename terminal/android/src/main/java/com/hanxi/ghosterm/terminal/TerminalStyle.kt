package com.hanxi.ghosterm.terminal

/** Renderer configuration, independent of application preferences and transport kinds. */
enum class CursorShape(val code: Int) { BAR(0), BLOCK(1), UNDERLINE(2) }

data class TerminalAppearance(
    val background: Int = 0x171b20,
    val foreground: Int = 0xdce3ec,
    val cursor: CursorShape = CursorShape.BLOCK,
    val blink: Boolean = true,
    val cursorColor: Int = foreground,
    val cursorText: Int = background,
    val selectionBackground: Int = 0x445b7d,
    val selectionForeground: Int = 0xffffff,
    val palette: List<Int> = listOf(0x1d1f21,0xcc6666,0xb5bd68,0xf0c674,0x81a2be,0xb294bb,0x8abeb7,0xc5c8c6,
        0x666666,0xd54e53,0xb9ca4a,0xe7c547,0x7aa6da,0xc397d8,0x70c0b1,0xeaeaea),
) {
    init { require(palette.size==16 && (listOf(background,foreground,cursorColor,cursorText,selectionBackground,selectionForeground)+palette).all { it in 0..0xffffff }) }
}

data class TerminalStyle(
    val fontSize: Float = 12f,
    val lineHeight: Float = 1.85f,
    val appearance: TerminalAppearance = TerminalAppearance(),
    val fontId: String = "system",
) {
    init { require(fontSize in 8f..32f && lineHeight in 1f..3f) }
}
