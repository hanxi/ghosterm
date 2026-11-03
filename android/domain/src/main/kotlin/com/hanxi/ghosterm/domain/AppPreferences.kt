package com.hanxi.ghosterm.domain

/** Stable on-disk IDs; display labels belong to the native settings UI. */
interface PreferenceChoice { val id: String }
enum class AppAppearance(override val id: String) : PreferenceChoice { SYSTEM("system"), LIGHT("light"), DARK("dark") }
@ConsistentCopyVisibility
data class TerminalTheme internal constructor(override val id: String, val label: String,
    val background: Int, val foreground: Int, val cursorColor: Int, val cursorText: Int,
    val selectionBackground: Int, val selectionForeground: Int, val palette: List<Int>) : PreferenceChoice {
    val light get() = ((background ushr 16)*299+(background ushr 8 and 255)*587+(background and 255)*114)>=128000
    companion object {
        // Ghostty's pinned default ANSI palette. Existing three preference IDs retain their colors.
        private val standard=listOf(0x1d1f21,0xcc6666,0xb5bd68,0xf0c674,0x81a2be,0xb294bb,0x8abeb7,0xc5c8c6,
            0x666666,0xd54e53,0xb9ca4a,0xe7c547,0x7aa6da,0xc397d8,0x70c0b1,0xeaeaea)
        val GRAPHITE=TerminalTheme("graphite","石墨蓝",0x171b20,0xdce3ec,0xdce3ec,0x171b20,0x445b7d,0xffffff,standard)
        val FOREST=TerminalTheme("forest","墨绿",0x14241c,0xe2ecdf,0xe2ecdf,0x14241c,0x445b7d,0xffffff,standard)
        val PAPER=TerminalTheme("paper","纸白",0xfaf8f2,0x283b33,0x283b33,0xfaf8f2,0x445b7d,0xffffff,standard)
        val entries:List<TerminalTheme> by lazy { listOf(GRAPHITE,FOREST,PAPER)+ghosttyThemes() }
    }
}
@ConsistentCopyVisibility
data class TerminalFont private constructor(override val id:String) : PreferenceChoice {
    companion object {
        val SYSTEM=TerminalFont("system")
        val JETBRAINS_MONO=TerminalFont("jetbrains_mono")
        val SOURCE_CODE_PRO=TerminalFont("source_code_pro")
        val entries=listOf(SYSTEM,JETBRAINS_MONO,SOURCE_CODE_PRO)
        fun fromId(id:String):TerminalFont?=entries.firstOrNull { it.id==id }
            ?: id.takeIf { it.matches(Regex("custom:[0-9a-f]{64}")) }?.let(::TerminalFont)
    }
}
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
    APPEARANCE("appearance"), TERMINAL_THEME("terminalTheme"), FONT_FAMILY("fontFamily"), FONT_SIZE("fontSize"),
    LINE_HEIGHT("lineHeight"), CURSOR("cursor"), BLINK("blink"), AUX_KEYS("auxKeys"),
    KEY_SET("keySet"), CTRL_MODE("ctrlMode"), AUTO_KEYBOARD("autoKeyboard"), KEEP_AWAKE("keepAwake"),
    SHOW_HIDDEN("showHidden"), SORT_BY("sortBy"), FOLDERS_FIRST("foldersFirst"),
    CONFLICT("conflict"), CLIPBOARD_AFTER_PASTE("clipboardAfterPaste")
}

/** The committed app defaults. Host configuration and credentials never enter this value. */
data class AppPreferences(
    val appearance: AppAppearance = AppAppearance.SYSTEM,
    val terminalTheme: TerminalTheme = TerminalTheme.GRAPHITE,
    val fontFamily: TerminalFont = TerminalFont.SYSTEM,
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
