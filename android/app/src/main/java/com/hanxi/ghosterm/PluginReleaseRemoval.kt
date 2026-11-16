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

/** Explicit removal of a reviewed, exclusively pinned local version. A journal
 * records intent, never authorizes startup replay or adoption by name prefix. */
internal class PluginReleaseRemoval private constructor(
    private val root: ParcelFileDescriptor,
    private val version: ParcelFileDescriptor?,
    private val header: Header,
    val members: List<Member>,
    private var checkpoint: (String) -> Unit,
    private var sync: (FileDescriptor) -> Unit,
) : AutoCloseable {
    enum class Reason { ENABLED, OCCUPIED, PENDING, STARTED, CHANGED, INVALID, LIMIT, IO, UNSUPPORTED }
    class Rejected(val reason: Reason) : IllegalStateException(when (reason) {
        Reason.ENABLED -> "请先禁用此版本的后续新实例，再预览回收。"
        Reason.OCCUPIED -> "此版本仍有运行引用或另一项预览，请先停止或取消后重试。"
        Reason.PENDING -> "已有待处理的删除意图，请先明确继续或撤销。"
        Reason.STARTED -> "删除已进入移除阶段，不能撤销；请核对记录后继续处理。"
        Reason.CHANGED -> "目录、选择或完整清单已变化，剩余内容已保留。"
        Reason.INVALID -> "删除记录或文件类型无效，不能根据名称删除。"
        Reason.LIMIT -> "回收清单超过读取上限，未删除文件。"
        Reason.IO -> "回收回执未完成，请刷新并核对保留的删除意图。"
        Reason.UNSUPPORTED -> "此文件系统不支持所需的原子移除准备操作。"
    })
    data class Stamp(val device: Long, val inode: Long, val owner: Int, val mode: Int, val links: Long,
        val size: Long, val modified: Long, val modifiedNS: Long, val changed: Long, val changedNS: Long,
        val identityKind: Int, val born: Long, val bornNS: Long) {
        fun sameObject(other: Stamp) = device == other.device && inode == other.inode && owner == other.owner &&
            (mode and OsConstants.S_IFMT) == (other.mode and OsConstants.S_IFMT) && identityKind == other.identityKind && born == other.born && bornNS == other.bornNS
        fun json() = JSONObject().apply {
            put("device", device.toString()); put("inode", inode.toString()); put("owner", owner.toString())
            put("mode", mode.toString()); put("links", links.toString()); put("size", size.toString())
            put("modified", modified.toString()); put("modifiedNS", modifiedNS.toString())
            put("changed", changed.toString()); put("changedNS", changedNS.toString())
            put("identityKind", identityKind.toString()); put("born", born.toString()); put("bornNS", bornNS.toString())
        }
    }
    data class Member(val path: String, val directory: Boolean, val stamp: Stamp, val sha256: String?) {
        val bytes get() = if (directory) 0L else stamp.size
        fun json() = JSONObject().put("path", path).put("directory", directory).put("stamp", stamp.json()).put("sha256", sha256 ?: JSONObject.NULL)
    }
    private data class Header(val operation: String, val releaseID: String, val revision: String?, val root: Stamp, val version: Stamp) {
        val tombstone get() = ".removing-$operation"
        fun json() = JSONObject().put("schema", 1).put("operation", operation).put("releaseID", releaseID)
            .put("tombstone", tombstone).put("revision", revision ?: JSONObject.NULL).put("root", root.json()).put("version", version.json())
    }
    data class Preview(val id: String, val releaseID: String, val directory: String, val members: List<Member>, val pending: Boolean) {
        val bytes get() = members.sumOf { it.bytes }
    }
    data class Result(val releaseID: String, val removed: Int, val observedAbsent: Int,
        val complete: Boolean, val durabilityConfirmed: Boolean, val issue: String? = null, val memoryIssue: String? = null,
        val forgottenPackages: List<Pair<String, String>> = emptyList())
    private data class Intent(val header: Header, val members: List<Member>, val raw: ByteArray, val stamp: Stamp)
    private var closed = false
    private var confirmed = false
    private var intent: Intent? = null
    val releaseID get() = header.releaseID
    fun preview(id: String) = Preview(id, releaseID, Os.readlink("/proc/self/fd/${root.fd}") + "/$releaseID", members, false)
    private fun rootPath(name: String) = "/proc/self/fd/${root.fd}/$name"
    private fun validate(owner: PluginReleaseCatalog) {
        demand(!closed); owner.validate()
        demand(header.root.sameObject(stamp(root)))
        val selected = owner.rootIdentity
        demand(selected.device == header.root.device && selected.inode == header.root.inode && selected.owner == header.root.owner)
    }
    private fun disabled(owner: PluginReleaseCatalog, revision: Boolean) {
        val selected = owner.selection()
        demand(!revision || selected.revision == header.revision)
        if (selected.choices.values.any { it.enabled && it.releaseID == releaseID }) throw Rejected(Reason.ENABLED)
    }
    /** Before the durable intent there are no writes; afterwards interruption
     * preserves the actual disk outcome and a record for explicit continuation. */
    fun confirm(owner: PluginReleaseCatalog, confirmationCheckpoint: ((String) -> Unit)? = null,
        rootSync: ((FileDescriptor) -> Unit)? = null): Result {
        confirmationCheckpoint?.let { checkpoint = it }; rootSync?.let { sync = it }
        demand(!confirmed, Reason.PENDING); validate(owner); owner.requireNoRemoval(); disabled(owner, true)
        val selected = checkNotNull(version)
        demand(named(root.fd, releaseID) == header.version && stamp(selected) == header.version)
        demand(scan(selected, header.root.device, checkpoint) == members)
        checkpoint("before-intent"); validate(owner); disabled(owner, true)
        demand(named(root.fd, releaseID) == header.version && scan(selected, header.root.device, checkpoint) == members)
        this.checkpoint("before-publish-intent"); validate(owner); disabled(owner, true)
        intent = writeIntent(root, header, members); confirmed = true
        return finish(owner, detached = false)
    }
    private fun finish(owner: PluginReleaseCatalog, detached: Boolean): Result {
        var removed = 0; var absent = 0
        fun result(complete: Boolean, durable: Boolean, reason: Reason? = null) = Result(releaseID, removed, absent, complete, durable, reason?.let { Rejected(it).message })
        try {
            validate(owner); verifyIntent(); disabled(owner, !detached && version != null)
            sync(root.fileDescriptor); checkpoint(if (detached) "resume-detached" else "after-intent")
            validate(owner); verifyIntent(); disabled(owner, !detached && version != null)
            val selected = version
            if (selected == null) {
                // Absence is an observation, never a claim that this call unlinked.
                demand(named(root.fd, releaseID) == null && named(root.fd, header.tombstone) == null)
                absent = members.size + 1; clearIntent(owner); return result(true, synchronized())
            }
            val current = scan(selected, header.root.device, checkpoint, writableDirectories = detached)
            val expected = members.associateBy { it.path }
            for (item in current) {
                val previous = expected[item.path] ?: throw Rejected(Reason.CHANGED)
                demand(if (item.directory) previous.directory && previous.stamp.sameObject(item.stamp) else item == previous)
            }
            absent = members.size - current.size
            if (!detached) {
                demand(current == members && named(root.fd, releaseID) == header.version)
                checkpoint("before-detach"); validate(owner); disabled(owner, true); verifyIntent()
                demand(scan(selected, header.root.device, checkpoint) == members)
                val error = PluginInstallFiles.detach(root.fd, selected.fd, releaseID, header.tombstone)
                if (error != 0) throw Rejected(if (error in setOf(OsConstants.ENOSYS, OsConstants.EOPNOTSUPP, OsConstants.EXDEV)) Reason.UNSUPPORTED else Reason.CHANGED)
            }
            sync(root.fileDescriptor); checkpoint("after-detach")
            for (member in current.sortedWith(compareByDescending<Member> { it.path.count { c -> c == '/' } }.thenBy { it.path })) {
                validate(owner); disabled(owner, false); verifyIntent(); requireDetached(selected)
                parent(selected, member.path, expected).use { parent ->
                    val name = member.path.substringAfterLast('/')
                    val now = named(parent.fd, name) ?: throw Rejected(Reason.CHANGED)
                    if (member.directory) {
                        demand(member.stamp.sameObject(now))
                        LocalPublicationFile.childDirectory(parent.fd, name).use { child ->
                            demand(member.stamp.sameObject(stamp(child)))
                            Os.fchmod(parent.fileDescriptor, 448)
                            demand(LocalPublicationFile.rmdir(parent.fd, child.fd, name) == 0, Reason.IO)
                            removed++
                        }
                    } else {
                        val proof = file(parent.fd, name, FILE_LIMIT)
                        demand(proof.first == member.stamp && proof.second == member.sha256)
                        LocalPublicationFile.retainedCandidate(parent.fd, name).use { input ->
                            demand(stamp(input) == member.stamp && named(parent.fd, name) == member.stamp)
                            Os.fchmod(parent.fileDescriptor, 448)
                            demand(LocalPublicationFile.remove(parent.fd, input.fd, name) == 0, Reason.IO)
                            removed++
                        }
                    }
                    sync(parent.fileDescriptor)
                }
                checkpoint("after-unlink")
            }
            validate(owner); disabled(owner, false); verifyIntent(); requireDetached(selected)
            demand(LocalPublicationFile.rmdir(root.fd, selected.fd, header.tombstone) == 0, Reason.IO); removed++
            sync(root.fileDescriptor); checkpoint("before-clear"); clearIntent(owner)
            return result(true, synchronized())
        } catch (failure: Exception) {
            return result(false, false, (failure as? Rejected)?.reason ?: Reason.IO)
        }
    }
    private fun synchronized() = runCatching { sync(root.fileDescriptor) }.isSuccess
    private fun requireDetached(selected: ParcelFileDescriptor) {
        demand(named(root.fd, header.tombstone)?.let(header.version::sameObject) == true && header.version.sameObject(stamp(selected)))
    }
    private fun verifyIntent() {
        val expected = checkNotNull(intent)
        val current = readIntent(root)
        demand(expected.stamp == current.stamp && expected.raw.contentEquals(current.raw))
    }
    private fun clearIntent(owner: PluginReleaseCatalog) {
        validate(owner); verifyIntent()
        LocalPublicationFile.retainedCandidate(root.fd, PluginReleaseCatalog.REMOVAL).use {
            demand(stamp(it) == checkNotNull(intent).stamp)
            demand(LocalPublicationFile.remove(root.fd, it.fd, PluginReleaseCatalog.REMOVAL) == 0, Reason.IO)
        }
    }
    override fun close() { if (!closed) { closed = true; version?.close(); root.close() } }
    companion object {
        private const val FILE_LIMIT = 16777216
        private const val TOTAL_LIMIT = 68157440L
        private const val RECORD_LIMIT = 8388608
        private fun demand(ok: Boolean, reason: Reason = Reason.CHANGED) { if (!ok) throw Rejected(reason) }
        private fun observed(s: StructStat, identityKind: Int, born: Long, bornNS: Long) = Stamp(s.st_dev, s.st_ino, s.st_uid, s.st_mode, s.st_nlink, s.st_size, s.st_mtim.tv_sec, s.st_mtim.tv_nsec, s.st_ctim.tv_sec, s.st_ctim.tv_nsec, identityKind, born, bornNS)
        private fun stamp(descriptor: ParcelFileDescriptor): Stamp {
            val before = Os.fstat(descriptor.fileDescriptor)
            val birth = PluginInstallFiles.identity(descriptor.fd)
            demand(birth.size == 4, Reason.IO)
            if (birth[0] != 0L) throw Rejected(if (birth[0] in setOf(OsConstants.ENOSYS.toLong(), OsConstants.EOPNOTSUPP.toLong(), OsConstants.ENOTTY.toLong(), OsConstants.EACCES.toLong(), OsConstants.EPERM.toLong())) Reason.UNSUPPORTED else Reason.IO)
            demand(birth[1] in 1..2 && birth[3] in 0..999999999 && (birth[1] != 2L || (birth[2] in 1..4294967295L && birth[3] == 0L)), Reason.INVALID)
            val value = observed(before, birth[1].toInt(), birth[2], birth[3])
            demand(value == observed(Os.fstat(descriptor.fileDescriptor), birth[1].toInt(), birth[2], birth[3]))
            return value
        }
        private fun privateDirectory(s: Stamp) = OsConstants.S_ISDIR(s.mode) && s.owner == Process.myUid() && s.mode and 63 == 0
        private fun named(parent: Int, name: String): Stamp? {
            val before = try { Os.lstat("/proc/self/fd/$parent/$name") }
                catch (failure: ErrnoException) { if (failure.errno == OsConstants.ENOENT) return null else throw Rejected(Reason.IO) }
            demand(OsConstants.S_ISDIR(before.st_mode) || OsConstants.S_ISREG(before.st_mode), Reason.INVALID)
            val descriptor = if (OsConstants.S_ISDIR(before.st_mode)) LocalPublicationFile.childDirectory(parent, name)
                else LocalPublicationFile.retainedCandidate(parent, name)
            return descriptor.use {
                val value = stamp(it)
                demand(value == observed(before, value.identityKind, value.born, value.bornNS))
                demand(value == observed(Os.lstat("/proc/self/fd/$parent/$name"), value.identityKind, value.born, value.bornNS))
                value
            }
        }
        private fun validPath(path: String) = path.toByteArray().size <= 4096 && Normalizer.normalize(path, Normalizer.Form.NFC) == path &&
            path.split('/').all { it.isNotEmpty() && it != "." && it != ".." && it.toByteArray().size <= 255 && it.none { c -> c < ' ' || c == '\u007f' || c == '\\' || c == ':' } }
        private fun file(parent: Int, name: String, limit: Int, content: ByteArrayOutputStream? = null): Pair<Stamp, String> {
            LocalPublicationFile.retainedCandidate(parent, name).use { input ->
                val before = stamp(input)
                demand(OsConstants.S_ISREG(before.mode) && before.owner == Process.myUid() && before.links == 1L && before.mode and 191 == 0, Reason.INVALID)
                demand(before.size in 0L..limit.toLong(), Reason.LIMIT)
                val digest = MessageDigest.getInstance("SHA-256"); val buffer = ByteArray(16384); var size = 0L
                while (true) {
                    val count = try { Os.read(input.fileDescriptor, buffer, 0, buffer.size) }
                        catch (failure: ErrnoException) { if (failure.errno == OsConstants.EINTR) continue else throw failure }
                    if (count == 0) break
                    demand(count > 0 && size <= limit - count, Reason.LIMIT); size += count
                    digest.update(buffer, 0, count); content?.write(buffer, 0, count)
                }
                demand(size == before.size && before == stamp(input) && before == named(parent, name))
                return before to digest.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
            }
        }
        private fun scan(directory: ParcelFileDescriptor, device: Long, checkpoint: (String) -> Unit,
            writableDirectories: Boolean = false): List<Member> {
            val found = mutableListOf<Member>(); var bytes = 0L
            fun walk(parent: ParcelFileDescriptor, prefix: String, depth: Int) {
                val before = stamp(parent)
                demand(privateDirectory(before) && before.device == device && (writableDirectories || before.mode and 146 == 0), Reason.INVALID)
                demand(depth <= 34, Reason.LIMIT)
                val names = File("/proc/self/fd/${parent.fd}").list() ?: throw Rejected(Reason.IO)
                demand(names.size <= 4096 - found.size, Reason.LIMIT)
                for (name in names.sorted()) {
                    checkpoint("scan-member")
                    val path = if (prefix.isEmpty()) name else "$prefix/$name"
                    demand(validPath(path), Reason.INVALID); demand(found.size < 4096, Reason.LIMIT)
                    val value = named(parent.fd, name) ?: throw Rejected(Reason.CHANGED)
                    demand(value.device == device && value.owner == Process.myUid(), Reason.INVALID)
                    if (OsConstants.S_ISDIR(value.mode)) {
                        demand(privateDirectory(value), Reason.INVALID); found += Member(path, true, value, null)
                        LocalPublicationFile.childDirectory(parent.fd, name).use { child ->
                            demand(stamp(child) == value); walk(child, path, depth + 1)
                        }
                    } else {
                        val checked = file(parent.fd, name, FILE_LIMIT); demand(checked.first == value)
                        bytes += value.size; demand(bytes <= TOTAL_LIMIT, Reason.LIMIT)
                        found += Member(path, false, value, checked.second)
                    }
                    demand(named(parent.fd, name) == value)
                }
                demand(stamp(parent) == before)
            }
            walk(directory, "", 0); return found.sortedBy { it.path }
        }
        private fun parent(root: ParcelFileDescriptor, path: String, expected: Map<String, Member>): ParcelFileDescriptor {
            var current = ParcelFileDescriptor.dup(root.fileDescriptor)
            try {
                var prefix = ""
                for (name in path.split('/').dropLast(1)) {
                    prefix += if (prefix.isEmpty()) name else "/$name"
                    val item = expected[prefix] ?: throw Rejected(Reason.INVALID)
                    demand(item.directory)
                    val next = LocalPublicationFile.childDirectory(current.fd, name)
                    try { demand(item.stamp.sameObject(stamp(next)) && privateDirectory(stamp(next))) }
                    catch (failure: Throwable) { next.close(); throw failure }
                    current.close(); current = next
                }
                return current
            } catch (failure: Throwable) { current.close(); throw failure }
        }
        fun prepare(owner: PluginReleaseCatalog, releaseID: String, revision: String?, checkpoint: (String) -> Unit = {},
            sync: (FileDescriptor) -> Unit = { Os.fsync(it) }): PluginReleaseRemoval {
            owner.requireNoRemoval(); demand(owner.selection().revision == revision)
            if (owner.selection().choices.values.any { it.enabled && it.releaseID == releaseID }) throw Rejected(Reason.ENABLED)
            val version = owner.openVersion(releaseID)
            var root: ParcelFileDescriptor? = null
            try {
                val error = PluginInstallFiles.lock(version.descriptor.fd)
                if (error != 0) throw Rejected(if (error == OsConstants.EAGAIN) Reason.OCCUPIED else Reason.IO)
                root = owner.maintenanceRoot()
                val header = Header(UUID.randomUUID().toString(), releaseID, revision, stamp(root), stamp(version.descriptor))
                val members = scan(version.descriptor, header.root.device, checkpoint)
                demand(named(root.fd, releaseID) == header.version); owner.validate()
                return PluginReleaseRemoval(root, version.descriptor, header, members, checkpoint, sync)
            } catch (failure: Throwable) { root?.close(); version.close(); throw failure }
        }
        private fun writeIntent(root: ParcelFileDescriptor, header: Header, members: List<Member>): Intent {
            PluginReleaseCatalog.requireNoRemoval(root.fd)
            val raw = buildString { append(header.json()); append('\n'); members.forEach { append(it.json()); append('\n') } }.toByteArray()
            demand(raw.size <= RECORD_LIMIT, Reason.LIMIT)
            val temporary = ".removal-record-${UUID.randomUUID()}"
            LocalPublicationFile.candidate(root.fd, temporary).use { output ->
                var input: ParcelFileDescriptor? = null; var published = false
                try {
                    var at = 0
                    while (at < raw.size) {
                        val count = try { Os.write(output.fileDescriptor, raw, at, minOf(16384, raw.size - at)) }
                            catch (failure: ErrnoException) { if (failure.errno == OsConstants.EINTR) continue else throw failure }
                        demand(count > 0, Reason.IO); at += count
                    }
                    Os.fchmod(output.fileDescriptor, 256); Os.fsync(output.fileDescriptor)
                    input = LocalPublicationFile.retainedCandidate(root.fd, temporary)
                    val before = stamp(input); demand(before == observed(Os.fstat(output.fileDescriptor), before.identityKind, before.born, before.bornNS))
                    output.close(); demand(named(root.fd, temporary) == before)
                    demand(LocalPublicationFile.publish(root.fd, root.fd, input.fd, temporary, PluginReleaseCatalog.REMOVAL, raw.size.toLong()) == 0, Reason.IO)
                    published = true
                    val after = stamp(input)
                    demand(before.sameObject(after) && named(root.fd, PluginReleaseCatalog.REMOVAL) == after)
                    return Intent(header, members, raw, after)
                } finally {
                    try { if (!published) demand(LocalPublicationFile.remove(root.fd, (input ?: output).fd, temporary) == 0, Reason.IO) }
                    finally { input?.close() }
                }
            }
        }
        private fun keys(value: JSONObject) = value.keys().asSequence().toSet()
        private fun parseStamp(value: JSONObject): Stamp {
            demand(keys(value) == setOf("device", "inode", "owner", "mode", "links", "size", "modified", "modifiedNS", "changed", "changedNS", "identityKind", "born", "bornNS"), Reason.INVALID)
            fun number(name: String) = (value.opt(name) as? String)?.let { text -> text.toLongOrNull()?.takeIf { it.toString() == text } } ?: throw Rejected(Reason.INVALID)
            val owner = number("owner"); val mode = number("mode")
            demand(owner == Process.myUid().toLong() && mode in 0..65535, Reason.INVALID)
            demand(number("identityKind") in 1..2, Reason.INVALID)
            val result = Stamp(number("device"), number("inode"), owner.toInt(), mode.toInt(), number("links"), number("size"), number("modified"), number("modifiedNS"), number("changed"), number("changedNS"), number("identityKind").toInt(), number("born"), number("bornNS"))
            demand(result.size >= 0 && result.links >= 1 && result.modifiedNS in 0..999999999 && result.changedNS in 0..999999999 && result.bornNS in 0..999999999 && (result.identityKind != 2 || (result.born in 1..4294967295L && result.bornNS == 0L)), Reason.INVALID)
            return result
        }
        private fun readIntent(root: ParcelFileDescriptor): Intent {
            val buffer = ByteArrayOutputStream(); val proof = file(root.fd, PluginReleaseCatalog.REMOVAL, RECORD_LIMIT, buffer)
            val raw = buffer.toByteArray(); demand(raw.isNotEmpty() && raw.last() == 10.toByte(), Reason.INVALID)
            val decoded = try { Charsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(raw)).toString() }
                catch (_: java.nio.charset.CharacterCodingException) { throw Rejected(Reason.INVALID) }
            val lines = decoded.split('\n'); demand(lines.size in 2..4098 && lines.last().isEmpty(), Reason.LIMIT)
            fun objectLine(line: String): JSONObject {
                demand(line.toByteArray().size <= 16384, Reason.LIMIT)
                return try { PluginReleaseSignature.declarationObject(line.toByteArray()) } catch (_: Exception) { throw Rejected(Reason.INVALID) }
            }
            val value = objectLine(lines.first())
            demand(keys(value) == setOf("schema", "operation", "releaseID", "tombstone", "revision", "root", "version") && value.opt("schema") is Number && value.opt("schema").toString() == "1", Reason.INVALID)
            val operation = value.opt("operation") as? String ?: throw Rejected(Reason.INVALID)
            demand(runCatching { UUID.fromString(operation).toString() == operation }.getOrDefault(false), Reason.INVALID)
            val release = value.opt("releaseID") as? String ?: throw Rejected(Reason.INVALID)
            demand(PluginReleaseCatalog.digest(release) && value.opt("tombstone") == ".removing-$operation", Reason.INVALID)
            val revision = if (value.opt("revision") === JSONObject.NULL) null else value.opt("revision") as? String ?: throw Rejected(Reason.INVALID)
            demand(revision == null || PluginReleaseCatalog.digest(revision), Reason.INVALID)
            val header = Header(operation, release, revision, parseStamp(value.optJSONObject("root") ?: throw Rejected(Reason.INVALID)), parseStamp(value.optJSONObject("version") ?: throw Rejected(Reason.INVALID)))
            demand(privateDirectory(header.root) && privateDirectory(header.version) && header.root.device == header.version.device, Reason.INVALID)
            val members = lines.drop(1).dropLast(1).map { line ->
                val item = objectLine(line)
                demand(keys(item) == setOf("path", "directory", "stamp", "sha256"), Reason.INVALID)
                val path = item.opt("path") as? String ?: throw Rejected(Reason.INVALID)
                val directory = item.opt("directory") as? Boolean ?: throw Rejected(Reason.INVALID)
                val stamp = parseStamp(item.optJSONObject("stamp") ?: throw Rejected(Reason.INVALID))
                val hash = if (item.opt("sha256") === JSONObject.NULL) null else item.opt("sha256") as? String ?: throw Rejected(Reason.INVALID)
                demand(validPath(path) && stamp.device == header.root.device, Reason.INVALID)
                demand(if (directory) privateDirectory(stamp) && hash == null else OsConstants.S_ISREG(stamp.mode) && stamp.links == 1L && stamp.mode and 191 == 0 && stamp.size <= FILE_LIMIT && hash != null && PluginReleaseCatalog.digest(hash), Reason.INVALID)
                Member(path, directory, stamp, hash)
            }
            demand(members.map { it.path }.distinct().size == members.size && members == members.sortedBy { it.path } && members.sumOf { it.bytes } <= TOTAL_LIMIT, Reason.INVALID)
            val byPath = members.associateBy { it.path }
            for (item in members) {
                val parts = item.path.split('/'); var parent = ""
                for (part in parts.dropLast(1)) { parent += if (parent.isEmpty()) part else "/$part"; demand(byPath[parent]?.directory == true, Reason.INVALID) }
                demand(parts.size <= 35, Reason.LIMIT)
            }
            return Intent(header, members, raw, proof.first)
        }
        fun pending(owner: PluginReleaseCatalog): Preview? {
            owner.maintenanceRoot().use { root ->
                if (named(root.fd, PluginReleaseCatalog.REMOVAL) == null) return null
                val record = readIntent(root); demand(record.header.root.sameObject(stamp(root)))
                val name = if (named(root.fd, record.header.tombstone) != null) record.header.tombstone else record.header.releaseID
                return Preview(record.header.operation, record.header.releaseID, Os.readlink("/proc/self/fd/${root.fd}") + "/$name", record.members, true)
            }
        }
        fun resume(owner: PluginReleaseCatalog, id: String, checkpoint: (String) -> Unit = {}, sync: (FileDescriptor) -> Unit = { Os.fsync(it) }): Result {
            val root = owner.maintenanceRoot(); var version: ParcelFileDescriptor? = null
            try {
                val record = readIntent(root); val header = record.header
                demand(header.operation == id && header.root.sameObject(stamp(root)))
                val detached = named(root.fd, header.tombstone)
                val original = named(root.fd, header.releaseID)
                val name = if (detached != null) header.tombstone else if (original != null) header.releaseID else null
                if (name != null) {
                    val before = checkNotNull(if (detached != null) detached else original)
                    demand(header.version.sameObject(before) && privateDirectory(before))
                    version = LocalPublicationFile.childDirectory(root.fd, name)
                    demand(stamp(version) == before)
                    val error = PluginInstallFiles.lock(version.fd)
                    if (error != 0) throw Rejected(if (error == OsConstants.EAGAIN) Reason.OCCUPIED else Reason.IO)
                }
                PluginReleaseRemoval(root, version, header, record.members, checkpoint, sync).use { removal ->
                    version = null; removal.confirmed = true; removal.intent = record
                    return removal.finish(owner, detached != null)
                }
            } catch (failure: Throwable) { version?.close(); root.close(); throw failure }
        }
        fun cancelPending(owner: PluginReleaseCatalog, id: String, sync: (FileDescriptor) -> Unit = { Os.fsync(it) }): Boolean {
            owner.maintenanceRoot().use { root ->
                val record = readIntent(root); demand(record.header.operation == id && record.header.root.sameObject(stamp(root)))
                if (named(root.fd, record.header.tombstone) != null) throw Rejected(Reason.STARTED)
                owner.openVersion(record.header.releaseID).use { version ->
                    val error = PluginInstallFiles.lock(version.descriptor.fd)
                    if (error != 0) throw Rejected(if (error == OsConstants.EAGAIN) Reason.OCCUPIED else Reason.IO)
                    demand(named(root.fd, record.header.releaseID) == record.header.version && scan(version.descriptor, record.header.root.device, {}) == record.members)
                    val current = readIntent(root); demand(current.stamp == record.stamp && current.raw.contentEquals(record.raw))
                    LocalPublicationFile.retainedCandidate(root.fd, PluginReleaseCatalog.REMOVAL).use { input ->
                        demand(stamp(input) == record.stamp)
                        demand(LocalPublicationFile.remove(root.fd, input.fd, PluginReleaseCatalog.REMOVAL) == 0, Reason.IO)
                    }
                    return runCatching { sync(root.fileDescriptor) }.isSuccess
                }
            }
        }
    }
}
