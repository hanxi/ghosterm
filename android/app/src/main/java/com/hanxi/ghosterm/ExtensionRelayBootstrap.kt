package com.hanxi.ghosterm

import android.util.JsonReader
import android.util.JsonToken
import java.io.StringReader
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

/** Builds the explicit remote command accepted by the SSH extension route.
 * Values are argv tokens quoted for the remote POSIX shell; this does not
 * discover or install gterm. */
object ExtensionRelayBootstrap {
    data class Attachment(val directory: String, val instance: String, val authorized: Boolean, val remoteProcesses: Boolean = false, val scopedFiles: Boolean = false) {
        val reviewDescription: String get() = "连接已有前台实例：$instance\n远端私有目录：$directory\n运行档、根目录与程序映射沿用前台启动配置，连接不会修改。\n已有只读文件范围：${if (scopedFiles) "明确允许沿用" else "未允许"}\n远端业务进程服务：${if (remoteProcesses) "明确允许沿用实例范围" else "未允许"}"
    }
    data class ProcessProgram(
        val name: String,
        val executable: String,
        val version: String,
        val sha256: String,
        val prefixArguments: List<String> = emptyList(),
    )

    data class NodeDependency(val executable: String, val version: String, val sha256: String,
        val sdkDirectory: String, val sdkCatalogSHA256: String)

