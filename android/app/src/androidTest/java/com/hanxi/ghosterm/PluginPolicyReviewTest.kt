package com.hanxi.ghosterm

import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.IOException
import java.util.Base64
import java.util.UUID

/** Signed metadata + an explicit contract consumer; actual Core is tested separately. */
internal object PluginPolicyFixture {
    val vector get() = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("plugin-release/vector.json").bufferedReader().use { it.readText() })
    val policy get() = vector.getString("policy").toByteArray()
    val identity = PluginReleaseSignature.Identity("org.ghosterm.signature", "1.0.0", "fixture", "a".repeat(64))
    fun changedPolicy(change: (JSONObject) -> Unit) = JSONObject(String(policy)).also(change).toString().toByteArray()
    fun slot(root: File): String {
        val value = vector; val raw = value.getString("descriptor").toByteArray(); val name = PluginReleaseCatalog.sha(raw)
        PluginReleaseStage.create(root, name).use {
            it.write("payload/placeholder", "contract consumer only".toByteArray()); it.write("release.json", raw)
            it.write("release.sig", Base64.getDecoder().decode(value.getString("signature"))); it.write("policy.json", policy); it.publish()
        }
        return name
    }
    fun owner(root: File) = checkNotNull(PluginReleaseCatalog.open(root))
    fun select(root: File, id: String, slot: String, enabled: Boolean = true) = owner(root).use { it.change(id, slot, enabled, it.selection().revision).selection }
    fun review(root: File, policy: ByteArray = this.policy, consume: (Int) -> PluginReleaseSignature.Identity = { identity }) =
        owner(root).use { PluginPolicyReview.observe(it, UUID.randomUUID().toString(), policy, consume) }
    fun reject(message: String = "unsafe policy transaction accepted", action: () -> Unit) { assertTrue(message, runCatching(action).exceptionOrNull() is Exception) }
}

