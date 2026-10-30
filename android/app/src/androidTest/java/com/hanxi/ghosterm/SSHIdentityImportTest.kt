package com.hanxi.ghosterm

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.AccessibilityServiceInfo
import android.app.UiAutomation
import android.content.res.Configuration
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.provider.Settings
import android.view.accessibility.AccessibilityNodeInfo
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.data.IdentityVault
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Real DocumentsUI -> provider pipe -> Keystore -> production SSH authentication. */
@RunWith(AndroidJUnit4::class)
class SSHIdentityImportTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val automation get() = instrumentation.uiAutomation
    private val fixture by lazy { sshFixture() }
    private lateinit var model: SessionsViewModel
    private lateinit var hosts: HostsViewModel
    private lateinit var vault: IdentityVault
    private lateinit var config: File
    private lateinit var known: File
    private var oldConfig: ByteArray? = null
    private var oldKnown: ByteArray? = null
    private var oldIds = emptySet<String>()
    private var previousRotation = 0
    private var previousAutoRotation = 0
    private val alias = "import-${UUID.randomUUID().toString().take(8)}"
    private fun <T> onMain(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result=block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun provider(method: String, bytes: ByteArray? = null): Bundle =
        checkNotNull(instrumentation.targetContext.contentResolver.call(Uri.parse("content://com.hanxi.ghosterm.test.keycontrol"),
            "fixture-$method",null,Bundle().apply { if(bytes!=null) putByteArray("material",bytes) }))
    @Before fun setup() {
        automation.serviceInfo = automation.serviceInfo.apply { flags = flags or AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS }
        previousRotation = ui.activity.display?.rotation ?: 0
        previousAutoRotation = Settings.System.getInt(ui.activity.contentResolver,Settings.System.ACCELEROMETER_ROTATION,0)
        // Freeze a settled starting orientation; restoring auto-rotation in the
        // preceding test can otherwise race this Activity's initial configuration.
        assertTrue(automation.setRotation(UiAutomation.ROTATION_FREEZE_0))
        await { ui.activity.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT }
        ui.waitForIdle()
        onMain { model=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]; hosts=ViewModelProvider(ui.activity)[HostsViewModel::class.java] }
        config=File(ui.activity.filesDir,"home/.ssh/config"); known=File(ui.activity.filesDir,"home/.ssh/known_hosts")
        oldConfig=config.takeIf(File::exists)?.readBytes(); oldKnown=known.takeIf(File::exists)?.readBytes()
        known.delete(); config.parentFile!!.mkdirs()
        config.writeText("Host $alias\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n User ${fixture.getString("user")}\n")
        vault=IdentityVault(File(ui.activity.filesDir,"identities")); oldIds=vault.list().map { it.id }.toSet()
        val bytes=fixture.getString("pem").toByteArray()
        try { provider("configure",bytes) } finally { bytes.fill(0) }
        onMain { hosts.reload() }
        await { !hosts.busy && hosts.snapshot?.text==config.readText() }
        connect()
    }
    @After fun cleanup() {
        if(::model.isInitialized) onMain { model.connecting?.let { model.cancelConnection(it.id) }; model.sessions.map { it.id }.forEach(model::close) }
        if(::vault.isInitialized) vault.list().filter { it.id !in oldIds }.forEach(vault::remove)
        if(::config.isInitialized) { oldConfig?.let(config::writeBytes) ?: config.delete(); oldKnown?.let(known::writeBytes) ?: known.delete() }
        provider("clear")
        automation.setRotation(if(previousAutoRotation==1) UiAutomation.ROTATION_UNFREEZE else previousRotation)
    }
    private fun await(predicate: () -> Boolean) {
        val end=SystemClock.elapsedRealtime()+15_000
        while(SystemClock.elapsedRealtime()<end) { if(onMain(predicate)) return; SystemClock.sleep(25) }
        fail("Expected import state did not arrive")
    }
    private fun connect() {
        ui.onNodeWithText(alias).performClick()
        await { model.connecting?.auth?.phase==SSHAuthPhase.HOSTKEY || model.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        if(onMain { model.connecting?.auth?.phase==SSHAuthPhase.HOSTKEY }) ui.onNodeWithText("信任并连接").performClick()
        await { model.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
    }
    private fun nodes(root: AccessibilityNodeInfo? = automation.rootInActiveWindow): List<AccessibilityNodeInfo> {
        val result=mutableListOf<AccessibilityNodeInfo>()
        fun visit(node: AccessibilityNodeInfo?) { if(node==null)return; result+=node; for(i in 0 until node.childCount) visit(node.getChild(i)) }
        visit(root); return result
    }
    private fun click(vararg labels: String): Boolean {
        for(match in nodes().filter { it.isVisibleToUser && (it.text?.toString() in labels || it.contentDescription?.toString() in labels) }) {
            var node: AccessibilityNodeInfo?=match
            while(node!=null) { if(node.isClickable && node.performAction(AccessibilityNodeInfo.ACTION_CLICK))return true; node=node.parent }
        }
        return false
    }
    private fun systemAwait(predicate: () -> Boolean) {
        val end=SystemClock.elapsedRealtime()+10_000
        while(SystemClock.elapsedRealtime()<end) { if(predicate())return; SystemClock.sleep(50) }
        fail("System picker did not reach expected state: "+nodes().map { it.text to it.contentDescription })
    }
    private fun openPicker() {
        ui.onNodeWithText("导入密钥").performScrollTo().performClick()
        systemAwait { nodes().any { it.packageName?.toString()?.contains("documentsui")==true } }
    }
    private fun select(id: String) {
        // Rotation recreates DocumentsUI too. It can already be in our directory;
        // opening the drawer then hides its last provider below the landscape fold.
        // Navigate the actual roots/list controls and tolerate a transient empty tree.
        systemAwait {
            val tree = nodes()
            val roots = tree.firstOrNull { it.isVisibleToUser && it.viewIdResourceName?.endsWith(":id/roots_list")==true }
            if(roots != null) {
                val row = nodes(roots).firstOrNull { it.isVisibleToUser && it.text?.toString()=="Ghosterm Test Keys" }
                if(row != null) {
                    var target: AccessibilityNodeInfo? = row
                    while(target != null && !target.isClickable) target = target.parent
                    target?.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                } else roots.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                false
            } else if(click("$id.pem")) true else {
                if(tree.any { it.isVisibleToUser && it.text?.toString()=="Ghosterm Test Keys" }) {
                    tree.firstOrNull { it.isScrollable && it.viewIdResourceName?.endsWith(":id/dir_list")==true }
                        ?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                } else click("Show roots","Open navigation drawer","显示根目录","显示位置")
                false
            }
        }
        systemAwait { nodes().none { it.packageName?.toString()?.contains("documentsui")==true } }
    }
    private fun imported() = vault.list().filter { it.id !in oldIds }
    private fun authenticate() {
        await { model.connecting?.identities?.any { it.id !in oldIds }==true && model.connecting?.credentialBusy==false }
        val identity=imported().single()
        val expected=fixture.getString("pem").toByteArray(); val actual=vault.unlock(identity)
        try { assertTrue("imported material differs",expected.contentEquals(actual)) } finally { expected.fill(0);actual.fill(0) }
        assertFalse(File(ui.activity.filesDir,"identities/${identity.id}.json").readText().contains("PRIVATE KEY"))
        ui.onNodeWithTag("ssh-key-${identity.id}").performScrollTo().performClick()
        ui.onNodeWithTag("ssh-secret").assertDoesNotExist()
        ui.onNodeWithText("认证并连接").performClick()
        await { model.connecting==null && model.active?.state?.connected==true }
        ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
        assertEquals(0,provider("status").getInt("writes"))
    }
    @Test fun realPickerImportSurvivesActivityRecreationAndAuthenticates() {
        val owner=model; val attempt=onMain { model.connecting!!.id }; val request=onMain { model.connecting!!.auth!!.request }
        val activity=ui.activity
        openPicker()
        assertTrue(automation.setRotation(UiAutomation.ROTATION_FREEZE_90))
        select("ssh-key")
        // The picker can disappear before the stopped Activity processes its
        // pending configuration change. Observe destruction, not that timing gap.
        await { activity.isDestroyed }
        assertNotSame("rotation must recreate the production Activity",activity,ui.activity)
        onMain { assertSame(owner,ViewModelProvider(ui.activity)[SessionsViewModel::class.java]); assertEquals(attempt,model.connecting!!.id); assertEquals(request,model.connecting!!.auth!!.request) }
        authenticate()
        assertEquals(1,provider("status").getInt("reads"))
    }
    @Test fun pickerCancelDoesNotSaveAndAllowsAnotherImport() {
        openPicker(); assertTrue(automation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_BACK))
        await { !model.identityPickerPending }
        assertTrue(imported().isEmpty()); assertEquals(0,provider("status").getInt("reads"))
        openPicker(); select("ssh-key"); authenticate()
    }
    @Test fun oldPickerResultCannotImportIntoANewConnection() {
        val target=onMain { model.connecting!!.target }; val previous=onMain { model.connecting!!.id }
        openPicker()
        onMain { model.cancelConnection(previous); model.connect(target) }
        await { model.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        assertNotEquals(previous,onMain { model.connecting!!.id })
        select("ssh-key")
        await { !model.identityPickerPending }
        assertTrue(imported().isEmpty()); assertEquals(0,provider("status").getInt("reads"))
        openPicker(); select("ssh-key"); authenticate()
    }
    @Test fun cancelledImportClosesBlockedProviderPipeWithoutSaving() {
        openPicker(); select("slow-key")
        systemAwait { provider("status").getInt("reads")==1 }
        await { model.connecting?.credentialBusy==true }
        ui.onNodeWithText("取消连接").performClick()
        await { model.connecting==null }
        systemAwait { provider("status").getInt("closed")==1 }
        assertTrue(imported().isEmpty()); assertEquals(0,provider("status").getInt("writes"))
        provider("release")
        connect(); openPicker(); select("ssh-key"); authenticate()
    }
    @Test fun invalidDeniedAndOversizedDocumentsStayVisibleAndDoNotSave() {
        for((id,message) in listOf("invalid" to "无法识别此密钥格式；不会猜测口令","empty" to "密钥必须为 1 字节至 1 MiB",
            "oversize" to "密钥必须为 1 字节至 1 MiB","denied" to "Fixture key read denied")) {
            openPicker(); select(id)
            await { model.connecting?.credentialError!=null && model.connecting?.credentialBusy==false }
            ui.onNodeWithTag("ssh-import-error").assertTextEquals(message)
            assertTrue(imported().isEmpty())
            assertEquals(0,provider("status").getInt("writes"))
        }
        openPicker(); select("ssh-key"); authenticate()
    }
}
