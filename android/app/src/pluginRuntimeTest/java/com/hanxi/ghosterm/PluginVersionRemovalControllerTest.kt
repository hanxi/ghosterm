package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import android.system.OsConstants
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginVersionRemovalControllerTest {
    private val fixture = PluginVersionSelectionFixture
    private fun revision(root: File) = checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().revision }
    private suspend fun preview(owner: ExtensionController, root: File, slot: String) = owner.previewVersionRemoval(UUID.randomUUID().toString(), slot, revision(root))
    private suspend fun select(owner: ExtensionController, root: File, slot: String): ExtensionController.VersionSelectionResult {
        val view = owner.previewInstalledVersion(UUID.randomUUID().toString(), slot, revision(root), fixture.policy)
        return owner.confirmInstalledVersion(view.id, true)
    }
    private suspend fun rejected(body: suspend () -> Unit) { assertTrue("operation unexpectedly succeeded", runCatching { body() }.exceptionOrNull() is Exception) }
    @Test fun previewNeedsNoPolicyOrSourceGrantButDeletionRequiresItsExactTokenAndAcknowledgement() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val install = fixture.install(owner, root); val slot = install.publication.releaseID
            val view = preview(owner, root, slot)
            assertTrue(view.members.any { it.path == "release.sig" }); assertFalse(view.pending)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
            rejected { owner.confirmVersionRemoval(view.id, false) }
            rejected { owner.confirmVersionRemoval(UUID.randomUUID().toString(), true) }
            assertTrue(File(root, slot).isDirectory)
            val result = owner.confirmVersionRemoval(view.id, true)
            assertTrue(result.toString(), result.complete); assertTrue(result.durabilityConfirmed); assertEquals(view.members.size + 1, result.removed)
            assertNull(owner.pendingVersionRemoval()); assertFalse(File(root, slot).exists())
            rejected { owner.confirmVersionRemoval(view.id, true) }
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun cancellationCloseAndOverlappingSelectionCannotLeakOrReplacePreviewReferences() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val slot = fixture.install(owner, root).publication.releaseID
            val view = preview(owner, root, slot)
            rejected { preview(owner, root, slot) }
            rejected { owner.previewInstalledVersion(UUID.randomUUID().toString(), slot, revision(root), fixture.policy) }
            LocalPublicationFile.directory(root, slot).use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.pin(it.fd)) }
            owner.cancelRemovalPreview(view.id); owner.installedVersions()
            LocalPublicationFile.directory(root, slot).use { assertEquals(0, PluginInstallFiles.pin(it.fd)) }
            rejected { owner.confirmVersionRemoval(view.id, true) }
            preview(owner, root, slot); owner.close()
            LocalPublicationFile.directory(root, slot).use { assertEquals(0, PluginInstallFiles.pin(it.fd)) }
            assertTrue(File(root, slot).isDirectory); assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists())
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun disabledVersionRemovalForgetsOnlyItsOwnDigestAndNeverRestoresDevelopmentFallback() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
        try {
            val a = fixture.install(owner, root); val b = fixture.install(owner, root, "2.0.0")
            val first = select(owner, root, a.publication.releaseID); val old = first.review!!
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(old.id, old.digest))
            val second = select(owner, root, b.publication.releaseID); val current = second.review!!
            assertEquals(ExtensionRegistry.Status.OK, owner.approve(current.id, current.digest))
            val removal = preview(owner, root, a.publication.releaseID)
            assertTrue(owner.confirmVersionRemoval(removal.id, true).complete)
            assertEquals(ExtensionRegistry.Status.STALE, owner.resolve("fixture", old.id, old.digest).status)
            assertEquals(ExtensionRegistry.Status.OK, owner.resolve("fixture", current.id, current.digest).status)
            rejected { preview(owner, root, b.publication.releaseID) }
            owner.disableInstalledVersion(current.id, revision(root), true)
            val last = preview(owner, root, b.publication.releaseID)
            assertTrue(owner.confirmVersionRemoval(last.id, true).complete)
            assertEquals(ExtensionRegistry.Status.STALE, owner.resolve("fixture", current.id, current.digest).status)
            val choice = checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().choices.getValue(current.id) }
            assertFalse(choice.enabled); assertEquals(b.publication.releaseID, choice.releaseID)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun pendingRecordIsReadOnlyAcrossControllerRestartAndRequiresSeparateResumeAcknowledgement() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val first = fixture.controller(root); var second: ExtensionController? = null
        try {
            val slot = fixture.install(first, root).publication.releaseID; val view = preview(first, root, slot)
            val result = first.confirmVersionRemoval(view.id, true, checkpoint = { if (it == "after-unlink") throw IOException("controlled interruption") })
            assertFalse(result.complete); assertEquals(1, result.removed)
            val pending = first.pendingVersionRemoval()!!; val raw = File(root, PluginReleaseCatalog.REMOVAL).readBytes()
            first.close(); val owner = fixture.controller(root); second = owner
            assertEquals(pending, owner.pendingVersionRemoval()); owner.installedVersions()
            assertArrayEquals(raw, File(root, PluginReleaseCatalog.REMOVAL).readBytes())
            rejected { owner.resumeVersionRemoval(pending.id, false) }
            rejected { owner.resumeVersionRemoval(UUID.randomUUID().toString(), true) }
            rejected { owner.cancelPendingVersionRemoval(pending.id, true) }
            val resumed = owner.resumeVersionRemoval(pending.id, true)
            assertTrue(resumed.toString(), resumed.complete); assertEquals(1, resumed.observedAbsent); assertNull(owner.pendingVersionRemoval())
        } finally { first.close(); second?.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun cancellationBeforeIntentWritesNothingAndAfterIntentPreservesActualOutcome() = runBlocking<Unit> {
        for (phase in listOf("before-publish-intent", "after-intent")) {
            val root = PluginInstallTestFiles.root(); val owner = fixture.controller(root)
            try {
                val slot = fixture.install(owner, root).publication.releaseID; val view = preview(owner, root, slot)
                val job = launch(Dispatchers.Default) {
                    val caller = currentCoroutineContext()
                    owner.confirmVersionRemoval(view.id, true, checkpoint = { if (it == phase) caller.cancel() })
                }
                job.join(); assertTrue(job.isCancelled)
                assertTrue(File(root, slot).isDirectory)
                val pending = owner.pendingVersionRemoval()
                if (phase == "before-publish-intent") assertNull(pending)
                else {
                    assertNotNull(pending); rejected { owner.cancelPendingVersionRemoval(pending!!.id, false) }
                    assertTrue(owner.cancelPendingVersionRemoval(pending!!.id, true)); assertNull(owner.pendingVersionRemoval())
                }
            } finally { owner.close(); PluginInstallTestFiles.clean(root) }
        }
    }
}
