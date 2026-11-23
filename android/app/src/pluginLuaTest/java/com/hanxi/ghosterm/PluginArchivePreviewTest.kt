package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.Base64
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class PluginArchivePreviewTest {
    @Test fun independentInspectorBindsFullDeclarationsAndKeepsParentUsable() = runBlocking {
        val instrumentation = InstrumentationRegistry.getInstrumentation(); val context = instrumentation.targetContext
        val fixture = JSONObject(instrumentation.context.assets.open("plugin-release/archive-lua-declarations.json").bufferedReader().use { it.readText() })
        val vector = fixture.getJSONArray("vectors").getJSONObject(0)
        val protocols = mutableListOf(1)
        val parent = ExtensionController(ExtensionController.HostProfile("1.0.0", protocols, listOf("lua"), listOf("session.rpc")), context)
        protocols.clear() // A later caller edit must not change the inspector profile.
        val child = parent.newReleaseInspector()
        val file = File(context.cacheDir, "archive-preview-${UUID.randomUUID()}").apply { writeBytes(Base64.getDecoder().decode(vector.getString("archive"))) }
        try {
            assertNotSame("ZIP work must use its own controller", parent, child)
            val preview = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                child.inspectSignedArchive(fd, vector.getString("descriptor").toByteArray(), Base64.getDecoder().decode(vector.getString("signature")), fixture.getString("policy").toByteArray())
            }
            assertEquals(listOf("client" to "session.rpc", "executor" to "session.rpc",
                "executor" to "process.spawn", "executor" to "process.pipes"),
                preview.declarations.permissions.map { it.side to it.capability })
            val permission = preview.declarations.permissions.single { it.capability == "process.spawn" }
            assertEquals(listOf("fixture-worker"), permission.programs)
            assertEquals(listOf(1), preview.declarations.protocolRange); assertEquals(listOf(1), preview.declarations.protocols)
            val executor = JSONObject(preview.declarations.executorJSON!!)
            assertEquals("executor.lua", executor.getString("entry"))
            assertEquals("fixture.run", executor.getJSONArray("rpc").getString(0))
            child.close()
            try { assertEquals(com.hanxi.ghosterm.bridge.ExtensionRegistry.Status.NOT_FOUND, parent.resolve("fixture").status) }
            catch (e: IllegalStateException) { fail("closing independent inspector closed parent: ${e.message}") }
        } finally { child.close(); parent.close(); assertTrue(file.delete() || !file.exists()) }
    }
    @Test fun closedParentCannotCreateAnotherInspector() = runBlocking {
        val parent = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("lua"), listOf("session.rpc")), InstrumentationRegistry.getInstrumentation().targetContext)
        parent.close()
        try { parent.newReleaseInspector().close(); fail("closed parent created inspector") } catch (_: IllegalStateException) { }
    }
}
