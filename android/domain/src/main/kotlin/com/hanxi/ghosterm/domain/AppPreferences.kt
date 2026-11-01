package com.hanxi.ghosterm.domain

/** Stable on-disk IDs; display labels belong to the native settings UI. */
interface PreferenceChoice { val id: String }
enum class AppAppearance(override val id: String) : PreferenceChoice { SYSTEM("system"), LIGHT("light"), DARK("dark") }
enum class TerminalTheme(override val id: String) : PreferenceChoice { GRAPHITE("graphite"), FOREST("forest"), PAPER("paper") }
enum class TerminalLineHeight(override val id: String, val factor: Float) : PreferenceChoice {
    COMPACT("compact", 1.5f), STANDARD("standard", 1.85f), RELAXED("relaxed", 2.15f)
}
enum class TerminalCursor(override val id: String) : PreferenceChoice { BLOCK("block"), BAR("bar"), UNDERLINE("underline") }
enum class AuxiliaryKeySet(override val id: String) : PreferenceChoice { BASIC("basic"), EXTENDED("extended") }
enum class CtrlBehavior(override val id: String) : PreferenceChoice { ONCE("once"), LOCKED("locked") }
enum class FileSort(override val id: String) : PreferenceChoice { NAME("name"), SIZE("size") }
val FileConflictPolicy.preferenceId: String get() = when (this) {
    FileConflictPolicy.ASK -> "ask"
    FileConflictPolicy.KEEP_BOTH -> "rename"
    FileConflictPolicy.SKIP -> "skip"
}
enum class ClipboardAfterPaste(override val id: String) : PreferenceChoice { KEEP("keep"), CLEAR("clear") }

enum class PreferenceKey(val id: String) {
    APPEARANCE("appearance"), TERMINAL_THEME("terminalTheme"), FONT_SIZE("fontSize"),
    LINE_HEIGHT("lineHeight"), CURSOR("cursor"), BLINK("blink"), AUX_KEYS("auxKeys"),
    KEY_SET("keySet"), CTRL_MODE("ctrlMode"), AUTO_KEYBOARD("autoKeyboard"), KEEP_AWAKE("keepAwake"),
    SHOW_HIDDEN("showHidden"), SORT_BY("sortBy"), FOLDERS_FIRST("foldersFirst"),
    CONFLICT("conflict"), CLIPBOARD_AFTER_PASTE("clipboardAfterPaste")
}

/** The committed app defaults. Host configuration and credentials never enter this value. */
data class AppPreferences(
    val appearance: AppAppearance = AppAppearance.SYSTEM,
    val terminalTheme: TerminalTheme = TerminalTheme.GRAPHITE,
    val fontSize: Int = 12,
    val lineHeight: TerminalLineHeight = TerminalLineHeight.STANDARD,
    val cursor: TerminalCursor = TerminalCursor.BLOCK,
    val blink: Boolean = true,
    val auxKeys: Boolean = true,
    val keySet: AuxiliaryKeySet = AuxiliaryKeySet.BASIC,
    val ctrlMode: CtrlBehavior = CtrlBehavior.ONCE,
    val autoKeyboard: Boolean = false,
    val keepAwake: Boolean = false,
    val showHidden: Boolean = false,
    val sortBy: FileSort = FileSort.NAME,
    val foldersFirst: Boolean = true,
    val conflict: FileConflictPolicy = FileConflictPolicy.ASK,
    val clipboardAfterPaste: ClipboardAfterPaste = ClipboardAfterPaste.KEEP,
) {
    init { require(fontSize in 10..20) { "字号必须在 10–20 sp 之间" } }
}

enum class PreferenceIssueKind { MISSING, INVALID }
data class PreferenceIssue(val key: PreferenceKey, val kind: PreferenceIssueKind)
data class DecodedPreferences(val values: AppPreferences, val issues: Set<PreferenceIssue> = emptySet())
