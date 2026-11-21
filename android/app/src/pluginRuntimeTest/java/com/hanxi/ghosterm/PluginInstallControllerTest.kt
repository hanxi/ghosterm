package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.Base64
import java.util.UUID
import java.util.concurrent.CountDownLatch

@RunWith(AndroidJUnit4::class)
class PluginInstallControllerTest {
    private val instrumentation get() = InstrumentationRegistry.getInstrumentation()
    private val context get() = instrumentation.targetContext
    private val fixture get() = JSONObject(instrumentation.context.assets.open("plugin-release/archive-declarations.json").bufferedReader().use { it.readText() })
    private fun vector(name: String = "stored"): JSONObject {
        val all = fixture.getJSONArray("vectors")
        return (0 until all.length()).map { all.getJSONObject(it) }.single { it.getString("name") == name }
    }
    private fun controller() = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"), listOf("session.rpc")), context)
    private suspend fun <T> archive(name: String = "stored", block: suspend (ParcelFileDescriptor, ByteArray, ByteArray, ByteArray) -> T): T {
        val value = vector(name); val file = File(context.cacheDir, "install-archive-${UUID.randomUUID()}")
        file.writeBytes(Base64.getDecoder().decode(value.getString("archive")))
        try { ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
            return block(fd, value.getString("descriptor").toByteArray(), Base64.getDecoder().decode(value.getString("signature")), fixture.getString("policy").toByteArray())
        } } finally { assertTrue(file.delete() || !file.exists()) }
    }
    @Test fun installsReadonlySignedVersionAndReopensWithFreshExplicitPolicy() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller()
        try { archive { fd, raw, sig, policy ->
            val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
            val installed = owner.installSignedArchive(fd, raw, sig, policy, preview, root)
            assertEquals(preview, installed.snapshot); assertTrue(installed.publication.durabilityConfirmed)
            assertEquals(listOf(preview.proof.descriptorSHA256), root.list()!!.toList())
            for (name in listOf("release.json", "release.sig", "policy.json"))
                assertTrue("signed installation metadata missing: $name", File(installed.publication.directory, name).isFile)
            assertArrayEquals(raw, File(installed.publication.directory, "release.json").readBytes())
            assertArrayEquals(sig, File(installed.publication.directory, "release.sig").readBytes())
            assertArrayEquals(policy, File(installed.publication.directory, "policy.json").readBytes())
            assertEquals(320, Os.lstat(installed.publication.directory.path).st_mode and 511)
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
            owner.close()
            val reopened = controller()
            try {
                val review = reopened.review(File(installed.publication.directory, "payload"))
                assertEquals(preview.review.digest, review.digest); assertFalse(review.sourceApproved)
                assertEquals(preview.proof, reopened.verifyDirectorySignature(review.id, review.digest, raw, sig, policy))
                assertEquals(ExtensionRegistry.Status.UNKNOWN, reopened.resolve("fixture").status)
            } finally { reopened.close() }
        } } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun channelAdvanceAtPublicationRequiresFreshArchiveReview() = runBlocking {
        val root = PluginInstallTestFiles.root()
        val cache = PluginInstallTestFiles.root()
        val channel = JSONObject(instrumentation.context.assets.open("plugin-release/archive-channel.json").bufferedReader().use { it.readText() })
        val store = PluginChannelPolicyStore(cache, channel.getString("channel"), Base64.getDecoder().decode(channel.getString("root")))
        val owner = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"), listOf("session.rpc")),
            context, root, store, channelClock = { 150u })
        try { archive { fd, raw, sig, policy ->
            fun refresh(name: String) {
                val vector = channel.getJSONObject(name)
                store.refresh(vector.getString("statement").toByteArray(), Base64.getDecoder().decode(vector.getString("signature")), policy, 150u)
            }
            refresh("first")
            val preview = owner.inspectSignedArchive(fd, raw, sig, byteArrayOf(), store) { 150u }
            assertNotNull(preview.channelStatementSHA256)
            try {
                owner.installSignedArchive(fd, raw, sig, policy, preview, root)
                fail("offline trust substituted for channel review")
            } catch (e: PluginReleaseSignature.Rejected) { assertEquals(PluginReleaseSignature.Failure.CHANNEL_HISTORY, e.reason) }
            try {
                owner.installSignedArchive(fd, raw, sig, byteArrayOf(), preview, root, checkpoint = {
                    if (it == "before-rename") refresh("second")
                }, channelStore = store, channelClock = { 150u })
                fail("superseded channel review was published")
            } catch (e: PluginReleaseSignature.Rejected) { assertEquals(PluginReleaseSignature.Failure.CHANNEL_HISTORY, e.reason) }
            assertTrue(root.list()!!.isEmpty())
            val current = owner.inspectSignedArchive(fd, raw, sig, byteArrayOf(), store) { 150u }
            assertNotEquals(preview.channelStatementSHA256, current.channelStatementSHA256)
            var publishing = false
            var publicationReads = 0
            try {
                owner.installSignedArchive(fd, raw, sig, byteArrayOf(), current, root, checkpoint = {
                    if (it == "before-rename") publishing = true
                }, channelStore = store, channelClock = {
                    if (publishing && ++publicationReads == 2) 200u else 150u
                })
                fail("channel expiring during publication was accepted")
            } catch (e: PluginReleaseSignature.Rejected) {
                assertEquals(PluginReleaseSignature.Failure.CHANNEL_FRESHNESS, e.reason)
            }
            assertEquals(2, publicationReads)
            assertTrue(root.list()!!.isEmpty())
            val installed = owner.installSignedArchive(fd, raw, sig, byteArrayOf(), current, root,
                channelStore = store, channelClock = { 150u })
            val releaseID = installed.publication.releaseID
            checkNotNull(PluginReleaseCatalog.open(root)).use { catalog ->
                catalog.openVersion(releaseID).use { assertTrue(it.requiresChannelPolicy()) }
            }
            val staleSelection = owner.previewInstalledVersion(UUID.randomUUID().toString(), releaseID, null, byteArrayOf())
            refresh("third")
            try {
                owner.confirmInstalledVersion(staleSelection.id, true)
                fail("superseded channel selection was confirmed")
            } catch (e: PluginReleaseSignature.Rejected) { assertEquals(PluginReleaseSignature.Failure.CHANNEL_HISTORY, e.reason) }
            val currentSelection = owner.previewInstalledVersion(UUID.randomUUID().toString(), releaseID, null, byteArrayOf())
            assertNotEquals(staleSelection.candidate.channelStatementSHA256, currentSelection.candidate.channelStatementSHA256)
            val selected = owner.confirmInstalledVersion(currentSelection.id, true)
            assertTrue(selected.observation.selection!!.choices.getValue(current.review.id).enabled)
        } } finally { owner.close(); PluginInstallTestFiles.clean(root); PluginInstallTestFiles.clean(cache) }
    }

    @Test fun changedPreviewAndDeclaredPermissionsCannotBeInstalled() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller()
        try { archive { fd, raw, sig, policy ->
            val good = owner.inspectSignedArchive(fd, raw, sig, policy)
            for (wrong in listOf(good.copy(review = good.review.copy(version = "2.0.0")),
                good.copy(declarations = good.declarations.copy(permissions = emptyList())))) {
                val error = runCatching { owner.installSignedArchive(fd, raw, sig, policy, wrong, root) }.exceptionOrNull()
                assertTrue("changed preview was accepted", error is IllegalStateException)
                assertTrue(root.list()!!.isEmpty())
            }
        } } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun changedPolicyAndSignatureFailBeforeAnyPersistentReservation() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller()
        try { archive { fd, raw, sig, policy ->
            val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
            val revoked = JSONObject(String(policy)).apply { getJSONArray("revokedKeyIds").put(getJSONArray("keys").getJSONObject(0).getString("id")) }.toString().toByteArray()
            try { owner.installSignedArchive(fd, raw, sig, revoked, preview, root); fail("revoked policy installed") }
            catch (e: PluginReleaseSignature.Rejected) { assertEquals(PluginReleaseSignature.Failure.REVOKED_KEY, e.reason) }
            val altered = sig.copyOf(); altered[0] = (altered[0].toInt() xor 1).toByte()
            try { owner.installSignedArchive(fd, raw, altered, policy, preview, root); fail("invalid signature installed") }
            catch (e: PluginReleaseSignature.Rejected) { assertEquals(PluginReleaseSignature.Failure.SIGNATURE, e.reason) }
            assertTrue(root.list()!!.isEmpty())
        } } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun differentAuthenticatedProofCannotReplaceConfirmedMaterialSnapshot() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller()
        try { archive("stored") { fd, raw, sig, policy ->
            val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
            archive("deflated") { changed, next, signed, trust ->
                var reservations = 0
                val error = runCatching { owner.installSignedArchive(changed, next, signed, trust, preview, root,
                    { if (it == "after-reservation") reservations++ }) }.exceptionOrNull()
                assertTrue("changed authenticated artifact installed", error is IllegalStateException)
                assertEquals("mismatched proof reached persistent reservation", 0, reservations)
                assertTrue(root.list()!!.isEmpty())
            }
        } } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun prePublicationCancellationWaitsForOwnedCleanup() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller(); val entered = CompletableDeferred<Unit>(); val release = CountDownLatch(1)
        try { archive { fd, raw, sig, policy ->
            val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
            val work = launch(Dispatchers.Default) { owner.installSignedArchive(fd, raw, sig, policy, preview, root, { phase ->
                if (phase == "after-file-create") { entered.complete(Unit); release.await() }
            }) }
            try {
                withTimeout(5000) { entered.await() }; work.cancel(); release.countDown()
                withTimeout(5000) { work.join() }
                assertTrue(work.isCancelled); assertTrue("cancelled install left owned state", root.list()!!.isEmpty())
            } finally { release.countDown(); work.cancelAndJoin() }
        } } finally { release.countDown(); owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun failedSyncAfterPublicationIsReportedWithoutRemovingVersion() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller()
        try { archive { fd, raw, sig, policy ->
            val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
            val installed = owner.installSignedArchive(fd, raw, sig, policy, preview, root, parentSync = { throw java.io.IOException("controlled parent sync") })
            assertFalse(installed.publication.durabilityConfirmed); assertNotNull(installed.publication.maintenanceIssue)
            assertTrue(File(installed.publication.directory, "payload/plugin.json").isFile)
            assertTrue(root.list()!!.any { it.startsWith(".install-") })
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
        } } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun repeatInstallCannotReplaceExistingVersionOrInheritSource() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller()
        try { archive { fd, raw, sig, policy ->
            val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
            val installed = owner.installSignedArchive(fd, raw, sig, policy, preview, root)
            val before = Os.lstat(installed.publication.directory.path)
            try { owner.installSignedArchive(fd, raw, sig, policy, preview, root); fail("duplicate install overwrote slot") }
            catch (e: PluginReleaseStage.Rejected) { assertEquals(PluginReleaseStage.Reason.CONFLICT, e.reason) }
            assertEquals(before.st_ino, Os.lstat(installed.publication.directory.path).st_ino)
            assertArrayEquals(raw, File(installed.publication.directory, "release.json").readBytes())
            assertEquals(1, root.list()!!.size); assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("fixture").status)
        } } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun anotherReleasePreservesPreviouslyCapturedPackageAndDirectory() = runBlocking {
        val root = PluginInstallTestFiles.root(); val owner = controller()
        try {
            lateinit var previous: ExtensionController.InstalledRelease
            archive("stored") { fd, raw, sig, policy ->
                val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
                previous = owner.installSignedArchive(fd, raw, sig, policy, preview, root)
            }
            val review = owner.review(File(previous.publication.directory, "payload")); val identity = Os.lstat(previous.publication.directory.path)
            archive("deflated") { fd, raw, sig, policy ->
                val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
                val next = owner.installSignedArchive(fd, raw, sig, policy, preview, root)
                assertNotEquals(previous.publication.releaseID, next.publication.releaseID)
            }
            assertEquals(identity.st_ino, Os.lstat(previous.publication.directory.path).st_ino)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture", review.id, review.digest).status)
            assertEquals(2, root.list()!!.size)
        } finally { owner.close(); PluginInstallTestFiles.clean(root) }
    }
}

