package com.hanxi.ghosterm

import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.security.MessageDigest
import java.util.Base64

/** Actual MainActivity Settings -> DocumentsUI -> fixed Core package + JNI proof. */
@RunWith(AndroidJUnit4::class)
class PluginSignatureReviewUITest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val control = Uri.parse("content://com.hanxi.ghosterm.test.preview-package-control")
    private val assets get() = instrumentation.context.assets
    private fun install(files: Map<String, ByteArray>) {
        val payload = JSONObject()
        files.forEach { (name, bytes) -> payload.put(name, Base64.getEncoder().encodeToString(bytes)) }
        val result = instrumentation.targetContext.contentResolver.call(control, "install", null,
            Bundle().apply { putString("files", payload.toString()) })!!
        assertTrue(result.getString("error"), result.getBoolean("ok"))
    }
    private fun <T> main(body: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result = body() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun nodes(): List<AccessibilityNodeInfo> {
        val all = mutableListOf<AccessibilityNodeInfo>()
        fun visit(node: AccessibilityNodeInfo?) {
            if (node == null) return
            all += node
            for (index in 0 until node.childCount) visit(node.getChild(index))
        }
        visit(instrumentation.uiAutomation.rootInActiveWindow)
        return all
    }
    private fun click(vararg names: String): Boolean {
        for (candidate in nodes().filter { it.isVisibleToUser && (it.text?.toString() in names || it.contentDescription?.toString() in names) }) {
            when (tapLegacyDocumentRow(instrumentation, candidate)) { true -> return true; false -> continue; null -> Unit }
            var node: AccessibilityNodeInfo? = candidate
            while (node != null) {
                if (node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true
                node = node.parent
            }
        }
        return false
    }
    private fun awaitSystem(label: String, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 15000
        while (SystemClock.elapsedRealtime() < end) {
            if (predicate()) return
            SystemClock.sleep(40)
        }
        fail("$label: ${nodes().take(80).joinToString { "${it.text}/${it.contentDescription}/${it.viewIdResourceName}/click=${it.isClickable}/selected=${it.isSelected}/enabled=${it.isEnabled}" }}")
    }
    private fun pickerVisible() = nodes().any { it.packageName?.toString()?.contains("documentsui") == true }
    private fun returnedToApp(): Boolean {
        val tree = nodes()
        return tree.any { it.packageName?.toString() == "com.hanxi.ghosterm" } &&
            tree.none { it.packageName?.toString()?.contains("documentsui") == true }
    }
    private fun root() {
        awaitSystem("DocumentsUI did not open", ::pickerVisible)
        awaitSystem("roots drawer") { click("Show roots", "Open navigation drawer", "显示根目录", "显示位置") }
        awaitSystem("fixture root") { click("Ghosterm Official Preview Package Fixture") }
    }
    private fun choose(kind: PluginSignatureMaterial, name: String) {
        ui.onNodeWithTag("plugin-signature-pick-${kind.name}").performScrollTo().performClick()
        ui.waitForIdle()
        root()
        awaitSystem("DocumentsUI selected $name and returned its result") {
            if (returnedToApp()) true else {
                if (pickerVisible() && !click("SELECT", "Select", "选择", "OPEN", "Open", "打开")) click(name)
                false
            }
        }
        ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-signature-material-${kind.name}").fetchSemanticsNodes().isNotEmpty() }
    }
    private fun verify() {
        ui.onNodeWithTag("plugin-signature-verify").assertIsEnabled().performScrollTo().performClick()
        try {
            ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-signature-verified").fetchSemanticsNodes().isNotEmpty() ||
                ui.onAllNodesWithTag("plugin-signature-issue").fetchSemanticsNodes().isNotEmpty() }
        } catch (_: ComposeTimeoutException) { fail("signature verification produced no visible result") }
    }
    @Test fun realSafMaterialsRequirePolicyConfirmationAndDoNotApproveSource() {
        val packageFiles = linkedMapOf<String, ByteArray>()
        fun capture(path: String) {
            val asset = "plugin-release/package" + if (path.isEmpty()) "" else "/$path"
            val names = assets.list(asset)!!
            if (names.isEmpty()) packageFiles[path] = assets.open(asset).use { it.readBytes() }
            else names.forEach { capture(if (path.isEmpty()) it else "$path/$it") }
        }
        capture("")
        val material = JSONObject(assets.open("plugin-release/controller.json").bufferedReader().use { it.readText() })
        val policy = material.getString("policy").toByteArray()
        val publicFiles = linkedMapOf("release.json" to material.getString("descriptor").toByteArray(),
            "release.sig" to Base64.getDecoder().decode(material.getString("signature")), "channel.json" to policy)
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            install(packageFiles)
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithText("选择插件文件夹").performScrollTo().performClick()
            root()
            awaitSystem("SAF tree choice") { click("Use this folder", "USE THIS FOLDER", "使用此文件夹", "ALLOW ACCESS TO \"GHOSTERM OFFICIAL PREVIEW PACKAGE FIXTURE\"") }
            awaitSystem("SAF permission") { click("Allow", "ALLOW", "允许") || !pickerVisible() }
            awaitSystem("Settings return") { !pickerVisible() }
            ui.waitUntil(10000) { main { !extensions.busy && extensions.entries.size == 1 } }
            val entry = main { extensions.entries.single() }
            assertEquals(JSONObject(material.getString("descriptor")).getString("packageSHA256"), entry.review.digest)
            install(publicFiles)
            ui.onNodeWithTag("plugin-signature-disclosure").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-signature-verify").assertIsNotEnabled()
            choose(PluginSignatureMaterial.DESCRIPTOR, "release.json")
            choose(PluginSignatureMaterial.SIGNATURE, "release.sig")
            choose(PluginSignatureMaterial.POLICY, "channel.json")
            val hash = MessageDigest.getInstance("SHA-256").digest(policy).joinToString("") { "%02x".format(it.toInt() and 255) }
            ui.onNodeWithTag("plugin-signature-material-POLICY").assertTextContains(hash, substring = true)
            ui.onNodeWithTag("plugin-signature-trust").assertIsOff()
            ui.onNodeWithTag("plugin-signature-verify").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-signature-trust").performScrollTo().performClick()
            verify()
            ui.onNodeWithTag("plugin-signature-verified").assertTextContains("fixture", substring = true)
            assertEquals("尚未批准", main { extensions.entries.single().decision })
            assertFalse(entry.review.publisherAuthenticated); assertFalse(entry.review.authorizationGranted)
            assertNull(main { extensions.activeSession }); assertNull(main { extensions.activeRuntime })
            assertEquals(ExtensionRegistry.Status.UNKNOWN, runBlocking { extensions.controller!!.resolve("fixture", entry.review.id, entry.review.digest).status })

            instrumentation.uiAutomation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK)
            val settings = ViewModelProvider(ui.activity)[SettingsViewModel::class.java]
            ui.waitUntil(10000) { main { settings.section == null } }
            ui.waitForIdle()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-signature-disclosure").performScrollTo().performClick()
            PluginSignatureMaterial.entries.forEach { ui.onNodeWithTag("plugin-signature-material-${it.name}").assertDoesNotExist() }
            ui.onNodeWithTag("plugin-signature-verified").assertDoesNotExist()
            ui.onNodeWithTag("plugin-signature-trust").assertIsOff()
            choose(PluginSignatureMaterial.DESCRIPTOR, "release.json")
            choose(PluginSignatureMaterial.SIGNATURE, "release.sig")
            choose(PluginSignatureMaterial.POLICY, "channel.json")
            ui.onNodeWithTag("plugin-signature-trust").performScrollTo().performClick(); verify()
            ui.onNodeWithTag("plugin-signature-verified").assertExists()

            // Editing the provider does not silently replace the selected policy
            // snapshot; selecting it again clears trust and adopts new bytes.
            val revoked = JSONObject(String(policy)).apply { getJSONArray("revokedKeyIds").put(getJSONArray("keys").getJSONObject(0).getString("id")) }.toString().toByteArray()
            install(publicFiles + ("channel.json" to revoked))
            ui.onNodeWithTag("plugin-signature-trust").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-signature-verified").assertDoesNotExist()
            ui.onNodeWithTag("plugin-signature-verify").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-signature-trust").performClick(); verify()
            ui.onNodeWithTag("plugin-signature-verified").assertExists()
            choose(PluginSignatureMaterial.POLICY, "channel.json")
            ui.onNodeWithTag("plugin-signature-trust").assertIsOff()
            ui.onNodeWithTag("plugin-signature-verified").assertDoesNotExist()
            ui.onNodeWithTag("plugin-signature-verify").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-signature-trust").performScrollTo().performClick(); verify()
            ui.onNodeWithTag("plugin-signature-issue").assertTextContains("撤销", substring = true)

            ui.onNodeWithTag("plugin-signature-pick-SIGNATURE").performScrollTo().performClick()
            ui.waitForIdle()
            awaitSystem("cancel picker opened", ::pickerVisible)
            instrumentation.uiAutomation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK)
            awaitSystem("cancel picker returned", ::returnedToApp)
            ui.onNodeWithTag("plugin-signature-material-SIGNATURE").assertDoesNotExist()
            ui.onNodeWithTag("plugin-signature-verify").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-signature-disclosure").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-signature-disclosure").performClick()
            PluginSignatureMaterial.entries.forEach { ui.onNodeWithTag("plugin-signature-material-${it.name}").assertDoesNotExist() }
            ui.onNodeWithTag("plugin-signature-trust").assertIsOff()
            ui.onNodeWithTag("plugin-signature-issue").assertDoesNotExist()
        } finally {
            runBlocking { extensions.controller?.close() }
            val tree = android.provider.DocumentsContract.buildTreeDocumentUri(PreviewPackageDocumentsProvider.AUTHORITY, "preview-root")
            instrumentation.targetContext.contentResolver.call(control, "cleanup", null, Bundle().apply {
                putString("uri", tree.toString()); putInt("flags", android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or android.content.Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
            })
        }
    }
}
