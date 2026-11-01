package com.hanxi.ghosterm.terminal

/** Renderer configuration, independent of application preferences and transport kinds. */
enum class CursorShape(val code: Int) { BAR(0), BLOCK(1), UNDERLINE(2) }

data class TerminalAppearance(
    val background: Int = 0x171b20,
    val foreground: Int = 0xdce3ec,
    val cursor: CursorShape = CursorShape.BLOCK,
    val blink: Boolean = true,
) {
    init { require(background in 0..0xffffff && foreground in 0..0xffffff) }
}

data class TerminalStyle(
    val fontSize: Float = 12f,
    val lineHeight: Float = 1.85f,
    val appearance: TerminalAppearance = TerminalAppearance(),
) {
    init { require(fontSize in 8f..32f && lineHeight in 1f..3f) }
}
