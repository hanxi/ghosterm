package com.hanxi.ghosterm

import android.os.Process
import android.system.Os
import android.system.OsConstants
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.CountDownLatch

/** Host-driven intentional deaths, excluded from ordinary assertion conformance. */
@RunWith(AndroidJUnit4::class)
class PluginUnclassifiedDeathTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val root get() = File(context.filesDir, "plugin-releases")
    private val arguments get() = InstrumentationRegistry.getArguments()
    private val token get() = arguments.getString("deathToken")!!.also { require(it.matches(Regex("[a-f0-9]{32}"))) }
    private val phase get() = arguments.getString("deathPhase")!!.also { require(it in listOf("before-intent", "after-intent", "after-detach", "after-unlink", "before-clear")) }
    private val marker get() = File(context.cacheDir, "plugin-unclassified-death-$token.json")
    private fun controller() = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"), listOf("session.rpc")), context)
    @Test fun waitAtRealUnknownRemovalCheckpointForExternalDeath() = runBlocking<Unit> {
        assertTrue("owned fresh namespace required", root.list()?.isEmpty() != false); assertFalse(marker.exists())
        if (!root.exists()) { check(root.mkdir()); Os.chmod(root.path, 448) }
        val stage = PluginJournalFixture.create(root, recorded = false)
        val originalRecord = PluginReleaseCatalog.sha(File(root, stage.recordName).readBytes())
        val owner = controller()
        try {
            val ref = owner.unclassifiedDirectories().single().reference!!
            val preview = owner.previewUnclassifiedRemoval(UUID.randomUUID().toString(), ref)
            owner.confirmVersionRemoval(preview.id, true, checkpoint = { current -> if (current == phase) {
                val value = JSONObject().put("pid", Process.myPid()).put("phase", phase).put("token", token)
                    .put("names", JSONArray(root.list()!!.sorted())).put("tree", snapshot())
                    .put("record", stage.recordName).put("recordSHA256", originalRecord)
                marker.outputStream().use { out -> out.write(value.toString().toByteArray()); out.fd.sync() }
                CountDownLatch(1).await()
            } }, acknowledgedUnclassified = true)
            fail("unknown removal checkpoint did not block")
        } finally { owner.close() }
    }
    private fun snapshot(): String {
        val result = JSONObject()
        LocalPublicationFile.directory(root, "").use { result.put(".", PluginReleaseRemoval.stamp(it).json()) }
        fun walk(directory: File, prefix: String) {
            for (file in directory.listFiles()!!.sortedBy { it.name }) {
                val path = if (prefix.isEmpty()) file.name else "$prefix/${file.name}"
                val stat = Os.lstat(file.path)
                result.put(path, JSONObject().put("mode", stat.st_mode).put("inode", stat.st_ino.toString()).put("owner", stat.st_uid)
                    .put("sha", if (OsConstants.S_ISREG(stat.st_mode)) MessageDigest.getInstance("SHA-256").digest(file.readBytes()).joinToString("") { "%02x".format(it.toInt() and 255) } else "directory"))
                if (OsConstants.S_ISDIR(stat.st_mode)) walk(file, path)
            }
        }
        walk(root, ""); return result.toString()
    }
    @Test fun freshStartupDoesNotReplayThenExplicitUnknownMaintenanceCompletes() = runBlocking<Unit> {
        val value = JSONObject(marker.readText()); assertEquals(phase, value.getString("phase")); assertEquals(token, value.getString("token"))
        try {
            ActivityScenario.launch(MainActivity::class.java).use {
                assertEquals("startup changed unknown tree or intent", value.getString("tree"), snapshot())
            }
            val owner = controller()
            try {
                val pending = owner.pendingVersionRemoval()
                assertEquals("observation replayed unknown deletion", value.getString("tree"), snapshot())
                val result = if (pending == null) {
                    assertEquals("before-intent", phase)
                    val ref = owner.unclassifiedDirectories().single().reference!!
                    val preview = owner.previewUnclassifiedRemoval(UUID.randomUUID().toString(), ref)
                    owner.confirmVersionRemoval(preview.id, true, acknowledgedUnclassified = true)
                } else {
                    assertNotNull(pending.unclassified)
                    owner.resumeVersionRemoval(pending.id, true, acknowledgedUnclassified = true)
                }
                assertTrue(result.toString(), result.complete)
                val record = File(root, value.getString("record"))
                assertEquals("unknown cleanup retired old ownership", value.getString("recordSHA256"), PluginReleaseCatalog.sha(record.readBytes()))
                val entry = owner.installationMaintenance().entries.single()
                assertEquals(PluginInstallJournal.State.ABSENT, entry.state)
                assertTrue(owner.retireInstallationRecord(entry.reference!!, true)); assertTrue(root.list()!!.isEmpty())
            } finally { owner.close() }
        } finally { PluginInstallTestFiles.clean(root); check(marker.delete() || !marker.exists()) }
    }
}
