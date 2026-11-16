package com.hanxi.ghosterm.bridge

/** Platform-only directory lock/publication. Borrowed fds are never consumed.
 * Caller owns content verification, directory synchronization and intent records. */
object PluginInstallFiles {
    init { System.loadLibrary("ghosterm_android") }
    fun lock(directory: Int): Int = lockDirectory(directory)
    /** Shared reference on a separately opened version directory. Its last
     * owned fd releases the reference; this method never consumes a borrowed fd. */
    fun pin(directory: Int): Int = pinDirectory(directory)
    /** errno, kind (1 birth time / 2 inode generation), identity value and ns.
     * Android 10 cannot call statx under its application seccomp policy.
     * Missing kernel/filesystem support is explicit; no identity is invented. */
    fun identity(descriptor: Int): LongArray = identityMetadata(descriptor)
    private external fun identityMetadata(descriptor: Int): LongArray
    fun publish(parent: Int, directory: Int, source: String, destination: String): Int {
        require(source.matches(Regex("\\.prepare-[0-9a-f-]{36}")) && destination.matches(Regex("[0-9a-f]{64}")))
        return publishDirectory(parent, directory, source.toByteArray(Charsets.UTF_8), destination.toByteArray(Charsets.UTF_8))
    }
    private external fun lockDirectory(directory: Int): Int
    private external fun pinDirectory(directory: Int): Int
    /** One no-replace rename of the exact pinned version; no overwrite fallback. */
    fun detach(parent: Int, directory: Int, releaseID: String, tombstone: String): Int {
        require(releaseID.matches(Regex("[0-9a-f]{64}")) && tombstone.matches(Regex("\\.removing-[0-9a-f-]{36}")))
        return detachDirectory(parent, directory, releaseID.toByteArray(Charsets.UTF_8), tombstone.toByteArray(Charsets.UTF_8))
    }
    private external fun detachDirectory(parent: Int, directory: Int, source: ByteArray, destination: ByteArray): Int
    private external fun publishDirectory(parent: Int, directory: Int, source: ByteArray, destination: ByteArray): Int
}
