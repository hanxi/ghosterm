package com.hanxi.ghosterm

import android.net.Uri
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.util.Base64

/** Pure draft scheduling checks are separate from the real Core/page tests. */
@RunWith(AndroidJUnit4::class)
class PluginSignatureDraftTest {
    private val uri = Uri.parse("content://explicit.fixture/material")
    private val fixture get() = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets
        .open("plugin-release/vector.json").bufferedReader().use { it.readText() })
    private fun bytes(kind: PluginSignatureMaterial): ByteArray = when (kind) {
        PluginSignatureMaterial.SIGNATURE -> Base64.getDecoder().decode(fixture.getString("signature"))
        PluginSignatureMaterial.DESCRIPTOR -> fixture.getString("descriptor").toByteArray()
        PluginSignatureMaterial.POLICY -> fixture.getString("policy").toByteArray()
    }
    private suspend fun proof() = withContext(Dispatchers.Default) {
        PluginReleaseSignature.verify(bytes(PluginSignatureMaterial.DESCRIPTOR), bytes(PluginSignatureMaterial.SIGNATURE), bytes(PluginSignatureMaterial.POLICY),
            PluginReleaseSignature.Identity("org.ghosterm.signature", "1.0.0", "fixture", "a".repeat(64)))
    }
    private suspend fun <T> main(body: () -> T): T = withContext(Dispatchers.Main.immediate) { body() }
    private suspend fun idle(model: PluginSignatureReviewModel) = withTimeout(3000) { while (main { model.busy }) delay(10) }
    private suspend fun fill(model: PluginSignatureReviewModel) {
        for (kind in PluginSignatureMaterial.entries) {
            main { model.completePick(model.beginPick(kind)!!, uri) }; idle(model)
        }
    }
    @Test fun allMaterialsAndExplicitConfirmationAreRequired() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); var calls = 0
        val value = proof()
        val model = PluginSignatureReviewModel(scope, { _, kind -> PluginSignatureDocument(bytes(kind)) }, { _, _, _ -> calls++; value })
        try {
            main { model.trustPolicy(true); model.checkSignature() }
            assertFalse(main { model.trustedPolicy }); assertEquals(0, calls)
            fill(model); main { model.checkSignature() }; assertEquals(0, calls)
            main { model.trustPolicy(true); assertTrue(model.canVerify); model.checkSignature() }; idle(model)
            assertEquals(value, main { model.receipt }); assertEquals(1, calls)
        } finally { main { model.close() }; scope.cancel() }
    }
    @Test fun policyReselectionClearsProofAndRequiresNewConfirmation() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        var calls = 0; val value = proof()
        val model = PluginSignatureReviewModel(scope, { _, kind -> PluginSignatureDocument(bytes(kind)) }, { _, _, _ -> calls++; value })
        try {
            fill(model); main { model.trustPolicy(true); model.checkSignature() }; idle(model)
            assertNotNull(main { model.receipt })
            val pick = main { model.beginPick(PluginSignatureMaterial.POLICY)!! }
            assertNull(main { model.receipt }); assertFalse(main { model.trustedPolicy })
            main { model.completePick(pick, uri) }; idle(model)
            main { model.checkSignature() }; assertEquals(1, calls)
            main { model.trustPolicy(true); model.checkSignature() }; idle(model); assertEquals(2, calls)
        } finally { main { model.close() }; scope.cancel() }
    }
    @Test fun withdrawnAndReconfirmedPolicyCannotAcceptLateVerification() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        val gate = CompletableDeferred<Unit>(); val value = proof()
        val model = PluginSignatureReviewModel(scope, { _, kind -> PluginSignatureDocument(bytes(kind)) }, { _, _, _ -> gate.await(); value })
        try {
            fill(model); main { model.trustPolicy(true); model.checkSignature(); model.trustPolicy(false); model.trustPolicy(true) }
            assertTrue(main { model.busy }); gate.complete(Unit); idle(model)
            assertNull("late proof returned after policy confirmation changed", main { model.receipt })
            main { model.checkSignature() }; idle(model); assertEquals(value, main { model.receipt })
        } finally { main { model.close() }; scope.cancel() }
    }
    @Test fun requestTokensRejectWrongDraftCancelledAndClosedCallbacks() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); var reads = 0
        val value = proof()
        fun draft() = PluginSignatureReviewModel(scope, { _, kind -> reads++; PluginSignatureDocument(bytes(kind)) }, { _, _, _ -> value })
        val first = draft(); val second = draft()
        try {
            val old = main { first.beginPick(PluginSignatureMaterial.POLICY)!! }
            main { second.completePick(old, uri); first.completePick(old, null) }
            val current = main { first.beginPick(PluginSignatureMaterial.DESCRIPTOR)!! }
            main { first.completePick(old, uri) }; assertSame(current, main { first.pending }); assertEquals(0, reads)
            main { first.failPick(current) }; assertNotNull(main { first.issue })
            val retired = main { first.beginPick(PluginSignatureMaterial.POLICY)!!; first.pending!! }
            main { first.close(); first.completePick(retired, uri) }
            assertEquals(0, reads); assertNull(main { first.beginPick(PluginSignatureMaterial.SIGNATURE) })
        } finally { main { first.close(); second.close() }; scope.cancel() }
    }
    @Test fun descriptorAndSignatureEditsClearProofWhileKeepingSamePolicyDecision() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate); val value = proof()
        val model = PluginSignatureReviewModel(scope, { _, kind -> PluginSignatureDocument(bytes(kind)) }, { _, _, _ -> value })
        try {
            fill(model); main { model.trustPolicy(true) }
            for (kind in listOf(PluginSignatureMaterial.DESCRIPTOR, PluginSignatureMaterial.SIGNATURE)) {
                main { model.checkSignature() }; idle(model); assertNotNull(main { model.receipt })
                val pick = main { model.beginPick(kind)!! }
                assertNull(main { model.receipt }); assertTrue(main { model.trustedPolicy }); assertFalse(main { model.canVerify })
                main { model.completePick(pick, uri) }; idle(model)
            }
        } finally { main { model.close() }; scope.cancel() }
    }
    @Test fun closeCancelsReadAndDropsCapturedMaterials() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        val started = CompletableDeferred<Unit>(); val ended = CompletableDeferred<Unit>(); val value = proof()
        val model = PluginSignatureReviewModel(scope, { _, _ ->
            started.complete(Unit)
            try { awaitCancellation() } finally { ended.complete(Unit) }
        }, { _, _, _ -> value })
        try {
            main { model.completePick(model.beginPick(PluginSignatureMaterial.POLICY)!!, uri) }
            withTimeout(3000) { started.await() }
            main { model.close() }; assertTrue("closed read was not cancelled", withTimeoutOrNull(1500) { ended.await(); true } == true); idle(model)
            assertTrue(main { model.documents.isEmpty() }); assertFalse(main { model.trustedPolicy })
            assertNull(main { model.receipt }); assertFalse(main { model.canVerify })
        } finally { main { model.close() }; scope.cancel() }
    }
    @Test fun failuresRemainVisibleAndBusyRequestsCannotOverlap() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
        val gate = CompletableDeferred<Unit>()
        val model = PluginSignatureReviewModel(scope, { _, kind -> PluginSignatureDocument(bytes(kind)) }, { _, _, _ ->
            gate.await(); throw PluginReleaseSignature.Rejected(PluginReleaseSignature.Failure.SIGNATURE)
        })
        try {
            fill(model); main { model.trustPolicy(true); model.checkSignature() }
            assertNull(main { model.beginPick(PluginSignatureMaterial.POLICY) })
            assertFalse(main { model.canVerify }); gate.complete(Unit); idle(model)
            assertNotNull(main { model.issue }); assertNull(main { model.receipt })
        } finally { main { model.close() }; scope.cancel() }
    }
}
