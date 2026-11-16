package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID

/** Explicit private app-cache fixtures; ownership is a persisted contract, not
 * a claim that fabricating metadata is a product install or process death. */
internal object PluginJournalFixture {
    fun create(root: File, recorded: Boolean = true, child: Boolean = true): PluginInstallJournal.Reference {
        val id = UUID.randomUUID().toString(); val name = ".prepare-$id"; val target = "a".repeat(64)
        LocalPublicationFile.directory(root, "").use { parent ->
            val before = PluginReleaseRemoval.stamp(parent)
            var selected: PluginReleaseRemoval.Stamp? = null
            if (child) {
                val directory = File(root, name).apply { check(mkdir()); Os.chmod(path, 448) }
                File(directory, "partial").apply { writeText("unfinished public package bytes"); Os.chmod(path, 384) }
                LocalPublicationFile.childDirectory(parent.fd, name).use { selected = PluginReleaseRemoval.stamp(it) }
            }
            val raw = JSONObject().put("schema", 2).put("operation", id).put("name", name).put("target", target)
                .put("parent", before.json()).put("directory", if (recorded) selected?.json() ?: JSONObject.NULL else JSONObject.NULL).toString().toByteArray()
            File(root, ".install-$id.json").apply { writeBytes(raw); Os.chmod(path, 256) }
            return PluginInstallJournal.Reference(id, name, PluginReleaseCatalog.sha(raw), target)
        }
    }
    fun owner(root: File) = checkNotNull(PluginReleaseCatalog.open(root))
    fun replace(root: File, ref: PluginInstallJournal.Reference, mutate: (JSONObject) -> Unit) {
        val file = File(root, ref.recordName); val value = JSONObject(file.readText()); mutate(value)
        Os.chmod(file.path, 384); file.writeText(value.toString()); Os.chmod(file.path, 256)
    }
}

