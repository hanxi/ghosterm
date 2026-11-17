package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import org.json.JSONArray
import java.io.StringReader
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

/** Configuration observed through a selected live endpoint, never a grant. */
data class PluginForegroundDeclaration(
    val instance: String, val runtime: String, val executorDigest: String, val clientDigest: String,
    val command: String, val arguments: List<String>, val methods: List<String>,
    val hostMethods: List<String>, val clientCapabilities: List<String>,
) {
    companion object {
        const val MAXIMUM_BYTES = 153600
        private val hex32 = Regex("[0-9a-f]{32}")
        private val hex64 = Regex("[0-9a-f]{64}")
        private val fields = setOf("schemaVersion", "instance", "runtime", "executorDigest", "clientDigest",
            "command", "arguments", "methods", "hostMethods", "clientCapabilities", "authorizationGranted")

        fun parse(bytes: ByteArray, expectedInstance: String): PluginForegroundDeclaration {
            require(bytes.size <= MAXIMUM_BYTES && hex32.matches(expectedInstance)) { "前台实例声明超限或标识无效" }
            val source = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
            val strings = mutableMapOf<String, String>()
            val arrays = mutableMapOf<String, List<String>>()
            JsonReader(StringReader(source)).use { reader ->
                reader.isLenient = false
                fun text(): String {
                    require(reader.peek() == JsonToken.STRING) { "前台声明字段必须是字符串" }
                    return reader.nextString().also {
                        require(Charsets.UTF_8.newEncoder().canEncode(it) && '\u0000' !in it && it.toByteArray().size <= 4096) { "前台声明字符串无效或超限" }
                    }
                }
                val seen = mutableSetOf<String>()
                reader.beginObject()
                while (reader.hasNext()) {
                    val key = reader.nextName()
                    require(key in fields && seen.add(key)) { "前台声明有重复或未知字段" }
                    when (key) {
                        "schemaVersion" -> require(reader.peek() == JsonToken.NUMBER && reader.nextString() == "1") { "不支持的前台声明版本" }
                        "authorizationGranted" -> require(reader.peek() == JsonToken.BOOLEAN && !reader.nextBoolean()) { "前台声明不能授予权限" }
                        "arguments", "methods", "hostMethods", "clientCapabilities" -> {
                            val maximum = when (key) { "arguments" -> 32; "clientCapabilities" -> 4; else -> 16 }
                            val values = mutableListOf<String>()
                            reader.beginArray()
                            while (reader.hasNext()) {
                                require(values.size < maximum) { "前台声明数组超限" }
                                values += text()
                            }
                            reader.endArray(); arrays[key] = values.toList()
                        }
                        else -> strings[key] = text()
                    }
                }
                reader.endObject()
                require(seen == fields && reader.peek() == JsonToken.END_DOCUMENT) { "前台声明缺失字段或有额外内容" }
            }
            val instance = strings.getValue("instance")
            val command = strings.getValue("command")
            val arguments = arrays.getValue("arguments")
            val methods = arrays.getValue("methods")
            val hostMethods = arrays.getValue("hostMethods")
            val services = arrays.getValue("clientCapabilities")
            require(instance == expectedInstance && hex64.matches(strings.getValue("executorDigest")) &&
                hex64.matches(strings.getValue("clientDigest")) && strings.getValue("runtime") in listOf("portable-js", "node", "exec") &&
                command.isNotEmpty() && command.toByteArray().size <= 519 &&
                arguments.sumOf { it.toByteArray().size + 1 } + command.toByteArray().size + 1 <= 16384 &&
                methods.size + hostMethods.size <= 16 && methods.none { it in hostMethods } &&
                services.distinct().size == services.size && services.all { it in listOf("ui.table", "ui.diff", "ui.task", "ui.views") }) { "前台声明无效或实例不匹配" }
            ExtensionRelayBootstrap.parseMethodsJson(JSONArray(methods).toString())
            ExtensionRelayBootstrap.parseHostMethodsJson(JSONArray(hostMethods).toString())
            return PluginForegroundDeclaration(instance, strings.getValue("runtime"), strings.getValue("executorDigest"),
                strings.getValue("clientDigest"), command, arguments, methods, hostMethods, services)
        }

        fun bootstrap(executable: String, directory: String, instance: String): String {
            require(executable.isNotEmpty() && hex32.matches(instance) && directory.startsWith('/') &&
                directory.split('/').drop(1).all { it.isNotEmpty() && it != "." && it != ".." } &&
                listOf(executable, directory).all { Charsets.UTF_8.newEncoder().canEncode(it) && it.toByteArray().size <= 4096 && it.none { c -> c == '\u0000' || c == '\n' || c == '\r' } }) {
                "请指定 gterm、规范绝对目录与 32 位小写实例标识"
            }
            return ("exec " + listOf(executable, "relay", "--inspect", directory, "--instance", instance)
                .joinToString(" ") { "'" + it.replace("'", "'\\''") + "'" }).also {
                require(it.toByteArray().size <= 1024) { "前台检查命令超过 SSH 启动上限" }
            }
        }
    }
}
