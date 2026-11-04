package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.os.SystemClock
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.PreferenceUpdate
import com.hanxi.ghosterm.data.IdentityVault
import com.hanxi.ghosterm.data.StoredIdentity
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.json.JSONArray
import org.json.JSONObject
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Screenshots of the production Activity/controllers. No production preview routes or fake success. */
@RunWith(AndroidJUnit4::class)
class AndroidVisualReviewTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private fun hosts()=ViewModelProvider(ui.activity)[HostsViewModel::class.java]
    private fun settings()=ViewModelProvider(ui.activity)[SettingsViewModel::class.java]
    private fun files()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun sessions()=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun tasks()=ViewModelProvider(ui.activity)[FileTasksViewModel::class.java]
    private val owner get()=(ui.activity.application as GhostermApplication).settings
    private lateinit var config:File
    private lateinit var preferences:File
    private var configBefore:ByteArray?=null
    private var preferencesBefore:ByteArray?=null
    private lateinit var known:File
    private var knownBefore:ByteArray?=null
    private var visualIdentity:StoredIdentity?=null
    private var visualRequest:String?=null
    private val vault get()=IdentityVault(File(ui.activity.filesDir,"identities"))
    private lateinit var directory:File
    private lateinit var shots:File
    private val receipt=JSONArray()
    private var prefix=""
    private val sample="Host production\n HostName prod.example.com\n User deploy\nHost dev-box\n HostName 192.0.2.24\n User dev\nHost home-lab\n HostName 192.0.2.86\n User alex\n"
    private fun await(label:String,block:()->Boolean) {
        val end=SystemClock.uptimeMillis()+10000
        while(SystemClock.uptimeMillis()<end) { if(ui.runOnIdle(block)) return;SystemClock.sleep(25) }
        fail(label)
    }
    @Before fun setup() {
        await("host/settings fixture did not load") { !hosts().busy && settings().canEdit }
        config=File(ui.activity.filesDir,"home/.ssh/config");configBefore=config.takeIf { it.exists() }?.readBytes()
        preferences=File(ui.activity.filesDir,"preferences/preferences.json");preferencesBefore=preferences.takeIf { it.exists() }?.readBytes()
        known=File(ui.activity.filesDir,"home/.ssh/known_hosts");knownBefore=known.takeIf { it.exists() }?.readBytes()
        directory=File(ui.activity.filesDir,"home/visual-${UUID.randomUUID()}").apply { mkdirs() }
        listOf("core","docs","scripts").forEach { File(directory,it).mkdir() }
        File(directory,"README.md").writeText("# Ghosterm\nYour terminal, everywhere.\n")
        File(directory,"build.log").writeText("Build finished\n")
        File(directory,".gitignore").writeText("build/\n")
        shots=File(ui.activity.cacheDir,"visual-review").apply { mkdirs() }
        config.writeText(sample);ui.runOnIdle { hosts().reload() };await("sample hosts not ready") { !hosts().busy && hosts().snapshot?.hosts?.size==3 }
        androidx.test.espresso.Espresso.closeSoftKeyboard()
        await("Initial review state must have the keyboard closed") {
            androidx.core.view.ViewCompat.getRootWindowInsets(ui.activity.window.decorView)
                ?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime())!=true
        }
    }
    @After fun restore() {
        visualRequest?.let { id ->
            ui.runOnIdle { tasks().tasks.firstOrNull { it.request.id==id }?.let { if(it.active) tasks().cancel(it) } }
            await("Visual task must release resources before fixture cleanup") { tasks().tasks.none { it.request.id==id && (it.active || it.recording) } }
            ui.runOnIdle {
                tasks().tasks.firstOrNull { it.request.id==id }?.let(tasks()::remove)
                tasks().history.firstOrNull { it.intent.id==id }?.let(tasks()::removeHistory)
            }
            await("Visual task record must be removed") { tasks().tasks.none { it.request.id==id } && tasks().history.none { it.intent.id==id } }
        }
        ui.runOnIdle {
            sessions().sessions.toList().forEach { sessions().close(it.id) }
            files().requestClipboardClear();files().confirmClipboardClear();files().leave()
            files().disconnectRemote()
        }
        if(::directory.isInitialized) directory.deleteRecursively()
        if(::config.isInitialized) { configBefore?.let(config::writeBytes) ?: config.delete() }
        if(::known.isInitialized) { knownBefore?.let(known::writeBytes) ?: known.delete() }
        visualIdentity?.let(vault::remove)
        if(::preferences.isInitialized) {
            await("preferences still saving") { owner.pendingCount.value==0 }
            preferencesBefore?.let(preferences::writeBytes) ?: preferences.delete()
            runBlocking { owner.reload(UUID.randomUUID()) }
        }
        if(::shots.isInitialized) File(shots,"$prefix-manifest.json").writeText(receipt.toString(2))
    }
    private fun capture(name:String) {
        ui.waitForIdle();ui.mainClock.advanceTimeBy(600);ui.waitForIdle()
        InstrumentationRegistry.getInstrumentation().waitForIdleSync()
        // UiAutomation captures SurfaceFlinger, after Compose's semantic/draw idle.
        // Material ripples finish on the render thread after semantic/draw idle.
        SystemClock.sleep(450)
        val bitmap=checkNotNull(InstrumentationRegistry.getInstrumentation().uiAutomation.takeScreenshot())
        File(shots,"$prefix-$name.png").outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG,100,it) }
        val root=ui.onAllNodes(isRoot()).onFirst().fetchSemanticsNode().boundsInRoot
        receipt.put(JSONObject().put("name",name).put("file","$prefix-$name.png")
            .put("width",bitmap.width).put("height",bitmap.height).put("fontScale",ui.activity.resources.configuration.fontScale)
            .put("density",ui.activity.resources.displayMetrics.density).put("rootWidth",root.width)
            .put("api",android.os.Build.VERSION.SDK_INT).put("pageSize",android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE))
            .put("hostPage",hosts().page.name).put("settingsSection",settings().section?.name)
            .put("filesVisible",files().visible).put("selection",files().selecting).put("clipboard",files().clipboard?.items?.size ?: 0)
            .put("remoteScope",files().remoteScope).put("awaitingRemote",files().awaitingRemote).put("terminalVisible",sessions().visible))
    }
    private fun back()=ui.onNode(hasText("返回") or hasContentDescription("返回")).performClick()
    private fun loaded()=await("file browser did not settle") { !files().loading }
    private fun home() { ui.runOnIdle { files().leave();sessions().home();while(hosts().page!=Page.HOSTS) hosts().back() } }
    private fun take(appearance:AppAppearance) {
        prefix=appearance.id
        val edits=PreferenceEdit.defaults()+PreferenceEdit.Appearance(appearance)
        assertTrue(runBlocking { owner.save(UUID.randomUUID(),edits) } is PreferenceUpdate.Saved)
        await("theme did not apply") { owner.state.value.committed.appearance==appearance }
        capture("hosts")
        ui.onNodeWithTag("host-menu:production").performClick();capture("host-menu")
        ui.onNodeWithText("编辑主机").performClick();capture("host-edit");back()
        ui.onNodeWithText("添加主机").performClick();capture("host-add");back()
        ui.onNodeWithTag("host-menu:production").performClick()
        androidx.test.espresso.Espresso.pressBack()
        ui.onNodeWithTag("hosts-search").performTextInput("no-match");capture("host-search-empty")
        ui.onNodeWithTag("hosts-search").performTextClearance();androidx.test.espresso.Espresso.closeSoftKeyboard()
        // Removing only the fixture config tests the production empty state.
        config.writeText("");ui.runOnIdle { hosts().reload() };await("empty host state not ready") { !hosts().busy && hosts().snapshot?.hosts?.isEmpty()==true }
        capture("hosts-empty")
        config.writeText(sample);ui.runOnIdle { hosts().reload() };await("sample hosts not restored") { !hosts().busy }
        ui.onNode(hasText("设置") or hasContentDescription("设置")).performClick();capture("settings")
        ui.onNodeWithText("查看 SSH 配置").performScrollTo().performClick();capture("ssh-config");back()
        for(section in SettingsSection.entries) {
            ui.onNodeWithTag("settings-section-${section.name}").performScrollTo().performClick();capture("settings-${section.name.lowercase()}")
            if(section==SettingsSection.APPEARANCE) {
                ui.onNodeWithTag("settings-row-terminalTheme").performScrollTo().performClick();capture("settings-choice")
                androidx.test.espresso.Espresso.pressBack()
            }
            back()
        }
        ui.onNodeWithText("恢复默认设置").performScrollTo().performClick();capture("settings-reset")
        ui.onNodeWithText("保留设置").performClick()
        ui.onNodeWithText("关于 Ghosterm").performScrollTo().performClick();capture("about");back()
        ui.onNodeWithTag("settings-section-SECURITY").performScrollTo().performClick()
        ui.onNodeWithText("SSH 身份").performScrollTo().performClick();capture("identities");back()
        ui.onNodeWithText("主机验证记录").performScrollTo().performClick();capture("known-hosts");back();back();back()
        ui.runOnIdle { files().enterLocal() };loaded()
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:${directory.name}"))
        ui.onNodeWithTag("file-entry:${directory.name}").performClick();loaded();capture("files-local")
        ui.onNodeWithTag("file-entry:README.md").performClick()
        await("Read-only preview did not load") { files().preview?.text!=null };capture("file-preview");back()
        ui.onNode(hasText("更多") or hasContentDescription("更多")).performClick();capture("files-menu")
        androidx.test.espresso.Espresso.pressBack()
        ui.onNodeWithTag("file-entry:README.md").performTouchInput { longClick() };capture("files-selection")
        ui.onNodeWithTag("file-copy").performClick()
        await("clipboard must be installed before taking its screenshot") { files().clipboard!=null && !files().selecting }
        ui.onNodeWithTag("file-clipboard").assertIsDisplayed();capture("files-clipboard")
        ui.onNodeWithTag("file-tab-remote").performClick()
        ui.onNodeWithTag("file-remote-empty").assertIsDisplayed();capture("files-remote-empty")
        home()
        ui.onNodeWithText("本地终端").performClick();await("actual PTY did not connect") { sessions().active?.state?.connected==true }
        val session=sessions().active!!
        session.surface.commit("printf '\\033[2J\\033[H\\033[38;2;181;213;165m$ pwd\\033[0m\\r\\n/data/user/0/com.hanxi.ghosterm/files/home\\r\\n\\r\\n\\033[38;2;181;213;165m$ ls\\033[0m\\r\\ncore  docs  scripts\\r\\n\\r\\n$ '\r")
        await("actual PTY output not rendered") { session.connection.frame(-1)?.let { frame -> (0 until frame.rows).any { frame.line(it).contains("core  docs  scripts") } }==true }
        capture("terminal")
        ui.showTerminalKeyboard()
        ui.waitUntil(10000) { terminalImeShown() }
        capture("terminal-keyboard")
        ui.terminalBack()
        ui.waitUntil(10000) { !terminalImeShown() }
        ui.openTerminalSessions();capture("sessions")
        androidx.test.espresso.Espresso.pressBack();ui.terminalBack();capture("hosts-resume")
        captureRemoteAndSecurity()
        captureActualCopy()
        captureLargeFontAndLandscape()
    }
    private fun captureRemoteAndSecurity() {
        val fixture=sshFixture()
        val key=vault.import("SFTP 验收身份",fixture.getString("filePem").toByteArray()).also { visualIdentity=it }
        known.delete()
        config.appendText("Host visual-sftp\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n User ${fixture.getString("user")}\n IdentityFile ghosterm-identity:${key.id}\n")
        ui.runOnIdle { hosts().reload() };await("SFTP host not loaded") { !hosts().busy && hosts().snapshot?.hosts?.size==4 }
        ui.onNodeWithTag("host-list").performScrollToNode(hasTestTag("host-menu:visual-sftp"))
        ui.onNodeWithTag("host-menu:visual-sftp").performClick()
        ui.onNodeWithText("浏览文件").performClick()
        await("SFTP host verification not shown") { sessions().connecting?.auth?.phase==SSHAuthPhase.HOSTKEY }
        ui.onNodeWithText("信任并连接").assertIsDisplayed()
        // Authentication intentionally uses FLAG_SECURE. Never relax it for a screenshot.
        ui.onNodeWithText("信任并连接").performClick()
        await("Real SFTP did not open") { sessions().connecting==null && files().location.endpoint.remoteAlias=="visual-sftp" && !files().loading }
        assertNull(files().error)
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:文本.txt"))
        ui.onNodeWithTag("file-entry:文本.txt").assertIsDisplayed();capture("files-remote")
        ui.onNodeWithTag("file-tab-local").performClick();loaded();capture("files-local-after-remote")
        home();ui.onNode(hasText("设置") or hasContentDescription("设置")).performClick()
        ui.onNodeWithTag("settings-section-SECURITY").performScrollTo().performClick()
        ui.onNodeWithText("SSH 身份").performScrollTo().performClick()
        ui.onNodeWithTag("identity-row-${key.id}").assertIsDisplayed();capture("identities-populated")
        ui.onNodeWithTag("identity-row-${key.id}").performClick()
        await("Identity metadata did not load") { !ViewModelProvider(ui.activity)[IdentitiesViewModel::class.java].busy }
        capture("identity-detail");back();back()
        ui.onNodeWithText("主机验证记录").performScrollTo().performClick()
        await("Host records did not load") { !ViewModelProvider(ui.activity)[KnownHostsViewModel::class.java].busy }
        ui.onNodeWithTag("known-host-row-0").assertIsDisplayed();capture("known-hosts-populated")
        ui.onNodeWithTag("known-host-row-0").performClick();capture("known-host-detail")
    }
    private fun captureActualCopy() {
        home();ui.runOnIdle { files().enterLocal() };loaded()
        // The local endpoint remembers the fixture directory across the remote tab.
        ui.onNodeWithTag("file-entry:core").performClick();loaded()
        ui.onNodeWithTag("file-paste").performClick()
        await("Copy preflight did not ask for confirmation") { tasks().tasks.any { it.phase==FileTaskPhase.CONFIRMING } }
        val task=ui.runOnIdle { tasks().tasks.first { it.phase==FileTaskPhase.CONFIRMING } }
        visualRequest=task.request.id
        capture("copy-confirmation")
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        await("Actual copy did not finish") { !task.active && !task.recording }
        assertEquals(FileCopyState.COMPLETE,task.result?.state)
        assertEquals(File(directory,"README.md").readText(),File(directory,"core/README.md").readText())
        capture("copy-result")
        await("Completed task cannot be archived") { tasks().canArchive(task) }
        ui.runOnIdle { tasks().archive(task) }
        await("Archived record not loaded") { tasks().history.any { it.intent.id==task.request.id } && tasks().tasks.none { it.request.id==task.request.id } }
        capture("copy-history")
    }
    private fun shell(command:String)=InstrumentationRegistry.getInstrumentation().uiAutomation.executeShellCommand(command).use {
        java.io.FileInputStream(it.fileDescriptor).bufferedReader().use { reader -> reader.readText().trim() }
    }
    private fun captureLargeFontAndLandscape() {
        val size=shell("wm size").lineSequence().firstOrNull { it.startsWith("Override size:") }?.substringAfter(":")?.trim() ?: "reset"
        val font=shell("settings get system font_scale")
        try {
            ui.runOnIdle { tasks().hide() }
            shell("settings put system font_scale 2.0");shell("wm size 320x640")
            await("Portrait large-font configuration did not apply") {
                ui.activity.resources.configuration.let { it.fontScale==2f && it.screenWidthDp<it.screenHeightDp }
            }
            ui.onNodeWithTag("file-entry:README.md").assertIsDisplayed();capture("files-large-font")
            shell("wm size 640x320")
            await("Landscape large-font configuration did not apply") {
                ui.activity.resources.configuration.let { it.fontScale==2f && it.screenWidthDp>it.screenHeightDp }
            }
            val row=ui.onNodeWithTag("file-entry:README.md").assertIsDisplayed().fetchSemanticsNode().boundsInRoot
            assertTrue("Real active summary and clipboard must leave a full 48dp row",row.height>=48f)
            capture("files-landscape-large-font")
            home();ui.runOnIdle { sessions().show(sessions().sessions.single().id) }
            ui.showTerminalKeyboard()
            ui.waitUntil(10000) { terminalImeShown() }
            capture("terminal-landscape-keyboard")
        } finally {
            androidx.test.espresso.Espresso.closeSoftKeyboard()
            shell("wm size $size");shell(if(font=="null") "settings delete system font_scale" else "settings put system font_scale $font")
        }
    }
    @Test fun lightScreensRenderActualControllersAndPty()=take(AppAppearance.LIGHT)
    @Test fun darkScreensRenderActualControllersAndPty()=take(AppAppearance.DARK)
}
