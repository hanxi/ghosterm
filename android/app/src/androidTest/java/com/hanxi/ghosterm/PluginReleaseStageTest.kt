package com.hanxi.ghosterm

import android.os.Build
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginReleaseCapacityTest {
    @Test fun fullFilesystemPreservesInstalledVersionAndDiscardsIncompleteStage() {
        val arguments = InstrumentationRegistry.getArguments()
        org.junit.Assume.assumeTrue(arguments.getString("ownedInstallCapacityAvd") == "true")
        check(Build.HARDWARE == "ranchu") { "Storage exhaustion requires an owned emulator" }
        val fixture = PluginInstallTestFiles.root()
        val root = File(fixture, "store")
        val priorID = "a".repeat(64)
        val candidateID = "b".repeat(64)
        val filler = File(fixture, "capacity-reservation")
        fun available(): Long = Os.statvfs(fixture.path).let { it.f_bavail * it.f_frsize }
        try {
            val stats = Os.statvfs(fixture.path)
            val capacity = stats.f_blocks * stats.f_frsize
            check(capacity in 1..3L * 1024 * 1024 * 1024) {
                "The fixture requires a bounded emulator data partition, found $capacity bytes"
            }
            PluginReleaseStage.create(root, priorID).use {
                it.write("payload/prior", "installed version".toByteArray())
                assertTrue(it.publish().durabilityConfirmed)
            }
            PluginReleaseStage.create(root, candidateID).use { candidate ->
                candidate.write("payload/complete", byteArrayOf(1, 2, 3))
                try {
                    val fd = Os.open(filler.path, OsConstants.O_WRONLY or OsConstants.O_CREAT or OsConstants.O_EXCL, 384)
                    try {
                        val reserve = 2L * 1024 * 1024
                        val size = available() - reserve
                        check(size > reserve)
                        Os.posix_fallocate(fd, 0, size)
                        assertTrue(available() < 8L * 1024 * 1024)
                        val failure = runCatching { candidate.write("payload/incomplete", ByteArray(8 * 1024 * 1024)) }
                            .exceptionOrNull()
                        assertEquals("candidate write must reach real ENOSPC", OsConstants.ENOSPC, (failure as? ErrnoException)?.errno)
                    } finally { Os.close(fd) }
                } finally { check(filler.delete() || !filler.exists()) }
            }
            assertEquals(listOf(priorID), root.list()!!.toList())
            assertEquals("installed version", File(root, "$priorID/payload/prior").readText())
        } finally { PluginInstallTestFiles.clean(fixture) }
    }
}

