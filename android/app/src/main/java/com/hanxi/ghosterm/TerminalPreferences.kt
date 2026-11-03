package com.hanxi.ghosterm

import com.hanxi.ghosterm.domain.*
import com.hanxi.ghosterm.terminal.*

data class TerminalControls(val auxiliaryKeys: Boolean = true, val extendedKeys: Boolean = false,
    val lockedCtrl: Boolean = false, val autoKeyboard: Boolean = false, val keepAwake: Boolean = false)

internal fun AppPreferences.terminalControls() = TerminalControls(auxKeys, keySet == AuxiliaryKeySet.EXTENDED,
    ctrlMode == CtrlBehavior.LOCKED, autoKeyboard, keepAwake)

internal fun AppPreferences.terminalStyle(): TerminalStyle {
    val theme=terminalTheme
    val shape = when (cursor) {
        TerminalCursor.BLOCK -> CursorShape.BLOCK
        TerminalCursor.BAR -> CursorShape.BAR
        TerminalCursor.UNDERLINE -> CursorShape.UNDERLINE
    }
    return TerminalStyle(fontSize.toFloat(),lineHeight.factor,TerminalAppearance(theme.background,theme.foreground,shape,blink,
        theme.cursorColor,theme.cursorText,theme.selectionBackground,theme.selectionForeground,theme.palette),fontFamily.id)
}
