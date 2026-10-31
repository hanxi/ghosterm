package com.hanxi.ghosterm

import android.os.SystemClock
import android.graphics.Bitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.launch
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Production navigation, task authentication, Keystore credentials and real SFTP. */
@RunWith(AndroidJUnit4::class)
class RemoteFileTasksProductTest {
    @get:Rule val ui=createAndroidComposeRule<MainActivity>()
    private val fixture by lazy { sshFixture() }
    private fun files()=ViewModelProvider(ui.activity)[FilesViewModel::class.java]
    private fun sessions()=ViewModelProvider(ui.activity)[SessionsViewModel::class.java]
    private fun tasks()=ViewModelProvider(ui.activity)[FileTasksViewModel::class.java]
    private fun hosts()=ViewModelProvider(ui.activity)[HostsViewModel::class.java]
    private val latest get()=tasks().tasks.first()
    private val unique="task-${UUID.randomUUID().toString().take(8)}"
    private lateinit var config:File
    private lateinit var known:File
    private var configBefore:ByteArray?=null
    private var knownBefore:ByteArray?=null
    private lateinit var home:File
    private lateinit var vault:IdentityVault
    private lateinit var key:StoredIdentity
    private val authIds=mutableSetOf<UUID>()
    @Before fun setup() {
        home=File(ui.activity.filesDir,"home")
        config=File(home,".ssh/config");known=File(home,".ssh/known_hosts")
        configBefore=config.takeIf { it.exists() }?.readBytes();knownBefore=known.takeIf { it.exists() }?.readBytes()
        config.parentFile!!.mkdirs();known.delete()
        vault=IdentityVault(File(ui.activity.filesDir,"identities"));key=vault.import(unique,fixture.getString("taskFilePem").toByteArray())
        config.writeText("Host $unique\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n User ${fixture.getString("user")}\n IdentityFile ghosterm-identity:${key.id}\n")
        File(home,"$unique.txt").writeText("真实任务上传 ☃\n")
        File(home,unique).mkdir()
        ui.runOnIdle { hosts().reload() };ui.waitUntil(10000) { !hosts().busy && hosts().snapshot?.text==config.readText() }
    }
    @After fun cleanup() {
        ui.runOnIdle {
            tasks().tasks.toList().forEach(tasks()::cancel)
            sessions().connecting?.let { sessions().cancelConnection(it.id) }
            files().leave();sessions().sessions.map { it.id }.forEach(sessions()::close)
        }
        ui.waitUntil(15000) { !tasks().busy && owners()==0 }
        vault.remove(key)
        configBefore?.let(config::writeBytes) ?: config.delete();knownBefore?.let(known::writeBytes) ?: known.delete()
        File(home,"$unique.txt").delete();File(home,unique).deleteRecursively()
    }
    private fun owners():Int {
        val type=Class.forName("com.hanxi.ghosterm.bridge.NativeFiles")
        return type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null)) as Int
    }
    private fun loaded() { ui.waitUntil(10000) { !files().loading } }
    private fun loginUntil(done:()->Boolean) {
        val until=SystemClock.uptimeMillis()+20000
        while(!done() && SystemClock.uptimeMillis()<until) {
            val attempt=sessions().connecting
            if(attempt!=null) {
                assertFalse("authentication failed: ${attempt.message}",attempt.failed)
                when(attempt.auth?.phase) {
                    SSHAuthPhase.HOSTKEY -> ui.onNodeWithText("信任并连接").performClick()
                    SSHAuthPhase.CREDENTIALS -> if(attempt.identities.any { it.id==key.id } && !attempt.credentialBusy) {
                        if(authIds.add(attempt.id)) {
                            ui.onNodeWithTag("ssh-key-${key.id}").performScrollTo().performClick()
                            ui.onNodeWithText("认证并连接").performClick()
                        }
                    }
                    else -> Unit
                }
            }
            SystemClock.sleep(40)
        }
        assertTrue("flow did not finish: ${sessions().connecting?.message} / ${tasks().tasks.firstOrNull()?.error}",done())
    }
    private fun openRemote() {
        if(!files().visible) ui.onNodeWithText("本机文件").performClick()
        ui.onNodeWithTag("file-location").performClick()
        ui.onNodeWithTag("file-connect:$unique").performScrollTo().performClick()
        loginUntil { sessions().connecting==null && files().location.endpoint.remoteAlias==unique && !files().loading }
        assertNull(files().error)
    }
    private fun entry(name:String):SemanticsNodeInteraction {
        ui.onNodeWithTag("file-list").performScrollToNode(hasTestTag("file-entry:$name"))
        return ui.onNodeWithTag("file-entry:$name")
    }
    private fun copy(name:String) {
        entry(name).performTouchInput { longClick() }
        ui.onNodeWithTag("file-copy").performClick()
    }
    private fun start(snapshot:FileClipboardSnapshot, target:FileLocation) {
        ui.runOnIdle { tasks().start(snapshot,target,target.directoryId,
            { request -> sessions().fileTaskConnections(request) { tasks().cancelRequest(request.id) } }, { files().copyCompleted(it) }) }
    }
    private fun ready() {
        loginUntil { latest.phase!=FileTaskPhase.SCANNING }
        assertEquals("preflight: ${latest.error}",FileTaskPhase.CONFIRMING,latest.phase)
    }
    private fun finish() {
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        loginUntil { !tasks().busy }
        assertEquals("copy: ${latest.result?.error}",FileCopyState.COMPLETE,latest.result?.state)
        ui.waitUntil(10000) { owners()==1 } // Browser is still independent and usable.
    }
    @Test fun uploadThroughPasteButtonSurvivesRotationAndKeepsBrowserAndClipboard() {
        ui.onNodeWithText("本机文件").performClick();loaded();copy("$unique.txt")
        val clipboard=files().clipboard!!.id
        openRemote();val endpoint=files().location.endpoint
        ui.onNodeWithTag("file-paste").performClick();ready()
        val picture=ui.onRoot().captureToImage().asAndroidBitmap()
        File(ui.activity.cacheDir,"file-task-confirmation.png").outputStream().use { picture.compress(Bitmap.CompressFormat.PNG,100,it) }
        val task=latest
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        ui.waitUntil(10000) { sessions().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        val attempt=sessions().connecting!!.id
        ui.activityRule.scenario.recreate();ui.waitForIdle()
        assertSame(task,latest);assertEquals(attempt,sessions().connecting!!.id)
        ui.onNodeWithTag("ssh-task-role").assertTextEquals("复制任务 · 写入目标")
        loginUntil { !tasks().busy };assertEquals(FileCopyState.COMPLETE,task.result?.state)
        assertEquals(clipboard,files().clipboard!!.id)
        ui.runOnIdle { tasks().hide();files().reload() };loaded()
        assertEquals(endpoint,files().location.endpoint)
        entry("$unique.txt").performClick()
        ui.waitUntil(10000) { files().preview?.text!=null || files().preview?.error!=null }
        assertEquals("真实任务上传 ☃\n",files().preview?.text)
        assertEquals(1,owners());assertEquals(3,authIds.size)
    }
    @Test fun downloadUsesSourceRoleAndLeavingFilePageKeepsTaskAlive() {
        openRemote();copy("文本.txt")
        ui.runOnIdle { files().choose(FileEndpoint.APP) };loaded()
        entry(unique).performClick();loaded()
        ui.onNodeWithTag("file-paste").performClick();ready()
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        ui.waitUntil(10000) { sessions().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        ui.onNodeWithTag("ssh-task-role").assertTextEquals("复制任务 · 读取来源")
        ui.runOnIdle { files().leave() }
        loginUntil { !tasks().busy }
        assertEquals(FileCopyState.COMPLETE,latest.result?.state)
        assertEquals("远端 UTF-8\nline two\n",File(home,"$unique/文本.txt").readText())
        ui.waitUntil(10000) { owners()==0 };assertNotNull(files().clipboard)
    }
    @Test fun remoteToRemoteUsesFourDistinctAuthenticationAttemptsThenDownloadsRealResult() {
        openRemote();val source=files().location
        val entry=files().entries.single { it.name=="文本.txt" }
        val snapshot=FileClipboardSnapshot.capture(source,source.directoryId,listOf(entry))
        // Upload to this class's dedicated root; the original browse fixture stays unchanged.
        val local=FileLocation(FileEndpoint.APP)
        val item=kotlinx.coroutines.runBlocking { LocalFileBrowser(home,ui.activity.contentResolver).browse(local).entries.single { it.name=="$unique.txt" } }
        start(FileClipboardSnapshot.capture(local,"应用文件",listOf(item)),source);ready();finish()
        ui.runOnIdle { files().reload() };loaded()
        val uploaded=files().entries.single { it.name=="$unique.txt" }
        val before=authIds.size
        val target=FileLocation(source.endpoint.copy(id="second-$unique"),fixture.getString("taskRemoteScratch"))
        start(FileClipboardSnapshot.capture(source,source.directoryId,listOf(uploaded)),target);ready();finish()
        assertEquals(4,authIds.size-before)
        val copied=latest.result!!.items.single();assertEquals(target.directoryId+"/$unique.txt",copied.createdId)
        // Read back via the same production task path, without changing the browser endpoint.
        val remoteItem=uploaded.copy(id=copied.createdId!!,modifiedMillis=null)
        // Obtain current metadata through an independent task connection; public source scanning
        // requires exact metadata, so the published item is read from the browser after navigation.
        val downSource=target
        val probe=FileClipboardSnapshot.capture(downSource,downSource.directoryId,listOf(remoteItem))
        // A missing mtime in a selection is conservatively rejected; inspect through a task connection below.
        val connectionRequest=FilePasteRequest.capture(probe,FileLocation(FileEndpoint.APP,unique),unique)
        var found:BrowserEntry?=null
        var failure:Throwable?=null
        val job=kotlinx.coroutines.CoroutineScope(kotlinx.coroutines.Dispatchers.IO).launch {
            try { sessions().fileTaskConnections(connectionRequest).use { scope ->
                val dir=scope.remote(FileTaskConnections.Role.SOURCE,downSource.endpoint)!!.session.browse(downSource.directoryId)
                val e=dir.entries.single { it.name=="$unique.txt" }
                found=BrowserEntry(e.path,e.name,FileKind.FILE,e.size,e.modifiedSeconds?.times(1000))
            } } catch(e:Throwable) { failure=e }
        }
        loginUntil { job.isCompleted };assertNull(failure)
        start(FileClipboardSnapshot.capture(downSource,downSource.directoryId,listOf(found!!)),FileLocation(FileEndpoint.APP,unique));ready();finish()
        assertEquals("真实任务上传 ☃\n",File(home,"$unique/$unique.txt").readText())
        assertEquals(snapshot.source,files().location)
    }
    @Test fun cancelledAuthenticationReleasesTaskAndRejectsOldCredentialResponse() {
        openRemote();val source=files().location
        val snapshot=FileClipboardSnapshot.capture(source,source.directoryId,listOf(files().entries.single { it.name=="文本.txt" }))
        start(snapshot,FileLocation(FileEndpoint.APP,unique))
        ui.waitUntil(10000) { sessions().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        val old=sessions().connecting!!;val prompt=old.auth!!
        ui.onNodeWithText("取消连接").performClick()
        ui.waitUntil(10000) { !tasks().busy && owners()==1 }
        assertTrue(latest.cancelled);assertNull(latest.result);assertFalse(File(home,"$unique/文本.txt").exists())
        start(snapshot,FileLocation(FileEndpoint.APP,unique))
        ui.waitUntil(10000) { sessions().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        val fresh=sessions().connecting!!;val secret="must be wiped".toByteArray()
        ui.runOnIdle { sessions().password(old.id,prompt,secret) }
        assertTrue(secret.all { it==0.toByte() });assertNotEquals(old.id,fresh.id)
        assertEquals(SSHAuthPhase.CREDENTIALS,sessions().connecting!!.auth!!.phase)
        ready()
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        ui.waitUntil(10000) { sessions().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        ui.onNodeWithText("取消连接").performClick()
        ui.waitUntil(10000) { !tasks().busy && owners()==1 }
        assertEquals(FileCopyState.CANCELLED,latest.result?.state)
        assertTrue(latest.cancelled);assertFalse(File(home,"$unique/文本.txt").exists())
        start(snapshot,FileLocation(FileEndpoint.APP,unique));ready();finish()
        assertEquals("远端 UTF-8\nline two\n",File(home,"$unique/文本.txt").readText())
    }
    @Test fun changedConfigurationAndUnsupportedOptionsCannotRedirectFrozenTask() {
        openRemote();val source=files().location
        val snapshot=FileClipboardSnapshot.capture(source,source.directoryId,listOf(files().entries.single { it.name=="文本.txt" }))
        val original=config.readText()
        for((changed,reason) in listOf(
            original.replace("127.0.0.1","127.0.0.2") to "主机地址或用户已改变",
            original+" ProxyCommand false\n" to "ProxyCommand",
            "# removed\n" to "主机配置已移除")) {
            config.writeText(changed);start(snapshot,FileLocation(FileEndpoint.APP,unique))
            ui.waitUntil(10000) { !tasks().busy }
            assertTrue("configuration error must preserve its reason: ${latest.error}",latest.error?.contains(reason,ignoreCase=true)==true)
            assertNull(latest.decisions);assertNull(sessions().connecting)
            assertEquals(1,owners());assertFalse(File(home,"$unique/文本.txt").exists())
        }
        config.writeText(original);start(snapshot,FileLocation(FileEndpoint.APP,unique));ready();finish()
    }
}
