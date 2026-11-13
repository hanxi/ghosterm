package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import org.json.JSONObject
import org.json.JSONArray

/** Validate before JSONObject's permissive parser. Java strings keep distinct
 * Unicode spellings; raw parameter/vendor bytes are never reserialized. */
internal object ClaudeJson {
    private val numberPattern = Regex("-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")

    /** Action params use JSON.stringify's strings and integer-index key order.
     * Vendor records are retained separately as their original byte strings. */
    fun compact(value: Any?): String = when (value) {
        null, JSONObject.NULL -> "null"
        is String -> quote(value)
        is Boolean, is Int, is Long -> value.toString()
        is JSONArray -> (0 until value.length()).joinToString(",", "[", "]") { compact(value.get(it)) }
        is JSONObject -> {
            fun index(key: String): Long? = key.toLongOrNull()?.takeIf { it in 0..4294967294L && key == it.toString() }
            val keys = value.keys().asSequence().toList()
            val ordered = keys.filter { index(it) != null }.sortedBy { index(it) } + keys.filter { index(it) == null }
            ordered.joinToString(",", "{", "}") { quote(it) + ":" + compact(value.get(it)) }
        }
        else -> error("Unsupported Claude action JSON value")
    }

    private fun quote(value: String): String {
        utf8(value)
        return buildString {
            append('"')
            value.forEach { character ->
                append(when (character) {
                    '"' -> "\\\""; '\\' -> "\\\\"; '\b' -> "\\b"; '\t' -> "\\t"; '\n' -> "\\n"; '\r' -> "\\r"; '\u000C' -> "\\f"
                    in '\u0000'..'\u001F' -> "\\u" + character.code.toString(16).padStart(4, '0')
                    else -> character.toString()
                })
            }
            append('"')
        }
    }
    fun utf8(value: String): ByteArray {
        val bytes = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
        return ByteArray(bytes.remaining()).also { bytes.get(it) }
    }

    fun objectValue(bytes: ByteArray, limit: Int = 600000): JSONObject {
        require(bytes.size <= limit) { "Claude JSON exceeds byte limit" }
        val source = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        var nodes = 0
        JsonReader(StringReader(source)).use { reader ->
            reader.isLenient = false
            fun string(): String = reader.nextString().also { utf8(it) }
            fun value(depth: Int) {
                require(depth <= 40 && ++nodes <= 10000) { "Claude JSON exceeds structure limit" }
                when (reader.peek()) {
                    JsonToken.BEGIN_OBJECT -> {
                        reader.beginObject(); val seen = mutableSetOf<String>()
                        while (reader.hasNext()) {
                            val name = reader.nextName(); utf8(name)
                            require(seen.add(name)) { "Duplicate Claude JSON member" }
                            value(depth + 1)
                        }
                        reader.endObject()
                    }
                    JsonToken.BEGIN_ARRAY -> {
                        reader.beginArray()
                        while (reader.hasNext()) value(depth + 1)
                        reader.endArray()
                    }
                    JsonToken.STRING -> string()
                    JsonToken.NUMBER -> require(reader.nextString().matches(numberPattern))
                    JsonToken.BOOLEAN -> reader.nextBoolean()
                    JsonToken.NULL -> reader.nextNull()
                    else -> error("Invalid Claude JSON value")
                }
            }
            require(reader.peek() == JsonToken.BEGIN_OBJECT) { "Claude JSON must be an object" }
            value(0)
            require(reader.peek() == JsonToken.END_DOCUMENT) { "Trailing Claude JSON data" }
        }
        return JSONObject(source)
    }
}
