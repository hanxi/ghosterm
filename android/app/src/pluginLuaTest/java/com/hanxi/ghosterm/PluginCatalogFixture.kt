package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.assertTrue
import java.io.File
import java.util.Base64
import java.util.UUID

internal object PluginCatalogFixture {
    val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    fun controller() = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("lua"), listOf("session.rpc")), context)
    suspend fun install(owner: ExtensionController, root: File, name: String = "stored"): ExtensionController.InstalledRelease {
        val fixture = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("plugin-release/archive-lua-declarations.json").bufferedReader().use { it.readText() })
        val vectors = fixture.getJSONArray("vectors")
        val value = (0 until vectors.length()).map { vectors.getJSONObject(it) }.single { it.getString("name") == name }
        val source = File(context.cacheDir, "catalog-public-archive-${UUID.randomUUID()}")
        source.writeBytes(Base64.getDecoder().decode(value.getString("archive")))
        try { ParcelFileDescriptor.open(source, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
            val raw = value.getString("descriptor").toByteArray(); val sig = Base64.getDecoder().decode(value.getString("signature")); val policy = fixture.getString("policy").toByteArray()
            val inspected = owner.inspectSignedArchive(fd, raw, sig, policy)
            return owner.installSignedArchive(fd, raw, sig, policy, inspected, root)
        } } finally { assertTrue(source.delete() || !source.exists()) }
    }
}
