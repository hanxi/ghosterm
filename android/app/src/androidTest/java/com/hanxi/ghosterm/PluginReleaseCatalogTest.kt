package com.hanxi.ghosterm

import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Storage and unverified declarations only. Real Core/signatures are a separate suite. */
@RunWith(AndroidJUnit4::class)
class PluginReleaseCatalogTest {
    private val plugin = "org.ghosterm.fixture"
    private val a = "a".repeat(64); private val b = "b".repeat(64)
    private fun owner(root: File) = checkNotNull(PluginReleaseCatalog.open(root))
    private fun record(root: File) = File(root, PluginReleaseCatalog.SELECTION)
    private fun writeRecord(root: File, raw: String) {
        val file = record(root); if (file.exists()) Os.chmod(file.path, 384)
        file.writeText(raw); Os.chmod(file.path, 256)
    }
    private fun rejected(message: String, body: () -> Unit) {
        val failure = runCatching(body).exceptionOrNull()
        assertTrue(message + ": " + failure, failure is Exception)
    }
    private fun declared(root: File, version: String = "1.0.0", padding: Int = 0): String {
        val declaration = JSONObject().put("pluginId", plugin).put("pluginVersion", version).put("publisher", "unverified fixture").put("packageSHA256", a)
        if (padding > 0) declaration.put("padding", "x".repeat(padding))
        val raw = declaration.toString().toByteArray()
        val id = PluginReleaseCatalog.sha(raw)
        PluginReleaseStage.create(root, id).use {
            it.write("payload/unverified-data", "not an executable Core package".toByteArray())
            it.write("release.json", raw); it.write("release.sig", ByteArray(64)); it.write("policy.json", "{}".toByteArray())
            it.publish()
        }
        return id
    }
    @Test fun missingRootIsKnownAbsenceAndIsNeverCreated() {
        val root = PluginInstallTestFiles.root()
        try {
            val absent = File(root, "absent")
            assertNull(PluginReleaseCatalog.open(absent)); assertFalse(absent.exists())
            owner(root).use { assertEquals(PluginReleaseCatalog.Selection(null, emptyMap()), it.selection()) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun unsafeRootAndDanglingLinkCannotBecomeEmptyState() {
        val root = PluginInstallTestFiles.root(); val link = File(root.parentFile, "catalog-link-${UUID.randomUUID()}")
        try {
            Os.chmod(root.path, 493); rejected("public root accepted") { owner(root).close() }; Os.chmod(root.path, 448)
            Os.symlink(File(root, "missing").path, link.path)
            rejected("dangling root treated as absent") { PluginReleaseCatalog.open(link)?.close() }
        } finally { link.delete(); PluginInstallTestFiles.clean(root) }
    }
    @Test fun declarationsStayUnverifiedAndUnknownEntriesAreOnlyCounted() {
        val root = PluginInstallTestFiles.root()
        try {
            val ids = listOf(declared(root), declared(root, "2.0.0"))
            val unknown = File(root, ".prepare-unknown").apply { mkdir(); File(this, "preserve").writeText("public unknown") }
            owner(root).use { catalog ->
                val value = catalog.observe()
                assertEquals(ids.sorted(), value.versions.map { it.releaseID }); assertTrue(value.issues.isEmpty())
                assertTrue(value.versions.all { it.publisher == "unverified fixture" && it.packageSHA256 == a })
                assertEquals(1, value.maintenanceCount); assertEquals(emptyMap<String, PluginReleaseCatalog.Choice>(), value.selection!!.choices)
            }
            assertEquals("public unknown", File(unknown, "preserve").readText()); assertFalse(record(root).exists())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun descriptorDigestAndAllReadonlyEvidenceAreRequired() {
        for (change in listOf("digest", "signature", "missing-policy", "writable", "public", "limit")) {
            val root = PluginInstallTestFiles.root()
            try {
                val id = declared(root, padding = if (change == "limit") 70000 else 0); val directory = File(root, id); val descriptor = File(directory, "release.json")
                when (change) {
                    "digest" -> { Os.chmod(descriptor.path, 384); descriptor.writeText(descriptor.readText().replace("unverified fixture", "tampered fixture")); Os.chmod(descriptor.path, 256) }
                    "signature" -> { val sig = File(directory, "release.sig"); Os.chmod(sig.path, 384); sig.writeBytes(ByteArray(63)); Os.chmod(sig.path, 256) }
                    "missing-policy" -> { Os.chmod(directory.path, 448); assertTrue(File(directory, "policy.json").delete()); Os.chmod(directory.path, 320) }
                    "writable" -> Os.chmod(descriptor.path, 384)
                    "public" -> Os.chmod(descriptor.path, 292)
                }
                owner(root).use { val view = it.observe(); assertTrue("invalid evidence accepted: $change", view.versions.isEmpty()); assertEquals(1, view.issues.size) }
                assertTrue(directory.exists())
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun symlinkVersionAndWritableDirectoryAreNotVersions() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = declared(root); Os.chmod(File(root, id).path, 448); Os.symlink(id, File(root, b).path)
            owner(root).use { val view = it.observe(); assertTrue(view.versions.isEmpty()); assertEquals(2, view.issues.size) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun atomicRecordSurvivesReopenWithReadonlyModeAndNoTemporary() {
        val root = PluginInstallTestFiles.root()
        try {
            val written = owner(root).use { it.change(plugin, a, true, null) }
            assertTrue(written.durabilityConfirmed); assertEquals(256, Os.lstat(record(root).path).st_mode and 511)
            assertEquals(listOf(PluginReleaseCatalog.SELECTION), root.list()!!.toList())
            owner(root).use { assertEquals(written.selection, it.selection()) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun updatesPreserveOtherChoicesAndAlwaysRotateApprovalGenerationAndRevision() {
        val root = PluginInstallTestFiles.root()
        try { owner(root).use { catalog ->
            val one = catalog.change(plugin, a, true, null).selection
            val other = catalog.change("org.other.fixture", b, false, one.revision).selection
            val two = catalog.change(plugin, a, true, other.revision).selection
            assertEquals(other.choices.getValue("org.other.fixture"), two.choices.getValue("org.other.fixture"))
            assertNotEquals(one.choices.getValue(plugin).generation, two.choices.getValue(plugin).generation)
            assertNotEquals(one.revision, two.revision)
            val disabled = catalog.change(plugin, a, false, two.revision).selection
            assertFalse(disabled.choices.getValue(plugin).enabled)
            assertNotEquals(two.choices.getValue(plugin).generation, disabled.choices.getValue(plugin).generation)
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun absentAndOldRevisionsCannotOverwriteANewerSelectionOrAba() {
        val root = PluginInstallTestFiles.root()
        try { owner(root).use { catalog ->
            val first = catalog.change(plugin, a, true, null).selection
            val second = catalog.change(plugin, b, false, first.revision).selection
            val back = catalog.change(plugin, a, true, second.revision).selection
            for (old in listOf(null, first.revision, second.revision)) {
                var touched = false
                val failure = runCatching { catalog.change(plugin, b, true, old, checkpoint = { touched = true }) }.exceptionOrNull()
                assertTrue("stale selection accepted", failure is PluginReleaseCatalog.Rejected && failure.reason == PluginReleaseCatalog.Reason.CONFLICT)
                assertFalse("stale selection created a candidate", touched)
                assertEquals(back, catalog.selection())
            }
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun changedRecordAtCommitIsPreservedAndOwnTemporaryIsRemoved() {
        val root = PluginInstallTestFiles.root()
        try { owner(root).use { catalog ->
            val initial = catalog.change(plugin, a, true, null).selection
            val external = record(root).readText().replace(a, b)
            rejected("commit ignored current record") { catalog.change(plugin, b, true, initial.revision,
                checkpoint = { if (it == "before-rename") writeRecord(root, external) }) }
            assertEquals(external, record(root).readText()); assertEquals(listOf(PluginReleaseCatalog.SELECTION), root.list()!!.toList())
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun replacedRootCannotReceiveWriteOrLoseUnknownData() {
        val root = PluginInstallTestFiles.root(); val held = File(root.parentFile, "held-catalog-${UUID.randomUUID()}")
        try { owner(root).use { catalog ->
            val initial = catalog.change(plugin, a, true, null).selection; val original = record(root).readText()
            rejected("write followed replacement root") { catalog.change(plugin, b, true, initial.revision,
                checkpoint = { if (it == "before-rename") {
                    assertTrue(root.renameTo(held)); assertTrue(root.mkdir()); writeRecord(root, "preserve replacement")
                } }) }
            assertEquals("preserve replacement", record(root).readText())
            assertEquals(original, record(held).readText()); assertEquals(listOf(PluginReleaseCatalog.SELECTION), held.list()!!.toList())
        } } finally { PluginInstallTestFiles.clean(root); PluginInstallTestFiles.clean(held) }
    }
    @Test fun replacedOrChangedTemporaryIsNotPublishedOrDeleted() {
        for (replacement in listOf(false, true)) {
            val root = PluginInstallTestFiles.root(); var changed: File? = null
            try { owner(root).use { catalog ->
                val initial = catalog.change(plugin, a, true, null).selection; val original = record(root).readText()
                rejected("changed temporary was accepted") { catalog.change(plugin, b, true, initial.revision,
                    checkpoint = { if (it == "before-rename") {
                        val file = root.listFiles()!!.single { it.name.startsWith(".selection-") }
                        if (replacement) { assertTrue(file.renameTo(File(root, "displaced"))); file.writeText("preserve replacement") }
                        else { Os.chmod(file.path, 384); file.writeText("preserve changed bytes") }
                        Os.chmod(file.path, 256); changed = file
                    } }) }
                assertEquals(original, record(root).readText()); assertTrue("unknown temporary was deleted", changed!!.isFile)
            } } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun failuresBeforeRenameCleanOwnedTemporaryAndLeavePreviousRecord() {
        for (phase in listOf("after-create", "file-chunk", "before-rename")) {
            val root = PluginInstallTestFiles.root()
            try { owner(root).use { catalog ->
                val initial = catalog.change(plugin, a, true, null).selection; val raw = record(root).readText()
                rejected("controlled write fault accepted") { catalog.change(plugin, b, true, initial.revision,
                    checkpoint = { if (it == phase) throw java.io.IOException("controlled $phase") }) }
                assertEquals(raw, record(root).readText()); assertEquals(listOf(PluginReleaseCatalog.SELECTION), root.list()!!.toList())
            } } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun parentSyncAndReceiptFailuresKeepPublishedSelection() {
        for (receipt in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try { owner(root).use { catalog ->
                val attempt = runCatching { catalog.change(plugin, a, false, null,
                    checkpoint = { if (receipt && it == "after-rename") throw java.io.IOException("controlled receipt") },
                    parentSync = { if (!receipt) throw java.io.IOException("controlled fsync") else Os.fsync(it) }) }
                assertTrue("published choice receipt was lost during cleanup: ${attempt.exceptionOrNull()}", attempt.isSuccess)
                val written = attempt.getOrThrow()
                assertFalse("failed publication sync was called durable", written.durabilityConfirmed)
                assertEquals(written.selection, catalog.selection()); assertEquals(listOf(PluginReleaseCatalog.SELECTION), root.list()!!.toList())
            } } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun rootEntrySyncFailureCannotWriteSelection() {
        val root = PluginInstallTestFiles.root()
        try {
            checkNotNull(PluginReleaseCatalog.open(root, rootSync = { throw java.io.IOException("controlled root sync") })).use { catalog ->
                rejected("unsynced root entry accepted") { catalog.change(plugin, a, true, null) }
                assertTrue(root.list()!!.isEmpty())
            }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced() {
        val root = PluginInstallTestFiles.root()
        try {
            val good = owner(root).use { it.change(plugin, a, true, null); record(root).readText() }
            val nonce = JSONObject(good).put("nonce", "not-a-uuid").toString()
            val generation = JSONObject(good).also { it.getJSONObject("choices").getJSONObject(plugin).put("generation", "not-a-uuid") }.toString()
            val extra = JSONObject(good).put("unexpected", true).toString()
            val extraChoice = JSONObject(good).also { it.getJSONObject("choices").getJSONObject(plugin).put("unexpected", true) }.toString()
            val overflow = JSONObject(good).also { value -> repeat(64) { value.getJSONObject("choices").put("org.other.p$it", JSONObject(value.getJSONObject("choices").getJSONObject(plugin).toString())) } }.toString()
            val invalidSchemas = listOf("null", "true", "\"1\"", "1.0", "2").map { token ->
                good.replace("\"schemaVersion\":1", "\"schemaVersion\":$token")
            }
            for (raw in invalidSchemas + listOf("{}",
                good.replace("\"enabled\":true", "\"enabled\":\"true\""), good.replace("\"nonce\":", "\"unexpected\":"),
                good.replace("\"choices\":", "\"choices\":{},\"choices\":"), good.replace(a, "../invalid"), nonce, generation, extra, extraChoice, overflow)) {
                assertNotEquals("fixture failed to alter valid record", good, raw); writeRecord(root, raw)
                owner(root).use { catalog ->
                    val view = catalog.observe(); assertNull("bad record collapsed to no choices", view.selection); assertNotNull(view.selectionIssue)
                    rejected("bad record automatically repaired") { catalog.change(plugin, b, true, null) }
                }
                assertEquals(raw, record(root).readText())
            }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun selectionSymlinkAndWritableRecordAreNotKnownChoices() {
        val root = PluginInstallTestFiles.root(); val target = File(root, "outside-public")
        try {
            owner(root).use { it.change(plugin, a, true, null) }; val raw = record(root).readText()
            target.writeText(raw); Os.chmod(target.path, 256); assertTrue(record(root).delete()); Os.symlink(target.name, record(root).path)
            owner(root).use { assertNull(it.observe().selection) }; assertEquals(raw, target.readText())
            assertTrue(record(root).delete()); writeRecord(root, raw); Os.chmod(record(root).path, 384)
            owner(root).use { assertNull(it.observe().selection) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun choiceCapacityAndInventoryCapacityAreBounded() {
        val root = PluginInstallTestFiles.root()
        try { owner(root).use { catalog ->
            var selection = catalog.selection()
            repeat(64) { selection = catalog.change("org.fixture.p$it", a, true, selection.revision).selection }
            rejected("choice capacity bypassed") { catalog.change("org.fixture.extra", b, true, selection.revision) }
            assertEquals(selection, catalog.selection())
            repeat(128) { File(root, "unknown-$it").writeText("preserve") }
            rejected("inventory capacity bypassed") { catalog.observe() }
            assertEquals(129, root.list()!!.size)
        } } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun versionAndPayloadReplacementsCannotBorrowOpenedIdentities() {
        for (payloadOnly in listOf(false, true)) {
            val root = PluginInstallTestFiles.root()
            try {
                val id = declared(root)
                owner(root).use { catalog -> catalog.openVersion(id).use { version -> version.payload().use { payload ->
                    val directory = if (payloadOnly) File(root, id) else root
                    Os.chmod(directory.path, 448)
                    val original = if (payloadOnly) File(directory, "payload") else File(root, id)
                    assertTrue(original.renameTo(File(directory, "held"))); assertTrue(original.mkdir()); Os.chmod(original.path, 320)
                    if (payloadOnly) Os.chmod(directory.path, 320)
                    rejected("replacement borrowed original fd identity") { catalog.validateVersion(version, payload) }
                } } }
            } finally { PluginInstallTestFiles.clean(root) }
        }
    }
    @Test fun catalogSerializesWithInstallerAndOtherCatalogOwners() {
        val root = PluginInstallTestFiles.root()
        try {
            owner(root).use {
                rejected("second catalog entered") { owner(root).close() }
                val failure = runCatching { PluginReleaseStage.create(root, a).close() }.exceptionOrNull()
                assertTrue(failure is PluginReleaseStage.Rejected && failure.reason == PluginReleaseStage.Reason.BUSY)
            }
            PluginReleaseStage.create(root, a).close(); assertTrue(root.list()!!.isEmpty())
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun repeatedObservationsReleaseAllOwnedDescriptors() {
        val root = PluginInstallTestFiles.root()
        try {
            declared(root); owner(root).use { it.observe() }
            val before = File("/proc/self/fd").list()!!.size
            repeat(24) { owner(root).use { assertEquals(1, it.observe().versions.size) } }
            assertTrue("catalog fd leak", File("/proc/self/fd").list()!!.size <= before + 2)
        } finally { PluginInstallTestFiles.clean(root) }
    }
    @Test fun versionReferenceAndBorrowSurviveVersionAndCatalogOwnerClosure() {
        val root = PluginInstallTestFiles.root()
        try {
            val id = declared(root)
            val catalog = owner(root); val version = catalog.openVersion(id)
            version.pin(); val borrowed = version.borrow()
            try {
                version.close(); catalog.close()
                LocalPublicationFile.directory(root, id).use { assertEquals(OsConstants.EAGAIN, PluginInstallFiles.lock(it.fd)) }
            } finally { borrowed.close(); version.close(); catalog.close() }
            LocalPublicationFile.directory(root, id).use { assertEquals(0, PluginInstallFiles.lock(it.fd)) }
        } finally { PluginInstallTestFiles.clean(root) }
    }
}
