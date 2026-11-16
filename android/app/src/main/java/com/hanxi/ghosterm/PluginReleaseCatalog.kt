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
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileDescriptor
import java.security.MessageDigest
import java.text.Normalizer
import java.util.UUID

/** Short-lived root owner shared by version observation and selection storage.
 * Observed declarations are unverified. No source grant, loading or replay. */
internal class PluginReleaseCatalog private constructor(
    private val location: File,
    private val rootSync: (FileDescriptor) -> Unit,
) : AutoCloseable {
    enum class Reason { ROOT, BUSY, CHANGED, INVALID, CONFLICT, LIMIT, IO, PENDING }
    class Rejected(val reason: Reason) : IllegalStateException(when (reason) {
        Reason.ROOT -> "无法读取本应用的私有安装根目录。"
        Reason.BUSY -> "安装目录正由另一项操作使用，请稍后刷新。"
        Reason.CHANGED -> "安装目录或文件已变化，请刷新后重新核对。"
        Reason.INVALID -> "版本或活动选择记录无效，当前状态查不出来。"
        Reason.CONFLICT -> "活动选择已变化，请刷新后重新确认。"
        Reason.LIMIT -> "版本列表或选择记录超过读取上限。"
        Reason.IO -> "版本选择回执未完成，结果需重新读取核对。"
        Reason.PENDING -> "已有待处理的删除意图，请先明确继续或撤销。"
    })
    data class Identity(val device: Long, val inode: Long, val owner: Int)
    private data class Stamp(val identity: Identity, val size: Long, val mode: Int, val links: Long,
        val modified: Long, val modifiedNS: Long, val changed: Long, val changedNS: Long)
    data class Choice(val releaseID: String, val enabled: Boolean, val generation: String)
    data class Selection(val revision: String?, val choices: Map<String, Choice>)
    data class DeclaredVersion(val releaseID: String, val pluginID: String, val version: String, val publisher: String, val packageSHA256: String)
    data class Observation(val versions: List<DeclaredVersion>, val issues: List<String>, val maintenanceCount: Int,
        val selection: Selection?, val selectionIssue: String?)
    data class Publication(val selection: Selection, val durabilityConfirmed: Boolean)
    private var root: ParcelFileDescriptor? = null
    private lateinit var identity: Identity
    private val fd get() = checkNotNull(root)
    val rootIdentity: Identity get() { validate(); return identity }
    /** A separately opened fd does not prolong this owner's root flock. */
    fun maintenanceRoot(): ParcelFileDescriptor {
        validate()
        return LocalPublicationFile.directory(location, "").also {
            if (objectIdentity(Os.fstat(it.fileDescriptor)) != identity) { it.close(); throw Rejected(Reason.CHANGED) }
        }
    }
    fun requireNoRemoval() { validate(); requireNoRemoval(fd.fd) }
    private fun path(name: String = "") = "/proc/self/fd/${fd.fd}" + if (name.isEmpty()) "" else "/$name"
    private fun demand(value: Boolean, reason: Reason = Reason.INVALID) { if (!value) throw Rejected(reason) }

    private fun start(expected: Identity) {
        root = LocalPublicationFile.directory(location, "")
        val selected = Os.fstat(fd.fileDescriptor)
        demand(privateDirectory(selected) && objectIdentity(selected) == expected, Reason.ROOT)
        identity = objectIdentity(selected)
        val locked = PluginInstallFiles.lock(fd.fd)
        demand(locked == 0, if (locked == OsConstants.EAGAIN) Reason.BUSY else Reason.IO)
        validate()
    }
    fun validate() {
        demand(privateDirectory(Os.fstat(fd.fileDescriptor)) && objectIdentity(Os.fstat(fd.fileDescriptor)) == identity, Reason.CHANGED)
        LocalPublicationFile.directory(location, "").use { current ->
            val named = Os.fstat(current.fileDescriptor)
            demand(privateDirectory(named) && objectIdentity(named) == identity, Reason.CHANGED)
        }
    }
    class Version internal constructor(val descriptor: ParcelFileDescriptor, val releaseID: String) : AutoCloseable {
        val identity = objectIdentity(Os.fstat(descriptor.fileDescriptor))
        fun read(name: String, limit: Int): ByteArray = readFile(descriptor.fd, name, limit)
        fun readPayload(name: String, limit: Int): ByteArray = payload().use { readFile(it.fd, name, limit) }
        fun payload(): ParcelFileDescriptor = LocalPublicationFile.childDirectory(descriptor.fd, "payload").also {
            if (!frozenDirectory(Os.fstat(it.fileDescriptor))) { it.close(); throw Rejected(Reason.INVALID) }
        }
        fun pin() { if (PluginInstallFiles.pin(descriptor.fd) != 0) throw Rejected(Reason.BUSY) }
        /** The duplicated fd keeps the same shared flock until the last lease closes. */
        fun borrow(): ParcelFileDescriptor = ParcelFileDescriptor.dup(descriptor.fileDescriptor)
        override fun close() = descriptor.close()
    }
    fun openVersion(id: String): Version {
        demand(digest(id)); validate()
        val selected = LocalPublicationFile.childDirectory(fd.fd, id)
        try { demand(frozenDirectory(Os.fstat(selected.fileDescriptor))); return Version(selected, id) }
        catch (failure: Throwable) { selected.close(); throw failure }
    }
    fun validateVersion(version: Version, payload: ParcelFileDescriptor) {
        validate()
        openVersion(version.releaseID).use { named -> demand(named.identity == version.identity, Reason.CHANGED) }
        version.payload().use { named ->
            demand(objectIdentity(Os.fstat(named.fileDescriptor)) == objectIdentity(Os.fstat(payload.fileDescriptor)), Reason.CHANGED)
        }
    }
    fun observe(): Observation {
        validate()
        val names = File(path()).list() ?: throw Rejected(Reason.IO)
        demand(names.size <= 128, Reason.LIMIT)
        val versions = mutableListOf<DeclaredVersion>(); val issues = mutableListOf<String>(); var maintenance = 0
        for (name in names.sorted()) {
            if (name == SELECTION) continue
            if (!digest(name)) { maintenance++; continue }
            try { openVersion(name).use { version ->
                val raw = version.read("release.json", 65536)
                demand(sha(raw) == name && version.read("release.sig", 64).size == 64)
                // Stored policy is evidence only. Never feed it into authentication.
                version.read("policy.json", 131072)
                val declared = PluginReleaseSignature.declarationObject(raw)
                fun text(key: String) = (declared.opt(key) as? String ?: throw Rejected(Reason.INVALID)).also {
                    demand(it.codePointCount(0, it.length) in 1..128 && Normalizer.normalize(it, Normalizer.Form.NFC) == it && it.codePoints().allMatch { c -> c >= 32 && c != 127 })
                }
                val id = text("pluginId"); val hash = text("packageSHA256")
                demand(pluginID(id) && digest(hash))
                versions += DeclaredVersion(name, id, text("pluginVersion"), text("publisher"), hash)
            } } catch (_: Exception) { issues += "$name：无法读取完整的只读版本声明。" }
        }
        val selected = runCatching { selection() }
        validate()
        return Observation(versions, issues, maintenance, selected.getOrNull(),
            if (selected.isFailure) "活动选择记录查不出来；未将损坏记录当作空选择。" else null)
    }
    fun selection(): Selection {
        validate()
        try { Os.lstat(path(SELECTION)) }
        catch (failure: ErrnoException) {
            if (failure.errno == OsConstants.ENOENT) return Selection(null, emptyMap())
            throw Rejected(Reason.INVALID)
        }
        val raw = readFile(fd.fd, SELECTION, 131072)
        val value = PluginReleaseSignature.declarationObject(raw)
        demand(keys(value) == setOf("schemaVersion", "nonce", "choices") &&
            value.opt("schemaVersion") is Number && value.opt("schemaVersion").toString() == "1" && uuid(value.opt("nonce") as? String))
        val entries = value.opt("choices") as? JSONObject ?: throw Rejected(Reason.INVALID)
        demand(entries.length() <= 64, Reason.LIMIT)
        val result = linkedMapOf<String, Choice>()
        for (id in keys(entries).sorted()) {
            val choice = entries.opt(id) as? JSONObject ?: throw Rejected(Reason.INVALID)
            demand(pluginID(id) && keys(choice) == setOf("releaseID", "enabled", "generation"))
            val release = choice.opt("releaseID") as? String ?: throw Rejected(Reason.INVALID)
            val enabled = choice.opt("enabled") as? Boolean ?: throw Rejected(Reason.INVALID)
            val generation = choice.opt("generation") as? String ?: throw Rejected(Reason.INVALID)
            demand(digest(release) && uuid(generation)); result[id] = Choice(release, enabled, generation)
        }
        return Selection(sha(raw), result.toMap())
    }
    /** Storage only. A caller must verify the actual package/current policy and
     * bind new-run admission before offering this transaction as a UI action. */
    fun change(plugin: String, releaseID: String, enabled: Boolean, expected: String?,
        checkpoint: (String) -> Unit = {}, parentSync: (FileDescriptor) -> Unit = { Os.fsync(it) }): Publication {
        demand(pluginID(plugin) && digest(releaseID))
        validate()
        if (enabled) requireNoRemoval()
        val previous = selection()
        val proposed = previous.choices + (plugin to Choice(releaseID, enabled, UUID.randomUUID().toString()))
        return writeSelection(proposed, previous, expected, checkpoint, parentSync)
    }
    /** One disable-only record replacement; no intermediate subset is published. */
    fun disable(pluginIDs: List<String>, expected: String?, checkpoint: (String) -> Unit = {},
        parentSync: (FileDescriptor) -> Unit = { Os.fsync(it) }): Publication {
        demand(pluginIDs.isNotEmpty() && pluginIDs.distinct().size == pluginIDs.size && pluginIDs.all(::pluginID))
        val previous = selection()
        val proposed = previous.choices.toMutableMap()
        for (id in pluginIDs) {
            val choice = previous.choices[id] ?: throw Rejected(Reason.INVALID)
            demand(choice.enabled)
            proposed[id] = choice.copy(enabled = false, generation = UUID.randomUUID().toString())
        }
        return writeSelection(proposed, previous, expected, checkpoint, parentSync)
    }
    private fun writeSelection(proposed: Map<String, Choice>, previous: Selection, expected: String?,
        checkpoint: (String) -> Unit, parentSync: (FileDescriptor) -> Unit): Publication {
        validate(); demand(previous.revision == expected, Reason.CONFLICT)
        LocalPublicationFile.directory(location.parentFile ?: throw Rejected(Reason.ROOT), "").use { ancestor ->
            LocalPublicationFile.childDirectory(ancestor.fd, location.name).use { named ->
                demand(objectIdentity(Os.fstat(named.fileDescriptor)) == identity, Reason.CHANGED)
            }
            rootSync(ancestor.fileDescriptor)
        }
        demand(proposed.size <= 64, Reason.LIMIT)
        val choices = JSONObject()
        proposed.toSortedMap().forEach { (id, value) -> choices.put(id, JSONObject().put("releaseID", value.releaseID).put("enabled", value.enabled).put("generation", value.generation)) }
        val raw = JSONObject().put("schemaVersion", 1).put("nonce", UUID.randomUUID().toString()).put("choices", choices).toString().toByteArray(Charsets.UTF_8)
        demand(raw.size <= 131072, Reason.LIMIT)
        val temporary = ".selection-${UUID.randomUUID()}"
        LocalPublicationFile.candidate(fd.fd, temporary).use { output ->
            var input: ParcelFileDescriptor? = null; var published = false; var candidateStamp: Stamp? = null
            try {
                checkpoint("after-create")
                var at = 0
                while (at < raw.size) {
                    checkpoint("file-chunk")
                    val count = try { Os.write(output.fileDescriptor, raw, at, minOf(16384, raw.size - at)) }
                        catch (failure: ErrnoException) { if (failure.errno == OsConstants.EINTR) continue else throw failure }
                    demand(count > 0, Reason.IO); at += count
                }
                Os.fchmod(output.fileDescriptor, 256); Os.fsync(output.fileDescriptor)
                input = LocalPublicationFile.retainedCandidate(fd.fd, temporary)
                val before = stamp(Os.fstat(input.fileDescriptor))
                candidateStamp = before
                demand(before == stamp(Os.fstat(output.fileDescriptor)))
                output.close()
                checkpoint("before-rename")
                validate()
                demand(selection().revision == expected, Reason.CONFLICT)
                demand(stamp(Os.lstat(path(temporary))) == before && stamp(Os.fstat(input.fileDescriptor)) == before, Reason.CHANGED)
                if (expected == null) {
                    val error = LocalPublicationFile.publish(fd.fd, fd.fd, input.fd, temporary, SELECTION, raw.size.toLong())
                    demand(error == 0, if (error == OsConstants.EEXIST) Reason.CONFLICT else Reason.IO)
                } else { Os.rename(path(temporary), path(SELECTION)) }
                published = true
                val durable = runCatching { checkpoint("after-rename"); validate(); parentSync(fd.fileDescriptor) }.isSuccess
                return Publication(Selection(sha(raw), proposed), durable)
            } finally {
                try { if (!published) {
                    if (input != null) demand(stamp(Os.fstat(input.fileDescriptor)) == candidateStamp && stamp(Os.lstat(path(temporary))) == candidateStamp, Reason.CHANGED)
                    demand(LocalPublicationFile.remove(fd.fd, (input ?: output).fd, temporary) == 0, Reason.IO)
                } }
                finally { input?.close() }
            }
        }
    }
    override fun close() { root?.close(); root = null }
    companion object {
        const val SELECTION = "selection-v1.json"
        const val REMOVAL = "removal-v1.jsonl"
        fun requireNoRemoval(directory: Int) {
            try { Os.lstat("/proc/self/fd/$directory/$REMOVAL") }
            catch (failure: ErrnoException) { if (failure.errno == OsConstants.ENOENT) return else throw Rejected(Reason.IO) }
            throw Rejected(Reason.PENDING)
        }
        fun open(location: File, rootSync: (FileDescriptor) -> Unit = { Os.fsync(it) }): PluginReleaseCatalog? {
            if (!location.isAbsolute || location.path.toByteArray().size > 3500 || '\u0000' in location.path) throw Rejected(Reason.ROOT)
            val before = try { Os.lstat(location.path) } catch (failure: ErrnoException) {
                if (failure.errno == OsConstants.ENOENT) return null else throw Rejected(Reason.ROOT)
            }
            if (!privateDirectory(before)) throw Rejected(Reason.ROOT)
            val owner = PluginReleaseCatalog(location, rootSync)
            try { owner.start(objectIdentity(before)); return owner }
            catch (failure: Throwable) { owner.close(); throw failure }
        }
        fun sha(raw: ByteArray) = MessageDigest.getInstance("SHA-256").digest(raw).joinToString("") { "%02x".format(it.toInt() and 255) }
        fun digest(value: String) = value.matches(Regex("[0-9a-f]{64}"))
        private fun pluginID(value: String) = value.length <= 128 && value.matches(Regex("[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+"))
        private fun uuid(value: String?) = value != null && runCatching { UUID.fromString(value).toString() == value }.getOrDefault(false)
        private fun keys(value: JSONObject) = value.keys().asSequence().toSet()
        private fun objectIdentity(v: StructStat) = Identity(v.st_dev, v.st_ino, v.st_uid)
        private fun stamp(v: StructStat) = Stamp(objectIdentity(v), v.st_size, v.st_mode, v.st_nlink, v.st_mtim.tv_sec, v.st_mtim.tv_nsec, v.st_ctim.tv_sec, v.st_ctim.tv_nsec)
        private fun privateDirectory(v: StructStat) = OsConstants.S_ISDIR(v.st_mode) && v.st_uid == Process.myUid() && v.st_mode and 63 == 0
        private fun frozenDirectory(v: StructStat) = privateDirectory(v) && v.st_mode and 146 == 0
        private fun readFile(parent: Int, name: String, limit: Int): ByteArray {
            require(limit in 1..131072)
            LocalPublicationFile.retainedCandidate(parent, name).use { input ->
                val before = Os.fstat(input.fileDescriptor)
                if (before.st_uid != Process.myUid() || before.st_nlink != 1L || before.st_mode and 191 != 0 || before.st_size !in 0L..limit.toLong()) throw Rejected(Reason.INVALID)
                val bytes = ByteArray(8192); val result = ByteArrayOutputStream()
                while (true) {
                    val count = try { Os.read(input.fileDescriptor, bytes, 0, bytes.size) }
                        catch (failure: ErrnoException) { if (failure.errno == OsConstants.EINTR) continue else throw failure }
                    if (count == 0) break
                    if (count < 0 || result.size() > limit - count) throw Rejected(Reason.LIMIT)
                    result.write(bytes, 0, count)
                }
                if (result.size().toLong() != before.st_size || stamp(before) != stamp(Os.fstat(input.fileDescriptor))) throw Rejected(Reason.CHANGED)
                return result.toByteArray()
            }
        }
    }
}
