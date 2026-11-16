package com.hanxi.ghosterm

import android.system.Os
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
import java.io.File
import java.util.UUID
import java.util.Base64

/** Actual MainActivity Settings -> DocumentsUI -> fixed Core package + JNI proof. */
@RunWith(AndroidJUnit4::class)
class PluginVersionChangesUITest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val control = Uri.parse("content://com.hanxi.ghosterm.test.preview-package-control")
    private val fixturePolicy get() = PluginVersionSelectionFixture.policy("capability-versions.json")
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
        instrumentation.uiAutomation.waitForIdle(200, 3000)
        var drawerEntry: AccessibilityNodeInfo? = null
        awaitSystem("fixture root in actual roots drawer") {
            drawerEntry = nodes().firstOrNull { item ->
                if (!item.isVisibleToUser || item.text?.toString() != "Ghosterm Official Preview Package Fixture") false
                else {
                    var parent = item.parent; var inDrawer = false
                    while (parent != null) { if (parent.viewIdResourceName?.endsWith(":id/roots_list") == true) inDrawer = true; parent = parent.parent }
                    inDrawer
                }
            }
            drawerEntry != null
        }
        var row = checkNotNull(drawerEntry)
        while (!row.isClickable) row = checkNotNull(row.parent)
        val bounds = Rect(); row.getBoundsInScreen(bounds); assertFalse(bounds.isEmpty)
        val started = SystemClock.uptimeMillis()
        for (action in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {
            val event = MotionEvent.obtain(started, SystemClock.uptimeMillis(), action, bounds.exactCenterX(), bounds.exactCenterY(), 0)
            event.source = InputDevice.SOURCE_TOUCHSCREEN
            try { assertTrue(instrumentation.uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
            if (action == MotionEvent.ACTION_DOWN) SystemClock.sleep(50)
        }
        awaitSystem("fixture root opened and drawer closed") {
            val tree = nodes()
            tree.none { it.isVisibleToUser && it.viewIdResourceName?.endsWith(":id/roots_list") == true } &&
                tree.any { it.isVisibleToUser && it.text?.toString() == "Ghosterm Official Preview Package Fixture" } &&
                tree.any { it.isVisibleToUser && it.text?.toString() == "channel.json" }
        }
    }
    private fun choosePolicy() {
        ui.onNodeWithTag("plugin-version-policy-pick").performScrollTo().performClick(); ui.waitForIdle(); root()
        // One gesture, then observe its result. Repeated taps against a closing
        // system window can land on the newly resumed picker button and open
        // another chooser before the first result has been delivered.
        instrumentation.uiAutomation.waitForIdle(200, 3000)
        val file = nodes().single { it.isVisibleToUser && it.text?.toString() == "channel.json" }
        if (android.os.Build.VERSION.SDK_INT <= 29) {
            // Android 10 exposes its document rows without a clickable bit.
            assertEquals(true, tapLegacyDocumentRow(instrumentation, file))
            awaitSystem("DocumentsUI selected policy and returned", ::returnedToApp)
            ui.waitUntil(10000) { ui.onAllNodesWithText("本次策略 ${fixturePolicy.size} 字节", substring = true).fetchSemanticsNodes().isNotEmpty() }
            return
        }
        var row: AccessibilityNodeInfo? = file
        while (row != null && row.viewIdResourceName?.endsWith(":id/item_root") != true) row = row.parent
        val target = checkNotNull(row); assertTrue(target.isClickable)
        val original = Rect(); target.getBoundsInScreen(original); SystemClock.sleep(100)
        val bounds = Rect(); assertTrue(target.refresh()); target.getBoundsInScreen(bounds)
        assertEquals("policy row was still moving", original, bounds)
        assertEquals("channel.json", file.text?.toString()); assertTrue(file.isVisibleToUser && file.isEnabled)
        var parent = target.parent
        while (parent != null) {
            val clip = Rect(); parent.getBoundsInScreen(clip); assertTrue(bounds.intersect(clip)); parent = parent.parent
        }
        assertFalse(bounds.isEmpty)
        val start = SystemClock.uptimeMillis()
        val properties = arrayOf(MotionEvent.PointerProperties().apply { id = 0; toolType = MotionEvent.TOOL_TYPE_FINGER })
        val coordinates = arrayOf(MotionEvent.PointerCoords().apply { x = bounds.exactCenterX(); y = bounds.exactCenterY(); pressure = 1f; size = 1f })
        fun inject(action: Int) {
            val event = MotionEvent.obtain(start, SystemClock.uptimeMillis(), action, 1, properties, coordinates,
                0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0)
            try { assertTrue(instrumentation.uiAutomation.injectInputEvent(event, true)) } finally { event.recycle() }
        }
        inject(MotionEvent.ACTION_DOWN); SystemClock.sleep(50); inject(MotionEvent.ACTION_UP)
        awaitSystem("DocumentsUI selected policy and returned", ::returnedToApp)
        ui.waitUntil(10000) { ui.onAllNodesWithText("本次策略 ${fixturePolicy.size} 字节", substring = true).fetchSemanticsNodes().isNotEmpty() }
    }
    private fun await(tag: String) {
        try { ui.waitUntil(15000) { ui.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty() } }
        catch (_: ComposeTimeoutException) { ui.onNodeWithTag(tag).assertExists("expected policy control: $tag") }
    }
    @Test fun actualSettingsShowsCapabilityUpgradeUnknownAndRollbackWithoutGuessingEmptyDifferencesOrGrantingSource() {
        val root = File(instrumentation.targetContext.filesDir, "plugin-releases")
        assertTrue("fresh owned namespace required", root.list()?.isEmpty() != false)
        val installer = PluginVersionSelectionFixture.controller(root, listOf("session.rpc", "ui.table", "ui.diff"))
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            val a = runBlocking { PluginVersionSelectionFixture.install(installer, root, "1.0.0", "capability-versions.json") }
            val b = runBlocking { PluginVersionSelectionFixture.install(installer, root, "2.0.0", "capability-versions.json") }
            val first = runBlocking { installer.previewInstalledVersion(UUID.randomUUID().toString(), a.publication.releaseID, null, fixturePolicy) }
            runBlocking { installer.confirmInstalledVersion(first.id, true) }
            install(mapOf("channel.json" to fixturePolicy))
            ui.onNodeWithContentDescription("设置").performClick(); ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick(); await("plugin-versions-count")
            choosePolicy(); ui.onNodeWithTag("plugin-version-policy-trust").performScrollTo().performClick()
            val selectB = "plugin-version-select-${b.publication.releaseID}"
            ui.onNodeWithTag(selectB).performScrollTo().performClick(); await("plugin-version-preview")
            ui.onNodeWithTag("plugin-version-capability-diff").assertTextEquals("必需能力声明新增：ui.table；移除：ui.diff。")
            ui.onNodeWithTag("plugin-version-capability-unknown").assertDoesNotExist(); ui.onNodeWithTag("plugin-version-permission-unknown").assertDoesNotExist()
            ui.onNodeWithText("新增或修改权限 3 项；移除或修改旧权限 3 项。").assertExists()
            ui.onNodeWithText("portable-reviewed", substring = true).assertExists()
            ui.onNodeWithTag("plugin-version-confirm").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-version-cancel").performScrollTo().performClick()
            val signature = File(root, "${a.publication.releaseID}/release.sig"); val original = signature.readBytes()
            Os.chmod(signature.path, 384); signature.writeBytes(ByteArray(64)); Os.chmod(signature.path, 256)
            ui.onNodeWithTag(selectB).performScrollTo().performClick(); await("plugin-version-preview")
            ui.onNodeWithTag("plugin-version-capability-diff").assertDoesNotExist()
            ui.onNodeWithTag("plugin-version-capability-unknown").assertTextContains("无法计算完整差异", substring = true)
            ui.onNodeWithTag("plugin-version-permission-unknown").assertTextContains("无法计算完整差异", substring = true)
            ui.onNodeWithTag("plugin-version-previous-declarations").assertDoesNotExist(); ui.onNodeWithTag("plugin-version-candidate-declarations").assertExists()
            ui.onNodeWithTag("plugin-version-confirm").assertIsNotEnabled(); ui.onNodeWithTag("plugin-version-acknowledge").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-confirm").performScrollTo().performClick(); await("plugin-version-selection-result")
            assertEquals("尚未批准", main { extensions.entries.single().decision })
            assertEquals(ExtensionRegistry.Status.UNKNOWN, runBlocking { extensions.controller!!.resolve("fixture").status })
            Os.chmod(signature.path, 384); signature.writeBytes(original); Os.chmod(signature.path, 256)
            ui.onNodeWithTag("plugin-version-select-${a.publication.releaseID}").performScrollTo().performClick(); await("plugin-version-preview")
            ui.onNodeWithTag("plugin-version-capability-diff").assertTextEquals("必需能力声明新增：ui.diff；移除：ui.table。")
            ui.onNodeWithTag("plugin-version-confirm").assertIsNotEnabled(); ui.onNodeWithTag("plugin-version-acknowledge").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-confirm").performScrollTo().performClick(); await("plugin-version-selection-result")
            assertTrue(main { extensions.entries.all { it.decision == "尚未批准" } }); assertNull(main { extensions.activeSession }); assertNull(main { extensions.activeRuntime })
        } finally {
            ui.activityRule.scenario.close(); runBlocking { installer.close(); extensions.controller?.close() }
            instrumentation.targetContext.contentResolver.call(control, "clear", null, null); PluginInstallTestFiles.clean(root)
        }
    }
}
