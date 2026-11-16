package com.hanxi.ghosterm.bridge

/** Platform-only directory lock/publication. Borrowed fds are never consumed.
 * Caller owns content verification, directory synchronization and intent records. */
object PluginInstallFiles {
    init { System.loadLibrary("ghosterm_android") }
    fun lock(directory: Int): Int = lockDirectory(directory)
    fun publish(parent: Int, directory: Int, source: String, destination: String): Int {
        require(source.matches(Regex("\\.prepare-[0-9a-f-]{36}")) && destination.matches(Regex("[0-9a-f]{64}")))
        return publishDirectory(parent, directory, source.toByteArray(Charsets.UTF_8), destination.toByteArray(Charsets.UTF_8))
    }
    private external fun lockDirectory(directory: Int): Int
    private external fun publishDirectory(parent: Int, directory: Int, source: ByteArray, destination: ByteArray): Int
}
