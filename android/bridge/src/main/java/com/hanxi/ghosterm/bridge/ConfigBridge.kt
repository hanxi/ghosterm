package com.hanxi.ghosterm.bridge

/** Configuration control plane, using standard UTF-8 throughout. */
object ConfigBridge {
    init { System.loadLibrary("ghosterm_android") }
    fun hosts(configuration: ByteArray): List<List<String>> =
        hostsBytes(configuration).map { it.toString(Charsets.UTF_8) }.also {
            check(it.size % 31 == 0) { "主机配置桥返回了不完整的来源记录" }
        }.chunked(31)

    fun edit(configuration: ByteArray, alias: String, changes: Map<String, String?>, create: Boolean): ByteArray =
        editBytes(configuration, alias.toByteArray(Charsets.UTF_8),
            changes.keys.map { it.toByteArray(Charsets.UTF_8) }.toTypedArray(),
            changes.values.map { it?.toByteArray(Charsets.UTF_8) }.toTypedArray(), create)

    fun rename(configuration: ByteArray, alias: String, replacement: String): ByteArray =
        renameBytes(configuration, alias.toByteArray(Charsets.UTF_8), replacement.toByteArray(Charsets.UTF_8))

    fun connectionIssues(configuration: ByteArray, alias: String): List<ConnectionIssue> =
        connectionIssueBytes(configuration, alias.toByteArray(Charsets.UTF_8))
            .map { it.toString(Charsets.UTF_8) }.also { check(it.size % 4 == 0) }
            .chunked(4).map { ConnectionIssue(it[0], it[1], it[2].toInt(), it[3] == "1") }

    private external fun connectionIssueBytes(configuration: ByteArray, alias: ByteArray): Array<ByteArray>
    private external fun renameBytes(configuration: ByteArray, alias: ByteArray, replacement: ByteArray): ByteArray
    private external fun hostsBytes(configuration: ByteArray): Array<ByteArray>
    private external fun editBytes(configuration: ByteArray, alias: ByteArray,
        keys: Array<ByteArray>, values: Array<ByteArray?>, create: Boolean): ByteArray
}

data class ConnectionIssue(val keyword: String, val sourcePath: String, val sourceLine: Int, val included: Boolean) {
    val description: String get() = "$keyword（${sourcePath.ifEmpty { "当前配置" }}:$sourceLine）"
}
