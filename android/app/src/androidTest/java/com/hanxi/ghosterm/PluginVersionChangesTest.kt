package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

/** Declaration comparisons only; the actual signed Core packages have separate tests. */
@RunWith(AndroidJUnit4::class)
class PluginVersionChangesTest {
    private val oldPermission = PluginReleaseDeclarations.Permission("executor", "process.spawn", "purpose", "instance", listOf("old-worker"))
    private fun signed(capabilities: List<String>, permissions: List<PluginReleaseDeclarations.Permission> = listOf(oldPermission), version: String = "1.0.0") =
        ExtensionController.SignedArchiveReview(
            ExtensionController.Review("org.ghosterm.fixture", version, "fixture", "MIT", "a".repeat(64), "lua", capabilities, listOf("ui.chat"), false, false, emptyList()),
            PluginReleaseSignature.Proof("org.ghosterm.fixture", "a".repeat(64), "fixture", "b".repeat(64), "fixture", "c".repeat(64), "d".repeat(64), "e".repeat(64), 32),
            PluginReleaseDeclarations.Data("f".repeat(64), "^1.0.0", "1.0.0", listOf(1), listOf(1), permissions, null))
    private fun view(candidate: ExtensionController.SignedArchiveReview, previous: ExtensionController.SignedArchiveReview?,
        state: ExtensionController.PreviousVersionState, enabled: Boolean? = true) =
        ExtensionController.VersionPreview("display", "a".repeat(64), "b".repeat(64), candidate, previous, state, "c".repeat(64), enabled)
    @Test fun firstSelectionShowsEveryRequiredCapabilityAndNoRemovedDeclarations() {
        val candidate = signed(listOf("session.rpc", "ui.table"))
        val value = view(candidate, null, ExtensionController.PreviousVersionState.NONE, null)
        assertEquals(listOf("session.rpc", "ui.table"), value.addedCapabilities)
        assertEquals(emptyList<String>(), value.removedCapabilities)
        assertEquals(candidate.declarations.permissions, value.addedPermissions); assertTrue(value.removedPermissions!!.isEmpty())
    }
    @Test fun verifiedSelectionShowsBothCapabilityDirectionsAndKeepsDeclarationOrder() {
        val value = view(signed(listOf("ui.table", "session.rpc", "ui.views")), signed(listOf("ui.diff", "ui.task", "session.rpc")), ExtensionController.PreviousVersionState.VERIFIED)
        assertEquals(listOf("ui.table", "ui.views"), value.addedCapabilities)
        assertEquals(listOf("ui.diff", "ui.task"), value.removedCapabilities)
    }
    @Test fun unknownPreviousNeverClaimsEmptyOrCompleteCapabilityAndPermissionDifferences() {
        val value = view(signed(listOf("session.rpc", "ui.table")), null, ExtensionController.PreviousVersionState.UNKNOWN)
        assertNull(value.addedCapabilities); assertNull(value.removedCapabilities)
        assertNull(value.addedPermissions); assertNull(value.removedPermissions)
        assertEquals(listOf("ui.chat"), value.candidate.review.optionalMissingCapabilities)
    }
    @Test fun disabledPreviousStillParticipatesInFullComparisonAndSameDeclarationsStayEmpty() {
        val disabled = view(signed(listOf("session.rpc", "ui.table")), signed(listOf("session.rpc", "ui.diff")), ExtensionController.PreviousVersionState.VERIFIED, false)
        assertEquals(listOf("ui.table"), disabled.addedCapabilities); assertEquals(listOf("ui.diff"), disabled.removedCapabilities)
        val same = signed(listOf("session.rpc"))
        val value = view(same, same, ExtensionController.PreviousVersionState.VERIFIED, false)
        assertTrue(value.addedCapabilities!!.isEmpty()); assertTrue(value.removedCapabilities!!.isEmpty())
        assertTrue(value.addedPermissions!!.isEmpty()); assertTrue(value.removedPermissions!!.isEmpty())
    }
    @Test fun purposeScopeProgramAndSideChangesEachRemainVisibleAsWholePermissions() {
        val changes = listOf(oldPermission.copy(purpose = "changed purpose"), oldPermission.copy(scope = "workspace"),
            oldPermission.copy(programs = listOf("new-worker")), oldPermission.copy(side = "client"))
        for (changed in changes) {
            val value = view(signed(listOf("session.rpc"), listOf(changed)), signed(listOf("session.rpc")), ExtensionController.PreviousVersionState.VERIFIED)
            assertEquals(listOf(changed), value.addedPermissions); assertEquals(listOf(oldPermission), value.removedPermissions)
        }
    }
    @Test fun rollbackComparesDeclarationsWithoutInferringVersionOrderingOrTrust() {
        val value = view(signed(listOf("ui.diff"), version = "1.0.0"), signed(listOf("ui.table"), version = "99.0.0"), ExtensionController.PreviousVersionState.VERIFIED)
        assertEquals(listOf("ui.diff"), value.addedCapabilities); assertEquals(listOf("ui.table"), value.removedCapabilities)
        assertFalse(value.candidate.review.authorizationGranted); assertFalse(value.candidate.review.sourceApproved)
    }
}
