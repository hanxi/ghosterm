package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginUnclassifiedControllerTest {
    @Test fun missingRootObservationDoesNotCreateAnything() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val missing = File(root, "missing"); val owner = PluginVersionSelectionFixture.controller(missing)
        try { assertTrue(owner.unclassifiedDirectories().isEmpty()); assertFalse(missing.exists()) }
        finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun actualInstalledVersionRemainsInKnownFlowAndUnknownCleanupNeverLoadsSource() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = PluginVersionSelectionFixture.controller(root)
        try {
            val installed = PluginVersionSelectionFixture.install(owner, root)
            val folder = PluginUnknownFixture.directory(root); val rows = owner.unclassifiedDirectories()
            assertEquals(listOf(folder.name), rows.map { it.name })
            val preview = owner.previewUnclassifiedRemoval(UUID.randomUUID().toString(), rows.single().reference!!)
            assertTrue(runCatching { owner.confirmVersionRemoval(preview.id, true) }.isFailure)
            assertTrue(folder.exists()); assertFalse(File(root, PluginReleaseCatalog.REMOVAL).exists())
            val next = owner.previewUnclassifiedRemoval(UUID.randomUUID().toString(), owner.unclassifiedDirectories().single().reference!!)
            val result = owner.confirmVersionRemoval(next.id, true, acknowledgedUnclassified = true)
            assertTrue(result.toString(), result.complete); assertTrue(result.forgottenPackages.isEmpty())
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve(installed.snapshot.review.commands.first().path).status)
            assertTrue(File(root, installed.publication.releaseID).isDirectory); assertTrue(owner.unclassifiedDirectories().isEmpty())
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun cancelAndStaleTokenCannotAuthorizeAnotherUnknownDirectory() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = PluginVersionSelectionFixture.controller(root)
        try {
            val folder = PluginUnknownFixture.directory(root); val ref = owner.unclassifiedDirectories().single().reference!!
            val first = owner.previewUnclassifiedRemoval(UUID.randomUUID().toString(), ref)
            owner.cancelRemovalPreview(first.id); owner.unclassifiedDirectories()
            assertTrue(runCatching { owner.confirmVersionRemoval(first.id, true, acknowledgedUnclassified = true) }.isFailure)
            val next = owner.previewUnclassifiedRemoval(UUID.randomUUID().toString(), ref)
            assertTrue(runCatching { owner.confirmVersionRemoval(UUID.randomUUID().toString(), true, acknowledgedUnclassified = true) }.isFailure)
            owner.cancelRemovalPreview(next.id); owner.unclassifiedDirectories(); assertTrue(folder.exists())
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun restartedControllerObservesIntentWithoutReplayAndRequiresExtraResumeAck() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); var owner = PluginVersionSelectionFixture.controller(root)
        try {
            val folder = PluginUnknownFixture.directory(root); val ref = owner.unclassifiedDirectories().single().reference!!
            val first = owner.previewUnclassifiedRemoval(UUID.randomUUID().toString(), ref)
            assertFalse(owner.confirmVersionRemoval(first.id, true, { if (it == "after-unlink") throw IOException("controlled") }, acknowledgedUnclassified = true).complete)
            owner.close(); owner = PluginVersionSelectionFixture.controller(root)
            val before = root.list()!!.toSet(); val saved = owner.pendingVersionRemoval()!!; assertEquals(ref, saved.unclassified)
            assertEquals(before, root.list()!!.toSet()); assertTrue(runCatching { owner.resumeVersionRemoval(saved.id, true) }.isFailure)
            assertEquals(before, root.list()!!.toSet())
            assertTrue(owner.resumeVersionRemoval(saved.id, true, acknowledgedUnclassified = true).complete); assertTrue(root.list()!!.isEmpty())
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
}
