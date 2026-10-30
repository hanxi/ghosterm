package com.hanxi.ghosterm

import android.accessibilityservice.AccessibilityServiceInfo
import android.app.UiAutomation
import android.content.res.Configuration
import android.graphics.Rect
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
import com.hanxi.ghosterm.data.*
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.security.KeyStore
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class IdentitySettingsTest {
    @get:Rule val ui = createAndroidComposeRule<MainActivity>()
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val automation get() = instrumentation.uiAutomation
    private val fixture by lazy { sshFixture() }
    private lateinit var model: IdentitiesViewModel
    private lateinit var hosts: HostsViewModel
    private lateinit var sessions: SessionsViewModel
    private lateinit var vault: IdentityVault
    private lateinit var directory: File
    private var oldIds = emptySet<String>()
    private var rotation = 0
    private var autoRotation = 0
    private fun <T> main(block: () -> T): T {
        var result: T? = null
        instrumentation.runOnMainSync { result = block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(label: String = "Identity settings did not reach expected state", condition: () -> Boolean) {
        val end = SystemClock.elapsedRealtime()+15_000
        while(SystemClock.elapsedRealtime()<end) { if(main(condition)) return; SystemClock.sleep(25) }
        fail(label + if (::model.isInitialized) main {
            "; page=${hosts.page}, busy=${model.busy}, picker=${model.pickerPending}, error=${model.error}"
        } else "")
    }
    private fun provider(method: String, bytes: ByteArray? = null): Bundle = checkNotNull(
        instrumentation.targetContext.contentResolver.call(Uri.parse("content://com.hanxi.ghosterm.test.keycontrol"),
            "fixture-$method",null,Bundle().apply { if(bytes!=null) putByteArray("material",bytes) }))
    private fun keys() = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    private fun added() = vault.inventory().filter { it.id !in oldIds }
    @Before fun setup() {
        automation.serviceInfo = automation.serviceInfo.apply { flags = flags or AccessibilityServiceInfo.FLAG_REPORT_VIEW_IDS }
        rotation = ui.activity.display?.rotation ?: 0
        autoRotation = Settings.System.getInt(ui.activity.contentResolver,Settings.System.ACCELEROMETER_ROTATION,0)
        assertTrue(automation.setRotation(UiAutomation.ROTATION_FREEZE_0))
        await { ui.activity.display?.rotation == 0 && ui.activity.resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT }
        ui.waitForIdle()
        main {
            model=ViewModelProvider(ui.activity)[IdentitiesViewModel::class.java]
            hosts=ViewModelProvider(ui.activity)[HostsViewModel::class.java]
            sessions=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
        }
        directory=File(ui.activity.filesDir,"identities"); vault=IdentityVault(directory)
        // Isolation/cleanup must not call the production parser under mutation.
        oldIds=directory.listFiles().orEmpty().map { it.nameWithoutExtension }.toSet()
        val bytes=fixture.getString("pem").toByteArray()
        try { provider("configure",bytes) } finally { bytes.fill(0) }
        await { !hosts.busy }
    }
    @After fun cleanup() {
        if(::model.isInitialized) main { model.leave(); sessions.connecting?.let { sessions.cancelConnection(it.id) }; sessions.sessions.map { it.id }.forEach(sessions::close) }
        provider("release")
        if(::directory.isInitialized) directory.listFiles().orEmpty().filter { it.nameWithoutExtension !in oldIds }.forEach { file ->
            keys().deleteEntry("ghosterm.ssh.identity.${file.nameWithoutExtension}")
            check(file.delete()) { "Cannot remove test identity fixture" }
        }
        provider("clear")
        automation.setRotation(if(autoRotation==1) UiAutomation.ROTATION_UNFREEZE else rotation)
    }
    private fun settings() {
        ui.onNodeWithText("设置").performClick()
        ui.onNodeWithText("SSH 身份").performScrollTo().performClick()
        await { !model.busy && hosts.page==Page.IDENTITIES }
    }
    private fun row(id: String) = ui.onNodeWithTag("identity-row-$id")
    private fun fingerprint() { ui.onNodeWithTag("identity-fingerprint").performScrollTo().assertTextEquals(fixture.getString("clientFingerprint")) }
    @Test fun settingsFingerprintAndConfirmedRemovalSurviveRecreation() {
        val identity=vault.import("settings-key",fixture.getString("pem").toByteArray())
        settings(); row(identity.id).performScrollTo().performClick(); await { !model.busy }
        ui.onNodeWithTag("identity-algorithm").assertTextEquals("类型：ssh-ed25519"); fingerprint()
        ui.onNodeWithText("移除此身份").performScrollTo().performClick()
        ui.activityRule.scenario.recreate()
        assertSame(model,main { ViewModelProvider(ui.activity)[IdentitiesViewModel::class.java] })
        ui.onNodeWithText("保留身份").performClick()
        assertTrue(keys().containsAlias("ghosterm.ssh.identity.${identity.id}")); assertNotNull(vault.list().find { it.id==identity.id })
        ui.onNodeWithText("移除此身份").performScrollTo().performClick(); ui.onNodeWithText("确认移除").performClick()
        await { !model.busy && model.selected==null }
        assertFalse(keys().containsAlias("ghosterm.ssh.identity.${identity.id}"))
        assertFalse(File(directory,"${identity.id}.json").exists())
        ui.onNodeWithTag("identity-notice").assertTextEquals("身份已移除")
        ui.onNodeWithText("返回").performClick(); assertEquals(Page.SETTINGS,main { hosts.page })
    }
    @Test fun corruptEntryIsVisibleAndRemovableWithoutHidingHealthyKey() {
        val healthy=vault.import("healthy-key",fixture.getString("pem").toByteArray())
        val broken=UUID.randomUUID().toString(); File(directory,"$broken.json").writeText("broken ciphertext container")
        settings(); row(healthy.id).assertExists(); row(broken).performScrollTo().performClick()
        ui.onNodeWithTag("identity-error").assertExists()
        ui.onNodeWithText("移除此身份").performScrollTo().performClick(); ui.onNodeWithText("确认移除").performClick()
        await { !model.busy && model.selected==null }
        row(healthy.id).performScrollTo().performClick(); await { !model.busy }; fingerprint()
        assertFalse(File(directory,"$broken.json").exists())
    }
    @Test fun staleRemovalPreservesChangedEnvelopeAndKeystoreKey() {
        val identity=vault.import("before",fixture.getString("pem").toByteArray())
        val record=added().single(); val file=File(directory,"${identity.id}.json"); val original=file.readText()
        val changed=JSONObject(original).put("name","after").toString(); file.writeText(changed)
        assertThrows(IllegalStateException::class.java) { vault.removeRecord(record) }
        assertEquals(changed,file.readText()); assertTrue(keys().containsAlias("ghosterm.ssh.identity.${identity.id}"))
        file.writeText(original); vault.removeRecord(record); assertFalse(file.exists())
    }
    @Test fun encryptedPublicInfoMatchesOracleAndMalformedDataIsExplicit() {
        val bytes=fixture.getString("encryptedPem").toByteArray()
        try {
            val info=IdentityPublicInfoReader.read(bytes)
            assertEquals("ssh-ed25519",info.algorithm); assertEquals(fixture.getString("clientFingerprint"),info.fingerprint)
            assertNull(info.unavailable)
        } finally { bytes.fill(0) }
        val malformed="-----BEGIN OPENSSH PRIVATE KEY-----\n!!!!\n-----END OPENSSH PRIVATE KEY-----".toByteArray()
        val invalid=IdentityPublicInfoReader.read(malformed)
        assertNull(invalid.fingerprint); assertNotNull(invalid.unavailable)
        val unsupported=IdentityPublicInfoReader.read("-----BEGIN ENCRYPTED PRIVATE KEY-----".toByteArray())
        assertNull(unsupported.fingerprint); assertNotNull(unsupported.unavailable)
    }
    @Test fun rsaAndAllNistCurvesMatchIndependentOpenSshFingerprints() {
        val fixtures=fixture.getJSONArray("publicInfoFixtures")
        assertEquals(4,fixtures.length())
        for(i in 0 until fixtures.length()) {
            val item=fixtures.getJSONObject(i); val bytes=item.getString("pem").toByteArray()
            try {
                val info=IdentityPublicInfoReader.read(bytes)
                assertEquals(item.getString("algorithm"),info.algorithm)
                assertEquals(item.getString("fingerprint"),info.fingerprint)
                assertNull(info.unavailable)
            } finally { bytes.fill(0) }
        }
    }
    @Test fun pickerCancelAndLateResultAfterLeavingDoNotImport() {
        settings(); openPicker(); automation.performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK)
        await { !model.pickerPending }; assertTrue(added().isEmpty())
        openPicker()
        main { model.leave(); hosts.back(); hosts.identities(); model.enter() }
        await { !model.busy }
        select("ssh-key"); ui.waitForIdle()
        await { !model.busy }
        assertTrue(added().isEmpty()); assertEquals(0,provider("status").getInt("reads"))
    }
    @Test fun leavingDuringPipeReadClosesProviderWithoutSaving() {
        settings(); openPicker(); select("slow-key")
        systemAwait { provider("status").getInt("reads")==1 }
        await { model.busy }
        ui.onNodeWithText("返回").performClick()
        systemAwait { provider("status").getInt("closed")==1 }
        assertTrue(added().isEmpty()); assertEquals(0,provider("status").getInt("writes"))
    }
    @Test fun realPickerRotationImportsIdentityUsableBySsh() {
        settings(); openPicker()
        val old=ui.activity
        assertTrue(automation.setRotation(UiAutomation.ROTATION_FREEZE_90))
        systemAwait {
            val root = automation.rootInActiveWindow
            val bounds = Rect(); root?.getBoundsInScreen(bounds)
            root?.packageName?.toString()?.contains("documentsui") == true && bounds.width() > bounds.height()
        }
        select("ssh-key")
        await("Rotation must destroy the old Activity") { old.isDestroyed }
        await("Exactly one settings identity must be imported") { !model.busy && added().size==1 }
        assertNotSame(old,ui.activity); assertSame(model,main { ViewModelProvider(ui.activity)[IdentitiesViewModel::class.java] })
        val identity=added().single(); row(identity.id).performScrollTo().performClick(); await { !model.busy }; fingerprint()
        ui.onNodeWithText("返回").performClick(); ui.onNodeWithText("返回").performClick(); ui.onNodeWithText("返回").performClick()
        val config=File(ui.activity.filesDir,"home/.ssh/config"); val known=File(ui.activity.filesDir,"home/.ssh/known_hosts")
        val before=config.takeIf(File::exists)?.readBytes(); val previousKnown=known.takeIf(File::exists)?.readBytes()
        try {
            known.delete(); config.parentFile!!.mkdirs()
            config.writeText("Host identity-settings\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n User ${fixture.getString("user")}\n")
            main { hosts.reload() }; await { !hosts.busy }
            ui.onNodeWithText("identity-settings").performClick(); await { sessions.connecting?.auth?.phase==SSHAuthPhase.HOSTKEY }
            ui.onNodeWithText("信任并连接").performClick(); await { sessions.connecting?.identities?.any { it.id==identity.id }==true }
            ui.onNodeWithTag("ssh-key-${identity.id}").performScrollTo().performClick()
            ui.onNodeWithText("认证并连接").performClick(); await { sessions.active?.state?.connected==true }
            ui.onNodeWithTag("terminal-surface").assertIsDisplayed()
            val connected=main { sessions.active!! }
            main { sessions.home() }; settings()
            row(identity.id).performScrollTo().performClick(); await { !model.busy }
            ui.onNodeWithText("移除此身份").performScrollTo().performClick(); ui.onNodeWithText("确认移除").performClick()
            await { !model.busy && model.selected==null }
            assertSame(connected,main { sessions.active }); assertTrue(main { connected.state.connected })
            assertEquals(0,provider("status").getInt("writes"))
        } finally { before?.let(config::writeBytes) ?: config.delete(); previousKnown?.let(known::writeBytes) ?: known.delete() }
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
        ui.onNodeWithText("导入私钥").performScrollTo().performClick()
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
        // A null/transient tree during rotation is not a completed picker result.
        systemAwait {
            val tree = nodes()
            tree.any { it.isVisibleToUser && it.packageName?.toString() == "com.hanxi.ghosterm" } &&
                tree.none { it.packageName?.toString()?.contains("documentsui") == true }
        }
        await("Picker result must be consumed") { !model.pickerPending }
    }
}
