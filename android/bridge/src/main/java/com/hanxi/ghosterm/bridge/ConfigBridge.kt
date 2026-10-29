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

    private external fun renameBytes(configuration: ByteArray, alias: ByteArray, replacement: ByteArray): ByteArray
    private external fun hostsBytes(configuration: ByteArray): Array<ByteArray>
    private external fun editBytes(configuration: ByteArray, alias: ByteArray,
        keys: Array<ByteArray>, values: Array<ByteArray?>, create: Boolean): ByteArray
}
