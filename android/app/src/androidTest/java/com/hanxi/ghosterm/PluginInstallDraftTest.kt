package com.hanxi.ghosterm

import android.net.Uri
import android.os.Bundle
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Controlled scheduling is distinct from the real SAF/Core UI conformance. */
@RunWith(AndroidJUnit4::class)
class PluginInstallDraftTest {
    private val uri = Uri.parse("content://explicit.fixture/material")
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private suspend fun captured(): PluginArchiveCapture {
        val value = context.contentResolver.call(PluginSignatureStreamProvider.URI, "set", null, Bundle().apply { putByteArray("bytes", ByteArray(32)) })!!
        check(value.getBoolean("ok"))
        return PluginArchiveCapture.read(context, context.contentResolver, PluginSignatureStreamProvider.URI)
    }
    private fun receipt() = ExtensionController.SignedArchiveReview(
        ExtensionController.Review("org.ghosterm.fixture", "1.0.0", "fixture", "MIT", "a".repeat(64), "portable-js", listOf("session.rpc"), emptyList(), false, false, emptyList()),
        PluginReleaseSignature.Proof("org.ghosterm.fixture", "a".repeat(64), "fixture", "b".repeat(64), "fixture", "c".repeat(64), "d".repeat(64), "e".repeat(64), 32),
        PluginReleaseDeclarations.Data("f".repeat(64), "^1.0.0", "1.0.0", listOf(1), listOf(1), emptyList(), null))
    private fun model(scope: CoroutineScope, install: suspend (PluginArchiveCapture, ByteArray, ByteArray, ByteArray?, ExtensionController.SignedArchiveReview) -> ExtensionController.InstalledRelease) =
        PluginArchiveReviewModel(scope, { captured() }, { _, _ -> PluginSignatureDocument(ByteArray(64)) }, { _, _, _, _ -> receipt() }, install)
    private fun published(expected: ExtensionController.SignedArchiveReview, durable: Boolean = true) = ExtensionController.InstalledRelease(expected,
        PluginReleaseStage.Publication(expected.proof.descriptorSHA256, java.io.File(context.filesDir, "fixture-no-write"), durable, if (durable) null else "unconfirmed"))
    private suspend fun ready(model: PluginArchiveReviewModel) { fill(model); main { model.trustPolicy(true); model.checkArchive() }; idle(model) }
    private suspend fun <T> main(body: () -> T) = withContext(Dispatchers.Main.immediate) { body() }
    private suspend fun idle(model: PluginArchiveReviewModel) = withTimeout(3000) { while (main { model.busy }) delay(10) }
    private suspend fun choose(model: PluginArchiveReviewModel, kind: PluginArchiveMaterial) { main { model.completePick(model.beginPick(kind)!!, uri) }; idle(model) }
    private suspend fun fill(model: PluginArchiveReviewModel) { for (kind in PluginArchiveMaterial.entries) choose(model, kind) }
    private suspend fun check(model: PluginArchiveReviewModel) { main { model.checkArchive() }; idle(model) }
    private fun scope() = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private fun released(value: PluginArchiveCapture) {
        try { value.borrow().close(); fail("capture owner retained") } catch (_: IllegalStateException) { }
    }
    private fun releaseProvider() { context.contentResolver.call(PluginSignatureStreamProvider.URI, "release", null, null) }
    @Test fun installationRequiresVerifiedSnapshotAndSeparateExplicitAcknowledgement() = runBlocking {
        val scope = scope(); var calls = 0; val model = model(scope) { _, _, _, _, expected -> calls++; published(expected) }
        try {
            main { model.acknowledgeInstall(true); model.installArchive() }; assertEquals(0, calls)
            ready(model); assertFalse(main { model.canInstall }); main { model.installArchive() }; assertEquals(0, calls)
            main { model.acknowledgeInstall(true); assertTrue(model.canInstall); model.installArchive() }; idle(model)
            assertEquals(1, calls); assertNotNull(main { model.installation }); assertFalse(main { model.canInstall }); assertFalse(main { model.installAcknowledged })
            main { model.acknowledgeInstall(true); model.installArchive() }; assertEquals(1, calls)
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun inputAndPolicyChangesInvalidateInstallationAcknowledgement() = runBlocking {
        val scope = scope(); var calls = 0; val model = model(scope) { _, _, _, _, expected -> calls++; published(expected) }
        try {
            ready(model); main { model.acknowledgeInstall(true) }; choose(model, PluginArchiveMaterial.SIGNATURE)
            assertFalse(main { model.installAcknowledged }); assertFalse(main { model.canInstall })
            check(model); assertFalse(main { model.canInstall })
            main { model.acknowledgeInstall(true); model.trustPolicy(false); model.trustPolicy(true) }
            assertFalse(main { model.installAcknowledged }); assertNull(main { model.receipt }); main { model.installArchive() }; assertEquals(0, calls)
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun installationFreezesPolicyAndRejectsAnotherPickerUntilReceipt() = runBlocking {
        val scope = scope(); val gate = CompletableDeferred<Unit>(); val model = model(scope) { _, _, _, _, expected -> gate.await(); published(expected) }
        try {
            ready(model); main { model.acknowledgeInstall(true); model.installArchive() }
            assertTrue(main { model.installing }); assertNull(main { model.beginPick(PluginArchiveMaterial.POLICY) })
            main { model.trustPolicy(false); model.acknowledgeInstall(false) }
            assertTrue(main { model.trustedPolicy }); assertTrue(main { model.installAcknowledged })
            gate.complete(Unit); idle(model); assertNotNull(main { model.installation }); assertFalse(main { model.installing })
        } finally { gate.complete(Unit); main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun closingPageDoesNotPretendToUndoACompletedPublication() = runBlocking {
        val scope = scope(); val gate = CompletableDeferred<Unit>(); var completed = false
        val model = model(scope) { capture, _, _, _, expected -> capture.borrow().use {
            withContext(NonCancellable) { gate.await(); completed = true; published(expected) }
        } }
        try {
            ready(model); main { model.acknowledgeInstall(true); model.installArchive() }
            val selected = main { model.archive!! }; main { model.close() }; gate.complete(Unit); idle(model)
            assertTrue(completed); released(selected); assertNull(main { model.installation }); assertNull(main { model.receipt })
        } finally { gate.complete(Unit); main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun unknownReceiptIsVisibleAndRequiresFreshAcknowledgementForRetry() = runBlocking {
        val scope = scope(); val model = model(scope) { _, _, _, _, _ -> throw java.io.IOException("DO_NOT_SHOW_PROVIDER_DETAIL") }
        try {
            ready(model); main { model.acknowledgeInstall(true); model.installArchive() }; idle(model)
            assertNull(main { model.installation }); assertFalse(main { model.installAcknowledged }); assertFalse(main { model.canInstall })
            assertTrue(main { model.issue!!.contains("未取得") }); assertFalse(main { model.issue!!.contains("DO_NOT_SHOW") })
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun publishedButUnconfirmedDurabilityIsKeptAsPublishedResult() = runBlocking {
        val scope = scope(); val model = model(scope) { _, _, _, _, expected -> published(expected, false) }
        try {
            ready(model); main { model.acknowledgeInstall(true); model.installArchive() }; idle(model)
            val result = main { model.installation }; assertNotNull(result); assertFalse(result!!.publication.durabilityConfirmed)
            assertNotNull(result.publication.maintenanceIssue); assertFalse(main { model.canInstall })
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
}
