package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest
import java.util.Base64
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class LuaReleaseInstallTest {
    @Test fun installsAllOfficialLuaPackagesAndReopensDeclarations() = runBlocking {
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("Requires the signed official Lua fixtures", args.containsKey("officialReleaseRoot"))
        assertTrue(BuildConfig.PLUGIN_RUNTIME_ENABLED)
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val fixtures = File(checkNotNull(args.getString("officialReleaseRoot")))
        val root = PluginInstallTestFiles.root()
        val store = File(root, "versions").apply { check(mkdir()) }
        val owner = ExtensionController(ExtensionController.HostProfile("1.0.0", listOf(1), listOf("lua"),
            listOf("session.rpc", "resource.receive", "file.readScoped", "ui.table", "ui.diff", "ui.task", "ui.views", "ui.form")),
            context, store)
        try {
            for (name in listOf("preview", "table", "diff", "task", "views", "logs", "form", "ai-codex", "ai-claude")) {
                val release = File(fixtures, "official-$name-release")
                val descriptor = File(release, "release.json").readBytes()
                val signature = File(release, "release.sig").readBytes()
                val policy = File(fixtures, "official-$name-policy.json").readBytes()
                ParcelFileDescriptor.open(File(release, "plugin.zip"), ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                    val review = owner.inspectSignedArchive(fd, descriptor, signature, policy)
                    assertEquals("org.ghosterm.$name", review.review.id)
                    assertFalse(review.review.sourceApproved)
                    val installed = owner.installSignedArchive(fd, descriptor, signature, policy, review, store)
                    assertTrue(installed.publication.durabilityConfirmed)
                    assertEquals(review, installed.snapshot)
                    val payload = File(installed.publication.directory, "payload")
                    assertTrue(File(payload, "manifest.json").isFile)
                    assertTrue(File(payload, "main.lua").isFile)
                    val digest = MessageDigest.getInstance("SHA-256").digest(File(payload, "integrity.json").readBytes())
                        .joinToString("") { "%02x".format(it.toInt() and 255) }
                    assertEquals(review.review.digest, digest)
                    val command = review.review.commands.first().path
                    assertNotEquals(ExtensionRegistry.Status.OK, owner.resolve(command, review.review.id, digest).status)
                    val reopened = owner.previewInstalledVersion(UUID.randomUUID().toString(),
                        review.proof.descriptorSHA256, null, policy)
                    assertEquals(review.declarations, reopened.candidate.declarations)
                    assertFalse(reopened.candidate.review.sourceApproved)
                    owner.cancelVersionPreview(reopened.id)
                    owner.installedVersions()
                }
            }
        } finally {
            owner.close()
            PluginInstallTestFiles.clean(root)
        }
    }

    @Test fun installsSignedPreviewAndReopensDeclarationsWithoutApprovingSource() = runBlocking {
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("Requires the signed Lua Preview fixture", args.containsKey("luaReleaseArchive"))
        assertTrue(BuildConfig.PLUGIN_RUNTIME_ENABLED)
        fun fixture(name: String) = Base64.getDecoder().decode(checkNotNull(args.getString(name)))
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val root = PluginInstallTestFiles.root()
        val store = File(root, "versions").apply { check(mkdir()) }
        val archive = File(root, "preview.zip").apply { writeBytes(fixture("luaReleaseArchive")) }
        val descriptor = fixture("luaReleaseDescriptor")
        val signature = fixture("luaReleaseSignature")
        val policy = fixture("luaReleasePolicy")
        val owner = ExtensionController(ExtensionController.HostProfile(
            "1.0.0", listOf(1), listOf("lua"), listOf("session.rpc", "resource.receive")), context, store)
        try {
            ParcelFileDescriptor.open(archive, ParcelFileDescriptor.MODE_READ_ONLY).use { fd ->
                val review = owner.inspectSignedArchive(fd, descriptor, signature, policy)
                assertEquals("org.ghosterm.preview", review.review.id)
                assertEquals(setOf("session.rpc", "resource.receive", "file.readScoped"),
                    review.declarations.permissions.map { it.capability }.toSet())
                assertEquals(setOf("client", "executor"), review.declarations.permissions.map { it.side }.toSet())
                assertFalse(review.review.sourceApproved)
                val installed = owner.installSignedArchive(fd, descriptor, signature, policy, review, store)
                assertTrue(installed.publication.durabilityConfirmed)
                assertEquals(review, installed.snapshot)
                val manifest = File(installed.publication.directory, "payload/manifest.json")
                assertTrue(manifest.isFile)
                assertEquals(0, Os.lstat(manifest.path).st_mode and 0x92)
                assertEquals(ExtensionRegistry.Status.NOT_FOUND, owner.resolve("preview").status)
                val version = owner.previewInstalledVersion(UUID.randomUUID().toString(),
                    review.proof.descriptorSHA256, null, policy)
                assertEquals(review.declarations, version.candidate.declarations)
                assertFalse(version.candidate.review.sourceApproved)
            }
        } finally {
            owner.close()
            PluginInstallTestFiles.clean(root)
        }
    }
}
