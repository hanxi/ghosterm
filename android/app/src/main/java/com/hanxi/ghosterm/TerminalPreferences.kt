package com.hanxi.ghosterm

import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.terminal.*

data class TerminalControls(val auxiliaryKeys: Boolean = true, val extendedKeys: Boolean = false,
    val lockedCtrl: Boolean = false, val autoKeyboard: Boolean = false, val keepAwake: Boolean = false)

internal fun AppPreferences.terminalControls() = TerminalControls(auxKeys, keySet == AuxiliaryKeySet.EXTENDED,
    ctrlMode == CtrlBehavior.LOCKED, autoKeyboard, keepAwake)

internal fun AppPreferences.terminalStyle(): TerminalStyle {
    val colors = when (terminalTheme) {
        TerminalTheme.GRAPHITE -> 0x171b20 to 0xdce3ec
        TerminalTheme.FOREST -> 0x14241c to 0xe2ecdf
        TerminalTheme.PAPER -> 0xfaf8f2 to 0x283b33
    }
    val shape = when (cursor) {
        TerminalCursor.BLOCK -> CursorShape.BLOCK
        TerminalCursor.BAR -> CursorShape.BAR
        TerminalCursor.UNDERLINE -> CursorShape.UNDERLINE
    }
    return TerminalStyle(fontSize.toFloat(), lineHeight.factor, TerminalAppearance(colors.first, colors.second, shape, blink))
}
