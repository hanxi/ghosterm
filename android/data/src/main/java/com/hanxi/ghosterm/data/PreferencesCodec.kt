package com.hanxi.ghosterm.data

import com.hanxi.ghosterm.domain.*
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

/** App-owned schema 1. Unsupported schemas and damaged documents remain untouched. */
internal object PreferencesCodec {
    const val LIMIT = 8192
    fun encode(p: AppPreferences): ByteArray {
        val values = JSONObject().apply {
            put("appearance", p.appearance.id); put("terminalTheme", p.terminalTheme.id)
            put("fontFamily",p.fontFamily.id)
            put("fontSize", p.fontSize); put("lineHeight", p.lineHeight.id); put("cursor", p.cursor.id)
            put("blink", p.blink); put("auxKeys", p.auxKeys); put("keySet", p.keySet.id)
            put("ctrlMode", p.ctrlMode.id); put("autoKeyboard", p.autoKeyboard); put("keepAwake", p.keepAwake)
            put("showHidden", p.showHidden); put("sortBy", p.sortBy.id); put("foldersFirst", p.foldersFirst)
            put("conflict", p.conflict.preferenceId); put("clipboardAfterPaste", p.clipboardAfterPaste.id)
        }
        return JSONObject().put("version", 1).put("values", values).toString().toByteArray(Charsets.UTF_8)
    }

    fun decode(bytes: ByteArray): DecodedPreferences {
        require(bytes.size in 1..LIMIT) { "偏好文件大小无效" }
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        // Bound Android JSON parser recursion before parsing any untrusted bytes.
        var depth = 0; var quoted = false; var escaped = false
        for (c in text) {
            if (quoted) {
                if (escaped) escaped = false else if (c == '\\') escaped = true else if (c == '"') quoted = false
            } else when (c) {
                '"' -> quoted = true
                '{', '[' -> { depth++; require(depth <= 4) { "偏好文件嵌套过深" } }
                '}', ']' -> { depth--; require(depth >= 0) }
                else -> require(c in " \t\r\n:,-+.0123456789truefalsenullEe") { "偏好文件格式无效" }
            }
        }
        require(!quoted && depth == 0) { "偏好文件不完整" }
        val root = JSONObject(text)
        require(root.toString() == text) { "偏好文件编码无效" }
        require(root.keys().asSequence().toSet() == setOf("version", "values")) { "偏好文件字段无效" }
        require(root.get("version") is Int && root.getInt("version") == 1) { "偏好版本不支持" }
        val values = root.get("values") as? JSONObject ?: error("偏好内容无效")
        require(values.keys().asSequence().all { name -> PreferenceKey.entries.any { it.id == name } }) {
            "偏好包含不支持的字段"
        }
        val defaults = AppPreferences()
        val issues = linkedSetOf<PreferenceIssue>()
        fun <T> value(key: PreferenceKey, fallback: T, convert: (Any) -> T?): T {
            if (!values.has(key.id)) { issues += PreferenceIssue(key, PreferenceIssueKind.MISSING); return fallback }
            val result = convert(values.get(key.id))
            if (result != null) return result
            issues += PreferenceIssue(key, PreferenceIssueKind.INVALID)
            return fallback
        }
        fun <T : PreferenceChoice> choice(key: PreferenceKey, fallback: T, choices: List<T>): T =
            value(key, fallback) { raw -> choices.firstOrNull { raw is String && it.id == raw } }
        fun boolean(key: PreferenceKey, fallback: Boolean) = value(key, fallback) { it as? Boolean }
        return DecodedPreferences(AppPreferences(
            appearance = choice(PreferenceKey.APPEARANCE, defaults.appearance, AppAppearance.entries),
            terminalTheme = choice(PreferenceKey.TERMINAL_THEME, defaults.terminalTheme, TerminalTheme.entries),
            // Additive schema-1 field: old documents already used Android's system monospace.
            fontFamily = if(!values.has(PreferenceKey.FONT_FAMILY.id)) TerminalFont.SYSTEM else
                value(PreferenceKey.FONT_FAMILY,defaults.fontFamily) { (it as? String)?.let(TerminalFont::fromId) },
            fontSize = value(PreferenceKey.FONT_SIZE, defaults.fontSize) { (it as? Int)?.takeIf { n -> n in 10..20 } },
            lineHeight = choice(PreferenceKey.LINE_HEIGHT, defaults.lineHeight, TerminalLineHeight.entries),
            cursor = choice(PreferenceKey.CURSOR, defaults.cursor, TerminalCursor.entries),
            blink = boolean(PreferenceKey.BLINK, defaults.blink),
            auxKeys = boolean(PreferenceKey.AUX_KEYS, defaults.auxKeys),
            keySet = choice(PreferenceKey.KEY_SET, defaults.keySet, AuxiliaryKeySet.entries),
            ctrlMode = choice(PreferenceKey.CTRL_MODE, defaults.ctrlMode, CtrlBehavior.entries),
            autoKeyboard = boolean(PreferenceKey.AUTO_KEYBOARD, defaults.autoKeyboard),
            keepAwake = boolean(PreferenceKey.KEEP_AWAKE, defaults.keepAwake),
            showHidden = boolean(PreferenceKey.SHOW_HIDDEN, defaults.showHidden),
            sortBy = choice(PreferenceKey.SORT_BY, defaults.sortBy, FileSort.entries),
            foldersFirst = boolean(PreferenceKey.FOLDERS_FIRST, defaults.foldersFirst),
            conflict = value(PreferenceKey.CONFLICT, defaults.conflict) { raw ->
                FileConflictPolicy.entries.firstOrNull { raw is String && it.preferenceId == raw }
            },
            clipboardAfterPaste = choice(PreferenceKey.CLIPBOARD_AFTER_PASTE, defaults.clipboardAfterPaste, ClipboardAfterPaste.entries),
        ), issues.toSet())
    }
}
