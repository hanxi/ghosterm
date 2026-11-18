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
class PluginInstallUITest {
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
        ui.onNodeWithTag("plugin-zip-check").performScrollTo().performClick()
        ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-zip-verified").fetchSemanticsNodes().isNotEmpty() || ui.onAllNodesWithTag("plugin-zip-issue").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("plugin-zip-verified").assertExists()
    }
    @Test fun explicitInstallationPublishesReadonlyVersionWithoutApprovingOrActivating() {
        val context = instrumentation.targetContext
        val fixture = JSONObject(assets.open("plugin-release/archive-declarations.json").bufferedReader().use { it.readText() })
        val vector = fixture.getJSONArray("vectors").getJSONObject(0)
        val descriptor = vector.getString("descriptor").toByteArray()
        val signature = Base64.getDecoder().decode(vector.getString("signature"))
        val policy = fixture.getString("policy").toByteArray()
        val releaseID = MessageDigest.getInstance("SHA-256").digest(descriptor).joinToString("") { "%02x".format(it.toInt() and 255) }
        val arguments = InstrumentationRegistry.getArguments()
        val channelSignature = arguments.getString("settingsChannelSignature")
        val cache = java.io.File(context.filesDir, "plugin-channel-policy")
        var ownsCache = false
        val root = java.io.File(context.filesDir, "plugin-releases")
        val before = root.list()?.toSet() ?: emptySet()
        val destination = java.io.File(root, releaseID)
        assertFalse("use an owned fresh test device; fixture slot already exists", destination.exists())
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            if (channelSignature != null) {
                assertFalse("use an owned fresh cache fixture", cache.exists())
                android.system.Os.mkdir(cache.path, 448)
                ownsCache = true
                val statement = Base64.getDecoder().decode(checkNotNull(arguments.getString("settingsChannelStatement")))
                val refresh = checkNotNull(PluginChannelRefresh.configured(context))
                refresh.store.refresh(statement, Base64.getDecoder().decode(channelSignature), policy,
                    (System.currentTimeMillis() / 1000).toULong())
            }
            install(mapOf("plugin.zip" to Base64.getDecoder().decode(vector.getString("archive")), "release.json" to descriptor, "release.sig" to signature, "channel.json" to policy))
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-zip-disclosure").performScrollTo().performClick()
            if (channelSignature != null) {
                ui.onNodeWithTag("plugin-channel-refresh").assertExists()
                ui.waitUntil(10000) {
                    ui.onAllNodes(hasTestTag("plugin-channel-cache") and isEnabled()).fetchSemanticsNodes().isNotEmpty()
                }
                ui.onNodeWithTag("plugin-channel-cache").performScrollTo()
                ui.onNodeWithTag("plugin-channel-refresh-status").assertDoesNotExist()
                ui.onNodeWithTag("plugin-channel-stop").assertDoesNotExist()
                ui.onNodeWithTag("plugin-channel-cache").performClick()
                ui.waitUntil(10000) {
                    ui.onAllNodesWithTag("plugin-channel-refresh-status").fetchSemanticsNodes().isNotEmpty()
                }
                ui.onNodeWithTag("plugin-channel-refresh-status").assertTextContains("第 1 代", substring = true)
                ui.onNodeWithTag("plugin-channel-affected").assertDoesNotExist()
                ui.onNodeWithTag("plugin-channel-stop").assertDoesNotExist()
                ui.onNodeWithTag("plugin-zip-channel").performScrollTo().performClick()
                ui.onNodeWithTag("plugin-zip-pick-POLICY").assertDoesNotExist()
            }
            choose(PluginArchiveMaterial.ARCHIVE, "plugin.zip"); choose(PluginArchiveMaterial.DESCRIPTOR, "release.json")
            choose(PluginArchiveMaterial.SIGNATURE, "release.sig")
            if (channelSignature == null) {
                choose(PluginArchiveMaterial.POLICY, "channel.json")
                ui.onNodeWithTag("plugin-zip-trust").performScrollTo().performClick()
            }
            verify()
            if (channelSignature != null)
                ui.onNodeWithText("已验证渠道策略 · 第 1 代").assertExists()
            ui.onNodeWithTag("plugin-zip-install").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-zip-install-confirmation").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-zip-install").assertIsEnabled().performScrollTo().performClick()
            try { ui.waitUntil(20000) { ui.onAllNodesWithTag("plugin-zip-installed").fetchSemanticsNodes().isNotEmpty() || ui.onAllNodesWithTag("plugin-zip-issue").fetchSemanticsNodes().isNotEmpty() } }
            catch (_: ComposeTimeoutException) { fail("installation produced no visible receipt") }
            ui.onNodeWithTag("plugin-zip-installed").assertTextContains("本机版本已发布", substring = true)
            ui.onNodeWithTag("plugin-zip-installed").assertTextContains("目录同步已确认", substring = true)
            ui.onNodeWithTag("plugin-zip-install").assertIsNotEnabled()
            assertTrue("no published version at expected slot", destination.isDirectory)
            assertArrayEquals(descriptor, java.io.File(destination, "release.json").readBytes())
            assertArrayEquals(signature, java.io.File(destination, "release.sig").readBytes())
            assertArrayEquals(policy, java.io.File(destination, "policy.json").readBytes())
            assertEquals(320, android.system.Os.lstat(destination.path).st_mode and 511)
            assertTrue(main { extensions.entries.isEmpty() }); assertNull(main { extensions.activeSession }); assertNull(main { extensions.activeRuntime })
            assertEquals(ExtensionRegistry.Status.NOT_FOUND, runBlocking { extensions.controller!!.resolve("fixture").status })
            if (channelSignature != null) {
                assertTrue(java.io.File(destination, "channel-policy-required").isFile)
                ui.onNodeWithTag("plugin-zip-disclosure").performScrollTo().performClick()
                ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick()
                ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-version-select-$releaseID").fetchSemanticsNodes().isNotEmpty() }
                ui.onNodeWithTag("plugin-version-select-$releaseID").assertIsEnabled().performScrollTo().performClick()
                ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-version-preview").fetchSemanticsNodes().isNotEmpty() }
                ui.onNodeWithText("已验证渠道策略 · 第 1 代").assertExists()
                ui.onNodeWithTag("plugin-version-confirm").assertIsNotEnabled()
                ui.onNodeWithTag("plugin-version-acknowledge").performScrollTo().performClick()
                ui.onNodeWithTag("plugin-version-confirm").assertIsEnabled().performScrollTo().performClick()
                ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-version-selection-result").fetchSemanticsNodes().isNotEmpty() }
                assertTrue(runBlocking { extensions.controller!!.installedVersions().selection!!.choices.values.single().enabled })
                return
            }
            verify()
            ui.onNodeWithTag("plugin-zip-install-confirmation").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-zip-install").performScrollTo().performClick()
            ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-zip-issue").fetchSemanticsNodes().isNotEmpty() }
            ui.onNodeWithTag("plugin-zip-issue").assertTextContains("槽位", substring = true)
            assertArrayEquals(descriptor, java.io.File(destination, "release.json").readBytes())
        } finally {
            ui.activityRule.scenario.close()
            runBlocking { extensions.controller?.close() }
            if (root.isDirectory) com.hanxi.ghosterm.bridge.LocalPublicationFile.directory(root, "").use { fd ->
                val end = SystemClock.elapsedRealtime() + 10000
                while (com.hanxi.ghosterm.bridge.PluginInstallFiles.lock(fd.fd) != 0 && SystemClock.elapsedRealtime() < end) SystemClock.sleep(20)
                assertEquals(0, com.hanxi.ghosterm.bridge.PluginInstallFiles.lock(fd.fd))
                for (name in (root.list()?.toSet() ?: emptySet()) - before) PluginInstallTestFiles.clean(java.io.File(root, name))
            }
            if (ownsCache) PluginInstallTestFiles.clean(cache)
            val tree = android.provider.DocumentsContract.buildTreeDocumentUri(PreviewPackageDocumentsProvider.AUTHORITY, "preview-root")
            context.contentResolver.call(control, "cleanup", null, Bundle().apply { putString("uri", tree.toString()); putInt("flags", android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or android.content.Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) })
        }
    }
}
