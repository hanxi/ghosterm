package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.assertTrue
import java.io.File
import java.util.Base64
import java.util.UUID
import java.util.zip.ZipInputStream

/** Disposable signed public fixtures, installed only under each test's root. */
internal object PluginVersionSelectionFixture {
    val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val fixture get() = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets.open("plugin-release/versions.json").bufferedReader().use { it.readText() })
    val policy get() = fixture.getString("policy").toByteArray()
    fun vector(name: String): JSONObject {
        val all = fixture.getJSONArray("vectors")
        return (0 until all.length()).map { all.getJSONObject(it) }.single { it.getString("name") == name }
    }
    fun controller(root: File) = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"), listOf("session.rpc")), context, root)
    suspend fun install(owner: ExtensionController, root: File, name: String = "1.0.0"): ExtensionController.InstalledRelease {
        val value = vector(name); val source = File(context.cacheDir, "version-archive-${UUID.randomUUID()}")
        source.writeBytes(Base64.getDecoder().decode(value.getString("archive")))
        try { ParcelFileDescriptor.open(source, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
            val raw = value.getString("descriptor").toByteArray(); val sig = Base64.getDecoder().decode(value.getString("signature"))
            val inspected = owner.inspectSignedArchive(fd, raw, sig, policy)
            return owner.installSignedArchive(fd, raw, sig, policy, inspected, root)
        } } finally { assertTrue(source.delete() || !source.exists()) }
    }
    /** Create an explicitly adversarial installed slot without using the
     * product installer. Its real signature deliberately disagrees with Core. */
    fun identityMismatch(root: File): String {
        val value = vector("identity"); val raw = value.getString("descriptor").toByteArray()
        val id = PluginReleaseCatalog.sha(raw)
        PluginReleaseStage.create(root, id).use { stage ->
            ZipInputStream(Base64.getDecoder().decode(value.getString("archive")).inputStream()).use { zip ->
                while (true) { val entry = zip.nextEntry ?: break; stage.write("payload/${entry.name}", zip.readBytes()) }
            }
            stage.write("release.json", raw); stage.write("release.sig", Base64.getDecoder().decode(value.getString("signature")))
            stage.write("policy.json", policy); stage.publish()
        }
        return id
    }
}
