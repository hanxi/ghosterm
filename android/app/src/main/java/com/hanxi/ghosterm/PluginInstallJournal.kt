package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileDescriptor
import java.util.UUID

/** Persistent install ownership only. Reading cannot publish, adopt a name,
 * replay installation, delete a tree or downgrade an existing root flock. */
internal object PluginInstallJournal {
    enum class State { ACTIVE, ABANDONED, PUBLISHED, ABSENT, UNCONFIRMED, CHANGED, INVALID }
    data class Reference(val id: String, val name: String, val digest: String, val target: String) {
        val recordName get() = ".install-$id.json"
        fun json() = JSONObject().put("id", id).put("name", name).put("digest", digest).put("target", target)
    }
    data class Entry(val name: String, val reference: Reference?, val state: State, val issue: String? = null)
    data class Observation(val entries: List<Entry>)
    private data class Record(val reference: Reference, val parent: PluginReleaseRemoval.Stamp,
        val directory: PluginReleaseRemoval.Stamp?, val raw: ByteArray, val stamp: PluginReleaseRemoval.Stamp)
    private fun demand(ok: Boolean) { if (!ok) throw PluginReleaseRemoval.Rejected(PluginReleaseRemoval.Reason.CHANGED) }
    private fun keys(value: JSONObject) = value.keys().asSequence().toSet()
    private fun uuid(value: String) = runCatching { UUID.fromString(value).toString() == value }.getOrDefault(false)
    fun parseReference(value: JSONObject): Reference {
        demand(keys(value) == setOf("id", "name", "digest", "target"))
        val id = value.opt("id") as? String ?: error("invalid ownership reference")
        val name = value.opt("name") as? String ?: error("invalid ownership reference")
        val digest = value.opt("digest") as? String ?: error("invalid ownership reference")
        val target = value.opt("target") as? String ?: error("invalid ownership reference")
        demand(uuid(id) && name == ".prepare-$id" && PluginReleaseCatalog.digest(digest) && PluginReleaseCatalog.digest(target))
        return Reference(id, name, digest, target)
    }
    private fun read(root: ParcelFileDescriptor, name: String): Record {
        demand(name.startsWith(".install-") && name.endsWith(".json"))
        val id = name.removePrefix(".install-").removeSuffix(".json"); demand(uuid(id))
        val output = ByteArrayOutputStream()
        val proof = PluginReleaseRemoval.file(root.fd, name, 16384, output)
        val raw = output.toByteArray(); val value = PluginReleaseSignature.declarationObject(raw)
        demand(keys(value) == setOf("schema", "operation", "name", "parent", "directory", "target"))
        demand(value.opt("schema") is Number && value.opt("schema").toString() == "2")
        demand(value.opt("operation") == id && value.opt("name") == ".prepare-$id")
        val target = value.opt("target") as? String ?: error("invalid target")
        demand(PluginReleaseCatalog.digest(target))
        val parent = PluginReleaseRemoval.parseStamp(value.optJSONObject("parent") ?: error("invalid parent"))
        val child = if (value.opt("directory") === JSONObject.NULL) null else
            PluginReleaseRemoval.parseStamp(value.optJSONObject("directory") ?: error("invalid directory"))
        demand(PluginReleaseRemoval.privateDirectory(parent) && parent.sameObject(PluginReleaseRemoval.stamp(root)))
        if (child != null) demand(PluginReleaseRemoval.privateDirectory(child) && child.device == parent.device)
        return Record(Reference(id, ".prepare-$id", proof.second, target), parent, child, raw, proof.first)
    }
    private fun sameRecord(root: ParcelFileDescriptor, record: Record) {
        val current = read(root, record.reference.recordName)
        demand(current.stamp == record.stamp && current.raw.contentEquals(record.raw))
    }
    private fun classify(root: ParcelFileDescriptor, record: Record, rootBusy: Boolean = false): State {
        val staging = PluginReleaseRemoval.named(root.fd, record.reference.name)
        val identity = record.directory
        if (identity == null) return if (staging == null && !rootBusy) State.ABSENT else State.UNCONFIRMED
        val name = when {
            staging != null -> { demand(identity.sameObject(staging)); record.reference.name }
            targetMatches(root, record) -> record.reference.target
            else -> return State.ABSENT
        }
        LocalPublicationFile.childDirectory(root.fd, name).use { selected ->
            demand(identity.sameObject(PluginReleaseRemoval.stamp(selected)))
            val lock = PluginInstallFiles.lock(selected.fd)
            if (lock == OsConstants.EAGAIN) return State.ACTIVE
            demand(lock == 0); sameRecord(root, record)
            return if (name == record.reference.name) State.ABANDONED else State.PUBLISHED
        }
    }
    private fun targetMatches(root: ParcelFileDescriptor, record: Record): Boolean {
        val expected = record.directory ?: return false
        val actual = try { Os.lstat("/proc/self/fd/${root.fd}/${record.reference.target}") }
            catch (failure: ErrnoException) { if (failure.errno == OsConstants.ENOENT) return false else throw failure }
        if (!OsConstants.S_ISDIR(actual.st_mode) || actual.st_dev != expected.device || actual.st_ino != expected.inode || actual.st_uid != expected.owner) return false
        return PluginReleaseRemoval.named(root.fd, record.reference.target)?.let(expected::sameObject) == true
    }
    fun observe(directory: File): Observation {
        try { Os.lstat(directory.path) }
        catch (failure: ErrnoException) { if (failure.errno == OsConstants.ENOENT) return Observation(emptyList()) else throw failure }
        LocalPublicationFile.directory(directory, "").use { root ->
            val identity = PluginReleaseRemoval.stamp(root); demand(PluginReleaseRemoval.privateDirectory(identity))
            val names = File("/proc/self/fd/${root.fd}").list() ?: error("cannot enumerate install ownership")
            if (names.size > 128) throw PluginReleaseRemoval.Rejected(PluginReleaseRemoval.Reason.LIMIT)
            val records = names.filter { it.startsWith(".install-") }
            if (records.size > 64) throw PluginReleaseRemoval.Rejected(PluginReleaseRemoval.Reason.LIMIT)
            val rootBusy = LocalPublicationFile.directory(directory, "").use { probe ->
                val error = PluginInstallFiles.lock(probe.fd)
                demand(error == 0 || error == OsConstants.EAGAIN); error == OsConstants.EAGAIN
            }
            val rows = records.sorted().map { name ->
                try {
                    val record = read(root, name)
                    try { val state = classify(root, record, rootBusy); sameRecord(root, record); Entry(name, record.reference, state) }
                    catch (_: Exception) { Entry(name, record.reference, State.CHANGED, "目录或记录已变化，未授予删除权限。") }
                } catch (_: Exception) { Entry(name, null, State.INVALID, "归属记录或目录查不出来，未采纳同名对象。") }
            }
            LocalPublicationFile.directory(directory, "").use { demand(identity.sameObject(PluginReleaseRemoval.stamp(it))) }
            return Observation(rows)
        }
    }
    /** A damaged ownership record is visible failure, never evidence that a
     * directory is unowned. Called while the catalog owns the root lock. */
    fun classifiedNames(root: ParcelFileDescriptor): Set<String> {
        val names = File("/proc/self/fd/${root.fd}").list() ?: error("cannot enumerate ownership")
        demand(names.size <= 128)
        val records = names.filter { it.startsWith(".install-") }; demand(records.size <= 64)
        return buildSet {
            for (name in records) {
                val record = read(root, name)
                when (classify(root, record, rootBusy = true)) {
                    State.ACTIVE, State.ABANDONED -> add(record.reference.name)
                    State.PUBLISHED -> add(record.reference.target)
                    else -> Unit
                }
                sameRecord(root, record)
            }
        }
    }
    fun verify(root: ParcelFileDescriptor, reference: Reference, allowMissing: Boolean = false) {
        if (allowMissing && PluginReleaseRemoval.named(root.fd, reference.recordName) == null) return
        demand(read(root, reference.recordName).reference == reference)
    }
    /** Caller already holds the root EX lock. The returned fd keeps the target
     * EX lock; no files are changed until a separately acknowledged removal. */
    fun openAbandoned(root: ParcelFileDescriptor, reference: Reference): ParcelFileDescriptor {
        val record = read(root, reference.recordName); demand(record.reference == reference)
        val identity = record.directory ?: throw PluginReleaseRemoval.Rejected(PluginReleaseRemoval.Reason.INVALID)
        val before = PluginReleaseRemoval.named(root.fd, reference.name) ?: throw PluginReleaseRemoval.Rejected(PluginReleaseRemoval.Reason.CHANGED)
        demand(identity.sameObject(before))
        val selected = LocalPublicationFile.childDirectory(root.fd, reference.name)
        try {
            demand(PluginReleaseRemoval.stamp(selected) == before)
            val lock = PluginInstallFiles.lock(selected.fd)
            if (lock != 0) throw PluginReleaseRemoval.Rejected(if (lock == OsConstants.EAGAIN) PluginReleaseRemoval.Reason.OCCUPIED else PluginReleaseRemoval.Reason.IO)
            sameRecord(root, record); return selected
        } catch (failure: Throwable) { selected.close(); throw failure }
    }
    /** Retire metadata only after an explicit decision; published versions and
     * unrelated conflict targets are never deleted. */
    fun retire(owner: PluginReleaseCatalog, reference: Reference, afterRemoval: Boolean = false,
        sync: (FileDescriptor) -> Unit = { Os.fsync(it) }): Boolean {
        owner.validate()
        if (!afterRemoval) owner.requireNoRemoval()
        owner.maintenanceRoot().use { root ->
            if (afterRemoval && PluginReleaseRemoval.named(root.fd, reference.recordName) == null) return runCatching { sync(root.fileDescriptor) }.isSuccess
            val record = read(root, reference.recordName); demand(record.reference == reference)
            if (afterRemoval) demand(PluginReleaseRemoval.named(root.fd, reference.name) == null)
            else demand(classify(root, record) in setOf(State.ABSENT, State.PUBLISHED))
            sameRecord(root, record)
            LocalPublicationFile.retainedCandidate(root.fd, reference.recordName).use { selected ->
                demand(PluginReleaseRemoval.stamp(selected) == record.stamp)
                demand(LocalPublicationFile.remove(root.fd, selected.fd, reference.recordName) == 0)
            }
            return runCatching { sync(root.fileDescriptor) }.isSuccess
        }
    }
}