    data class Execution(val runtime: Runtime, val temporaryDirectory: String, val authorized: Boolean) {
        sealed interface Runtime {
            data class Node(val dependency: NodeDependency) : Runtime
            data object Exec : Runtime
        }
        val reviewDescription: String get() {
            val detail = when (val selected = runtime) {
                Runtime.Exec -> "Exec（使用已检查包内的原生入口）"
                is Runtime.Node -> with(selected.dependency) {
                    "Node $version · $executable\n可执行 SHA-256：$sha256\nHost SDK：$sdkDirectory\nSDK catalog SHA-256：$sdkCatalogSHA256"
                }
            }
            return detail + "\n远端临时目录：$temporaryDirectory\n进程执行：" + if (authorized) "本次已明确允许" else "尚未允许"
        }
    }

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
        val processEnvironmentNames: List<String> = emptyList(),
        val execution: Execution? = null,
        val attachment: Attachment? = null,
        val clientCapabilities: List<String> = emptyList(),
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
                        var prefixArguments = emptyList<String>()
                        val seen = mutableSetOf<String>()
                        while (reader.hasNext()) {
                            val key = reader.nextName()
                            require(seen.add(key) && key in setOf("name", "executable", "version", "sha256", "prefixArguments")) {
                                "进程映射对象字段重复或未知"
                            }
                            if (key == "prefixArguments") {
                                prefixArguments = stringArray(reader, 16, "固定进程参数")
                                continue
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
                        require(seen - "prefixArguments" == setOf("name", "executable", "version", "sha256")) {
                            "进程映射须包含 name、executable、version、sha256，可另提供 prefixArguments"
                        }
                        add(ProcessProgram(checkNotNull(name), checkNotNull(executable),
                            checkNotNull(version), checkNotNull(sha256), prefixArguments.toList()))
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

    fun parseProcessEnvironmentJson(source: String): List<String> =
        parseStringArray(source, 27, "执行端环境变量名称").also(::validateEnvironmentNames)

    private fun validateEnvironmentNames(names: List<String>) {
        val defaults = setOf("HOME", "PATH", "XDG_CONFIG_HOME", "CODEX_HOME", "TMPDIR")
        require(names.size <= 27 && names.distinct().size == names.size && names.all {
            it.length <= 128 && it.matches(Regex("[A-Za-z_][A-Za-z0-9_]*")) && it !in defaults
        }) { "执行端环境变量名称非法、重复或与默认候选重复" }
    }

    private fun stringArray(reader: JsonReader, limit: Int, label: String): List<String> {
        require(reader.peek() == JsonToken.BEGIN_ARRAY) { "$label 必须是 JSON 字符串数组" }
        reader.beginArray()
        val result = buildList {
            while (reader.hasNext()) {
                require(size < limit && reader.peek() == JsonToken.STRING) { "$label 类型错误或超过 $limit 项" }
                add(reader.nextString())
            }
        }
        reader.endArray()
        return result
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
        val argv = arguments(request)
        val environment = request.execution?.let { "TMPDIR=" + quote(it.temporaryDirectory) + " " }.orEmpty()
        return (environment + "exec " + argv.joinToString(" ", transform = ::quote)).also {
            require(it.toByteArray().size <= 1024) { "远端启动命令超过 SSH 插件路由上限" }
        }
    }

    fun buildMultiplex(connection: String, scopes: List<ExtensionController.MultiplexScope>, requests: List<Request>, temporaryDirectory: String, live: Boolean = false): String {
        fun identity(value: String) = value.matches(Regex("[a-f0-9]{32}")) && value.any { it != '0' }
        require(identity(connection) && requests.size in 1..8 && scopes.size == requests.size &&
            scopes.map { it.instance }.distinct().size == scopes.size && scopes.map { it.epoch }.distinct().size == scopes.size &&
            scopes.all { identity(it.instance) && it.epoch != 0uL } &&
            requests.all { it.executable == requests.first().executable && it.attachment == null } && absolutePath(temporaryDirectory) &&
            requests.all { it.execution == null || it.execution.temporaryDirectory == temporaryDirectory }) {
            "启动组需要同一 gterm、明确临时目录与一至八个独立实例，不能包含已有前台附着"
        }
        val argv = buildList {
            addAll(listOf(requests.first().executable, "relay", "--multiplex"))
            if (live) add("--live")
            addAll(listOf("--connection", connection))
            scopes.zip(requests).forEach { (scope, request) ->
                val hex = multiplexProfile(request)
                addAll(listOf("--instance", scope.instance, scope.epoch.toString(), hex))
            }
        }
        require(argv.size <= 64 && argv.sumOf { it.toByteArray().size + 1 } <= 16384) { "连接启动配置超过 Native 参数预算" }
        return ("TMPDIR=" + quote(temporaryDirectory) + " exec " + argv.joinToString(" ", transform = ::quote)).also {
            require(it.toByteArray().size <= 32768) { "连接启动命令超过 32 KiB" }
        }
    }

    fun multiplexProfile(request: Request): String {
        val hexDigits = "0123456789abcdef"
        val profile = arguments(request).drop(3)
        require(profile.size in 1..256)
        val encoded = java.io.ByteArrayOutputStream()
        fun u16(value: Int) { encoded.write(value ushr 8); encoded.write(value and 255) }
        encoded.write(1); u16(profile.size)
        profile.forEach { value ->
            val bytes = value.toByteArray(Charsets.UTF_8); require(bytes.size <= 65535)
            u16(bytes.size); encoded.write(bytes)
        }
        val hex = buildString { encoded.toByteArray().forEach { byte ->
            val value = byte.toInt() and 255; append(hexDigits[value ushr 4]); append(hexDigits[value and 15])
        } }
        require(hex.length <= 4096) { "单个实例启动配置超过 Native 参数预算" }
        return hex
    }

    private fun arguments(request: Request): List<String> {
        require(request.clientCapabilities.size <= 4 && request.clientCapabilities.distinct().size == request.clientCapabilities.size &&
            request.clientCapabilities.all { it in listOf("ui.table", "ui.diff", "ui.task", "ui.views") }) {
            "此 Host 仅支持显式声明的标准 Client 服务"
        }
        require(request.executable.isNotBlank()) { "请指定远端 gterm 可执行文件" }
        require(request.attachment != null || request.executorDirectory.isNotBlank()) { "请指定远端 Executor 包目录" }
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
        validateEnvironmentNames(request.processEnvironmentNames)
        require(request.processEnvironmentNames.isEmpty() || request.processPrograms.isNotEmpty() || request.execution != null) {
            "选择执行端环境名称需要固定进程映射或 Node / Exec 运行档"
        }
        request.attachment?.let { attachment ->
            require(attachment.authorized && attachment.instance.matches(Regex("[a-f0-9]{32}")) &&
                canonicalDirectory(attachment.directory) && request.execution == null && request.executorDirectory.isEmpty() &&
                request.workspaceRootDirectory == null && request.processPrograms.isEmpty() && request.processEnvironmentNames.isEmpty()) {
                "已有前台实例需要明确允许、规范绝对目录与 32 位小写实例标识；不能同时重配运行档、包目录、根或进程"
            }
            val argv = listOf(request.executable, "relay", "--attach", attachment.directory, "--instance", attachment.instance, "--", request.command) + request.arguments
            require(argv.all { validArgument(it, 4096) }) { "连接参数包含不支持的字符或过长" }
            return argv
        }
        val argv = buildList {
            add(request.executable)
            add("relay"); add("--stdio")
            add("--trust-development-digest"); add(request.executorDigest)
            add("--client-digest"); add(request.clientDigest)
            request.execution?.let { execution ->
                require(execution.authorized) { "Node / Exec 需要明确允许本次进程执行" }
                require(absolutePath(execution.temporaryDirectory)) { "请指定远端已存在的绝对临时目录" }
                when (val selected = execution.runtime) {
                    Execution.Runtime.Exec -> { add("--runtime"); add("exec"); add("--allow-process-execution") }
                    is Execution.Runtime.Node -> with(selected.dependency) {
                        require(absolutePath(executable) && absolutePath(sdkDirectory) &&
                            version.length in 1..128 && version.matches(Regex("[0-9][0-9A-Za-z.+-]*")) &&
                            sha256.matches(Regex("[a-f0-9]{64}")) && sdkCatalogSHA256.matches(Regex("[a-f0-9]{64}"))) {
                            "Node 需要绝对路径、无 v 前缀的版本、可执行摘要和 Host SDK catalog 摘要"
                        }
                        add("--runtime"); add("node"); add("--allow-process-execution")
                        add("--node-runtime"); add(executable); add(version); add(sha256)
                        add("--node-sdk"); add(sdkDirectory); add(sdkCatalogSHA256)
                    }
                }
            }
            request.methods.forEach { add("--method"); add(it) }
            request.clientCapabilities.forEach { add("--client-capability"); add(it) }
            request.hostMethods.forEach { add("--host-method"); add(it) }
            request.workspaceRootDirectory?.let { add("--root"); add("workspace"); add(it) }
            request.processPrograms.forEach {
                add("--process-program"); add(it.name); add(it.executable); add(it.version); add(it.sha256)
                it.prefixArguments.forEach { argument -> add("--process-argument"); add(it.name); add(argument) }
            }
            request.processEnvironmentNames.forEach { add("--process-environment"); add(it) }
            add("--"); add(request.executorDirectory); add(request.command)
            addAll(request.arguments)
        }
        require(argv.all { validArgument(it, 4096) }) {
            "远端启动参数包含不支持的字符或过长"
        }
        return argv
    }

    private fun absolutePath(value: String): Boolean = value.startsWith('/') && validArgument(value, 4096)
    private fun canonicalDirectory(value: String): Boolean = absolutePath(value) &&
        value.drop(1).split('/').all { it.isNotEmpty() && it != "." && it != ".." }

    private fun validMethod(value: String): Boolean = value.length <= 128 &&
        value.matches(Regex("[a-z][A-Za-z0-9]*(?:[._-][A-Za-z0-9]+)*"))

    private fun validProcessProgram(program: ProcessProgram): Boolean =
        program.prefixArguments.size <= 16 && program.prefixArguments.all { validArgument(it, 4096) } &&
        program.prefixArguments.sumOf { it.toByteArray().size + 1 } <= 16384 &&
        program.name.length in 1..128 && program.name.all {
            it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9' || it == '.' || it == '_' || it == '-'
        } && program.executable.startsWith('/') && validArgument(program.executable, 4096) &&
        program.version.isNotBlank() && validArgument(program.version, 128) &&
        '\u0000' !in program.name && '\n' !in program.name && '\r' !in program.name &&
        '\u0000' !in program.executable && '\n' !in program.executable && '\r' !in program.executable &&
        '\u0000' !in program.version && '\n' !in program.version && '\r' !in program.version &&
        program.sha256.matches(Regex("[a-f0-9]{64}"))

    private fun validArgument(value: String, limit: Int): Boolean =
        '\u0000' !in value && '\n' !in value && '\r' !in value && runCatching {
            Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value)).remaining() <= limit
        }.getOrDefault(false)

    private fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
}
