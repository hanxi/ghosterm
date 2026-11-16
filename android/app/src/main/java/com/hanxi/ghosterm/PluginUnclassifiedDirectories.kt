package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.Process
import android.system.Os
import android.system.OsConstants
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.json.JSONObject
import java.io.File
import java.text.Normalizer

/** A fresh deletion decision about current contents, never installation origin. */
internal object PluginUnclassifiedDirectories {
    data class Reference(val name: String, val token: String) {
        val nameSHA256 get() = PluginReleaseCatalog.sha(name.toByteArray(Charsets.UTF_8))
        fun json() = JSONObject().put("name", name).put("token", token)
    }
    data class Entry(val name: String, val reference: Reference?, val issue: String?)
    fun allowedName(name: String) = name.isNotEmpty() && name.toByteArray(Charsets.UTF_8).size <= 255 &&
        Normalizer.normalize(name, Normalizer.Form.NFC) == name && name.none { it < ' ' || it == '\u007f' || it == '/' || it == '\\' || it == ':' } &&
        name !in setOf(".", "..", PluginReleaseCatalog.SELECTION, PluginReleaseCatalog.REMOVAL) &&
        listOf(".install-", ".intent-", ".selection-", ".removal-record-").none(name::startsWith)
    private fun demand(ok: Boolean, reason: PluginReleaseRemoval.Reason = PluginReleaseRemoval.Reason.CHANGED) {
        if (!ok) throw PluginReleaseRemoval.Rejected(reason)
    }
    fun token(root: PluginReleaseRemoval.Stamp, name: String, value: PluginReleaseRemoval.Stamp): String {
        val parent = "${root.device}:${root.inode}:${root.owner}:${root.identityKind}:${root.born}:${root.bornNS}:"
        return PluginReleaseCatalog.sha((parent + name + "\u0000" + value.json().toString()).toByteArray(Charsets.UTF_8))
    }
    fun parse(value: JSONObject): Reference {
        demand(value.keys().asSequence().toSet() == setOf("name", "token"), PluginReleaseRemoval.Reason.INVALID)
        val name = value.opt("name") as? String ?: error("invalid directory name")
        val token = value.opt("token") as? String ?: error("invalid observation")
        demand(allowedName(name) && PluginReleaseCatalog.digest(token), PluginReleaseRemoval.Reason.INVALID)
        return Reference(name, token)
    }
    fun listing(owner: PluginReleaseCatalog): List<Entry> {
        owner.requireNoRemoval()
        // selection failure remains unknown; it cannot enable a removal entry.
        owner.selection()
        val known = owner.observe().versions.map { it.releaseID }.toMutableSet()
        owner.maintenanceRoot().use { root ->
            val parent = PluginReleaseRemoval.stamp(root)
            known += PluginInstallJournal.classifiedNames(root)
            val names = File("/proc/self/fd/${root.fd}").list() ?: error("cannot enumerate unknown entries")
            demand(names.size <= 128, PluginReleaseRemoval.Reason.LIMIT)
            val result = names.sorted().filter { allowedName(it) && it !in known }.map { name ->
                val before = Os.lstat("/proc/self/fd/${root.fd}/$name")
                if (!OsConstants.S_ISDIR(before.st_mode) || before.st_uid != Process.myUid() || before.st_dev != parent.device || before.st_mode and 63 != 0)
                    Entry(name, null, "类型、设备或权限不满足私有目录条件，保留此条目。")
                else {
                    val value = PluginReleaseRemoval.named(root.fd, name) ?: error("directory changed")
                    demand(PluginReleaseRemoval.privateDirectory(value) && value.device == parent.device)
                    Entry(name, Reference(name, token(parent, name, value)), null)
                }
            }
            demand(PluginReleaseRemoval.stamp(root) == parent); owner.validate()
            return result
        }
    }
    /** Preview must still be unknown. Withdrawal uses the already recorded full
     * snapshot instead; neither path turns a prefix into an ownership claim. */
    fun open(owner: PluginReleaseCatalog, reference: Reference, classify: Boolean = true): ParcelFileDescriptor {
        demand(allowedName(reference.name) && PluginReleaseCatalog.digest(reference.token), PluginReleaseRemoval.Reason.INVALID)
        if (classify) demand(listing(owner).any { it.reference == reference })
        owner.maintenanceRoot().use { root ->
            val parent = PluginReleaseRemoval.stamp(root)
            val before = PluginReleaseRemoval.named(root.fd, reference.name) ?: error("directory absent")
            demand(PluginReleaseRemoval.privateDirectory(before) && before.device == parent.device)
            demand(token(parent, reference.name, before) == reference.token)
            val selected = LocalPublicationFile.childDirectory(root.fd, reference.name)
            try {
                demand(PluginReleaseRemoval.stamp(selected) == before)
                val lock = PluginInstallFiles.lock(selected.fd)
                if (lock != 0) throw PluginReleaseRemoval.Rejected(if (lock == OsConstants.EAGAIN) PluginReleaseRemoval.Reason.OCCUPIED else PluginReleaseRemoval.Reason.IO)
                demand(PluginReleaseRemoval.named(root.fd, reference.name) == before); owner.validate()
                return selected
            } catch (failure: Throwable) { selected.close(); throw failure }
        }
    }
}
