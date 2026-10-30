package com.hanxi.ghosterm.bridge

import android.os.ParcelFileDescriptor
import java.io.File
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction

/** Android filesystem adapter. All calls belong on a task IO worker.
 * Directory descriptors pin the selected inode; they do not follow later path changes.
 * Named private candidates are separate from anonymous TransferStagingStore snapshots.
 */
object LocalPublicationFile {
    init { System.loadLibrary("ghosterm_android") }
    private fun bytes(value: String): ByteArray {
        require('\u0000' !in value) { "文件路径包含 NUL" }
        val encoded=Charsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).encode(CharBuffer.wrap(value))
        return ByteArray(encoded.remaining()).also { encoded.get(it) }
    }
    fun directory(root: File, relative: String): ParcelFileDescriptor =
        ParcelFileDescriptor.adoptFd(openDirectory(bytes(root.absolutePath),bytes(relative)))
    fun candidate(directory: Int, name: String): ParcelFileDescriptor =
        ParcelFileDescriptor.adoptFd(openCandidate(directory,bytes(name)))
    /** errno (zero on success); no overwrite fallback, retry, or borrowed-fd close.
     * Requires a complete read-only candidate with its writer already closed. */
    fun publish(destination: Int, staging: Int, candidate: Int, source: String, name: String, size: Long): Int =
        publishCandidate(destination,staging,candidate,bytes(source),bytes(name),size)
    fun remove(staging: Int, candidate: Int, source: String): Int = removeCandidate(staging,candidate,bytes(source))
    private external fun openDirectory(root: ByteArray, relative: ByteArray): Int
    private external fun openCandidate(directory: Int, name: ByteArray): Int
    private external fun publishCandidate(destination: Int, staging: Int, candidate: Int, source: ByteArray, name: ByteArray, size: Long): Int
    private external fun removeCandidate(staging: Int, candidate: Int, name: ByteArray): Int
}
