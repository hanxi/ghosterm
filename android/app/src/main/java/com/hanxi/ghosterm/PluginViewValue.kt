package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.util.Collections

/** Strict shared value boundary for standard Native view contributions. */
internal object PluginViewValue {
    fun <T> captured(values: List<T>): List<T> = Collections.unmodifiableList(ArrayList(values))
    sealed interface Value {
        data class Fields(val values: Map<String, Value>) : Value
        data class Items(val values: List<Value>) : Value
        data class Text(val value: String) : Value
        data class Number(val value: Long) : Value
        data class Bool(val value: Boolean) : Value
        data object Null : Value
    }
    fun utf8(value: String): ByteArray {
        val bytes = Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
        return ByteArray(bytes.remaining()).also { bytes.get(it) }
    }
    fun success(bytes: ByteArray): Map<String, Value> {
        require(bytes.size <= 3072) { "视图回包超过上限" }
        val source = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        val root = JsonReader(StringReader(source)).use { reader ->
            reader.isLenient = false
            var nodes = 0
            fun string(): String = reader.nextString().also { utf8(it) }
            fun read(depth: Int): Value {
                require(++nodes <= 4096 && depth <= 32) { "视图 JSON 结构超过上限" }
                return when (reader.peek()) {
                    JsonToken.BEGIN_OBJECT -> {
                        reader.beginObject(); val fields = linkedMapOf<String, Value>()
                        while (reader.hasNext()) {
                            val name = reader.nextName(); utf8(name)
                            require(!fields.containsKey(name) && name !in setOf("__proto__", "constructor", "prototype")) { "视图 JSON 字段重复或无效" }
                            fields[name] = read(depth + 1)
                        }
                        reader.endObject(); Value.Fields(fields)
                    }
                    JsonToken.BEGIN_ARRAY -> {
                        reader.beginArray(); val items = mutableListOf<Value>()
                        while (reader.hasNext()) items += read(depth + 1)
                        reader.endArray(); Value.Items(items)
                    }
                    JsonToken.STRING -> Value.Text(string())
                    JsonToken.NUMBER -> {
                        val token = reader.nextString()
                        require(token.matches(Regex("-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")))
                        val number = token.toDouble()
                        require(number.isFinite() && number in -9007199254740991.0..9007199254740991.0 && number == number.toLong().toDouble()) { "视图数值必须是准确整数" }
                        Value.Number(number.toLong())
                    }
                    JsonToken.BOOLEAN -> Value.Bool(reader.nextBoolean())
                    JsonToken.NULL -> { reader.nextNull(); Value.Null }
                    else -> error("视图 JSON 格式无效")
                }
            }
            val result = read(0).fields(); require(reader.peek() == JsonToken.END_DOCUMENT) { "视图 JSON 后存在额外内容" }; result
        }
        require((root["kind"] as? Value.Text)?.value == "success") { "视图结果尚未确认" }
        root.keys(setOf("kind", "value"))
        val value = checkNotNull(root["value"])
        require(utf8(compact(value)).size <= 2048) { "视图内容超过上限" }
        return value.fields()
    }
    fun compact(value: Value): String = when (value) {
        is Value.Fields -> value.values.entries.joinToString(",", "{", "}") { quote(it.key) + ":" + compact(it.value) }
        is Value.Items -> value.values.joinToString(",", "[", "]", transform = ::compact)
        is Value.Text -> quote(value.value)
        is Value.Number -> value.value.toString()
        is Value.Bool -> value.value.toString()
        Value.Null -> "null"
    }
    fun quote(value: String): String = buildString {
        append('"')
        value.forEach { append(when (it) {
            '"' -> "\\\""; '\\' -> "\\\\"; '\b' -> "\\b"; '\t' -> "\\t"; '\n' -> "\\n"; '\r' -> "\\r"; '\u000c' -> "\\f"
            in '\u0000'..'\u001f' -> "\\u" + it.code.toString(16).padStart(4, '0')
            else -> it.toString()
        }) }; append('"')
    }
    fun Value?.fields() = (this as? Value.Fields)?.values ?: error("视图字段必须是对象")
    fun Value?.items() = (this as? Value.Items)?.values ?: error("视图字段必须是数组")
    fun Map<String, Value>.keys(expected: Set<String>) { require(keys == expected) { "视图字段缺失或未知" } }
    fun Value?.id(): String = (this as? Value.Text)?.value?.also {
        require(it.matches(Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,63}"))) { "视图 ID 无效" }
    } ?: error("视图 ID 必须是字符串")
    fun Value?.revision(): String = (this as? Value.Text)?.value?.also {
        require(it.matches(Regex("[1-9][0-9]{0,19}")) && it.toULongOrNull() != null) { "视图修订无效" }
    } ?: error("视图修订必须是字符串")
    fun Value?.integer(range: IntRange): Int {
        val value = (this as? Value.Number)?.value ?: error("视图计数必须是整数")
        require(value in range.first.toLong()..range.last.toLong()) { "视图计数超过上限" }; return value.toInt()
    }
    fun Value?.text(maximum: Int, nonempty: Boolean = false): String = (this as? Value.Text)?.value?.also {
        require(it.codePointCount(0, it.length) <= maximum && (!nonempty || it.isNotEmpty()) &&
            it.none { c -> c.code in 0..8 || c.code in 11..12 || c.code in 14..31 || c.code == 127 }) { "视图文字无效或过长" }
    } ?: error("视图文字必须是字符串")
}
