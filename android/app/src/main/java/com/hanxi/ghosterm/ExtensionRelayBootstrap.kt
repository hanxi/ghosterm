package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import java.io.StringReader

/** Builds the explicit remote command accepted by the SSH extension route.
 * Values are argv tokens quoted for the remote POSIX shell; this does not
 * discover or install gterm. */
object ExtensionRelayBootstrap {
    data class ProcessProgram(
        val name: String,
        val executable: String,
        val version: String,
        val sha256: String,
    )

    data class Request(
        val executable: String,
        val executorDirectory: String,
        val executorDigest: String,
        val clientDigest: String,
        val methods: List<String>,
        val hostMethods: List<String> = emptyList(),
        val command: String,
        val arguments: List<String> = emptyList(),
        val workspaceRootDirectory: String? = null,
        val processPrograms: List<ProcessProgram> = emptyList(),
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

    fun parseHostMethodsJson(source: String): List<String> {
        val methods = parseStringArray(source, 16, "Host RPC 方法")
        require(methods.distinct().size == methods.size && methods.all(::validMethod)) {
            "Host RPC 方法名无效或重复"
        }
        return methods
    }

    fun parseProcessProgramsJson(source: String): List<ProcessProgram> {
        require(source.toByteArray().size <= 65536) { "进程映射 JSON 超过 64 KiB" }
        return try {
            JsonReader(StringReader(source)).use { reader ->
                reader.isLenient = false
                require(reader.peek() == JsonToken.BEGIN_ARRAY) { "进程映射必须是 JSON 对象数组" }
                reader.beginArray()
                val result = buildList {
                    while (reader.hasNext()) {
                        require(size < 16) { "进程映射最多 16 项" }
                        reader.beginObject()
                        var name: String? = null
                        var executable: String? = null
                        var version: String? = null
                        var sha256: String? = null
                        val seen = mutableSetOf<String>()
                        while (reader.hasNext()) {
                            val key = reader.nextName()
                            require(seen.add(key) && key in setOf("name", "executable", "version", "sha256")) {
                                "进程映射对象字段重复或未知"
                            }
                            require(reader.peek() == JsonToken.STRING) { "进程映射字段必须是字符串" }
                            when (key) {
                                "name" -> name = reader.nextString()
                                "executable" -> executable = reader.nextString()
                                "version" -> version = reader.nextString()
                                "sha256" -> sha256 = reader.nextString()
                            }
                        }
                        reader.endObject()
                        require(seen == setOf("name", "executable", "version", "sha256")) {
                            "进程映射必须且只能包含 name、executable、version、sha256"
                        }
                        add(ProcessProgram(checkNotNull(name), checkNotNull(executable),
                            checkNotNull(version), checkNotNull(sha256)))
                    }
                }
                reader.endArray()
                require(reader.peek() == JsonToken.END_DOCUMENT) { "进程映射 JSON 后不能有额外内容" }
                require(result.distinctBy { it.name }.size == result.size && result.all(::validProcessProgram)) {
                    "进程映射字段无效、重复或超过限制"
                }
                result
            }
        } catch (failure: IllegalArgumentException) {
            throw failure
        } catch (failure: Exception) {
            throw IllegalArgumentException("进程映射必须是有效的严格 JSON 对象数组", failure)
        }
    }

    private fun parseStringArray(source: String, limit: Int, label: String): List<String> {
        require(source.toByteArray().size <= 65536) { "$label JSON 超过 64 KiB" }
        return try {
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
        require(request.workspaceRootDirectory == null || request.workspaceRootDirectory.isNotBlank()) {
            "资源工作区目录不能为空；留空表示不授权目录"
        }
        require(request.methods.size <= 16 && request.methods.distinct().size == request.methods.size &&
            request.methods.all(::validMethod)) {
            "RPC 方法名无效（最多 16 个）"
        }
        require(request.methods.size + request.hostMethods.size <= 16 && request.hostMethods.distinct().size == request.hostMethods.size &&
            request.hostMethods.all(::validMethod) && request.hostMethods.none { it in request.methods }) {
            "Host RPC 方法名无效、重复或与 Executor 方法冲突（最多 16 个）"
        }
        require(request.arguments.size <= 32) { "命令参数最多 32 项" }
        require(request.processPrograms.size <= 16 && request.processPrograms.distinctBy { it.name }.size == request.processPrograms.size &&
            request.processPrograms.all(::validProcessProgram) &&
            (request.processPrograms.isEmpty() || request.workspaceRootDirectory != null)) {
            "进程映射必须唯一有效，并且需要显式 workspace root"
        }
        val argv = buildList {
            add(request.executable)
            add("relay"); add("--stdio")
            add("--trust-development-digest"); add(request.executorDigest)
            add("--client-digest"); add(request.clientDigest)
            request.methods.forEach { add("--method"); add(it) }
            request.hostMethods.forEach { add("--host-method"); add(it) }
            request.workspaceRootDirectory?.let { add("--root"); add("workspace"); add(it) }
            request.processPrograms.forEach {
                add("--process-program"); add(it.name); add(it.executable); add(it.version); add(it.sha256)
            }
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

    private fun validProcessProgram(program: ProcessProgram): Boolean =
        program.name.length in 1..128 && program.name.all {
            it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it == '.' || it == '_' || it == '-'
        } && program.executable.startsWith('/') && program.executable.toByteArray().size <= 4096 &&
        program.version.isNotBlank() && program.version.toByteArray().size <= 128 &&
        '\u0000' !in program.name && '\n' !in program.name && '\r' !in program.name &&
        '\u0000' !in program.executable && '\n' !in program.executable && '\r' !in program.executable &&
        '\u0000' !in program.version && '\n' !in program.version && '\r' !in program.version &&
        program.sha256.matches(Regex("[a-f0-9]{64}"))

    private fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
}