@RunWith(AndroidJUnit4::class)
class PluginInstallJournalTest {
    private val f = PluginJournalFixture
    private fun rejected(body: () -> Unit) { assertTrue("unsafe operation succeeded", runCatching(body).exceptionOrNull() is Exception) }
    @Test fun realStagePersistsStrongIdentityAndHoldsItsOwnLockBeforeAnyPayloadWrite() {
        val root = PluginInstallTestFiles.root(); var observed = false
        try {
            PluginReleaseStage.create(root, "a".repeat(64), checkpoint = { phase -> if (phase == "after-directory-record") {
                val entry = PluginInstallJournal.observe(root).entries.single()
                assertEquals("production stage ownership not active", PluginInstallJournal.State.ACTIVE, entry.state)
                val raw = JSONObject(File(root, entry.name).readText())
                assertEquals(2, raw.getInt("schema")); assertTrue(raw.getJSONObject("directory").getInt("identityKind") in 1..2)
                observed = true
            } }).close()
            assertTrue(observed); assertTrue(root.list()!!.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun missingRootObservationDoesNotCreateAnything() {
        val root = PluginInstallTestFiles.root()
        try { val missing = File(root, "missing"); assertTrue(PluginInstallJournal.observe(missing).entries.isEmpty()); assertFalse(missing.exists()) }
        finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun onlyConfirmedPersistentIdentityAllowsAbandonedClassificationAndPinnedOpen() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root); val raw = File(root, ref.recordName).readBytes()
            assertEquals(PluginInstallJournal.State.ABANDONED, PluginInstallJournal.observe(root).entries.single().state)
            f.owner(root).use { owner -> owner.maintenanceRoot().use { parent -> PluginInstallJournal.openAbandoned(parent, ref).use {
                assertEquals(OsConstants.EAGAIN, LocalPublicationFile.directory(root, ref.name).use { PluginInstallFiles.lock(it.fd) })
                assertEquals(PluginInstallJournal.State.ACTIVE, PluginInstallJournal.observe(root).entries.single().state)
            } } }
            assertArrayEquals(raw, File(root, ref.recordName).readBytes()); assertTrue(File(root, "${ref.name}/partial").isFile)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun observationNeverDowngradesAnActiveRootOrStageLock() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root)
            LocalPublicationFile.directory(root, "").use { parent ->
                assertEquals(0, PluginInstallFiles.lock(parent.fd))
                LocalPublicationFile.directory(root, ref.name).use { stage ->
                    assertEquals(0, PluginInstallFiles.lock(stage.fd)); assertEquals(PluginInstallJournal.State.ACTIVE, PluginInstallJournal.observe(root).entries.single().state)
                    LocalPublicationFile.directory(root, "").use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.lock(it.fd)) }
                    LocalPublicationFile.directory(root, ref.name).use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.lock(it.fd)) }
                }
            }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun reservationWithoutIdentityCannotAdoptDirectoryAndBusyAbsenceStaysUnconfirmed() {
        for (exists in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val ref = f.create(root, recorded = false, child = exists)
                LocalPublicationFile.directory(root, "").use { parent ->
                    assertEquals(0, PluginInstallFiles.lock(parent.fd))
                    assertEquals(PluginInstallJournal.State.UNCONFIRMED, PluginInstallJournal.observe(root).entries.single().state)
                    rejected { PluginInstallJournal.openAbandoned(parent, ref).close() }
                }
                assertEquals(if (exists) PluginInstallJournal.State.UNCONFIRMED else PluginInstallJournal.State.ABSENT, PluginInstallJournal.observe(root).entries.single().state)
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun recordDigestNameAndParentPersistentIdentityAreAllBound() {
        for (kind in listOf("digest", "name", "parent", "legacy")) {
            val root = PluginInstallTestFiles.root()
            try {
                val ref = f.create(root)
                when (kind) {
                    "digest" -> f.replace(root, ref) { it.put("target", "b".repeat(64)) }
                    "name" -> f.replace(root, ref) { it.put("name", ".prepare-${UUID.randomUUID()}") }
                    "parent" -> f.replace(root, ref) { val p = it.getJSONObject("parent"); p.put("born", (p.getString("born").toLong() + 1).toString()) }
                    "legacy" -> f.replace(root, ref) { it.put("schema", 1) }
                }
                if (kind != "digest") assertEquals("invalid metadata was adopted", PluginInstallJournal.State.INVALID, PluginInstallJournal.observe(root).entries.single().state)
                f.owner(root).use { owner -> owner.maintenanceRoot().use { parent -> rejected { PluginInstallJournal.openAbandoned(parent, ref).close() } } }
                assertTrue(File(root, "${ref.name}/partial").isFile); assertTrue(File(root, ref.recordName).isFile)
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun replacedStageCannotInheritOwnershipAndConflictTargetIsPreserved() {
        for (replace in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val ref = f.create(root); val held = File(root, "preserved")
                File(root, ref.target).writeText("unrelated existing slot")
                if (replace) { check(File(root, ref.name).renameTo(held)); File(root, ref.name).apply { check(mkdir()); Os.chmod(path, 448) } }
                assertEquals(if (replace) PluginInstallJournal.State.CHANGED else PluginInstallJournal.State.ABANDONED, PluginInstallJournal.observe(root).entries.single().state)
                if (replace) f.owner(root).use { owner -> owner.maintenanceRoot().use { rejected { PluginInstallJournal.openAbandoned(it, ref).close() } } }
                assertEquals("unrelated existing slot", File(root, ref.target).readText())
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun publishedAndAbsentRecordsCanOnlyRetireMetadataAndNeedCurrentDigest() {
        for (published in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val ref = f.create(root)
                if (published) check(File(root, ref.name).renameTo(File(root, ref.target))) else PluginInstallTestFiles.clean(File(root, ref.name))
                assertEquals(if (published) PluginInstallJournal.State.PUBLISHED else PluginInstallJournal.State.ABSENT, PluginInstallJournal.observe(root).entries.single().state)
                f.owner(root).use {
                    rejected { PluginInstallJournal.retire(it, ref.copy(digest = "f".repeat(64))) }
                    assertTrue(PluginInstallJournal.retire(it, ref))
                }
                assertFalse(File(root, ref.recordName).exists()); assertEquals(published, File(root, ref.target).isDirectory)
                if (published) assertEquals("unfinished public package bytes", File(root, "${ref.target}/partial").readText())
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun activeOrAbandonedOrUnconfirmedDirectoriesCannotLoseTheirOwnershipRecord() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root)
            f.owner(root).use { rejected { PluginInstallJournal.retire(it, ref) } }
            assertTrue(File(root, ref.recordName).exists()); assertTrue(File(root, ref.name).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun corruptRecordsAreUnknownAndCountTowardBoundedEnumeration() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root); val file = File(root, ref.recordName)
            Os.chmod(file.path, 384); file.writeText("{\"schema\":2,\"schema\":2}"); Os.chmod(file.path, 256)
            assertEquals(PluginInstallJournal.State.INVALID, PluginInstallJournal.observe(root).entries.single().state)
            repeat(64) { File(root, ".install-bad-$it").writeText("invalid") }
            rejected { PluginInstallJournal.observe(root) }; assertTrue(File(root, ref.name).isDirectory)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun retirementSyncFailureKeepsActualRemovalVisible() {
        val root = PluginInstallTestFiles.root()
        try {
            val ref = f.create(root, recorded = false, child = false)
            f.owner(root).use { assertFalse(PluginInstallJournal.retire(it, ref, sync = { throw IOException("controlled sync") })) }
            assertFalse(File(root, ref.recordName).exists()); assertTrue(PluginInstallJournal.observe(root).entries.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
}
