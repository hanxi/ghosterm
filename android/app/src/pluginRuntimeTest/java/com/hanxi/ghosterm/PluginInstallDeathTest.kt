package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.Process
import android.system.Os
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
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
    private val root get() = File(context.filesDir, "plugin-releases")
    private val arguments get() = InstrumentationRegistry.getArguments()
    private val token get() = arguments.getString("deathToken")!!.also { require(it.matches(Regex("[a-f0-9]{32}"))) }
    private val phase get() = arguments.getString("deathPhase")!!.also { require(it in listOf("after-reservation", "after-directory-record", "after-file-create", "before-rename", "after-rename", "after-publication-sync")) }
    private val marker get() = File(context.cacheDir, "plugin-install-death-$token.json")
    private val fixture get() = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("plugin-release/archive-declarations.json").bufferedReader().use { it.readText() })
    private fun controller() = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"), listOf("session.rpc")), context)
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
                        .put("release", preview.proof.descriptorSHA256).put("names", JSONArray(root.list()!!.sorted()))
                    marker.outputStream().use { out -> out.write(receipt.toString().toByteArray()); out.fd.sync() }
                    CountDownLatch(1).await()
                }
            })
            fail("death checkpoint did not block")
        } } finally { owner.close(); check(file.delete() || !file.exists()) }
    }
    @Test fun freshStartupObservesHeldNamespaceWithoutReplayThenVerifiesAndCleans() = runBlocking {
        val receipt = JSONObject(marker.readText()); assertEquals(phase, receipt.getString("phase")); assertEquals(token, receipt.getString("token"))
        val previous = receipt.getJSONArray("names").let { names -> (0 until names.length()).map { names.getString(it) }.sorted() }
        try {
            ActivityScenario.launch(MainActivity::class.java).use {
                assertEquals("startup replayed or rewrote pending installation", previous, root.list()!!.sorted())
            }
            val id = receipt.getString("release"); val destination = File(root, id)
            val published = phase in listOf("after-rename", "after-publication-sync")
            assertEquals(published, destination.isDirectory)
            val journal = root.listFiles()!!.single { it.name.startsWith(".install-") }
            val record = JSONObject(journal.readText()); assertEquals(id, record.getString("target"))
            if (phase == "after-reservation") { assertTrue(record.isNull("directory")); assertFalse(root.list()!!.any { it.startsWith(".prepare-") }) }
            else assertFalse(record.isNull("directory"))
            if (published) {
                assertEquals(320, Os.lstat(destination.path).st_mode and 511)
                val raw = File(destination, "release.json").readBytes()
                assertEquals(id, MessageDigest.getInstance("SHA-256").digest(raw).joinToString("") { "%02x".format(it.toInt() and 255) })
                val owner = controller()
                try {
                    val reviewed = owner.review(File(destination, "payload"))
                    owner.verifyDirectorySignature(reviewed.id, reviewed.digest, raw, File(destination, "release.sig").readBytes(), fixture.getString("policy").toByteArray())
                    assertEquals(ExtensionRegistry.Status.UNKNOWN, owner.resolve("fixture").status)
                } finally { owner.close() }
            } else assertTrue(root.list()!!.none { it.matches(Regex("[0-9a-f]{64}")) })
        } finally {
            PluginInstallTestFiles.clean(root)
            check(marker.delete() || !marker.exists())
            val archive = File(context.cacheDir, "plugin-install-death-$token.zip"); check(archive.delete() || !archive.exists())
        }
    }
}
