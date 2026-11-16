package com.hanxi.ghosterm

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
class PluginReleaseControllerTest {
    private fun directory(): File {
        val testContext = InstrumentationRegistry.getInstrumentation().context
        val target = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(target.cacheDir, "release-signature-${UUID.randomUUID()}").apply { check(mkdirs()) }
        fun copy(asset: String, destination: File) {
            val entries = testContext.assets.list(asset)!!
            if (entries.isNotEmpty()) {
                check(destination.mkdirs() || destination.isDirectory)
                for (name in entries) copy("$asset/$name", File(destination, name))
            } else testContext.assets.open(asset).use { input -> destination.outputStream().use { input.copyTo(it) } }
        }
        copy("plugin-release/package", root)
        return root
    }
    @Test fun signatureBindsCoreSnapshotWithoutSourceOrRuntimeGrant() = runBlocking {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val material = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets
            .open("plugin-release/controller.json").bufferedReader().use { it.readText() })
        val descriptor = material.getString("descriptor").toByteArray()
        val policy = material.getString("policy").toByteArray()
        val signature = Base64.getDecoder().decode(material.getString("signature"))
        val controller = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1),
            listOf("portable-js"), listOf("session.rpc")), context)
        val root = directory()
        try {
            val review = controller.review(root)
            assertEquals(JSONObject(String(descriptor)).getString("packageSHA256"), review.digest)
            assertFalse(review.sourceApproved); assertFalse(review.publisherAuthenticated)
            assertFalse(review.authorizationGranted)
            val proof = controller.verifyDirectorySignature(review.id, review.digest, descriptor, signature, policy)
            assertEquals(review.digest, proof.packageSHA256)
            assertEquals(ExtensionRegistry.Status.UNKNOWN, controller.resolve("fixture", review.id, review.digest).status)
            val unrelated = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets
                .open("plugin-release/vector.json").bufferedReader().use { it.readText() })
            try {
                controller.verifyDirectorySignature(review.id, review.digest,
                    unrelated.getString("descriptor").toByteArray(),
                    Base64.getDecoder().decode(unrelated.getString("signature")),
                    unrelated.getString("policy").toByteArray())
                fail("signed claims replaced the actual Core package identity")
            } catch (expected: PluginReleaseSignature.Rejected) {
                assertEquals(PluginReleaseSignature.Failure.IDENTITY, expected.reason)
            }
            // Core owns fixed bytes. A later change of the provider directory
            // cannot change the identity to which the public proof is bound.
            File(root, "plugin.json").writeText("{}")
            assertEquals(proof, controller.verifyDirectorySignature(review.id, review.digest, descriptor, signature, policy))
            try {
                controller.verifyDirectorySignature(review.id, "0".repeat(64), descriptor, signature, policy)
                fail("unknown package owner accepted")
            } catch (expected: IllegalStateException) { assertTrue(expected.message!!.contains("失效")) }
            controller.close()
            try {
                controller.verifyDirectorySignature(review.id, review.digest, descriptor, signature, policy)
                fail("closed controller accepted")
            } catch (_: IllegalStateException) { }
        } finally { controller.close(); check(root.deleteRecursively() || !root.exists()) }
    }
}
