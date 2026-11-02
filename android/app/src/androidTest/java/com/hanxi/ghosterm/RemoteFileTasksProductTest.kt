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
    private fun auth()=tasks().authentication
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
    private val trustedAttempts=mutableSetOf<UUID>()
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
        ui.runOnIdle { tasks().tasks.toList().forEach(tasks()::remove);files().requestClipboardClear();files().confirmClipboardClear() }
        ui.waitUntil(10000) { tasks().tasks.isEmpty() }
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
            val attempt=auth().connecting ?: sessions().connecting
            if(attempt!=null) {
                assertFalse("authentication failed: ${attempt.message}",attempt.failed)
                when(attempt.auth?.phase) {
                    SSHAuthPhase.HOSTKEY -> if(trustedAttempts.add(attempt.id)) ui.onNodeWithText("信任并连接").performClick()
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
        assertTrue("flow did not finish: ${(auth().connecting ?: sessions().connecting)?.message} / ${tasks().tasks.firstOrNull()?.error}",done())
    }
    private fun openRemote(alias:String=unique) {
        if(!files().visible) ui.onNodeWithText("本机文件").performClick()
        ui.onNodeWithTag("file-location").performClick()
        ui.onNodeWithTag("file-connect:$alias").performScrollTo().performClick()
        loginUntil { sessions().connecting==null && files().location.endpoint.remoteAlias==alias && !files().loading }
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
        ui.runOnIdle { tasks().start(snapshot,target,target.directoryId) }
    }
    private fun ready() {
        loginUntil { latest.phase==FileTaskPhase.CONFIRMING || latest.phase==FileTaskPhase.FINISHED }
        assertEquals("preflight: ${latest.error}",FileTaskPhase.CONFIRMING,latest.phase)
    }
    private fun finish() {
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        loginUntil { !tasks().busy }
        assertEquals("copy: ${latest.result?.error}",FileCopyState.COMPLETE,latest.result?.state)
        ui.waitUntil(10000) { owners()==1 } // Browser is still independent and usable.
    }
    @Test fun historicalContentUsesFreshDestinationAndStoredDigestAfterSourceDisappears() {
        openRemote();val destination=files().location
        val local=FileLocation(FileEndpoint.APP)
        val entry=kotlinx.coroutines.runBlocking { LocalFileBrowser(home,ui.activity.contentResolver).browse(local).entries.single { it.name=="$unique.txt" } }
        val clipboard=FileClipboardSnapshot.capture(local,"local",listOf(entry))
        start(clipboard,destination);ready();finish()
        val written=latest.result!!.items.single();assertNotNull(written.sourceDigest)
        val request=FilePasteRequest.capture(clipboard,destination,"history target")
        val item=written.copy(state=FileCopyItemState.UNKNOWN)
        val journal=FileTaskJournal(File(ui.activity.filesDir,"file-history/tasks.db"))
        kotlinx.coroutines.runBlocking { journal.accept(request);journal.phase(request.id,FileJournalPhase.CONFIRMING);journal.begin(request.id,listOf(item)) }
        assertTrue(File(home,"$unique.txt").delete())
        try {
            ui.runOnIdle { tasks().reloadHistory();tasks().show() }
            ui.waitUntil(10000) { !tasks().journalLoading && tasks().history.any { it.intent.id==request.id } }
            val before=kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==request.id } }
            val authBefore=authIds.size
            ui.onNodeWithTag("file-tasks").performScrollToNode(hasTestTag("file-history:${request.id}"))
            ui.onNode(hasTestTag("file-history-details") and hasAnyAncestor(hasTestTag("file-history:${request.id}"))).performScrollTo().performClick()
            ui.onNodeWithTag("file-history-content:$unique.txt:false").performScrollTo().performClick()
            loginUntil { tasks().inspectingId==null && tasks().contentInspections.keys.any { it.requestId==request.id } }
            assertEquals(FileHistoryContentState.MATCH,tasks().contentInspections.values.single().state)
            ui.onNodeWithTag("file-history-content-result").assertTextEquals("内容与复制时的来源一致")
            assertEquals(authBefore+1,authIds.size);assertEquals(1,owners())
            assertEquals(before,kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==request.id } })
            assertEquals(FileCopyItemState.UNKNOWN,before.items.single().value.state)
        } finally {
            ui.runOnIdle { tasks().cancelInspection() };ui.waitUntil(15000) { tasks().inspectingId==null }
            val record=tasks().history.single { it.intent.id==request.id }
            ui.runOnIdle { tasks().removeHistory(record) };ui.waitUntil(10000) { !tasks().journalLoading }
        }
    }
    @Test fun historicalInspectionUsesFreshDestinationAuthenticationWithoutSourceOrReplay() {
        val destination=FileLocation(FileEndpoint("history-$unique",unique,fixture.getString("taskRemoteScratch"),
            remote=RemoteFileIdentity(unique,"127.0.0.1",fixture.getString("user"),fixture.getInt("port"))))
        // This old source no longer has any configuration. Inspection only reads the destination.
        val source=FileLocation(FileEndpoint("gone-$unique","gone","/",remote=RemoteFileIdentity("gone-$unique","missing.invalid","nobody",22)))
        val entry=BrowserEntry("/gone.txt","gone.txt",FileKind.FILE,3u,null)
        val request=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"gone",listOf(entry)),destination,"history target")
        val item=FileCopyItemResult(FileScanEntry(source,entry,"gone.txt"),FileCopyItemState.UNKNOWN,
            destination,"$unique-missing",candidatePath=destination.directoryId+"/.$unique-candidate")
        val journal=FileTaskJournal(File(ui.activity.filesDir,"file-history/tasks.db"))
        kotlinx.coroutines.runBlocking {
            journal.load();journal.accept(request);journal.phase(request.id,FileJournalPhase.CONFIRMING);journal.begin(request.id,listOf(item))
        }
        try {
            ui.runOnIdle { tasks().reloadHistory();tasks().show() }
            ui.waitUntil(10000) { !tasks().journalLoading && tasks().history.any { it.intent.id==request.id } }
            val before=kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==request.id } }
            assertTrue(tasks().inspections.isEmpty());assertEquals(0,owners())
            ui.onNode(hasTestTag("file-history-inspect") and hasAnyAncestor(hasTestTag("file-history:${request.id}")))
                .performScrollTo().performClick()
            loginUntil { tasks().inspections.containsKey(request.id) }
            assertNull(tasks().inspectionError);assertEquals(1,authIds.size)
            assertEquals(listOf(FileHistoryPresence.ABSENT,FileHistoryPresence.ABSENT),
                tasks().inspections.getValue(request.id).observations.map { it.presence })
            assertEquals(before,kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==request.id } })
            assertTrue(tasks().tasks.isEmpty());assertTrue(sessions().sessions.isEmpty());assertEquals(0,owners())
        } finally {
            ui.runOnIdle { tasks().cancelInspection() }
            ui.waitUntil(15000) { tasks().inspectingId==null }
            val record=tasks().history.single { it.intent.id==request.id }
            ui.runOnIdle { tasks().removeHistory(record) }
            ui.waitUntil(10000) { !tasks().journalLoading }
        }
    }
    @Test fun historicalRetryAuthenticatesFreshTargetAndWritesNewLinkedResult() {
        val source=FileLocation(FileEndpoint.APP)
        val entry=kotlinx.coroutines.runBlocking { LocalFileBrowser(home,ui.activity.contentResolver).browse(source).entries.single { it.name=="$unique.txt" } }
        val destination=FileLocation(FileEndpoint("history-retry-$unique",unique,fixture.getString("taskRemoteScratch"),
            remote=RemoteFileIdentity(unique,"127.0.0.1",fixture.getString("user"),fixture.getInt("port"))))
        val old=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"local source",listOf(entry)),destination,"history target")
        val journal=FileTaskJournal(File(ui.activity.filesDir,"file-history/tasks.db"))
        kotlinx.coroutines.runBlocking { journal.load();journal.accept(old) }
        try {
            val before=kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==old.id } }
            ui.runOnIdle { tasks().reloadHistory();tasks().show() }
            ui.waitUntil(10000) { !tasks().journalLoading && tasks().history.any { it.intent.id==old.id } }
            assertEquals(0,owners());assertTrue(tasks().tasks.isEmpty())
            ui.onNode(hasTestTag("file-history-retry") and hasAnyAncestor(hasTestTag("file-history:${old.id}"))).performScrollTo().performClick()
            assertTrue(tasks().tasks.isEmpty())
            ui.onNodeWithTag("file-history-retry-confirm").performClick()
            loginUntil { tasks().tasks.singleOrNull()?.phase in setOf(FileTaskPhase.CONFIRMING,FileTaskPhase.FINISHED) }
            val child=tasks().tasks.single()
            assertEquals(child.error,FileTaskPhase.CONFIRMING,child.phase)
            assertEquals(old.id,child.request.retryOf);assertNotEquals(old.id,child.request.id)
            ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
            loginUntil { !tasks().busy }
            assertEquals(child.result?.error,FileCopyState.COMPLETE,child.result?.state)
            assertEquals(2,authIds.size);assertEquals(0,owners())
            assertEquals(before,kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==old.id } })
            assertEquals(old.id,kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==child.request.id }.intent.retryOf })
            kotlinx.coroutines.runBlocking {
                val connection=RemoteFileSession(ResolvedSSHConnection("127.0.0.1",fixture.getInt("port"),fixture.getString("user"),known.path,byteArrayOf(127,0,0,1)))
                try {
                    kotlinx.coroutines.withTimeout(10000) {
                        var prompt=connection.authentication()
                        while(prompt.phase!=SSHAuthPhase.CREDENTIALS) {
                            assertFalse(connection.state().finished);kotlinx.coroutines.delay(10);prompt=connection.authentication()
                        }
                        connection.privateKey(prompt,fixture.getString("taskFilePem").toByteArray())
                        while(!connection.state().ready) { assertFalse(connection.state().finished);kotlinx.coroutines.delay(10) }
                    }
                    assertArrayEquals("真实任务上传 ☃\n".toByteArray(),connection.preview(destination.directoryId+"/$unique.txt"))
                } finally { kotlinx.coroutines.withContext(kotlinx.coroutines.NonCancellable) { connection.closeAndAwait() } }
            }
            assertEquals(0,owners())
        } finally {
            ui.runOnIdle { tasks().tasks.toList().forEach(tasks()::cancel) }
            ui.waitUntil(15000) { !tasks().busy }
            tasks().history.firstOrNull { it.intent.id==old.id }?.let { record -> ui.runOnIdle { tasks().removeHistory(record) } }
            ui.waitUntil(10000) { !tasks().journalLoading }
        }
    }
    @Test fun uncertainHistoricalCopyUsesFreshRemoteAuthenticationAndPreservesOldResult() {
        val source=FileLocation(FileEndpoint.APP)
        val entry=kotlinx.coroutines.runBlocking { LocalFileBrowser(home,ui.activity.contentResolver).browse(source).entries.single { it.name=="$unique.txt" } }
        val destination=FileLocation(FileEndpoint("history-retry-$unique",unique,fixture.getString("taskRemoteScratch"),
            remote=RemoteFileIdentity(unique,"127.0.0.1",fixture.getString("user"),fixture.getInt("port"))))
        val old=FilePasteRequest.capture(FileClipboardSnapshot.capture(source,"local source",listOf(entry)),destination,"history target")
        val journal=FileTaskJournal(File(ui.activity.filesDir,"file-history/tasks.db"))
        kotlinx.coroutines.runBlocking {
            journal.load();journal.accept(old);journal.phase(old.id,FileJournalPhase.CONFIRMING)
            val item=FileCopyItemResult(FileScanEntry(source,entry,entry.name),FileCopyItemState.NOT_ATTEMPTED,destination,entry.name)
            journal.begin(old.id,listOf(item));journal.checkpoint(old.id,item,FileCopyCheckpoint.REMOTE_CREATE)
        }
        try {
            val before=kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==old.id } }
            ui.runOnIdle { tasks().reloadHistory();tasks().show() }
            ui.waitUntil(10000) { !tasks().journalLoading && tasks().history.any { it.intent.id==old.id } }
            assertEquals(0,owners());assertTrue(tasks().tasks.isEmpty())
            ui.onNode(hasTestTag("file-history-recovery") and hasAnyAncestor(hasTestTag("file-history:${old.id}"))).performScrollTo().performClick()
            assertTrue(tasks().tasks.isEmpty())
            ui.onNodeWithTag("file-history-recovery-confirm").assertIsNotEnabled()
            ui.onNodeWithTag("file-history-recovery-select:$unique.txt").assertIsOff().performClick().assertIsOn()
            ui.onNodeWithTag("file-history-recovery-confirm").performClick()
            loginUntil { tasks().tasks.singleOrNull()?.phase in setOf(FileTaskPhase.CONFIRMING,FileTaskPhase.FINISHED) }
            val child=tasks().tasks.single()
            assertEquals(child.error,FileTaskPhase.CONFIRMING,child.phase)
            assertEquals(old.id,child.request.retryOf);assertNotEquals(old.id,child.request.id)
            ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
            loginUntil { !tasks().busy }
            assertEquals(child.result?.error,FileCopyState.COMPLETE,child.result?.state)
            assertEquals(2,authIds.size);assertEquals(0,owners())
            assertEquals(before,kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==old.id } })
            assertEquals(old.id,kotlinx.coroutines.runBlocking { journal.load().single { it.intent.id==child.request.id }.intent.retryOf })
            kotlinx.coroutines.runBlocking {
                val connection=RemoteFileSession(ResolvedSSHConnection("127.0.0.1",fixture.getInt("port"),fixture.getString("user"),known.path,byteArrayOf(127,0,0,1)))
                try {
                    kotlinx.coroutines.withTimeout(10000) {
                        var prompt=connection.authentication()
                        while(prompt.phase!=SSHAuthPhase.CREDENTIALS) {
                            assertFalse(connection.state().finished);kotlinx.coroutines.delay(10);prompt=connection.authentication()
                        }
                        connection.privateKey(prompt,fixture.getString("taskFilePem").toByteArray())
                        while(!connection.state().ready) { assertFalse(connection.state().finished);kotlinx.coroutines.delay(10) }
                    }
                    assertArrayEquals("真实任务上传 ☃\n".toByteArray(),connection.preview(destination.directoryId+"/$unique.txt"))
                } finally { kotlinx.coroutines.withContext(kotlinx.coroutines.NonCancellable) { connection.closeAndAwait() } }
            }
            assertEquals(0,owners())
        } finally {
            ui.runOnIdle { tasks().tasks.toList().forEach(tasks()::cancel) }
            ui.waitUntil(15000) { !tasks().busy }
            tasks().history.firstOrNull { it.intent.id==old.id }?.let { record -> ui.runOnIdle { tasks().removeHistory(record) } }
            ui.waitUntil(10000) { !tasks().journalLoading }
        }
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
        ui.waitUntil(10000) { auth().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        val attempt=auth().connecting!!.id
        ui.activityRule.scenario.recreate();ui.waitForIdle()
        assertSame(task,latest);assertEquals(attempt,auth().connecting!!.id)
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
        ui.waitUntil(10000) { auth().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        ui.onNodeWithTag("ssh-task-role").assertTextEquals("复制任务 · 读取来源")
        ui.runOnIdle { files().leave() }
        loginUntil { !tasks().busy }
        assertEquals(FileCopyState.COMPLETE,latest.result?.state)
        assertEquals("远端 UTF-8\nline two\n",File(home,"$unique/文本.txt").readText())
        ui.waitUntil(10000) { owners()==0 };assertNotNull(files().clipboard)
    }
    @Test fun remoteToRemoteUsesFourDistinctAuthenticationAttemptsThenDownloadsRealResult() = remoteCopy(false)
    @Test fun differentRemoteIdentityRequiresFreshAcknowledgementBeforeRealCopy() = remoteCopy(true)
    private fun remoteCopy(crossHost: Boolean) {
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
        val alias="$unique-other"
        if(crossHost) config.appendText("Host $alias\n HostName localhost\n Port ${fixture.getInt("port")}\n User ${fixture.getString("user")}\n IdentityFile ghosterm-identity:${key.id}\n")
        val endpoint=source.endpoint.copy(id="second-$unique",remote=if(crossHost)
            source.endpoint.remote!!.copy(alias=alias,host="localhost") else source.endpoint.remote)
        val target=FileLocation(endpoint,fixture.getString("taskRemoteScratch"))
        val clipboard=FileClipboardSnapshot.capture(source,source.directoryId,listOf(uploaded))
        start(clipboard,target);ready()
        if(crossHost) {
            val cancelled=latest;val old=checkNotNull(cancelled.namespaceReview)
            assertNull(cancelled.decisions);assertNull(cancelled.result)
            ui.onNodeWithTag("file-task-namespace-review").performScrollTo().assertIsDisplayed()
            ui.onNodeWithTag("file-task-confirm").assertDoesNotExist()
            ui.runOnIdle { tasks().cancel(cancelled) };ui.waitUntil(10000) { !cancelled.active }
            start(clipboard,target);ready();val current=latest
            ui.runOnIdle { tasks().confirmNamespace(current,old);tasks().confirmNamespace(cancelled,old) }
            assertNotNull(current.namespaceReview);assertNull(current.decisions)
            ui.activityRule.scenario.recreate();ui.waitForIdle()
            ui.onNodeWithTag("file-task-namespace-confirm").performScrollTo().performClick()
            ready();assertNull(latest.namespaceReview);assertTrue(latest.namespaceAcknowledged)
            assertNotNull(latest.decisions);assertNull(latest.result)
        }
        finish()
        // Cancelled scan, new scan, acknowledged rescan and execution each own two connections.
        assertEquals(if(crossHost)8 else 4,authIds.size-before)
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
            try { auth().fileTaskConnections(connectionRequest).use { scope ->
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
        ui.waitUntil(10000) { auth().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        val old=auth().connecting!!;val prompt=old.auth!!
        ui.onNodeWithText("取消连接").performClick()
        ui.waitUntil(10000) { !tasks().busy && owners()==1 }
        assertTrue(latest.cancelled);assertNull(latest.result);assertFalse(File(home,"$unique/文本.txt").exists())
        start(snapshot,FileLocation(FileEndpoint.APP,unique))
        ui.waitUntil(10000) { auth().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        val fresh=auth().connecting!!;val secret="must be wiped".toByteArray()
        ui.runOnIdle { auth().password(old.id,prompt,secret) }
        assertTrue(secret.all { it==0.toByte() });assertNotEquals(old.id,fresh.id)
        assertEquals(SSHAuthPhase.CREDENTIALS,auth().connecting!!.auth!!.phase)
        ready()
        ui.onNodeWithTag("file-task-confirm").performScrollTo().performClick()
        ui.waitUntil(10000) { auth().connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        ui.onNodeWithText("取消连接").performClick()
        ui.waitUntil(10000) { !tasks().busy && owners()==1 }
        assertEquals(FileCopyState.CANCELLED,latest.result?.state)
        assertTrue(latest.cancelled);assertFalse(File(home,"$unique/文本.txt").exists())
        start(snapshot,FileLocation(FileEndpoint.APP,unique));ready();finish()
        assertEquals("远端 UTF-8\nline two\n",File(home,"$unique/文本.txt").readText())
    }
    @Test fun queuedRemoteEndpointsWaitForTheirOwnAuthenticationAndKeepIndependentBytes() {
        openRemote();val sourceOne=files().location
        assertTrue("first endpoint fixture text missing",files().entries.any { it.name=="文本.txt" })
        val snapshotOne=FileClipboardSnapshot.capture(sourceOne,"first endpoint",listOf(files().entries.single { it.name=="文本.txt" }))
        val secondAlias="$unique-second"
        val peer=fixture.getJSONObject("queuePeer")
        config.appendText("Host $secondAlias\n HostName 127.0.0.1\n Port ${peer.getInt("port")}\n User ${fixture.getString("user")}\n IdentityFile ghosterm-identity:${key.id}\n")
        ui.runOnIdle { hosts().reload();files().leave() }
        ui.waitUntil(10000) { !hosts().busy && hosts().snapshot?.text==config.readText() }
        openRemote(secondAlias);val sourceTwo=files().location
        assertEquals("second endpoint initial directory",peer.getString("root"),sourceTwo.directoryId)
        assertTrue("second endpoint fixture text missing",files().entries.any { it.name=="文本.txt" })
        val snapshotTwo=FileClipboardSnapshot.capture(sourceTwo,"second endpoint",listOf(files().entries.single { it.name=="文本.txt" }))
        assertNotEquals(sourceOne.endpoint.remote,sourceTwo.endpoint.remote)
        assertEquals(peer.getString("root"),sourceTwo.directoryId)
        for(name in listOf("first","second","cancelled","block")) File(home,"$unique/$name").mkdir()
        val local=FileLocation(FileEndpoint.APP,"")
        assertTrue("local source missing",File(home,"$unique.txt").isFile)
        val item=kotlinx.coroutines.runBlocking { LocalFileBrowser(home,ui.activity.contentResolver).browse(local).entries.single { it.name=="$unique.txt" } }
        start(FileClipboardSnapshot.capture(local,"local blocker",listOf(item)),FileLocation(FileEndpoint.APP,"$unique/block"));ready()
        val blocker=latest
        start(snapshotOne,FileLocation(FileEndpoint.APP,"$unique/first"));val first=latest
        start(snapshotTwo,FileLocation(FileEndpoint.APP,"$unique/second"));val second=latest
        start(snapshotTwo,FileLocation(FileEndpoint.APP,"$unique/cancelled"));val cancelled=latest
        val attemptsBefore=authIds.size
        ui.runOnIdle {
            assertEquals(FileTaskPhase.QUEUED,first.phase);assertEquals(FileTaskPhase.QUEUED,second.phase)
            assertNull(auth().connecting);assertEquals(1,owners())
            tasks().cancel(cancelled)
            tasks().cancel(blocker)
        }
        loginUntil { first.phase==FileTaskPhase.CONFIRMING || first.phase==FileTaskPhase.FINISHED }
        assertEquals(FileTaskPhase.FINISHED,cancelled.phase)
        assertEquals("first: ${first.error}",FileTaskPhase.CONFIRMING,first.phase)
        assertEquals(FileTaskPhase.QUEUED,second.phase)
        val old=first.decisions!!.reference
        ui.runOnIdle { tasks().confirm(first,old) }
        loginUntil { first.phase==FileTaskPhase.FINISHED && (second.phase==FileTaskPhase.CONFIRMING || second.phase==FileTaskPhase.FINISHED) }
        assertEquals(FileCopyState.COMPLETE,first.result?.state)
        assertEquals("远端 UTF-8\nline two\n",File(home,"$unique/first/文本.txt").readText())
        assertEquals("second: ${second.error}",FileTaskPhase.CONFIRMING,second.phase)
        ui.runOnIdle { tasks().confirm(second,old) }
        assertEquals(FileTaskPhase.CONFIRMING,second.phase);assertNotNull(second.error)
        ui.runOnIdle { tasks().confirm(second,second.decisions!!.reference) }
        loginUntil { !tasks().busy }
        assertEquals(FileCopyState.COMPLETE,second.result?.state)
        assertEquals("第二端点的独立内容 ☃\n",File(home,"$unique/second/文本.txt").readText())
        assertFalse(File(home,"$unique/cancelled/文本.txt").exists())
        assertFalse(File(home,"$unique/block/$unique.txt").exists())
        assertEquals(attemptsBefore+4,authIds.size)
        assertEquals(sourceTwo,files().location);ui.waitUntil(10000) { owners()==1 }
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
            assertNull(latest.decisions);assertNull(auth().connecting)
            assertEquals(1,owners());assertFalse(File(home,"$unique/文本.txt").exists())
        }
        config.writeText(original);start(snapshot,FileLocation(FileEndpoint.APP,unique));ready();finish()
    }
}
