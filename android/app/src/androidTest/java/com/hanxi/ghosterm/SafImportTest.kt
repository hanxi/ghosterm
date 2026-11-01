package com.hanxi.ghosterm

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.AccessibilityServiceInfo
import android.app.Application
import android.os.SystemClock
import android.provider.DocumentsContract
import android.view.accessibility.AccessibilityNodeInfo
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real ACTION_OPEN_DOCUMENT and a test-APK DocumentsProvider, never an intercepted activity result. */
@RunWith(AndroidJUnit4::class)
class SafImportTest {
    @get:Rule val ui = createComposeRule()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val automation get() = instrumentation.uiAutomation
    private lateinit var directory: File
    private lateinit var file: File
    private lateinit var model: HostsViewModel
    private val original = "# existing\nHost original\n  User owner\n"
    private val imported = "# imported 🔑\r\nHost imported-host\r\n  HostName imported.test\r\n  User owner\r\n"

    @Before fun setup() {
        automation.serviceInfo=automation.serviceInfo.apply { flags=flags or AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS }
        val context = instrumentation.targetContext
        directory = File(context.cacheDir, "saf-ui-${UUID.randomUUID()}").apply { mkdirs() }
        file = File(directory, "home/.ssh/config").apply { parentFile!!.mkdirs(); writeText(original) }
        val application = object : Application() {
            init { attachBaseContext(context) }
            override fun getFilesDir(): File = directory
        }
        model = HostsViewModel(application)
        val runtime = RuntimeInfoViewModel()
        ui.setContent { GhostermApp(model, runtime) }
        await { !model.busy && model.snapshot != null }
        ui.onNodeWithText("设置").performClick()
        ui.onNodeWithTag("settings-section-SESSIONS").performScrollTo().performClick()
    }
    @After fun cleanup() { directory.deleteRecursively() }
    private fun await(predicate: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + 10_000
        while (SystemClock.elapsedRealtime() < deadline) {
            if (predicate()) return
            SystemClock.sleep(50)
        }
        fail("Timed out waiting for expected state: " + nodes().joinToString("\n") {
            "${it.viewIdResourceName}: ${it.text} / ${it.contentDescription}; visible=${it.isVisibleToUser}, click=${it.isClickable}, scroll=${it.isScrollable}"
        })
    }
    private fun nodes(): List<AccessibilityNodeInfo> {
        val found = mutableListOf<AccessibilityNodeInfo>()
        fun visit(node: AccessibilityNodeInfo?) {
            if (node == null) return
            found += node
            for (index in 0 until node.childCount) visit(node.getChild(index))
        }
        visit(automation.rootInActiveWindow)
        return found
    }
    private fun clickSystem(vararg labels: String): Boolean {
        for (match in nodes().filter { it.isVisibleToUser && (it.text?.toString() in labels || it.contentDescription?.toString() in labels) }) {
            when(tapLegacyDocumentRow(instrumentation,match)) { true -> return true; false -> continue; null -> Unit }
            var node: AccessibilityNodeInfo? = match
            while (node != null) {
                if (node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true
                node = node.parent
            }
        }
        return false
    }
    private fun openPicker() {
        ui.onNodeWithText("导入 SSH 配置").performClick()
        await { nodes().any { it.packageName?.toString()?.contains("documentsui") == true } }
    }
    private fun select(id: String) {
        openPicker()
        // A drawer can expose the covered directory's accessibility nodes too.
        // Resolve the actual roots list first, then the requested provider/file.
        await {
            val tree=nodes()
            val roots=tree.firstOrNull { it.isVisibleToUser && it.viewIdResourceName?.endsWith(":id/roots_list")==true }
            if(roots!=null) {
                if(!clickSystem("Ghosterm Test Files")) roots.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                false
            } else if(tree.any { it.isVisibleToUser && (it.text?.toString()=="Ghosterm Test Files" || it.contentDescription?.toString()=="Ghosterm Test Files") }) {
                if(clickSystem("$id.conf")) true else {
                    tree.firstOrNull { it.isScrollable && it.viewIdResourceName?.endsWith(":id/dir_list")==true }
                        ?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                    false
                }
            } else {
                clickSystem("Show roots","Open navigation drawer","显示根目录","显示位置")
                false
            }
        }
        // A prior error is not delivery of this picker result. Observe the
        // actual system window closing before inspecting the new model state.
        await {
            val tree=nodes()
            if(tree.any { it.isVisibleToUser && it.packageName?.toString()?.contains("documentsui")==true })
                clickSystem("SELECT","Select","选择")
            tree.any { it.isVisibleToUser && it.packageName?.toString()=="com.hanxi.ghosterm" } &&
                tree.none { it.packageName?.toString()?.contains("documentsui")==true }
        }
        await { model.imported != null || model.error != null }
        await { !model.busy }
        ui.waitForIdle()
    }
    private fun unchanged() {
        assertEquals(original, file.readText())
        assertEquals(original, model.snapshot!!.text)
        assertTrue(file.parentFile!!.listFiles()!!.none { it.name.startsWith("config.backup-") })
    }
    private fun assertProviderUntouched(id: String, expected: String? = null) {
        val uri = DocumentsContract.buildDocumentUri("com.hanxi.ghosterm.test.documents", id)
        val resolver = instrumentation.targetContext.contentResolver
        resolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_SUMMARY), null, null, null)!!.use {
            assertTrue(it.moveToFirst()); assertEquals("writes=0", it.getString(0))
        }
        if (expected != null) assertEquals(expected, resolver.openInputStream(uri)!!.bufferedReader().use { it.readText() })
    }
    @Test fun selectedDocumentPreviewsThenCommitsExactBytes() {
        select("valid")
        ui.onNodeWithText("valid.conf").assertIsDisplayed()
        ui.onNodeWithTag("config-import-preview").assertTextEquals(imported)
        unchanged()
        assertProviderUntouched("valid", imported)
        ui.onNodeWithText("确认导入").performClick()
        await { !model.busy }
        assertNull(model.error)
        assertEquals(Page.HOSTS, model.page)
        ui.onNodeWithText("imported-host").assertIsDisplayed()
        assertArrayEquals(imported.toByteArray(), file.readBytes())
        assertEquals(original, file.parentFile!!.listFiles()!!.single { it.name.startsWith("config.backup-") }.readText())
        assertProviderUntouched("valid", imported)
    }
    @Test fun pickerAndPreviewCancelDoNotChangeConfiguration() {
        openPicker()
        assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK))
        await { nodes().none { it.packageName?.toString()?.contains("documentsui") == true } }
        ui.waitForIdle()
        assertNull(model.imported); assertNull(model.error); unchanged()
        select("valid")
        ui.onNodeWithText("取消").performClick()
        ui.onNodeWithText("替换应用内 SSH 配置？").assertDoesNotExist()
        assertNull(model.imported); unchanged(); assertProviderUntouched("valid", imported)
    }
    @Test fun invalidDocumentsExplainFailureWithoutReplacingConfig() {
        for ((id, message) in listOf("oversize" to "SSH 配置不能超过 1 MiB", "encoding" to "SSH 配置必须为 UTF-8 文本",
            "private" to "SSH 配置中不能包含私钥材料", "include" to "Configuration parse failed", "denied" to "Fixture provider denied read",
            "nul" to "Configuration contains NUL")) {
            select(id)
            assertNull(model.imported)
            ui.onNodeWithText(message).assertIsDisplayed()
            ui.onNodeWithText("替换应用内 SSH 配置？").assertDoesNotExist()
            unchanged(); assertProviderUntouched(id)
        }
    }
    @Test fun importConflictStaysVisibleAndRequiresReloadBeforeRetry() {
        select("valid")
        val external = "Host external\n"
        file.writeText(external)
        ui.onNodeWithText("确认导入").performClick()
        await { !model.busy }
        assertNull(model.snapshot)
        assertNotNull(model.imported)
        ui.onNodeWithTag("config-import-error").assertIsDisplayed()
        ui.onNodeWithText("确认导入").assertIsNotEnabled()
        assertEquals(external, file.readText())
        assertTrue(file.delete()); assertTrue(file.mkdir())
        ui.onNodeWithText("重新载入当前配置").performClick()
        await { !model.busy }
        ui.onNodeWithTag("config-import-error").assertIsDisplayed()
        ui.onNodeWithText("确认导入").assertIsNotEnabled()
        assertNotNull(model.imported)
        assertTrue(file.delete()); file.writeText(external)
        ui.onNodeWithText("重新载入当前配置").performClick()
        await { !model.busy }
        assertEquals(external, model.snapshot!!.text)
        ui.onNodeWithText("确认导入").assertIsEnabled().performClick()
        await { !model.busy }
        assertEquals(imported, file.readText())
        assertEquals(external, file.parentFile!!.listFiles()!!.single { it.name.startsWith("config.backup-") }.readText())
        assertProviderUntouched("valid", imported)
    }
    @Test fun truncatedPreviewImportsWholeFileAndKeepsErrorsVisible() {
        select("large")
        ui.onNodeWithTag("config-import-truncated").assertIsDisplayed()
        ui.onNodeWithTag("config-import-preview").assertTextEquals("#" + "a".repeat(8190))
        unchanged()
        file.writeText("Host external\n")
        ui.onNodeWithText("确认导入").performClick()
        await { !model.busy }
        ui.onNodeWithTag("config-import-error").assertIsDisplayed()
        ui.onNodeWithText("重新载入当前配置").assertIsDisplayed().performClick()
        await { !model.busy }
        ui.onNodeWithText("确认导入").assertIsDisplayed().performClick()
        await { !model.busy }
        val full = "#" + "a".repeat(8190) + "🔑\nHost after-preview\n"
        assertEquals(full, file.readText())
        ui.onNodeWithText("after-preview").assertIsDisplayed()
        assertProviderUntouched("large", full)
    }
    @Test fun emptyDocumentRequiresConfirmationAndKeepsBackup() {
        select("empty")
        ui.onNodeWithTag("config-import-preview").assertTextEquals("（空配置）")
        unchanged()
        ui.onNodeWithText("确认导入").performClick()
        await { !model.busy }
        assertEquals("", file.readText())
        ui.onNodeWithText("还没有主机").assertIsDisplayed()
        assertEquals(original, file.parentFile!!.listFiles()!!.single { it.name.startsWith("config.backup-") }.readText())
        assertProviderUntouched("empty", "")
    }
}