@RunWith(AndroidJUnit4::class)
class PluginPolicyReviewTest {
    private val f = PluginPolicyFixture
    private fun root(body: (File) -> Unit) { val root = PluginInstallTestFiles.root(); try { body(root) } finally { PluginInstallTestFiles.clean(root) } }
    @Test fun policyIsStrictlyValidatedBeforeEvenAnEmptyEnabledList() = root { root ->
        f.reject("empty list bypassed policy validation") { f.review(root, "{}".toByteArray()) }
        f.reject { PluginPolicyReview.observe(null, "empty", byteArrayOf(), { f.identity }) }
        val good = f.review(root); assertTrue(good.value.entries.isEmpty()); assertEquals("fixture", good.value.channel)
        assertTrue(root.list()!!.isEmpty()); assertEquals(PluginReleaseCatalog.sha(f.policy), good.value.policySHA256)
    }
    @Test fun acceptedRejectedAndUnknownRowsStayDistinctAndDisabledChoicesAreNotReviewed() = root { root ->
        val slot = f.slot(root); f.select(root, f.identity.id, slot); f.select(root, "org.ghosterm.alias", slot)
        f.select(root, "org.ghosterm.missing", "b".repeat(64)); f.select(root, "org.ghosterm.disabled", slot, false)
        val before = File(root, PluginReleaseCatalog.SELECTION).readBytes(); var calls = 0
        val policy = f.policy; val originalHash = PluginReleaseCatalog.sha(policy)
        val review = f.review(root, policy) { calls++; policy.fill(0); f.identity }.value
        assertEquals("policy observation did not own its input bytes", originalHash, review.policySHA256)
        assertEquals(2, calls); assertEquals(3, review.entries.size)
        assertEquals(PluginPolicyReview.State.ACCEPTED, review.entries.single { it.pluginID == f.identity.id }.state)
        assertEquals(PluginPolicyReview.State.REJECTED, review.entries.single { it.pluginID == "org.ghosterm.alias" }.state)
        assertEquals(PluginPolicyReview.State.UNKNOWN, review.entries.single { it.pluginID == "org.ghosterm.missing" }.state)
        assertArrayEquals(before, File(root, PluginReleaseCatalog.SELECTION).readBytes())
    }
    @Test fun revokedChannelAndSignatureFailuresArePolicyRejectionRatherThanUnknownOrUniversalRevocation() = root { root ->
        val slot = f.slot(root); f.select(root, f.identity.id, slot)
        val revoked = f.changedPolicy { it.getJSONArray("revokedKeyIds").put(JSONObject(f.vector.getString("descriptor")).getString("keyId")) }
        val row = f.review(root, revoked).value.entries.single(); assertEquals(PluginPolicyReview.State.REJECTED, row.state); assertTrue(row.reason.contains("撤销"))
        val wrong = f.review(root, f.changedPolicy { it.put("channel", "other") }).value.entries.single()
        assertEquals(PluginPolicyReview.State.REJECTED, wrong.state); assertFalse(wrong.reason.contains("撤销"))
        val signature = File(root, "$slot/release.sig"); Os.chmod(signature.path, 384)
        signature.writeBytes(signature.readBytes().also { it[0] = (it[0].toInt() xor 1).toByte() }); Os.chmod(signature.path, 256)
        assertEquals(PluginPolicyReview.State.REJECTED, f.review(root).value.entries.single().state)
    }
    @Test fun captureFailureIsUnknownAndItsTemporarySharedReferenceAlwaysCloses() = root { root ->
        val slot = f.slot(root); f.select(root, f.identity.id, slot)
        val failed = f.review(root) { throw IOException("PRIVATE_DETAIL") }.value.entries.single()
        assertEquals(PluginPolicyReview.State.UNKNOWN, failed.state); assertFalse(failed.reason.contains("PRIVATE_DETAIL"))
        assertEquals(0, LocalPublicationFile.directory(root, slot).use { PluginInstallFiles.lock(it.fd) })
        assertEquals(PluginPolicyReview.State.REJECTED, f.review(root) { f.identity.copy(packageSHA256 = "b".repeat(64)) }.value.entries.single().state)
    }
    @Test fun missingDescriptorCannotBorrowAnOldPolicyOrBecomeAccepted() = root { root ->
        val slot = f.slot(root); f.select(root, f.identity.id, slot)
        val directory = File(root, slot); Os.chmod(directory.path, 448); assertTrue(File(directory, "release.json").delete()); Os.chmod(directory.path, 320)
        var calls = 0; val row = f.review(root) { calls++; f.identity }.value.entries.single()
        assertEquals(PluginPolicyReview.State.UNKNOWN, row.state); assertEquals(0, calls)
    }
    @Test fun explicitSubsetPublishesOnceRotatesOnlySelectedGenerationAndNeverEnablesOrDeletes() = root { root ->
        val slot = f.slot(root); f.select(root, f.identity.id, slot); f.select(root, "org.ghosterm.first", slot)
        f.select(root, "org.ghosterm.second", "b".repeat(64)); val previous = f.select(root, "org.ghosterm.disabled", slot, false)
        val review = f.review(root); var commits = 0
        f.owner(root).use { owner ->
            val result = review.apply(owner, listOf("org.ghosterm.first", "org.ghosterm.second"), f.policy, { if (it == "after-rename") commits++ })
            assertEquals(1, commits); assertTrue(result.durabilityConfirmed)
            for (id in listOf("org.ghosterm.first", "org.ghosterm.second")) {
                assertFalse(result.selection.choices.getValue(id).enabled); assertNotEquals(previous.choices.getValue(id).generation, result.selection.choices.getValue(id).generation)
                assertEquals(previous.choices.getValue(id).releaseID, result.selection.choices.getValue(id).releaseID)
            }
            assertEquals(previous.choices.getValue(f.identity.id), result.selection.choices.getValue(f.identity.id))
            assertEquals(previous.choices.getValue("org.ghosterm.disabled"), result.selection.choices.getValue("org.ghosterm.disabled"))
            assertTrue(File(root, slot).isDirectory); f.reject { review.apply(owner, listOf("org.ghosterm.first"), f.policy) }
        }
    }
    @Test fun emptyDuplicateAcceptedAndUnlistedSubsetsCannotWriteAnything() = root { root ->
        val slot = f.slot(root); f.select(root, f.identity.id, slot); f.select(root, "org.ghosterm.alias", slot)
        val review = f.review(root); val before = File(root, PluginReleaseCatalog.SELECTION).readBytes()
        f.owner(root).use { owner ->
            for (ids in listOf(emptyList(), listOf("org.ghosterm.alias", "org.ghosterm.alias"), listOf(f.identity.id), listOf("org.ghosterm.unlisted")))
                f.reject("invalid explicit subset accepted: $ids") { review.apply(owner, ids, f.policy) }
        }
        assertArrayEquals(before, File(root, PluginReleaseCatalog.SELECTION).readBytes())
    }
    @Test fun policyRawBytesAndStrongRootIdentityAreRecheckedBeforeDisable() = root { root ->
        val slot = f.slot(root); f.select(root, "org.ghosterm.alias", slot); val review = f.review(root)
        f.owner(root).use { owner -> f.reject { review.apply(owner, listOf("org.ghosterm.alias"), f.policy + byteArrayOf(10)) } }
        val other = PluginInstallTestFiles.root()
        try {
            f.select(other, "org.ghosterm.alias", slot)
            val record = File(other, PluginReleaseCatalog.SELECTION); Os.chmod(record.path, 384); record.writeBytes(File(root, PluginReleaseCatalog.SELECTION).readBytes()); Os.chmod(record.path, 256)
            f.owner(other).use { owner -> f.reject("different parent accepted old ticket") { review.apply(owner, listOf("org.ghosterm.alias"), f.policy) } }
        } finally { PluginInstallTestFiles.clean(other) }
    }
    @Test fun anyWholeSelectionRevisionChangeInvalidatesEvenAnUnchangedSelectedRow() = root { root ->
        val slot = f.slot(root); f.select(root, "org.ghosterm.alias", slot); val review = f.review(root)
        f.select(root, "org.ghosterm.other", slot, false)
        val before = File(root, PluginReleaseCatalog.SELECTION).readBytes()
        f.owner(root).use { owner -> f.reject("stale whole revision accepted") { review.apply(owner, listOf("org.ghosterm.alias"), f.policy) } }
        assertArrayEquals(before, File(root, PluginReleaseCatalog.SELECTION).readBytes())
    }
    @Test fun beforeRenameFailureLeavesAllOldChoicesAndAfterRenameFailureReportsVisibleUnsyncedEffect() = root { root ->
        val slot = f.slot(root); f.select(root, "org.ghosterm.alias", slot); val review = f.review(root)
        val before = File(root, PluginReleaseCatalog.SELECTION).readBytes()
        f.owner(root).use { owner -> f.reject { review.apply(owner, listOf("org.ghosterm.alias"), f.policy, { if (it == "before-rename") throw IOException("controlled") }) } }
        assertArrayEquals(before, File(root, PluginReleaseCatalog.SELECTION).readBytes()); assertFalse(root.list()!!.any { it.startsWith(".selection-") })
        f.owner(root).use { owner ->
            val result = review.apply(owner, listOf("org.ghosterm.alias"), f.policy, { if (it == "after-rename") throw IOException("controlled") })
            assertFalse(result.durabilityConfirmed); assertFalse(owner.selection().choices.getValue("org.ghosterm.alias").enabled)
        }
    }
    @Test fun finalCatalogCASDoesNotOverwriteAnotherChangeDuringCommit() = root { root ->
        val slot = f.slot(root); f.select(root, "org.ghosterm.alias", slot); val review = f.review(root)
        f.owner(root).use { owner ->
            f.reject { review.apply(owner, listOf("org.ghosterm.alias"), f.policy, { if (it == "before-rename") owner.change("org.ghosterm.other", slot, false, owner.selection().revision) }) }
            assertTrue(owner.selection().choices.getValue("org.ghosterm.alias").enabled); assertFalse(owner.selection().choices.getValue("org.ghosterm.other").enabled)
        }
    }
    @Test fun corruptSelectionRemainsUnknownAndNeverAnEmptySuccessfulReview() = root { root ->
        File(root, PluginReleaseCatalog.SELECTION).apply { writeText("broken"); Os.chmod(path, 256) }
        f.reject("corrupt selection became empty review") { f.review(root) }
    }
}