@RunWith(AndroidJUnit4::class)
class PluginReleaseStageTest {
    private val id = "a".repeat(64)
    private fun stage(root: File) = root.listFiles()!!.single { it.name.startsWith(".prepare-") }
    private fun journal(root: File) = root.listFiles()!!.single { it.name.startsWith(".install-") }
    private fun rejected(block: () -> Unit) { try { block(); fail("unsafe install operation accepted") } catch (_: Exception) { } }
    @Test fun publishesCompleteReadonlyTreeAndRetiresIntent() {
        val root = PluginInstallTestFiles.root()
        try {
            val attempt = runCatching { PluginReleaseStage.create(root, id).use { stage ->
                stage.write("payload/client/index.js", "public code".toByteArray(), executable = true)
                stage.write("release.json", "public descriptor".toByteArray())
                stage.publish()
            } }
            assertTrue("owned atomic publication failed: ${attempt.exceptionOrNull()}", attempt.isSuccess)
            val result = attempt.getOrThrow()
            assertTrue(result.durabilityConfirmed); assertNull(result.maintenanceIssue)
            assertEquals(listOf(id), root.list()!!.toList())
            assertEquals("public code", File(result.directory, "payload/client/index.js").readText())
            for (path in listOf(result.directory, File(result.directory, "payload"), File(result.directory, "payload/client"), File(result.directory, "payload/client/index.js")))
                assertEquals(320, Os.lstat(path.path).st_mode and 511)
            assertEquals(256, Os.lstat(File(result.directory, "release.json").path).st_mode and 511)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun ordinaryCloseDiscardsOnlyOwnedStageAndJournal() {
        val root = PluginInstallTestFiles.root()
        try {
            File(root, "unrelated").writeText("preserve")
            PluginReleaseStage.create(root, id).use { it.write("payload/a/b", ByteArray(128)) }
            assertEquals(listOf("unrelated"), root.list()!!.toList()); assertEquals("preserve", File(root, "unrelated").readText())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun rootEntrySyncMustSucceedBeforeReservationIncludingRetryOfExistingRoot() {
        val owner = PluginInstallTestFiles.root(); val root = File(owner, "new-install-root"); var calls = 0
        try {
            repeat(2) {
                val failure = runCatching { PluginReleaseStage.create(root, id, rootSync = { ancestor ->
                    calls++
                    assertEquals("sync must pin the root's actual parent", Os.lstat(owner.path).st_ino, Os.fstat(ancestor).st_ino)
                    assertTrue("reservation preceded root entry sync", root.list()!!.isEmpty())
                    throw java.io.IOException("controlled root entry sync failure")
                }).close() }.exceptionOrNull()
                assertTrue("unsynced root entry was accepted", failure is java.io.IOException)
                assertTrue(root.isDirectory); assertTrue(root.list()!!.isEmpty())
            }
            PluginReleaseStage.create(root, id, rootSync = { calls++; Os.fsync(it) }).use { it.write("payload/a", byteArrayOf(1)); it.publish() }
            assertEquals(3, calls); assertTrue(File(root, "$id/payload/a").isFile)
        } finally { PluginInstallTestFiles.clean(owner) }
    }
    @Test fun durableReservationPrecedesDirectoryCreationAndBindsIdentity() {
        val root = PluginInstallTestFiles.root(); var reserved = false; var bound = false
        try {
            PluginReleaseStage.create(root, id, { phase ->
                if (phase == "after-reservation") {
                    assertTrue("durable intent missing before mkdir", root.list()!!.any { it.startsWith(".install-") })
                    val value = JSONObject(journal(root).readText())
                    assertTrue(value.isNull("directory")); assertEquals(id, value.getString("target"))
                    assertFalse(root.list()!!.any { it.startsWith(".prepare-") }); reserved = true
                }
                if (phase == "after-directory-record") {
                    val saved = JSONObject(journal(root).readText())
                    assertFalse("created directory identity was not recorded", saved.isNull("directory"))
                    val actual = Os.lstat(stage(root).path); val value = saved.getJSONObject("directory")
                    assertEquals(actual.st_ino.toString(), value.getString("inode")); assertEquals(actual.st_dev.toString(), value.getString("device")); bound = true
                }
            }).use { it.write("payload/a", byteArrayOf(1)) }
            assertTrue(reserved && bound); assertTrue(root.list()!!.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun secondOwnerCannotEnterWhileParentLockIsHeld() {
        val root = PluginInstallTestFiles.root()
        try {
            PluginReleaseStage.create(root, id).use { first ->
                try { PluginReleaseStage.create(root, "b".repeat(64)).close(); fail("concurrent owner entered") }
                catch (e: PluginReleaseStage.Rejected) { assertEquals(PluginReleaseStage.Reason.BUSY, e.reason) }
                first.write("payload/a", byteArrayOf(1))
            }
            PluginReleaseStage.create(root, "b".repeat(64)).close(); assertTrue(root.list()!!.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun existingEmptyDirectoryFileAndDanglingLinkAreNeverOverwritten() {
        for (kind in listOf("directory", "file", "link")) {
            val root = PluginInstallTestFiles.root(); val destination = File(root, id)
            try {
                when (kind) { "directory" -> check(destination.mkdir()); "file" -> destination.writeText("preserve"); else -> Os.symlink("missing-target", destination.path) }
                val original = Os.lstat(destination.path)
                PluginReleaseStage.create(root, id).use { candidate ->
                    candidate.write("payload/a", byteArrayOf(1))
                    try { candidate.publish(); fail("existing slot overwritten") }
                    catch (e: PluginReleaseStage.Rejected) { assertEquals(PluginReleaseStage.Reason.CONFLICT, e.reason) }
                }
                assertEquals(original.st_ino, Os.lstat(destination.path).st_ino)
                assertEquals(listOf(id), root.list()!!.toList())
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun parentSyncFailureAfterRenameKeepsPublishedTreeAndIntent() {
        val root = PluginInstallTestFiles.root()
        try {
            val attempt = runCatching { PluginReleaseStage.create(root, id, parentSync = { throw java.io.IOException("controlled fsync failure") }).use { candidate ->
                candidate.write("payload/a", byteArrayOf(1)); candidate.publish()
            } }
            assertTrue("published receipt was lost during disposal: ${attempt.exceptionOrNull()}", attempt.isSuccess)
            val result = attempt.getOrThrow()
            assertFalse(result.durabilityConfirmed); assertNotNull(result.maintenanceIssue)
            assertTrue(File(root, "$id/payload/a").isFile)
            assertEquals(id, JSONObject(journal(root).readText()).getString("target"))
            assertFalse(root.list()!!.any { it.startsWith(".prepare-") })
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun thrownReceiptAfterRenameCannotRollbackPublication() {
        val root = PluginInstallTestFiles.root()
        try {
            val result = PluginReleaseStage.create(root, id, { if (it == "after-rename") throw java.io.IOException("receipt fault") }).use {
                it.write("payload/a", byteArrayOf(1)); it.publish()
            }
            assertFalse(result.durabilityConfirmed); assertTrue(File(root, "$id/payload/a").isFile); assertTrue(journal(root).isFile)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun replacedPublishedVersionRetainsInstallIntent() {
        val root = PluginInstallTestFiles.root()
        val destination = File(root, id)
        val detached = File(root, "detached")
        try {
            val result = PluginReleaseStage.create(root, id, checkpoint = { phase ->
                if (phase == "after-publication-sync") {
                    Os.rename(destination.path, detached.path)
                    Os.mkdir(destination.path, 448)
                }
            }).use {
                it.write("payload/a", byteArrayOf(1))
                it.publish()
            }
            assertFalse("A replaced version cannot report durable installation", result.durabilityConfirmed)
            assertEquals(id, JSONObject(journal(root).readText()).getString("target"))
            assertArrayEquals(byteArrayOf(1), File(detached, "payload/a").readBytes())
            assertTrue(destination.list().orEmpty().isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun writeFaultAndPreRenameCancellationCleanOwnedState() {
        for (phase in listOf("after-reservation", "after-directory-record", "after-file-create", "after-file-write", "before-rename")) {
            val root = PluginInstallTestFiles.root()
            try {
                rejected { PluginReleaseStage.create(root, id, { if (it == phase) throw java.io.IOException("controlled $phase") }).use {
                    it.write("payload/a", byteArrayOf(1)); it.publish()
                } }
                assertTrue("owned state survived $phase", root.list()!!.isEmpty())
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun changedFileAndUnknownEntryArePreservedAndNeverPublished() {
        for (change in listOf("content", "identity", "extra", "link")) {
            val root = PluginInstallTestFiles.root(); var changed: File? = null
            try {
                rejected { PluginReleaseStage.create(root, id, { phase -> if (phase == "before-rename") {
                    val payload = File(stage(root), "payload")
                    when (change) {
                        "content" -> File(payload, "a").also { Os.chmod(it.path, 384); it.writeText("changed"); Os.chmod(it.path, 256); changed = it }
                        "identity" -> {
                            Os.chmod(payload.path, 448)
                            val replacement = File(payload, "replacement")
                            replacement.writeBytes(byteArrayOf(1))
                            Os.chmod(replacement.path, 256)
                            changed = File(payload, "a").also { Os.rename(replacement.path, it.path) }
                        }
                        "extra" -> File(payload, "extra").also { Os.chmod(payload.path, 448); it.writeText("unknown"); Os.chmod(it.path, 256); changed = it }
                        else -> File(payload, "link").also { Os.chmod(payload.path, 448); Os.symlink("a", it.path); changed = it }
                    }
                    Os.chmod(payload.path, 320)
                } }).use { it.write("payload/a", byteArrayOf(1)); it.publish() } }
                assertFalse(File(root, id).exists()); assertNotNull(changed); assertTrue(journal(root).isFile)
                assertTrue(java.nio.file.Files.exists(changed!!.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS))
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun missingKnownFileCannotPublishEvenWhenAllRemainingEntriesAreSafe() {
        val root = PluginInstallTestFiles.root()
        try {
            rejected { PluginReleaseStage.create(root, id, { if (it == "before-rename") {
                val payload = File(stage(root), "payload")
                Os.chmod(payload.path, 448)
                assertTrue("fixture failed to remove known file", File(payload, "a").delete())
                Os.chmod(payload.path, 320)
            } }).use { it.write("payload/a", byteArrayOf(1)); it.publish() } }
            assertFalse(File(root, id).exists()); assertTrue(journal(root).isFile)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun changedMemberDuringFinalAuthorizationCannotPublish() {
        val root = PluginInstallTestFiles.root()
        try {
            var member: File? = null
            val failure = runCatching {
                PluginReleaseStage.create(root, id).use { candidate ->
                    candidate.write("payload/a", byteArrayOf(1))
                    candidate.publish { rename ->
                        val file = File(stage(root), "payload/a")
                        Os.chmod(file.path, 384)
                        file.writeBytes(byteArrayOf(2))
                        Os.chmod(file.path, 256)
                        member = file
                        rename()
                    }
                }
            }.exceptionOrNull()
            assertTrue("A member changed during final authorization must not publish: $failure", failure is PluginReleaseStage.Rejected)
            assertFalse(File(root, id).exists())
            assertTrue(journal(root).isFile)
            assertArrayEquals(byteArrayOf(2), checkNotNull(member).readBytes())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun writableNestedDirectoryAfterFreezeIsRejected() {
        val root = PluginInstallTestFiles.root()
        try {
            rejected { PluginReleaseStage.create(root, id, { if (it == "before-rename") Os.chmod(File(stage(root), "payload").path, 448) }).use {
                it.write("payload/a", byteArrayOf(1)); it.publish()
            } }
            assertFalse(File(root, id).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun replacementRootAndStageCannotRedirectPublicationOrCleanup() {
        for (replaceRoot in listOf(false, true)) {
            val root = PluginInstallTestFiles.root(); val held = File(root.parentFile, "held-${UUID.randomUUID()}")
            try {
                rejected { PluginReleaseStage.create(root, id, { if (it == "before-rename") {
                    val original = if (replaceRoot) root else stage(root)
                    // Moving a directory across parents updates its '..' link;
                    // the fixture must first allow that write on a frozen tree.
                    Os.chmod(original.path, 448)
                    assertTrue("fixture failed to move selected directory", original.renameTo(held))
                    assertTrue("fixture failed to create replacement", original.mkdir())
                    File(original, "preserve").writeText("unknown")
                } }).use { it.write("payload/a", byteArrayOf(1)); it.publish() } }
                assertFalse(File(root, id).exists()); assertFalse(File(held, id).exists())
                val replacement = if (replaceRoot) root else root.listFiles()!!.single { it.name.startsWith(".prepare-") }
                assertEquals("unknown", File(replacement, "preserve").readText())
            } finally { PluginInstallTestFiles.clean(root); PluginInstallTestFiles.clean(held) }
        }
    }
    @Test fun changedIntentPreventsDeletionOfAnyStage() {
        val root = PluginInstallTestFiles.root()
        try {
            val candidate = PluginReleaseStage.create(root, id)
            candidate.write("payload/a", byteArrayOf(1))
            val record = journal(root); Os.chmod(record.path, 384); record.writeText("changed intent")
            rejected { candidate.close() }
            assertTrue(File(stage(root), "payload/a").isFile); assertEquals("changed intent", record.readText())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun unknownRootEntriesCountAgainstCapacity() {
        val root = PluginInstallTestFiles.root()
        try {
            repeat(64) { File(root, "unknown-$it").writeText("preserve") }
            try { PluginReleaseStage.create(root, id).close(); fail("capacity bypassed") }
            catch (e: PluginReleaseStage.Rejected) { assertEquals(PluginReleaseStage.Reason.LIMIT, e.reason) }
            assertEquals(64, root.list()!!.size)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun privateRootRejectsWorldAccessibleDirectoryAndFinalSymlink() {
        val root = PluginInstallTestFiles.root(); val link = File(root.parentFile, "install-link-${UUID.randomUUID()}")
        try {
            Os.chmod(root.path, 493); rejected { PluginReleaseStage.create(root, id).close() }
            Os.chmod(root.path, 448); Os.symlink(root.path, link.path)
            rejected { PluginReleaseStage.create(link, id).close() }; assertTrue(root.list()!!.isEmpty())
        } finally { check(link.delete() || !link.exists()); PluginInstallTestFiles.clean(root) }
    }
    @Test fun repeatedCancelledStagesDoNotLeakBorrowedDescriptors() {
        val root = PluginInstallTestFiles.root()
        try {
            PluginReleaseStage.create(root, id).close()
            val before = File("/proc/self/fd").list()!!.size
            repeat(24) { PluginReleaseStage.create(root, id).use { it.write("payload/a", byteArrayOf(1)) } }
            assertTrue("stage fd leak", File("/proc/self/fd").list()!!.size <= before + 2)
            assertTrue(root.list()!!.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
}
