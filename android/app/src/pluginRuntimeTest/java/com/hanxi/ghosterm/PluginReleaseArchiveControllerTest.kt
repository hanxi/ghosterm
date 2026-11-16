package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.Base64
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginReleaseArchiveControllerTest {
    private val testContext get() = InstrumentationRegistry.getInstrumentation().context
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private fun controller() = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"), listOf("session.rpc")), context)
    private val fixture get() = JSONObject(testContext.assets.open("plugin-release/archive.json").bufferedReader().use { it.readText() })
    private fun vector(name: String): JSONObject {
        val values = fixture.getJSONArray("vectors")
        return (0 until values.length()).map { values.getJSONObject(it) }.single { it.getString("name") == name }
    }
    private fun staging() = context.cacheDir.list()!!.filter { it.startsWith("signed-release-review-") }.sorted()
    private suspend fun preview(controller: ExtensionController, name: String): ExtensionController.SignedArchiveReview {
        val data = vector(name); val file = File(context.cacheDir, "signed-archive-${UUID.randomUUID()}")
        file.writeBytes(Base64.getDecoder().decode(data.getString("archive")))
        return try {
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                controller.inspectSignedArchive(fd, data.getString("descriptor").toByteArray(),
                    Base64.getDecoder().decode(data.getString("signature")), fixture.getString("policy").toByteArray())
            }
        } finally { assertTrue(file.delete() || !file.exists()) }
    }
    @Test fun signedStoredAndDeflatedArchivesBindRealCoreWithoutSourceOrSession() = runBlocking {
        val controller = controller(); val before = staging()
        try {
            for (name in listOf("stored", "deflated")) {
                val result = preview(controller, name)
                assertEquals("org.ghosterm.signature", result.review.id)
                assertEquals("1.0.0", result.review.version)
                assertEquals(result.review.digest, result.proof.packageSHA256)
                assertEquals(JSONObject(vector(name).getString("descriptor")).getString("packageSHA256"), result.review.digest)
                assertFalse(result.review.sourceApproved); assertFalse(result.review.authorizationGranted)
                assertFalse(result.review.publisherAuthenticated)
                assertEquals(ExtensionRegistry.Status.STALE, controller.resolve("fixture", result.review.id, result.review.digest).status)
                try { controller.stepSession(); fail("archive created Session") }
                catch (e: IllegalStateException) { assertTrue(e.message!!.contains("尚未接管")) }
                try { controller.stepRuntime(); fail("archive started Runtime") }
                catch (e: IllegalStateException) { assertTrue(e.message!!.contains("尚未启动")) }
                assertEquals(before, staging())
            }
        } finally {
            controller.close()
            // This isolated test owns new previews since its before snapshot;
            // clean fault-injected leftovers even when an assertion has failed.
            for (name in staging() - before.toSet()) assertTrue(File(context.cacheDir, name).deleteRecursively())
        }
    }
    @Test fun signedIdentityMismatchIsRejectedAfterRealCoreCapture() = runBlocking {
        val controller = controller(); val before = staging()
        try {
            try { preview(controller, "identity"); fail("signed claim replaced actual Core identity") }
            catch (e: PluginReleaseSignature.Rejected) { assertEquals(PluginReleaseSignature.Failure.IDENTITY, e.reason) }
            assertEquals(before, staging())
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, controller.resolve("fixture").status)
            assertNotNull(preview(controller, "stored"))
        } finally { controller.close() }
    }
    @Test fun signedArchiveCrcAndCoreIntegrityFailuresLeaveNoStage() = runBlocking {
        val controller = controller(); val before = staging()
        try {
            try { preview(controller, "crc"); fail("signed archive bypassed CRC") }
            catch (e: PluginReleaseArchive.Rejected) { assertEquals(PluginReleaseArchive.Failure.CONTENT, e.reason) }
            assertEquals(before, staging())
            try { preview(controller, "integrity"); fail("signature bypassed actual Core integrity") }
            catch (e: IllegalStateException) { assertTrue(e.message!!.contains("不兼容")) }
            assertEquals(before, staging()); assertNotNull(preview(controller, "deflated"))
        } finally { controller.close(); assertEquals(before, staging()) }
    }
    @Test fun signatureIsAuthenticatedBeforeArchiveIoAndNoRejectedClaimEntersRegistry() = runBlocking {
        val controller = controller(); val before = staging(); val data = vector("stored")
        val pipe = ParcelFileDescriptor.createPipe()
        try {
            val signature = Base64.getDecoder().decode(data.getString("signature")); signature[0] = (signature[0].toInt() xor 1).toByte()
            try {
                controller.inspectSignedArchive(pipe[0], data.getString("descriptor").toByteArray(), signature, fixture.getString("policy").toByteArray())
                fail("invalid signature accepted")
            } catch (e: PluginReleaseSignature.Rejected) { assertEquals(PluginReleaseSignature.Failure.SIGNATURE, e.reason) }
            assertEquals(before, staging()); assertEquals(ExtensionRegistry.Status.NOT_FOUND, controller.resolve("fixture").status)
        } finally { pipe.forEach { it.close() }; controller.close() }
    }
    @Test fun repeatedPreviewNeverRegistersPackageOrLeaksClosedControllerDescriptors() = runBlocking {
        val controller = controller(); val before = staging()
        val root = File(context.cacheDir, "archive-package-${UUID.randomUUID()}")
        fun copy(asset: String, destination: File) {
            val names = testContext.assets.list(asset)!!
            if (names.isNotEmpty()) { check(destination.mkdirs() || destination.isDirectory); for (name in names) copy("$asset/$name", File(destination, name)) }
            else testContext.assets.open(asset).use { input -> destination.outputStream().use { input.copyTo(it) } }
        }
        try {
            repeat(3) { preview(controller, "stored") }
            copy("plugin-release/package", root)
            val review = controller.review(root) // Would conflict if preview had inserted the Core owner.
            assertFalse(review.sourceApproved)
            controller.close()
            val fdCount = File("/proc/self/fd").list()!!.size
            repeat(12) {
                try { preview(controller, "stored"); fail("closed controller accepted archive") }
                catch (_: IllegalStateException) { }
            }
            assertTrue("controller leaked archive descriptor", File("/proc/self/fd").list()!!.size <= fdCount + 2)
            assertEquals(before, staging())
        } finally { controller.close(); assertTrue(root.deleteRecursively() || !root.exists()) }
    }
}