@RunWith(AndroidJUnit4::class)
class PluginOfficialReleaseInstallTest {
    @Test fun installsNineCandidatesWithoutApprovingTheirSources() = runBlocking {
        val input = InstrumentationRegistry.getArguments().getString("officialReleaseRoot")
        org.junit.Assume.assumeNotNull(input)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = PluginInstallTestFiles.root()
        val owner = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"),
            listOf("session.rpc", "resource.receive", "file.readScoped", "ui.table", "ui.diff", "ui.task", "ui.views")), context)
        try {
            for (name in listOf("preview", "table", "diff", "task", "views", "logs", "form", "ai-codex", "ai-claude")) {
                val directory = File(input, "official-$name-release")
                val raw = File(directory, "release.json").readBytes()
                val signature = File(directory, "release.sig").readBytes()
                val policy = File(input, "official-$name-policy.json").readBytes()
                ParcelFileDescriptor.open(File(directory, "plugin.zip"), ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                    val preview = owner.inspectSignedArchive(fd, raw, signature, policy)
                    assertEquals("org.ghosterm.$name", preview.review.id)
                    val installed = owner.installSignedArchive(fd, raw, signature, policy, preview, root)
                    assertTrue(installed.publication.durabilityConfirmed)
                    assertFalse(installed.snapshot.review.sourceApproved)
                    val payload = File(installed.publication.directory, "payload/integrity.json")
                    val digest = java.security.MessageDigest.getInstance("SHA-256").digest(payload.readBytes())
                        .joinToString("") { "%02x".format(it.toInt() and 255) }
                    assertEquals(preview.review.digest, digest)
                    val command = preview.review.commands.first().path
                    assertNotEquals(ExtensionRegistry.Status.OK, owner.resolve(command, preview.review.id, preview.review.digest).status)
                }
            }
        } finally {
            owner.close()
            PluginInstallTestFiles.clean(root)
        }
    }
}
