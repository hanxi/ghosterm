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
import java.io.File
import java.util.UUID
import java.util.Base64

/** Actual MainActivity Settings -> DocumentsUI -> fixed Core package + JNI proof. */
@RunWith(AndroidJUnit4::class)
class PluginPolicyReviewUITest {
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
            ui.waitUntil(10000) { ui.onAllNodesWithText("本次策略 ${PluginVersionSelectionFixture.policy.size} 字节", substring = true).fetchSemanticsNodes().isNotEmpty() }
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
        ui.waitUntil(10000) { ui.onAllNodesWithText("本次策略 ${PluginVersionSelectionFixture.policy.size} 字节", substring = true).fetchSemanticsNodes().isNotEmpty() }
    }
    private fun await(tag: String) {
        try { ui.waitUntil(15000) { ui.onAllNodesWithTag(tag).fetchSemanticsNodes().isNotEmpty() } }
        catch (_: ComposeTimeoutException) { ui.onNodeWithTag(tag).assertExists("expected policy control: $tag") }
    }
    @Test fun actualSettingsTrustsPolicyShowsThreeStatesAndDisablesOnlyAFreshExplicitSubset() {
        val root = File(instrumentation.targetContext.filesDir, "plugin-releases")
        assertTrue("fresh owned namespace required", root.list()?.isEmpty() != false)
        val installer = PluginVersionSelectionFixture.controller(root)
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            val saved = runBlocking { PluginVersionSelectionFixture.install(installer, root) }
            checkNotNull(PluginReleaseCatalog.open(root)).use { owner ->
                for ((id, slot) in listOf(saved.snapshot.review.id to saved.publication.releaseID, "org.ghosterm.alias" to saved.publication.releaseID, "org.ghosterm.missing" to "b".repeat(64)))
                    owner.change(id, slot, true, owner.selection().revision)
            }
            val before = File(root, PluginReleaseCatalog.SELECTION).readBytes()
            install(mapOf("channel.json" to PluginVersionSelectionFixture.policy))
            ui.onNodeWithContentDescription("设置").performClick(); ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick(); await("plugin-versions-count")
            ui.onNodeWithTag("plugin-policy-review-open").assertIsNotEnabled(); choosePolicy()
            ui.onNodeWithTag("plugin-version-policy-trust").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-policy-review-open").performScrollTo().performClick(); await("plugin-policy-review-summary")
            ui.onNodeWithTag("plugin-policy-state-${saved.snapshot.review.id}").assertTextContains("此策略接受", substring = true)
            ui.onNodeWithTag("plugin-policy-state-org.ghosterm.alias").assertTextContains("此策略拒绝", substring = true)
            ui.onNodeWithTag("plugin-policy-state-org.ghosterm.missing").assertTextContains("查不出来", substring = true)
            ui.onNodeWithTag("plugin-policy-select-${saved.snapshot.review.id}").assertDoesNotExist()
            ui.onNodeWithTag("plugin-policy-select-org.ghosterm.alias").assertIsOff(); ui.onNodeWithTag("plugin-policy-select-org.ghosterm.missing").assertIsOff()
            ui.onNodeWithTag("plugin-policy-review-apply").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-policy-select-org.ghosterm.alias").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-policy-review-cancel").performScrollTo().performClick(); assertArrayEquals(before, File(root, PluginReleaseCatalog.SELECTION).readBytes())
            ui.onNodeWithTag("plugin-policy-review-open").performScrollTo().performClick(); await("plugin-policy-review-summary")
            ui.onNodeWithTag("plugin-policy-select-org.ghosterm.alias").assertIsOff()
            ui.onNodeWithTag("plugin-version-policy-trust").performScrollTo().performClick(); ui.onNodeWithTag("plugin-policy-review-summary").assertDoesNotExist()
            ui.onNodeWithTag("plugin-policy-review-open").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-version-policy-trust").performScrollTo().performClick(); ui.onNodeWithTag("plugin-policy-review-open").performScrollTo().performClick(); await("plugin-policy-review-summary")
            ui.onNodeWithTag("plugin-policy-select-org.ghosterm.alias").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-policy-review-apply").performScrollTo().performClick(); await("plugin-policy-review-result")
            checkNotNull(PluginReleaseCatalog.open(root)).use { owner ->
                assertFalse(owner.selection().choices.getValue("org.ghosterm.alias").enabled)
                assertTrue(owner.selection().choices.getValue(saved.snapshot.review.id).enabled); assertTrue(owner.selection().choices.getValue("org.ghosterm.missing").enabled)
            }
            assertTrue(File(root, saved.publication.releaseID).isDirectory)
            ui.runOnIdle { assertTrue(extensions.entries.isEmpty()); assertNull(extensions.activeSession); assertNull(extensions.activeRuntime) }
        } finally {
            ui.activityRule.scenario.close(); runBlocking { installer.close(); extensions.controller?.close() }
            instrumentation.targetContext.contentResolver.call(control, "clear", null, null); PluginInstallTestFiles.clean(root)
        }
    }
}
