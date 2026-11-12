package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import java.io.StringReader

/** Builds the explicit remote command accepted by the SSH extension route.
 * Values are argv tokens quoted for the remote POSIX shell; this does not
 * discover or install gterm. */
object ExtensionRelayBootstrap {
    data class Request(
        val executable: String,
        val executorDirectory: String,
        val executorDigest: String,
        val clientDigest: String,
        val methods: List<String>,
        val command: String,
        val arguments: List<String> = emptyList(),
    )

    /** Accept a JSON string array so the UI never invents a shell tokenizer. */
    fun parseArgumentsJson(source: String): List<String> = parseStringArray(source, 32, "命令参数")

    fun parseMethodsJson(source: String): List<String> {
        val methods = parseStringArray(source, 16, "RPC 方法")
        require(methods.distinct().size == methods.size && methods.all(::validMethod)) {
            "RPC 方法名无效或重复"
        }
        return methods
    }

    private fun parseStringArray(source: String, limit: Int, label: String): List<String> {
        require(source.toByteArray().size <= 65536) { "$label JSON 超过 64 KiB" }
        try {
            JsonReader(StringReader(source)).use { reader ->
                reader.isLenient = false
                require(reader.peek() == JsonToken.BEGIN_ARRAY) { "$label 必须是 JSON 字符串数组" }
                reader.beginArray()
                val result = buildList {
                    while (reader.hasNext()) {
                        require(size < limit) { "$label 最多 $limit 项" }
                        require(reader.peek() == JsonToken.STRING) { "$label 必须是 JSON 字符串数组" }
                        add(reader.nextString())
                    }
                }
                reader.endArray()
                require(reader.peek() == JsonToken.END_DOCUMENT) { "$label JSON 后不能有额外内容" }
                result
            }
        } catch (failure: IllegalArgumentException) {
            throw failure
        } catch (failure: Exception) {
            throw IllegalArgumentException("$label 必须是有效的严格 JSON 字符串数组", failure)
        }
    }

    fun build(request: Request): String {
        require(request.executable.isNotBlank()) { "请指定远端 gterm 可执行文件" }
        require(request.executorDirectory.isNotBlank()) { "请指定远端 Executor 包目录" }
        require(request.executorDigest.matches(Regex("[a-f0-9]{64}"))) { "Executor 摘要必须是 64 位小写 SHA-256" }
        require(request.clientDigest.matches(Regex("[a-f0-9]{64}"))) { "Client 摘要必须是 64 位小写 SHA-256" }
        require(request.command.isNotBlank()) { "请指定完整插件命令" }
        require(request.methods.size <= 16 && request.methods.distinct().size == request.methods.size &&
            request.methods.all(::validMethod)) {
            "RPC 方法名无效（最多 16 个）"
        }
        require(request.arguments.size <= 32) { "命令参数最多 32 项" }
        val argv = buildList {
            add(request.executable)
            add("relay"); add("--stdio")
            add("--trust-development-digest"); add(request.executorDigest)
            add("--client-digest"); add(request.clientDigest)
            request.methods.forEach { add("--method"); add(it) }
            add("--"); add(request.executorDirectory); add(request.command)
            addAll(request.arguments)
        }
        require(argv.all { '\u0000' !in it && '\n' !in it && '\r' !in it && it.toByteArray().size <= 4096 }) {
            "远端启动参数包含不支持的字符或过长"
        }
        return "exec " + argv.joinToString(" ", transform = ::quote).also {
            require(it.toByteArray().size <= 1024) { "远端启动命令超过 SSH 插件路由上限" }
        }
    }

    private fun validMethod(value: String): Boolean = value.length <= 128 &&
        value.matches(Regex("[a-z][A-Za-z0-9]*(?:[._-][A-Za-z0-9]+)*"))

    private fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
}
