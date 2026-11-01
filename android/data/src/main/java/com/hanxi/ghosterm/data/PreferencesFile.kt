package com.hanxi.ghosterm.data

import android.system.Os
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.FileAlreadyExistsException
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.NoSuchFileException
import java.nio.file.Path
import java.nio.file.StandardOpenOption.*
import java.nio.file.attribute.BasicFileAttributes
import java.util.UUID

/** A private, bounded document. Missing storage is distinct from denied or corrupt storage. */
class PreferencesFile(private val directory: File, private val ops: ConfigFileOps = ConfigFileOps()) : PreferencesStorage {
    private val target = directory.toPath().resolve("preferences.json")
    companion object { private val processLock = Any() }

    private fun attributes(path: Path): BasicFileAttributes? = try {
        Files.readAttributes(path, BasicFileAttributes::class.java, NOFOLLOW_LINKS)
    } catch (_: NoSuchFileException) { null }

    private fun checkDirectory(create: Boolean): Boolean {
        var attrs = attributes(directory.toPath())
        if (attrs == null) {
            if (!create) return false
            try { Files.createDirectory(directory.toPath()) } catch (_: FileAlreadyExistsException) { /* Recheck the winner. */ }
            attrs = checkNotNull(attributes(directory.toPath()))
        }
        check(attrs.isDirectory && !attrs.isSymbolicLink) { "偏好目录类型无效" }
        return true
    }

    private fun raw(): ByteArray? {
        if (!checkDirectory(false)) return null
        val attrs = attributes(target) ?: return null
        check(attrs.isRegularFile && !attrs.isSymbolicLink && attrs.size() in 1..PreferencesCodec.LIMIT.toLong()) {
            "偏好文件类型或大小无效"
        }
        return FileChannel.open(target, READ, NOFOLLOW_LINKS).use { channel ->
            val output = ByteArrayOutputStream()
            val buffer = ByteBuffer.allocate(1024)
            while (true) {
                val count = channel.read(buffer)
                if (count < 0) break
                check(output.size() + count <= PreferencesCodec.LIMIT) { "偏好文件过大" }
                output.write(buffer.array(), 0, count); buffer.clear()
            }
            output.toByteArray()
        }
    }

    private fun same(first: ByteArray?, second: ByteArray?): Boolean =
        if (first == null || second == null) first == null && second == null else first.contentEquals(second)

    private fun <T> locked(block: () -> T): T {
        checkDirectory(true)
        Os.chmod(directory.path, 0x1c0) // 0700
        return FileChannel.open(directory.toPath().resolve(".preferences.lock"), CREATE, WRITE, NOFOLLOW_LINKS).use { channel ->
            Os.chmod(directory.toPath().resolve(".preferences.lock").toString(), 0x180)
            channel.lock().use { block() }
        }
    }

    override fun read(): ByteArray? = synchronized(processLock) {
        // First use does not create an empty document or directory.
        if (!checkDirectory(false)) null else locked { raw() }
    }

    override fun write(expected: ByteArray?, bytes: ByteArray): PreferenceWrite = synchronized(processLock) {
        try {
            // Recheck the encoded candidate before creating any temporary file.
            check(PreferencesCodec.decode(bytes).issues.isEmpty())
            locked {
                if (!same(raw(), expected)) return@locked PreferenceWrite.Unknown
                val candidate = directory.toPath().resolve(".preferences-${UUID.randomUUID()}.tmp")
                var owned = false
                try {
                    ops.writeExclusive(candidate, bytes)
                    owned = true
                    // A non-cooperating external editor must not be overwritten.
                    if (!same(raw(), expected)) return@locked PreferenceWrite.Unknown
                    ops.replace(candidate, target)
                    ops.syncDirectory(directory.toPath())
                    check(same(raw(), bytes)) { "偏好发布后的内容无法确认" }
                    PreferenceWrite.Saved
                } finally {
                    if (owned) Files.deleteIfExists(candidate)
                }
            }
        } catch (_: Exception) {
            // Rename or directory sync may have succeeded before reporting an
            // error. Only verified original bytes justify an unchanged result.
            try { if (same(raw(), expected)) PreferenceWrite.Unchanged else PreferenceWrite.Unknown }
            catch (_: Exception) { PreferenceWrite.Unknown }
        }
    }
}
