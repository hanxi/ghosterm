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
class PluginArchiveDraftTest {
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
    private fun model(scope: CoroutineScope, inspect: suspend (PluginArchiveCapture, ByteArray, ByteArray, ByteArray) -> ExtensionController.SignedArchiveReview = { _, _, _, _ -> receipt() }) =
        PluginArchiveReviewModel(scope, { captured() }, { _, _ -> PluginSignatureDocument(ByteArray(64)) }, inspect)
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
    @Test fun requiresArchiveAllMaterialsAndExplicitPolicyConfirmation() = runBlocking {
        val scope = scope(); var calls = 0; val model = model(scope) { _, _, _, _ -> calls++; receipt() }
        try {
            main { model.trustPolicy(true); assertFalse(model.trustedPolicy) }
            for (kind in PluginArchiveMaterial.entries.filter { it != PluginArchiveMaterial.ARCHIVE }) choose(model, kind)
            main { model.trustPolicy(true); assertFalse(model.canInspect) }
            choose(model, PluginArchiveMaterial.ARCHIVE)
            main { model.trustPolicy(false); assertFalse(model.canInspect) }; check(model); assertEquals(0, calls)
            main { model.trustPolicy(true); assertTrue(model.canInspect) }; check(model)
            assertEquals(1, calls); assertNotNull(main { model.receipt })
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun archiveReselectionReleasesOldOwnerAndCancellationClearsSlot() = runBlocking {
        val scope = scope(); val model = model(scope)
        try {
            fill(model); main { model.trustPolicy(true) }; check(model)
            val old = main { model.archive!! }
            main { model.completePick(model.beginPick(PluginArchiveMaterial.ARCHIVE)!!, null) }
            released(old); assertNull(main { model.archive }); assertNull(main { model.receipt }); assertFalse(main { model.canInspect })
            choose(model, PluginArchiveMaterial.ARCHIVE); assertTrue(main { model.canInspect }); check(model)
            assertNotNull(main { model.receipt })
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun materialChangesClearProofAndPolicyReselectionClearsTrust() = runBlocking {
        val scope = scope(); val model = model(scope)
        try {
            fill(model); main { model.trustPolicy(true) }; check(model)
            choose(model, PluginArchiveMaterial.SIGNATURE); assertNull(main { model.receipt }); assertTrue(main { model.trustedPolicy })
            check(model); choose(model, PluginArchiveMaterial.POLICY)
            assertNull(main { model.receipt }); assertFalse(main { model.trustedPolicy }); assertFalse(main { model.canInspect })
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun withdrawnAndReconfirmedTrustRejectsLatePreview() = runBlocking {
        val scope = scope(); val gate = CompletableDeferred<Unit>(); val model = model(scope) { _, _, _, _ -> gate.await(); receipt() }
        try {
            fill(model); main { model.trustPolicy(true); model.checkArchive(); model.trustPolicy(false); model.trustPolicy(true) }
            gate.complete(Unit); idle(model); assertNull(main { model.receipt })
            check(model); assertNotNull(main { model.receipt })
        } finally { gate.complete(Unit); main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun wrongCancelledAndClosedPickerResultsCannotPopulateAnotherDraft() = runBlocking {
        val scope = scope(); val first = model(scope); val second = model(scope)
        try {
            val old = main { first.beginPick(PluginArchiveMaterial.POLICY)!! }
            main { second.completePick(old, uri); first.completePick(old, null) }
            val current = main { first.beginPick(PluginArchiveMaterial.SIGNATURE)!! }
            main { first.completePick(old, uri) }; assertSame(current, main { first.pending }); assertTrue(main { first.documents.isEmpty() })
            main { first.close(); first.completePick(current, uri) }; assertTrue(main { first.documents.isEmpty() })
        } finally { main { first.close(); second.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun closingDraftCancelsReadAndClosesCaptureReturnedLate() = runBlocking {
        val scope = scope(); val gate = CompletableDeferred<Unit>(); val value = captured(); var cancelled = false
        val model = PluginArchiveReviewModel(scope, {
            try { gate.await() } catch (_: CancellationException) { cancelled = true; withContext(NonCancellable) { gate.await() } }
            value
        }, { _, _ -> PluginSignatureDocument(ByteArray(64)) }, { _, _, _, _ -> receipt() })
        try {
            main { model.completePick(model.beginPick(PluginArchiveMaterial.ARCHIVE)!!, uri) }
            main { model.close() }; gate.complete(Unit); idle(model)
            assertTrue("draft did not cancel active read", cancelled); released(value); assertNull(main { model.archive })
        } finally { gate.complete(Unit); main { model.close() }; value.close(); scope.cancel(); releaseProvider() }
    }
    @Test fun closeReleasesSelectedCaptureAndClearsAllProofState() = runBlocking {
        val scope = scope(); val model = model(scope)
        try {
            fill(model); main { model.trustPolicy(true) }; check(model)
            val value = main { model.archive!! }; main { model.close() }
            released(value); assertNull(main { model.receipt }); assertTrue(main { model.documents.isEmpty() }); assertFalse(main { model.canInspect })
        } finally { main { model.close() }; scope.cancel(); releaseProvider() }
    }
    @Test fun failedProviderHasVisibleFixedErrorAndNoOverlappingRequest() = runBlocking {
        val scope = scope()
        val model = PluginArchiveReviewModel(scope, { throw IllegalStateException("ATTACK_PROVIDER_STRING") }, { _, _ -> error("unused") }, { _, _, _, _ -> error("unused") })
        try {
            val pick = main { model.beginPick(PluginArchiveMaterial.ARCHIVE)!! }
            assertNull(main { model.beginPick(PluginArchiveMaterial.SIGNATURE) })
            main { model.completePick(pick, uri) }; idle(model)
            assertNotNull(main { model.issue }); assertFalse(main { model.issue!!.contains("ATTACK_PROVIDER_STRING") }); assertNull(main { model.archive })
        } finally { main { model.close() }; scope.cancel() }
    }
}
