package com.hanxi.ghosterm

import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.graphics.Rect
import android.view.InputDevice
import android.view.MotionEvent
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
class PluginArchiveReviewUITest {
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
            if (candidate.text?.toString() in setOf("plugin.zip", "release.json", "release.sig", "channel.json") &&
                candidate.packageName?.toString()?.contains("documentsui") == true) {
                if (touchFile(candidate)) return true else continue
            }
            var node: AccessibilityNodeInfo? = candidate
            while (node != null) {
                if (node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true
                node = node.parent
            }
        }
        return false
    }
    private fun touchFile(node: AccessibilityNodeInfo): Boolean {
        val text = node.text?.toString()
        try { instrumentation.uiAutomation.waitForIdle(100, 1500) }
        catch (_: java.util.concurrent.TimeoutException) { return false }
        if (!node.refresh() || !node.isVisibleToUser || !node.isEnabled || node.text?.toString() != text) return false
        val bounds = Rect(); node.getBoundsInScreen(bounds)
        var parent = node.parent
        while (parent != null) {
            val clip = Rect(); parent.getBoundsInScreen(clip)
            if (!bounds.intersect(clip)) return false
            parent = parent.parent
        }
        if (bounds.isEmpty) return false
        val start = SystemClock.uptimeMillis()
        val properties = arrayOf(MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER })
        val coordinates = arrayOf(MotionEvent.PointerCoords().apply { x = bounds.exactCenterX(); y = bounds.exactCenterY(); pressure = 1f; size = 1f })
        fun inject(action: Int): Boolean {
            val event = MotionEvent.obtain(start, SystemClock.uptimeMillis(), action, 1, properties, coordinates,
                0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            return try { instrumentation.uiAutomation.injectInputEvent(event, true) } finally { event.recycle() }
        }
        if (!inject(MotionEvent.ACTION_DOWN)) return false
        SystemClock.sleep(60)
        val result = inject(MotionEvent.ACTION_UP)
        SystemClock.sleep(150)
        return result
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
    private fun choose(kind: PluginArchiveMaterial, name: String) {
        ui.onNodeWithTag("plugin-zip-pick-${kind.name}").performScrollTo().performClick()
        ui.waitForIdle()
        root()
        awaitSystem("DocumentsUI selected $name and returned its result") {
            if (returnedToApp()) true else {
                if (pickerVisible() && !click("SELECT", "Select", "选择", "OPEN", "Open", "打开")) click(name)
                false
            }
        }
        ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-zip-material-${kind.name}").fetchSemanticsNodes().isNotEmpty() }
    }
    private fun verify() {
        ui.onNodeWithTag("plugin-zip-check").assertIsEnabled().performScrollTo().performClick()
        ui.waitForIdle()
        try { ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-zip-verified").fetchSemanticsNodes().isNotEmpty() ||
            ui.onAllNodesWithTag("plugin-zip-issue").fetchSemanticsNodes().isNotEmpty() } }
        catch (_: ComposeTimeoutException) { fail("ZIP inspection produced no visible result") }
    }
    @Test fun actualSafZipAndDeclarationsRequireExplicitTrustAndReleaseOnLeaving() {
        val fixture = JSONObject(assets.open("plugin-release/archive-declarations.json").bufferedReader().use { it.readText() })
        val vector = fixture.getJSONArray("vectors").getJSONObject(0)
        val policy = fixture.getString("policy").toByteArray()
        val files = linkedMapOf("plugin.zip" to Base64.getDecoder().decode(vector.getString("archive")),
            "release.json" to vector.getString("descriptor").toByteArray(), "release.sig" to Base64.getDecoder().decode(vector.getString("signature")),
            "channel.json" to policy)
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            install(files)
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            assertTrue("ZIP review entry missing from Settings", ui.onAllNodesWithTag("plugin-zip-disclosure").fetchSemanticsNodes().size == 1)
            ui.onNodeWithTag("plugin-zip-disclosure").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-zip-check").assertIsNotEnabled()
            choose(PluginArchiveMaterial.ARCHIVE, "plugin.zip")
            choose(PluginArchiveMaterial.DESCRIPTOR, "release.json")
            choose(PluginArchiveMaterial.SIGNATURE, "release.sig")
            choose(PluginArchiveMaterial.POLICY, "channel.json")
            ui.onNodeWithTag("plugin-zip-trust").assertIsOff(); ui.onNodeWithTag("plugin-zip-check").assertIsNotEnabled()
            val hash = MessageDigest.getInstance("SHA-256").digest(files.getValue("plugin.zip")).joinToString("") { "%02x".format(it.toInt() and 255) }
            ui.onNodeWithTag("plugin-zip-material-ARCHIVE").assertTextContains(hash, substring = true)
            ui.onNodeWithTag("plugin-zip-trust").performScrollTo().performClick(); verify()
            ui.onNodeWithTag("plugin-zip-verified").assertTextContains("org.ghosterm.signature", substring = true)
            assertTrue("permission declaration missing from verified ZIP", ui.onAllNodesWithText("Run only the separately authorized fixture worker for this instance.", substring = true).fetchSemanticsNodes().size == 1)
            ui.onNodeWithText("Run only the separately authorized fixture worker for this instance.", substring = true).performScrollTo().assertIsDisplayed()
            ui.onNodeWithText("Executor 声明", substring = true).performScrollTo().assertTextContains("^1.2.0", substring = true)
            assertTrue(main { extensions.entries.isEmpty() }); assertNull(main { extensions.activeSession }); assertNull(main { extensions.activeRuntime })
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, runBlocking { extensions.controller!!.resolve("fixture").status })

            // A later provider edit cannot replace the captured ZIP fd.
            install(files + ("plugin.zip" to ByteArray(32)))
            verify(); ui.onNodeWithTag("plugin-zip-verified").assertExists()
            choose(PluginArchiveMaterial.ARCHIVE, "plugin.zip")
            ui.onNodeWithTag("plugin-zip-verified").assertDoesNotExist()
            verify(); ui.onNodeWithTag("plugin-zip-issue").assertTextContains("摘要", substring = true)
            install(files); choose(PluginArchiveMaterial.ARCHIVE, "plugin.zip"); verify()
            ui.onNodeWithTag("plugin-zip-verified").assertExists()

            val revoked = JSONObject(String(policy)).apply { getJSONArray("revokedKeyIds").put(getJSONArray("keys").getJSONObject(0).getString("id")) }.toString().toByteArray()
            install(files + ("channel.json" to revoked))
            choose(PluginArchiveMaterial.POLICY, "channel.json")
            ui.onNodeWithTag("plugin-zip-trust").assertIsOff(); ui.onNodeWithTag("plugin-zip-check").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-zip-trust").performScrollTo().performClick(); verify()
            ui.onNodeWithTag("plugin-zip-issue").assertTextContains("撤销", substring = true)

            ui.onNodeWithTag("plugin-zip-pick-ARCHIVE").performScrollTo().performClick(); ui.waitForIdle()
            awaitSystem("cancel archive picker", ::pickerVisible)
            instrumentation.uiAutomation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK)
            awaitSystem("cancel archive returned", ::returnedToApp)
            ui.onNodeWithTag("plugin-zip-material-ARCHIVE").assertDoesNotExist(); ui.onNodeWithTag("plugin-zip-check").assertIsNotEnabled()
            instrumentation.uiAutomation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK)
            val settings = ViewModelProvider(ui.activity)[SettingsViewModel::class.java]
            ui.waitUntil(10000) { main { settings.section == null } }
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-zip-disclosure").performScrollTo().performClick()
            PluginArchiveMaterial.entries.forEach { ui.onNodeWithTag("plugin-zip-material-${it.name}").assertDoesNotExist() }
            ui.onNodeWithTag("plugin-zip-trust").assertIsOff(); ui.onNodeWithTag("plugin-zip-verified").assertDoesNotExist()
            ui.onNodeWithTag("plugin-zip-issue").assertDoesNotExist()
        } finally {
            runBlocking { extensions.controller?.close() }
            val tree = android.provider.DocumentsContract.buildTreeDocumentUri(PreviewPackageDocumentsProvider.AUTHORITY, "preview-root")
            instrumentation.targetContext.contentResolver.call(control, "cleanup", null, Bundle().apply {
                putString("uri", tree.toString()); putInt("flags", android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or android.content.Intent.FLAG_GRANT_PREFIX_URI_PERMISSION)
            })
        }
    }
}
