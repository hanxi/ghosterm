package com.hanxi.ghosterm

/** Builds the explicit remote command accepted by the SSH extension route.
 * Values are argv tokens quoted for the remote POSIX shell; this does not
 * discover or install gterm. */
internal object ExtensionRelayBootstrap {
    data class Request(
        val executable: String,
        val executorDirectory: String,
        val executorDigest: String,
        val clientDigest: String,
        val methods: List<String>,
        val command: String,
        val arguments: List<String> = emptyList(),
    )

    fun build(request: Request): String {
        require(request.executable.isNotBlank()) { "请指定远端 gterm 可执行文件" }
        require(request.executorDirectory.isNotBlank()) { "请指定远端 Executor 包目录" }
        require(request.executorDigest.matches(Regex("[a-f0-9]{64}"))) { "Executor 摘要必须是 64 位小写 SHA-256" }
        require(request.clientDigest.matches(Regex("[a-f0-9]{64}"))) { "Client 摘要必须是 64 位小写 SHA-256" }
        require(request.command.isNotBlank()) { "请指定完整插件命令" }
        require(request.methods.size <= 16 && request.methods.distinct().size == request.methods.size &&
            request.methods.all { it.matches(Regex("[A-Za-z][A-Za-z0-9_.-]{0,127}")) }) {
            "RPC 方法名无效（最多 16 个）"
        }
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

    private fun quote(value: String): String = "'" + value.replace("'", "'\\''") + "'"
}
