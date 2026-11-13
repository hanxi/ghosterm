package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import org.json.JSONObject

/** Streaming, strict, fixed schema: unknown fields and duplicates are rejected. */
internal object CodexChangeJson {
    fun reference(marker: JSONObject?, thread: Any?, turn: Any?, item: Any?): CodexChangeReference? = runCatching {
        marker ?: return null
        val id = marker.opt("snapshotID") as? String ?: return null
        val units = integer(marker.opt("snapshotUnits")) ?: return null
        val bytes = integer(marker.opt("snapshotBytes")) ?: return null
        CodexChangeReference.create(id, thread as? String ?: return null, turn as? String ?: return null,
            item as? String ?: return null, units, bytes)
    }.getOrNull()

    private fun integer(value: Any?): Long? = when (value) {
        is Int -> value.toLong()
        is Long -> value
        else -> null
    }

    fun chunk(value: JSONObject): CodexChangeChunk {
        fun count(key: String): Int {
            val number = integer(value.opt(key)) ?: error("完整补丁计数格式错误。")
            require(number in 0..CodexChangeReference.MAX_BYTES.toLong())
            return number.toInt()
        }
        return CodexChangeChunk(value.opt("status") as? String ?: error("缺少补丁状态。"),
            value.opt("snapshotID") as? String ?: error("缺少补丁身份。"), count("offset"), count("nextOffset"),
            count("totalUnits"), count("totalBytes"), value.opt("content") as? String ?: error("缺少补丁内容。"),
            value.opt("done") as? Boolean ?: error("缺少补丁结束状态。"))
    }

    fun document(bytes: ByteArray, reference: CodexChangeReference): CodexChangeDocument {
        require(bytes.size == reference.totalBytes && bytes.size <= CodexChangeReference.MAX_BYTES)
        val source = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        require(source.length == reference.totalUnits)
        JsonReader(StringReader(source)).use { reader ->
            reader.isLenient = false
            var version = 0L
            var thread: String? = null; var turn: String? = null; var item: String? = null
            var files: List<CodexCompleteChangeFile>? = null
            val seen = mutableSetOf<String>()
            reader.beginObject()
            while (reader.hasNext()) {
                val name = reader.nextName(); require(seen.add(name)) { "补丁字段重复。" }
                when (name) {
                    "schemaVersion" -> {
                        require(reader.peek() == JsonToken.NUMBER && reader.nextString() == "1")
                        version = 1L
                    }
                    "threadId" -> thread = string(reader)
                    "turnId" -> turn = string(reader)
                    "itemId" -> item = string(reader)
                    "changes" -> files = files(reader)
                    else -> error("未知的完整补丁字段。")
                }
            }
            reader.endObject()
            require(reader.peek() == JsonToken.END_DOCUMENT && seen.size == 5 && version == 1L &&
                thread == reference.threadID && turn == reference.turnID && item == reference.itemID) { "完整补丁身份或版本不匹配。" }
            return CodexChangeDocument(checkNotNull(thread), checkNotNull(turn), checkNotNull(item), checkNotNull(files))
        }
    }

    private fun string(reader: JsonReader): String {
        require(reader.peek() == JsonToken.STRING) { "完整补丁字段必须是字符串。" }
        return reader.nextString().also { codexChangeUtf8(it) }
    }

    private fun path(reader: JsonReader): String = string(reader).also {
        require(it.isNotEmpty() && codexChangeUtf8(it).size <= 16384) { "完整补丁路径无效或过长。" }
    }

    private fun files(reader: JsonReader): List<CodexCompleteChangeFile> {
        val result = mutableListOf<CodexCompleteChangeFile>()
        reader.beginArray()
        while (reader.hasNext()) {
            require(result.size < 8192) { "完整补丁文件数超限。" }
            var path: String? = null; var kind: Pair<String, String?>? = null; var diff: String? = null
            val seen = mutableSetOf<String>()
            reader.beginObject()
            while (reader.hasNext()) {
                val name = reader.nextName(); require(seen.add(name)) { "补丁文件字段重复。" }
                when (name) {
                    "path" -> path = path(reader)
                    "kind" -> kind = kind(reader)
                    "diff" -> diff = string(reader)
                    else -> error("未知的补丁文件字段。")
                }
            }
            reader.endObject()
            require(seen.size == 3)
            val type = checkNotNull(kind)
            result += CodexCompleteChangeFile(checkNotNull(path), type.first, type.second, checkNotNull(diff))
        }
        reader.endArray()
        return result.toList()
    }

    private fun kind(reader: JsonReader): Pair<String, String?> {
        var type: String? = null; var destination: String? = null
        val seen = mutableSetOf<String>()
        reader.beginObject()
        while (reader.hasNext()) {
            val name = reader.nextName(); require(seen.add(name))
            when (name) {
                "type" -> type = string(reader)
                "move_path" -> if (reader.peek() == JsonToken.NULL) reader.nextNull() else destination = path(reader)
                else -> error("未知的补丁变更类型字段。")
            }
        }
        reader.endObject()
        require(type in setOf("add", "delete", "update"))
        require(if (type == "update") seen == setOf("type", "move_path") else seen == setOf("type"))
        return checkNotNull(type) to destination
    }
}
