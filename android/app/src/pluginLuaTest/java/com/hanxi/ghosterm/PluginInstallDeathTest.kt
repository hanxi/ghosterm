package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.Process
import android.system.Os
import android.system.OsConstants
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest
import java.util.Base64
import java.util.concurrent.CountDownLatch

/** Explicit host-driven death scan only; not included in the normal combination. */
@RunWith(AndroidJUnit4::class)
class PluginInstallDeathTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val root get() = luaVersionDirectory(context)
    private val arguments get() = InstrumentationRegistry.getArguments()
    private val token get() = arguments.getString("deathToken")!!.also { require(it.matches(Regex("[a-f0-9]{32}"))) }
    private val phase get() = arguments.getString("deathPhase")!!.also { require(it in listOf("after-reservation", "after-directory-create", "after-directory-record", "after-file-create", "after-file-write", "before-rename", "after-rename", "after-publication-sync", "after-stage-unlink")) }
    private val marker get() = File(context.cacheDir, "plugin-install-death-$token.json")
    private val fixture get() = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("plugin-release/archive-lua-declarations.json").bufferedReader().use { it.readText() })
    private fun controller() = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("lua"), listOf("session.rpc")), context)
    @org.junit.Before fun requireOwnedDeathScan() {
        org.junit.Assume.assumeTrue(arguments.containsKey("deathToken") && arguments.containsKey("deathPhase"))
    }
    @Test fun waitAtRealInstallationCheckpointForExternalProcessDeath() = runBlocking {
        assertTrue("death scan requires owned empty install namespace", root.list()?.isEmpty() != false)
        assertFalse(marker.exists())
        val vector = fixture.getJSONArray("vectors").getJSONObject(0)
        val raw = vector.getString("descriptor").toByteArray(); val sig = Base64.getDecoder().decode(vector.getString("signature"))
        val policy = fixture.getString("policy").toByteArray()
        val file = File(context.cacheDir, "plugin-install-death-$token.zip").apply { writeBytes(Base64.getDecoder().decode(vector.getString("archive"))) }
        val owner = controller()
        try { ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
            val preview = owner.inspectSignedArchive(fd, raw, sig, policy)
            owner.installSignedArchive(fd, raw, sig, policy, preview, checkpoint = { current ->
                if (current == phase) {
                    val receipt = JSONObject().put("pid", Process.myPid()).put("phase", phase).put("token", token)
                        .put("release", preview.proof.descriptorSHA256).put("names", JSONArray(root.list()!!.sorted())).put("tree", snapshot())
                    marker.outputStream().use { out -> out.write(receipt.toString().toByteArray()); out.fd.sync() }
                    CountDownLatch(1).await()
                }
            })
            fail("death checkpoint did not block")
        } } finally { owner.close(); check(file.delete() || !file.exists()) }
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
    @Test fun waitAtStageRemovalCheckpointForExternalProcessDeath() = runBlocking {
        val owner = controller()
        try {
            val reference = owner.installationMaintenance().entries.single().reference!!
            val preview = owner.previewStageRemoval(java.util.UUID.randomUUID().toString(), reference)
            owner.confirmVersionRemoval(preview.id, true, checkpoint = { current -> if (current == "after-unlink") {
                val receipt = JSONObject().put("pid", Process.myPid()).put("phase", "after-stage-unlink").put("token", token)
                    .put("release", reference.target).put("names", JSONArray(root.list()!!.sorted())).put("tree", snapshot())
                marker.outputStream().use { out -> out.write(receipt.toString().toByteArray()); out.fd.sync() }
                CountDownLatch(1).await()
            } })
            fail("stage removal checkpoint did not block")
        } finally { owner.close() }
    }
    @Test fun freshStartupObservesHeldNamespaceWithoutReplayThenVerifiesAndCleans() = runBlocking {
        val receipt = JSONObject(marker.readText()); assertEquals(phase, receipt.getString("phase")); assertEquals(token, receipt.getString("token"))
        val previous = receipt.getJSONArray("names").let { names -> (0 until names.length()).map { names.getString(it) }.sorted() }
        try {
            ActivityScenario.launch(MainActivity::class.java).use {
                assertEquals("startup replayed or rewrote pending installation", previous, root.list()!!.sorted())
                assertEquals("startup changed owned install contents", receipt.getString("tree"), snapshot())
            }
            if (phase == "after-stage-unlink") {
                val owner = controller()
                try {
                    val pending = owner.pendingVersionRemoval()!!; assertNotNull(pending.stage)
                    assertTrue(owner.resumeVersionRemoval(pending.id, true).complete)
                    assertTrue(root.list()!!.isEmpty())
                } finally { owner.close() }
                return@runBlocking
            }
            val id = receipt.getString("release"); val destination = File(root, id)
            val published = phase in listOf("after-rename", "after-publication-sync")
            assertEquals(published, destination.isDirectory)
            val journal = root.listFiles()!!.single { it.name.startsWith(".install-") }
            val record = JSONObject(journal.readText()); assertEquals(id, record.getString("target"))
            if (phase in listOf("after-reservation", "after-directory-create")) { assertTrue(record.isNull("directory")); assertEquals(phase == "after-directory-create", root.list()!!.any { it.startsWith(".prepare-") }) }
            else assertFalse(record.isNull("directory"))
            if (published) {
                assertEquals(320, Os.lstat(destination.path).st_mode and 511)
                val raw = File(destination, "release.json").readBytes()
                assertEquals(id, MessageDigest.getInstance("SHA-256").digest(raw).joinToString("") { "%02x".format(it.toInt() and 255) })
                val owner = controller()
                try {
                    val reviewed = owner.review(File(destination, "payload"))
                    owner.verifyDirectorySignature(reviewed.id, reviewed.digest, raw, File(destination, "release.sig").readBytes(), fixture.getString("policy").toByteArray())
                    assertEquals("stored version must not grant a missing activity choice", ExtensionRegistry.Status.DENIED, owner.resolve(reviewed.commands.first().path, reviewed.id, reviewed.digest).status)
                } finally { owner.close() }
            } else assertTrue(root.list()!!.none { it.matches(Regex("[0-9a-f]{64}")) })
            val owner = controller()
            try {
                val entry = owner.installationMaintenance().entries.single()
                if (phase == "after-directory-create") {
                    assertEquals(PluginInstallJournal.State.UNCONFIRMED, entry.state)
                    assertTrue(runCatching { owner.previewStageRemoval(java.util.UUID.randomUUID().toString(), entry.reference!!) }.isFailure)
                    assertTrue(File(root, record.getString("name")).isDirectory)
                } else if (published || phase == "after-reservation") {
                    assertEquals(if (published) PluginInstallJournal.State.PUBLISHED else PluginInstallJournal.State.ABSENT, entry.state)
                    assertTrue(owner.retireInstallationRecord(entry.reference!!, true))
                    assertEquals(published, destination.isDirectory)
                } else {
                    assertEquals(PluginInstallJournal.State.ABANDONED, entry.state)
                    val preview = owner.previewStageRemoval(java.util.UUID.randomUUID().toString(), entry.reference!!)
                    assertTrue(owner.confirmVersionRemoval(preview.id, true).complete)
                    assertTrue(root.list()!!.isEmpty())
                }
            } finally { owner.close() }
        } finally {
            PluginInstallTestFiles.clean(root)
            check(marker.delete() || !marker.exists())
            val archive = File(context.cacheDir, "plugin-install-death-$token.zip"); check(archive.delete() || !archive.exists())
        }
    }
}
