package com.hanxi.ghosterm

import com.hanxi.ghosterm.domain.*

/** Only the selected keys travel to the app owner, preserving other windows' edits. */
sealed class PreferenceEdit(val key: PreferenceKey) {
    abstract fun apply(before: AppPreferences): AppPreferences
    data class Appearance(val value: AppAppearance) : PreferenceEdit(PreferenceKey.APPEARANCE) { override fun apply(before: AppPreferences) = before.copy(appearance = value) }
    data class Theme(val value: TerminalTheme) : PreferenceEdit(PreferenceKey.TERMINAL_THEME) { override fun apply(before: AppPreferences) = before.copy(terminalTheme = value) }
    data class FontFamily(val value: TerminalFont) : PreferenceEdit(PreferenceKey.FONT_FAMILY) { override fun apply(before: AppPreferences) = before.copy(fontFamily=value) }
    data class FontSize(val value: Int) : PreferenceEdit(PreferenceKey.FONT_SIZE) { override fun apply(before: AppPreferences) = before.copy(fontSize = value) }
    data class LineHeight(val value: TerminalLineHeight) : PreferenceEdit(PreferenceKey.LINE_HEIGHT) { override fun apply(before: AppPreferences) = before.copy(lineHeight = value) }
    data class Cursor(val value: TerminalCursor) : PreferenceEdit(PreferenceKey.CURSOR) { override fun apply(before: AppPreferences) = before.copy(cursor = value) }
    data class Blink(val value: Boolean) : PreferenceEdit(PreferenceKey.BLINK) { override fun apply(before: AppPreferences) = before.copy(blink = value) }
    data class AuxKeys(val value: Boolean) : PreferenceEdit(PreferenceKey.AUX_KEYS) { override fun apply(before: AppPreferences) = before.copy(auxKeys = value) }
    data class KeySet(val value: AuxiliaryKeySet) : PreferenceEdit(PreferenceKey.KEY_SET) { override fun apply(before: AppPreferences) = before.copy(keySet = value) }
    data class CtrlMode(val value: CtrlBehavior) : PreferenceEdit(PreferenceKey.CTRL_MODE) { override fun apply(before: AppPreferences) = before.copy(ctrlMode = value) }
    data class AutoKeyboard(val value: Boolean) : PreferenceEdit(PreferenceKey.AUTO_KEYBOARD) { override fun apply(before: AppPreferences) = before.copy(autoKeyboard = value) }
    data class KeepAwake(val value: Boolean) : PreferenceEdit(PreferenceKey.KEEP_AWAKE) { override fun apply(before: AppPreferences) = before.copy(keepAwake = value) }
    data class ShowHidden(val value: Boolean) : PreferenceEdit(PreferenceKey.SHOW_HIDDEN) { override fun apply(before: AppPreferences) = before.copy(showHidden = value) }
    data class Sort(val value: FileSort) : PreferenceEdit(PreferenceKey.SORT_BY) { override fun apply(before: AppPreferences) = before.copy(sortBy = value) }
    data class FoldersFirst(val value: Boolean) : PreferenceEdit(PreferenceKey.FOLDERS_FIRST) { override fun apply(before: AppPreferences) = before.copy(foldersFirst = value) }
    data class Conflict(val value: FileConflictPolicy) : PreferenceEdit(PreferenceKey.CONFLICT) { override fun apply(before: AppPreferences) = before.copy(conflict = value) }
    data class Clipboard(val value: ClipboardAfterPaste) : PreferenceEdit(PreferenceKey.CLIPBOARD_AFTER_PASTE) { override fun apply(before: AppPreferences) = before.copy(clipboardAfterPaste = value) }

    companion object {
        fun defaults(): List<PreferenceEdit> = AppPreferences().let { p -> listOf(
            Appearance(p.appearance), Theme(p.terminalTheme), FontFamily(p.fontFamily), FontSize(p.fontSize), LineHeight(p.lineHeight),
            Cursor(p.cursor), Blink(p.blink), AuxKeys(p.auxKeys), KeySet(p.keySet), CtrlMode(p.ctrlMode),
            AutoKeyboard(p.autoKeyboard), KeepAwake(p.keepAwake), ShowHidden(p.showHidden), Sort(p.sortBy),
            FoldersFirst(p.foldersFirst), Conflict(p.conflict), Clipboard(p.clipboardAfterPaste),
        ) }
    }
}
