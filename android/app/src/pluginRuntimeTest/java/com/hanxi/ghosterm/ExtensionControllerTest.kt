package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class ExtensionControllerTest {
    private fun digest(bytes: ByteArray) = MessageDigest.getInstance("SHA-256")
        .digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }

    private fun packageDirectory(id: String): File {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = File(context.cacheDir, "controller-$id-${UUID.randomUUID()}")
        check(root.mkdirs())
        val array = { values: List<Any> -> JSONArray(values) }
        val manifest = JSONObject().put("schemaVersion", 1).put("id", id).put("pluginVersion", "0.1.0")
            .put("sdkApiRange", "^1.0.0").put("bridgeProtocolRange", array(listOf(1)))
            .put("publisher", "fixture").put("license", "MIT")
            .put("commands", array(listOf(JSONObject().put("path", array(listOf("fixture")))
                .put("title", "Fixture").put("argsSchema", "schemas/input.json"))))
            .put("client", JSONObject().put("runtime", "portable-js").put("entry", "client/index.js")
                .put("requires", array(listOf("session.rpc"))))
            .put("permissions", JSONArray())
        val schema = JSONObject().put("type", "object").put("properties", JSONObject())
            .put("required", JSONArray()).put("additionalProperties", false)
        val files = linkedMapOf(
            "plugin.json" to manifest.toString().toByteArray(),
            "README" to "fixture".toByteArray(),
            "LICENSES/MIT.txt" to "MIT".toByteArray(),
            "schemas/input.json" to schema.toString().toByteArray(),
            "client/index.js" to "export function activate(){return {dispose(){}}}".toByteArray(),
        )
        val members = JSONArray()
        for ((path, bytes) in files) {
            val file = File(root, path)
            check(file.parentFile!!.mkdirs() || file.parentFile!!.isDirectory)
            file.writeBytes(bytes)
            members.put(JSONObject().put("path", path).put("bytes", bytes.size).put("sha256", digest(bytes)))
        }
        File(root, "integrity.json").writeText(JSONObject().put("schemaVersion", 1).put("members", members).toString())
        return root
    }

    @Test fun reviewApprovalConflictDenialAndCloseAreExplicit() = runBlocking {
        val firstDir = packageDirectory("org.ghosterm.controller-first")
        val secondDir = packageDirectory("org.ghosterm.controller-second")
        val controller = ExtensionController(ExtensionController.HostProfile(
            sdkVersion = "1.0.0", protocols = listOf(1), runtimes = listOf("portable-js"),
            capabilities = listOf("session.rpc", "session.resources")),
            InstrumentationRegistry.getInstrumentation().targetContext)
        try {
            val first = controller.review(firstDir)
            val second = controller.review(secondDir)
            assertEquals(listOf("fixture"), first.commands.map { it.path })
            assertFalse(first.sourceApproved)
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.CONFLICT,
                controller.resolve("fixture").status)
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.UNKNOWN,
                controller.resolve("fixture", first.id, first.digest).status)
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK,
                controller.approve(first.id, first.digest))
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.CONFLICT,
                controller.resolve("fixture").status)
            val selected = controller.resolve("fixture", first.id, first.digest)
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.OK, selected.status)
            assertEquals(first.digest, selected.packageDigest)
            assertEquals("fixture", selected.command?.path)
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.DENIED,
                controller.deny(first.id, first.digest))
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.DENIED,
                controller.resolve("fixture", first.id, first.digest).status)
            assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.UNKNOWN,
                controller.resolve("fixture").status)
            assertEquals("org.ghosterm.controller-second", second.id)
        } finally {
            controller.close()
            firstDir.deleteRecursively()
            secondDir.deleteRecursively()
        }
        try {
            controller.resolve("fixture")
            fail("closed controller accepted work")
        } catch (_: IllegalStateException) { }
    }
}
