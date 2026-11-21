package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.Process
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.system.StructStat
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.json.JSONObject
import java.io.File
import java.io.FileDescriptor
import java.util.UUID

/** One private, locked destination-filesystem transaction. Names alone never
 * authorize adoption or cleanup. Intent records have no automatic replay. */
internal class PluginReleaseStage private constructor(
    private val location: File,
    val releaseID: String,
    private val checkpoint: (String) -> Unit,
    private val parentSync: (FileDescriptor) -> Unit,
    private val rootSync: (FileDescriptor) -> Unit,
) : AutoCloseable {
    enum class Reason { ROOT, BUSY, LIMIT, CHANGED, CONFLICT, UNSUPPORTED, IO }
    class Rejected(val reason: Reason) : IllegalStateException(when (reason) {
        Reason.ROOT -> "安装根目录须为本应用的私有普通目录。"
        Reason.BUSY -> "已有插件版本正在安装或维护，请稍后再试。"
        Reason.LIMIT -> "保留的版本或暂存记录已达到安装上限。"
        Reason.CHANGED -> "安装目录或文件已变化；保留位置和归属记录供核对。"
        Reason.CONFLICT -> "目标版本槽位已存在，未覆盖原对象。"
        Reason.UNSUPPORTED -> "当前文件系统不支持不覆盖的原子目录发布。"
        Reason.IO -> "安装回执或清理未完成；结果需核对，可能保留版本、暂存和归属记录。"
    })
    data class Publication(val releaseID: String, val directory: File, val durabilityConfirmed: Boolean, val maintenanceIssue: String?)
    private data class Identity(val device: Long, val inode: Long, val owner: Int) {
        fun json() = JSONObject().put("device", device.toString()).put("inode", inode.toString()).put("owner", owner.toString())
    }
    private data class Stamp(val identity: Identity, val size: Long, val mode: Int, val links: Long,
        val modified: Long, val modifiedNS: Long, val changed: Long, val changedNS: Long)
    private val operation = UUID.randomUUID().toString()
    private val name = ".prepare-$operation"
    private val journalName = ".install-$operation.json"
    private var parent: ParcelFileDescriptor? = null
    private var directory: ParcelFileDescriptor? = null
    private var parentIdentity: Identity? = null
    private var directoryIdentity: Identity? = null
    private var persistentParent: PluginReleaseRemoval.Stamp? = null
    private var persistentDirectory: PluginReleaseRemoval.Stamp? = null
    private var record: ByteArray? = null
    private var recordStamp: Stamp? = null
    private var childCreated = false
    private var published = false
    private var closed = false
    private val files = linkedMapOf<String, Stamp>()
    private val directories = linkedMapOf<String, Identity>()
    private val parentFD get() = checkNotNull(parent)
    private val rootFD get() = checkNotNull(directory)
    private fun proc(fd: ParcelFileDescriptor, name: String = "") = "/proc/self/fd/${fd.fd}" + if (name.isEmpty()) "" else "/$name"
    private fun demand(value: Boolean, reason: Reason = Reason.CHANGED) { if (!value) throw Rejected(reason) }

    private fun start() {
        demand(location.isAbsolute && releaseID.matches(Regex("[0-9a-f]{64}")), Reason.ROOT)
        try { Os.mkdir(location.path, 448) } catch (e: ErrnoException) { if (e.errno != OsConstants.EEXIST) throw Rejected(Reason.ROOT) }
        parent = LocalPublicationFile.directory(location, "")
        val value = Os.fstat(parentFD.fileDescriptor)
        demand(privateDirectory(value), Reason.ROOT); parentIdentity = identity(value)
        val lock = PluginInstallFiles.lock(parentFD.fd)
        demand(lock == 0, if (lock == OsConstants.EAGAIN) Reason.BUSY else Reason.IO)
        validateParent()
        PluginReleaseCatalog.requireNoRemoval(parentFD.fd)
        // Sync even an existing root: a previous attempt may have created it
        // and failed before syncing its directory entry. Pin the actual parent.
        LocalPublicationFile.directory(location.parentFile ?: throw Rejected(Reason.ROOT), "").use { ancestor ->
            LocalPublicationFile.childDirectory(ancestor.fd, location.name).use { named ->
                demand(identity(Os.fstat(named.fileDescriptor)) == parentIdentity)
            }
            rootSync(ancestor.fileDescriptor)
        }
        validateParent()
        val names = File(proc(parentFD)).list() ?: throw Rejected(Reason.IO)
        demand(names.size < 64, Reason.LIMIT)
        persistentParent = PluginReleaseRemoval.stamp(parentFD)
        writeRecord(null)
        checkpoint("after-reservation")
        demand(LocalPublicationFile.mkdir(parentFD.fd, name) == 0, Reason.IO)
        childCreated = true
        directory = LocalPublicationFile.childDirectory(parentFD.fd, name)
        val selected = Os.fstat(rootFD.fileDescriptor)
        demand(privateDirectory(selected)); directoryIdentity = identity(selected)
        val owned = PluginInstallFiles.lock(rootFD.fd)
        demand(owned == 0, if (owned == OsConstants.EAGAIN) Reason.BUSY else Reason.IO)
        checkpoint("after-directory-create")
        persistentDirectory = PluginReleaseRemoval.stamp(rootFD)
        writeRecord(persistentDirectory)
        checkpoint("after-directory-record")
        validate()
    }
    private fun validateParent() {
        val selected = Os.fstat(parentFD.fileDescriptor)
        demand(privateDirectory(selected) && identity(selected) == parentIdentity)
        LocalPublicationFile.directory(location, "").use { current ->
            val named = Os.fstat(current.fileDescriptor)
            demand(privateDirectory(named) && identity(named) == parentIdentity)
        }
    }
    private fun validate() {
        demand(!closed && !published)
        validateDirectory(name)
        verifyRecord()
    }
    private fun validateDirectory(entry: String) {
        validateParent()
        val selected = Os.fstat(rootFD.fileDescriptor)
        val named = Os.lstat(proc(parentFD, entry))
        demand(privateDirectory(selected) && privateDirectory(named) && identity(selected) == directoryIdentity && identity(named) == directoryIdentity)
    }
    private fun writeRecord(selected: PluginReleaseRemoval.Stamp?) {
        validateParent()
        if (record != null) verifyRecord()
        val raw = JSONObject().put("schema", 2).put("operation", operation).put("name", name)
            .put("parent", checkNotNull(persistentParent).json()).put("directory", selected?.json() ?: JSONObject.NULL)
            .put("target", releaseID).toString().toByteArray(Charsets.UTF_8)
        val temporary = ".intent-${UUID.randomUUID()}"
        LocalPublicationFile.candidate(parentFD.fd, temporary).use { output ->
            var input: ParcelFileDescriptor? = null
            try {
                writeAll(output.fileDescriptor, raw)
                Os.fchmod(output.fileDescriptor, 256); Os.fsync(output.fileDescriptor)
                input = LocalPublicationFile.retainedCandidate(parentFD.fd, temporary)
                demand(stamp(Os.fstat(input.fileDescriptor)) == stamp(Os.fstat(output.fileDescriptor)))
                output.close()
                if (record != null) verifyRecord()
                val source = Os.lstat(proc(parentFD, temporary))
                demand(stamp(source) == stamp(Os.fstat(input.fileDescriptor)))
                if (record == null) {
                    demand(LocalPublicationFile.publish(parentFD.fd, parentFD.fd, input.fd, temporary, journalName, raw.size.toLong()) == 0, Reason.IO)
                } else {
                    Os.rename(proc(parentFD, temporary), proc(parentFD, journalName))
                }
                record = raw
                LocalPublicationFile.retainedCandidate(parentFD.fd, journalName).use { saved -> recordStamp = stamp(Os.fstat(saved.fileDescriptor)) }
                Os.fsync(parentFD.fileDescriptor)
            } finally {
                // Only this temporary's inode may be removed. Missing means it
                // was already published, never permission to adopt a replacement.
                try { demand(LocalPublicationFile.remove(parentFD.fd, (input ?: output).fd, temporary) == 0, Reason.IO) }
                finally { input?.close() }
            }
        }
    }
    private fun verifyRecord() {
        val expected = record ?: throw Rejected(Reason.CHANGED)
        LocalPublicationFile.retainedCandidate(parentFD.fd, journalName).use { input ->
            val value = Os.fstat(input.fileDescriptor)
            demand(value.st_uid == Process.myUid() && value.st_nlink == 1L && value.st_mode and 63 == 0 &&
                value.st_size == expected.size.toLong() && stamp(value) == recordStamp)
            val bytes = ByteArray(expected.size)
            var at = 0
            while (at < bytes.size) { val count = Os.read(input.fileDescriptor, bytes, at, bytes.size - at); demand(count > 0); at += count }
            demand(bytes.contentEquals(expected) && stamp(Os.fstat(input.fileDescriptor)) == recordStamp)
        }
    }
    private fun retireRecord() {
        validateParent(); verifyRecord()
        LocalPublicationFile.retainedCandidate(parentFD.fd, journalName).use {
            demand(LocalPublicationFile.remove(parentFD.fd, it.fd, journalName) == 0, Reason.IO)
        }
        record = null; recordStamp = null
        Os.fsync(parentFD.fileDescriptor)
    }
    private fun openParent(path: String, create: Boolean): Pair<ParcelFileDescriptor, String> {
        val parts = path.split('/')
        demand(parts.size in 1..34 && parts.all { it.isNotEmpty() && it != "." && it != ".." && '\u0000' !in it && '\\' !in it && ':' !in it })
        var current = ParcelFileDescriptor.dup(rootFD.fileDescriptor)
        try {
            var prefix = ""
            for (part in parts.dropLast(1)) {
                prefix += if (prefix.isEmpty()) part else "/$part"
                if (prefix !in directories) {
                    demand(create && directories.size < 4096, Reason.LIMIT)
                    demand(LocalPublicationFile.mkdir(current.fd, part) == 0, Reason.IO)
                    LocalPublicationFile.childDirectory(current.fd, part).use { child ->
                        val value = Os.fstat(child.fileDescriptor); demand(privateDirectory(value))
                        directories[prefix] = identity(value)
                    }
                }
                val next = LocalPublicationFile.childDirectory(current.fd, part)
                try { demand(privateDirectory(Os.fstat(next.fileDescriptor)) && identity(Os.fstat(next.fileDescriptor)) == directories[prefix]) }
                catch (e: Throwable) { next.close(); throw e }
                current.close(); current = next
            }
            return current to parts.last()
        } catch (e: Throwable) { current.close(); throw e }
    }
    fun write(path: String, bytes: ByteArray, executable: Boolean = false) {
        validate()
        demand(path !in files && files.size < 1027, Reason.LIMIT)
        val (selected, leaf) = openParent(path, create = true)
        selected.use { parent -> LocalPublicationFile.candidate(parent.fd, leaf).use { output ->
            files[path] = stamp(Os.fstat(output.fileDescriptor))
            try {
                checkpoint("after-file-create")
                writeAll(output.fileDescriptor, bytes)
                Os.fchmod(output.fileDescriptor, if (executable) 320 else 256)
                Os.fsync(output.fileDescriptor)
            } finally { files[path] = stamp(Os.fstat(output.fileDescriptor)) }
        } }
        checkpoint("after-file-write")
    }
    fun payload(): ParcelFileDescriptor {
        validate()
        return LocalPublicationFile.childDirectory(rootFD.fd, "payload").also {
            if (identity(Os.fstat(it.fileDescriptor)) != directories["payload"]) { it.close(); throw Rejected(Reason.CHANGED) }
        }
    }
    private fun tree(freeze: Boolean, remove: Boolean = false, frozen: Boolean = false) {
        var visited = 0; var fileCount = 0; val seen = mutableSetOf<String>()
        fun walk(current: ParcelFileDescriptor, prefix: String, depth: Int) {
            demand(depth <= 34)
            if (frozen) demand(Os.fstat(current.fileDescriptor).st_mode and 146 == 0)
            val names = File(proc(current)).list() ?: throw Rejected(Reason.IO)
            if (remove) Os.fchmod(current.fileDescriptor, 448)
            for (name in names) {
                demand(++visited <= 8192 && name.isNotEmpty() && name != "." && name != ".." && '/' !in name)
                val path = if (prefix.isEmpty()) name else "$prefix/$name"
                val value = Os.lstat(proc(current, name))
                if (OsConstants.S_ISDIR(value.st_mode)) {
                    demand(privateDirectory(value) && identity(value) == directories[path]); seen += path
                    LocalPublicationFile.childDirectory(current.fd, name).use { child ->
                        demand(identity(Os.fstat(child.fileDescriptor)) == directories[path]); walk(child, path, depth + 1)
                        if (remove) demand(LocalPublicationFile.rmdir(current.fd, child.fd, name) == 0, Reason.IO)
                    }
                } else {
                    demand(OsConstants.S_ISREG(value.st_mode) && value.st_nlink == 1L && value.st_uid == Process.myUid() && files[path] == stamp(value))
                    if (freeze || frozen) demand(value.st_mode and 146 == 0)
                    fileCount++
                    if (remove) LocalPublicationFile.retainedCandidate(current.fd, name).use { input ->
                        demand(stamp(Os.fstat(input.fileDescriptor)) == files[path])
                        demand(LocalPublicationFile.remove(current.fd, input.fd, name) == 0, Reason.IO)
                    }
                }
            }
            if (freeze) { Os.fchmod(current.fileDescriptor, 320); Os.fsync(current.fileDescriptor) }
        }
        walk(rootFD, "", 0)
        demand(fileCount == files.size && seen == directories.keys)
    }
    fun publish(authorizePublication: (() -> Int) -> Int = { it() }): Publication {
        validate(); tree(freeze = true)
        checkpoint("before-rename")
        val error = authorizePublication {
            validate()
            tree(freeze = false, frozen = true)
            PluginInstallFiles.publish(parentFD.fd, rootFD.fd, name, releaseID)
        }
        if (error != 0) throw Rejected(when (error) {
            OsConstants.EEXIST, OsConstants.ENOTEMPTY -> Reason.CONFLICT
            OsConstants.ENOSYS, OsConstants.EOPNOTSUPP, OsConstants.EXDEV -> Reason.UNSUPPORTED
            OsConstants.EINVAL -> Reason.CHANGED
            else -> Reason.IO
        })
        published = true
        val durable = runCatching {
            checkpoint("after-rename")
            parentSync(parentFD.fileDescriptor)
            checkpoint("after-publication-sync")
            validateDirectory(releaseID)
        }.isSuccess
        var issue = if (durable) null else "版本已发布；持久性未确认，安装归属记录保留待核对。"
        if (durable) try { retireRecord() } catch (_: Exception) { issue = "版本已发布；归属记录清理未确认。" }
        return Publication(releaseID, File(location, releaseID), durable, issue)
    }
    override fun close() {
        if (closed) return
        try {
            if (!published && directoryIdentity != null) {
                validate(); tree(freeze = false); tree(freeze = false, remove = true)
                validate(); demand(LocalPublicationFile.rmdir(parentFD.fd, rootFD.fd, name) == 0, Reason.IO)
                Os.fsync(parentFD.fileDescriptor); retireRecord()
            } else if (!published && !childCreated && record != null) {
                validateParent(); retireRecord()
            }
        } finally { closed = true; directory?.close(); directory = null; parent?.close(); parent = null }
    }
    private fun writeAll(fd: FileDescriptor, bytes: ByteArray) {
        var at = 0
        while (at < bytes.size) {
            checkpoint("file-chunk")
            val count = try { Os.write(fd, bytes, at, minOf(16384, bytes.size - at)) }
                catch (e: ErrnoException) { if (e.errno == OsConstants.EINTR) continue else throw e }
            if (count <= 0) throw Rejected(Reason.IO)
            at += count
        }
    }
    companion object {
        fun create(location: File, releaseID: String, checkpoint: (String) -> Unit = {}, parentSync: (FileDescriptor) -> Unit = { Os.fsync(it) },
            rootSync: (FileDescriptor) -> Unit = { Os.fsync(it) }): PluginReleaseStage {
            val stage = PluginReleaseStage(location, releaseID, checkpoint, parentSync, rootSync)
            try { stage.start(); return stage }
            catch (failure: Throwable) { try { stage.close() } catch (cleanup: Throwable) { failure.addSuppressed(cleanup) }; throw failure }
        }
        private fun identity(value: StructStat) = Identity(value.st_dev, value.st_ino, value.st_uid)
        private fun stamp(v: StructStat) = Stamp(identity(v), v.st_size, v.st_mode, v.st_nlink, v.st_mtim.tv_sec, v.st_mtim.tv_nsec, v.st_ctim.tv_sec, v.st_ctim.tv_nsec)
        private fun privateDirectory(value: StructStat) = OsConstants.S_ISDIR(value.st_mode) && value.st_uid == Process.myUid() && value.st_mode and 63 == 0
    }
}
