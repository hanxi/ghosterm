package com.hanxi.ghosterm

import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class PluginPolicyReviewControllerTest {
    private val f = PluginVersionSelectionFixture
    private fun revoked() = JSONObject(String(f.policy)).also { it.getJSONArray("revokedKeyIds").put(it.getJSONArray("keys").getJSONObject(0).getString("id")) }.toString().toByteArray()
    private fun choice(root: File, id: String, slot: String) = checkNotNull(PluginReleaseCatalog.open(root)).use { it.change(id, slot, true, it.selection().revision) }
    @Test fun actualCoreSignedAcceptanceNeitherRegistersSourceNorRetainsPackageOrDirectoryLease() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = f.controller(root)
        try {
            val installed = f.install(owner, root); choice(root, installed.snapshot.review.id, installed.publication.releaseID)
            val before = File(root, PluginReleaseCatalog.SELECTION).readBytes()
            val view = owner.previewPolicyReview(UUID.randomUUID().toString(), f.policy)
            assertEquals(PluginPolicyReview.State.ACCEPTED, view.entries.single().state)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
            assertEquals(0, LocalPublicationFile.directory(root, installed.publication.releaseID).use { PluginInstallFiles.lock(it.fd) })
            assertArrayEquals(before, File(root, PluginReleaseCatalog.SELECTION).readBytes())
            assertTrue(runCatching { owner.applyPolicyReview(view.id, listOf(installed.snapshot.review.id), f.policy, true) }.isFailure)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun revokedPolicyExplicitDisablePersistsAcrossControllersAndDoesNotDeleteOrGrantAnything() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); var owner = f.controller(root)
        try {
            val installed = f.install(owner, root); choice(root, installed.snapshot.review.id, installed.publication.releaseID)
            val policy = revoked(); val view = owner.previewPolicyReview(UUID.randomUUID().toString(), policy)
            assertEquals(PluginPolicyReview.State.REJECTED, view.entries.single().state)
            val result = owner.applyPolicyReview(view.id, listOf(installed.snapshot.review.id), policy, true)
            assertFalse(result.observation.selection!!.choices.getValue(installed.snapshot.review.id).enabled)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
            assertTrue(File(root, installed.publication.releaseID).isDirectory)
            owner.close(); owner = f.controller(root)
            assertTrue(owner.previewPolicyReview(UUID.randomUUID().toString(), policy).entries.isEmpty())
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun tamperedPayloadCannotBorrowOldSignatureAndReadFailureStaysUnknown() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = f.controller(root)
        try {
            val installed = f.install(owner, root); choice(root, installed.snapshot.review.id, installed.publication.releaseID)
            val file = File(root, "${installed.publication.releaseID}/payload/main.lua")
            Os.chmod(file.path, 384); file.appendText("\n// controlled payload change\n"); Os.chmod(file.path, 256)
            val view = owner.previewPolicyReview(UUID.randomUUID().toString(), f.policy)
            assertEquals("old signature bypassed actual Core capture", PluginPolicyReview.State.UNKNOWN, view.entries.single().state)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun validSignatureWithWrongCoreIdentityIsRejectedRatherThanAcceptedOrUnknown() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = f.controller(root)
        try {
            val slot = f.identityMismatch(root); choice(root, "org.ghosterm.signature", slot)
            val view = owner.previewPolicyReview(UUID.randomUUID().toString(), f.policy)
            assertEquals(PluginPolicyReview.State.REJECTED, view.entries.single().state)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun knownTicketNeedsAcknowledgementAndRejectsWrongIDCancellationAndReplay() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = f.controller(root)
        try {
            val installed = f.install(owner, root); choice(root, installed.snapshot.review.id, installed.publication.releaseID)
            val policy = revoked(); val view = owner.previewPolicyReview(UUID.randomUUID().toString(), policy); val ids = listOf(installed.snapshot.review.id)
            assertTrue(runCatching { owner.applyPolicyReview(view.id, ids, policy, false) }.isFailure)
            assertTrue(runCatching { owner.applyPolicyReview(UUID.randomUUID().toString(), ids, policy, true) }.isFailure)
            owner.cancelPolicyReview(view.id); owner.installedVersions()
            assertTrue(runCatching { owner.applyPolicyReview(view.id, ids, policy, true) }.isFailure)
            val fresh = owner.previewPolicyReview(UUID.randomUUID().toString(), policy)
            assertTrue(owner.applyPolicyReview(fresh.id, ids, policy, true).durabilityConfirmed)
            assertTrue(runCatching { owner.applyPolicyReview(fresh.id, ids, policy, true) }.isFailure)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun policyChangesAndAnotherControllersRevisionCannotReuseOldDecision() = runBlocking<Unit> {
        val root = PluginInstallTestFiles.root(); val owner = f.controller(root); val other = f.controller(root)
        try {
            val installed = f.install(owner, root); choice(root, installed.snapshot.review.id, installed.publication.releaseID)
            val policy = revoked(); val ids = listOf(installed.snapshot.review.id)
            val first = owner.previewPolicyReview(UUID.randomUUID().toString(), policy)
            assertTrue(runCatching { owner.applyPolicyReview(first.id, ids, policy + byteArrayOf(10), true) }.isFailure)
            val second = owner.previewPolicyReview(UUID.randomUUID().toString(), policy)
            val revision = other.installedVersions().selection!!.revision
            other.disableInstalledVersion(installed.snapshot.review.id, revision, true)
            assertTrue(runCatching { owner.applyPolicyReview(second.id, ids, policy, true) }.isFailure)
        } finally { other.close(); owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun cancellationDuringIndependentCoreInspectionCannotInstallLateTicketOrLeakReferences() = runBlocking<Unit> {
        for (cancelCaller in listOf(false, true)) {
            val root = PluginInstallTestFiles.root(); val owner = f.controller(root); val entered = CountDownLatch(1); val release = CountDownLatch(1); val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
            try {
                val installed = f.install(owner, root); choice(root, installed.snapshot.review.id, installed.publication.releaseID)
                val id = UUID.randomUUID().toString()
                val pending = scope.async { owner.previewPolicyReview(id, f.policy) { entered.countDown(); check(release.await(5, TimeUnit.SECONDS)) } }
                assertTrue("inspection did not reach owned barrier", entered.await(5, TimeUnit.SECONDS))
                if (cancelCaller) pending.cancel() else { owner.cancelPolicyReview(id); assertTrue(runCatching { owner.installedVersions() }.isFailure) }
                release.countDown(); assertTrue(runCatching { pending.await() }.isFailure)
                assertTrue(runCatching { owner.applyPolicyReview(id, listOf(installed.snapshot.review.id), f.policy, true) }.isFailure)
                val next = owner.previewPolicyReview(UUID.randomUUID().toString(), f.policy)
                assertEquals(PluginPolicyReview.State.ACCEPTED, next.entries.single().state)
                owner.cancelPolicyReview(next.id); owner.installedVersions()
                assertEquals(0, LocalPublicationFile.directory(root, installed.publication.releaseID).use { PluginInstallFiles.lock(it.fd) })
            } finally { release.countDown(); scope.cancel(); owner.close(); PluginInstallTestFiles.clean(root) }
        }
    }
}
