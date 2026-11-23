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
class PluginVersionSelectionUITest {
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
        awaitSystem("fixture root inside drawer") {
            val row = nodes().firstOrNull { item ->
                item.isVisibleToUser && item.text?.toString() == "Ghosterm Official Preview Package Fixture" &&
                    generateSequence(item.parent) { it.parent }
                        .any { it.viewIdResourceName?.endsWith(":id/roots_list") == true }
            } ?: return@awaitSystem false
            tapLegacyDocumentRow(instrumentation, row) ?: generateSequence(row) { it.parent }
                .firstOrNull { it.isClickable }?.performAction(AccessibilityNodeInfo.ACTION_CLICK) ?: false
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
    private fun awaitPreview() {
        ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-version-preview").fetchSemanticsNodes().isNotEmpty() || ui.onAllNodesWithTag("plugin-version-selection-issue").fetchSemanticsNodes().isNotEmpty() }
        ui.onNodeWithTag("plugin-version-preview").assertExists()
    }
    private fun awaitResult() {
        ui.waitUntil(15000) { ui.onAllNodesWithTag("plugin-version-selection-result").fetchSemanticsNodes().isNotEmpty() || ui.onAllNodesWithTag("plugin-version-selection-issue").fetchSemanticsNodes().isNotEmpty() }
        val errors = ui.onAllNodesWithTag("plugin-version-selection-issue").fetchSemanticsNodes()
        assertTrue("activity selection failed: ${errors.map { it.config.getOrElse(androidx.compose.ui.semantics.SemanticsProperties.Text) { emptyList() } }}", errors.isEmpty())
        ui.onNodeWithTag("plugin-version-selection-result").assertTextContains("已更新", substring = true)
        ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-versions-count").fetchSemanticsNodes().isNotEmpty() }
    }
    @Test fun actualSettingsTrustsCurrentPolicyReviewsDifferencesAndSeparatelyConfirmsSelectionDisableAndRollback() {
        val context = instrumentation.targetContext; val root = luaVersionDirectory(context)
        assertTrue("fresh owned test namespace required", root.list()?.isEmpty() != false)
        val installer = PluginVersionSelectionFixture.controller(root)
        val extensions = ViewModelProvider(ui.activity)[ExtensionPackagesViewModel::class.java]
        try {
            val a = runBlocking { PluginVersionSelectionFixture.install(installer, root) }
            val b = runBlocking { PluginVersionSelectionFixture.install(installer, root, "2.0.0") }
            val view = runBlocking { installer.previewInstalledVersion(UUID.randomUUID().toString(), a.publication.releaseID, null, PluginVersionSelectionFixture.policy) }
            val selected = runBlocking { installer.confirmInstalledVersion(view.id, true) }
            install(mapOf("channel.json" to PluginVersionSelectionFixture.policy))
            ui.onNodeWithContentDescription("设置").performClick()
            ui.onNodeWithTag("settings-section-PLUGINS").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-versions-disclosure").performScrollTo().performClick()
            ui.waitUntil(10000) { ui.onAllNodesWithTag("plugin-versions-count").fetchSemanticsNodes().isNotEmpty() }
            val selectB = "plugin-version-select-${b.publication.releaseID}"
            ui.onNodeWithTag(selectB).assertIsNotEnabled(); choosePolicy()
            ui.onNodeWithTag(selectB).assertIsNotEnabled()
            ui.onNodeWithTag("plugin-version-policy-trust").performScrollTo().performClick()
            ui.onNodeWithTag(selectB).assertIsEnabled().performScrollTo().performClick(); awaitPreview()
            ui.onNodeWithTag("plugin-version-previous-state").assertTextContains("旧版已用本次策略核验", substring = true)
            ui.onNodeWithTag("plugin-version-candidate-declarations").assertExists()
            ui.onNodeWithTag("plugin-version-previous-declarations").assertExists()
            ui.onNodeWithText("新增或修改权限 0 项；移除或修改旧权限 0 项。", substring = false).assertExists()
            ui.onNodeWithTag("plugin-version-confirm").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-version-cancel").performScrollTo().performClick()
            assertEquals(selected.observation.selection, checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection() })
            ui.onNodeWithTag(selectB).performScrollTo().performClick(); awaitPreview()
            ui.onNodeWithTag("plugin-version-acknowledge").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-confirm").assertIsEnabled().performScrollTo().performClick(); awaitResult()
            val review = b.snapshot.review
            assertEquals(b.publication.releaseID, checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().choices.getValue(review.id).releaseID })
            assertEquals(review.digest, main { extensions.entries.single().review.digest })
            assertEquals("尚未批准", main { extensions.entries.single().decision })
            assertEquals(ExtensionRegistry.Status.UNKNOWN, runBlocking { extensions.controller!!.resolve("fixture").status })
            ui.onNodeWithTag("plugin-versions-choice-${review.id}").assertTextContains(b.publication.releaseID, substring = true)
            ui.onNodeWithText("批准此摘要").performScrollTo().performClick()
            ui.waitUntil(10000) { main { extensions.entries.single().decision == "已批准（本进程）" } }
            ui.onNodeWithTag("plugin-version-disable-${review.id}").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-disable-confirm").assertIsNotEnabled()
            ui.onNodeWithTag("plugin-version-disable-acknowledge").performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-disable-confirm").performScrollTo().performClick(); awaitResult()
            assertFalse(checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().choices.getValue(review.id).enabled })
            assertEquals("尚未批准", main { extensions.entries.single().decision })
            assertEquals(ExtensionRegistry.Status.DENIED, runBlocking { extensions.controller!!.approve(review.id, review.digest) })
            ui.onNodeWithTag("plugin-version-select-${a.publication.releaseID}").performScrollTo().performClick(); awaitPreview()
            ui.onNodeWithTag("plugin-version-acknowledge").assertIsOff().performScrollTo().performClick()
            ui.onNodeWithTag("plugin-version-confirm").performScrollTo().performClick(); awaitResult()
            assertEquals(a.publication.releaseID, checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().choices.getValue(review.id).releaseID })
            assertEquals(2, main { extensions.entries.size }); assertTrue(main { extensions.entries.all { it.decision == "尚未批准" } })
            assertNull(main { extensions.activeSession }); assertNull(main { extensions.activeRuntime })
        } finally {
            ui.activityRule.scenario.close(); runBlocking { installer.close(); extensions.controller?.close() }
            val end = SystemClock.elapsedRealtime() + 5000; var released = false
            while (!released && SystemClock.elapsedRealtime() < end) {
                runCatching { PluginReleaseCatalog.open(root)?.close(); released = true }
                if (!released) SystemClock.sleep(10)
            }
            assertTrue("observer retained installation root", released); PluginInstallTestFiles.clean(root)
            val tree = android.provider.DocumentsContract.buildTreeDocumentUri(PreviewPackageDocumentsProvider.AUTHORITY, "preview-root")
            context.contentResolver.call(control, "cleanup", null, Bundle().apply { putString("uri", tree.toString()); putInt("flags", android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or android.content.Intent.FLAG_GRANT_PREFIX_URI_PERMISSION) })
        }
    }
}
