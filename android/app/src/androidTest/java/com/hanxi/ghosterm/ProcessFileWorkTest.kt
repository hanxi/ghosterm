package com.hanxi.ghosterm

import android.accessibilityservice.AccessibilityService
import androidx.lifecycle.Lifecycle
import android.app.UiAutomation
import android.graphics.Rect
import android.provider.Settings
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.view.accessibility.AccessibilityNodeInfo
import androidx.lifecycle.ViewModelProvider
import androidx.test.core.app.ActivityScenario
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.data.*
import com.hanxi.ghosterm.domain.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Multiple real Activities, actual file IO and independent native SSH authentication. */
@RunWith(AndroidJUnit4::class)
class ProcessFileWorkTest {
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val context get()=instrumentation.targetContext
    private val application get()=context.applicationContext as GhostermApplication
    private val work get()=application.fileWork
    private val unique="process-files-${UUID.randomUUID()}"
    private val home get()=File(context.filesDir,"home")
    private val root get()=File(home,unique)
    private val holders=mutableListOf<Holder>()
    private val releases=mutableListOf<CountDownLatch>()
    private var configBefore:ByteArray?=null
    private var knownBefore:ByteArray?=null
    private var remoteConfigured=false
    private var rotationToRestore:Int?=null
    private var autoRotation=false
    private val config get()=File(home,".ssh/config")
    private val known get()=File(home,".ssh/known_hosts")
    private val fixture by lazy { sshFixture() }
    private val vault get()=IdentityVault(File(context.filesDir,"identities"))
    private var oldIds=emptySet<String>()
    private lateinit var key:StoredIdentity
    private class Holder(val scenario:ActivityScenario<MainActivity>) {
        lateinit var activity:MainActivity
        lateinit var files:FilesViewModel
        lateinit var tasks:FileTasksViewModel
        lateinit var sessions:SessionsViewModel
        fun refresh() { scenario.onActivity {
            activity=it;val models=ViewModelProvider(it)
            files=models[FilesViewModel::class.java];tasks=models[FileTasksViewModel::class.java];sessions=models[SessionsViewModel::class.java]
        } }
    }
    private fun <T> main(block:()->T):T {
        var result:T?=null
        instrumentation.runOnMainSync { result=block() }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun await(message:String="expected state",condition:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+15000
        while(SystemClock.elapsedRealtime()<end) { if(main(condition))return;SystemClock.sleep(30) }
        fail("Timed out: $message; task authentication error="+main { work.authentication.connecting?.credentialError }+
            "; tasks="+main { work.tasks.tasks.map { "${it.phase}: ${it.error}" } })
    }
    private fun launch():Holder {
        val intent=Intent(context,MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK)
        val h=Holder(ActivityScenario.launch(intent));h.refresh();holders+=h
        main { assertSame("all Activity pages must use the application task owner",work.tasks.tasks,h.tasks.tasks) }
        await("foreground file presenter") { h.tasks.presentsAuthentication }
        return h
    }
    private fun close(h:Holder) { h.scenario.close();holders.remove(h);assertTrue(h.activity.isDestroyed) }
    @Before fun setup() {
        main { assertFalse("previous test left an active task",work.tasks.busy) }
        File(root,"source").mkdirs();File(root,"target").mkdirs();File(root,"other").mkdirs()
        File(root,"source/a.txt").writeText("process-owned bytes ☃\n")
        File(root,"source/b.txt").writeText("second source\n")
    }
    @After fun cleanup() {
        releases.forEach { it.countDown() }
        main { work.tasks.tasks.toList().forEach(work.tasks::cancel) }
        await("task cleanup") { !work.tasks.busy }
        holders.toList().forEach(::close)
        main { work.tasks.tasks.toList().forEach(work.tasks::remove);work.clipboard.snapshot=null }
        if(remoteConfigured) {
            vault.list().filter { it.id !in oldIds }.forEach(vault::remove)
            configBefore?.let(config::writeBytes) ?: config.delete();knownBefore?.let(known::writeBytes) ?: known.delete()
            provider("clear")
        }
        root.deleteRecursively()
        rotationToRestore?.let { instrumentation.uiAutomation.setRotation(if(autoRotation) UiAutomation.ROTATION_UNFREEZE else it) }
    }
    private fun browse(h:Holder,path:String) {
        main { h.files.enter();while(h.files.crumbs.size>1) h.files.back() };await { !h.files.loading }
        for(part in path.split('/').filter { it.isNotEmpty() }) {
            main { assertNull(h.files.error);h.files.tap(h.files.entries.single { it.name==part }) }
            await { !h.files.loading }
        }
        main { assertNull(h.files.error) }
    }
    private fun copy(h:Holder,name:String) = main {
        h.files.longPress(h.files.entries.single { it.name==name });h.files.copySelected()
    }
    private fun source(h:Holder):FileClipboardSnapshot {
        browse(h,"$unique/source");copy(h,"a.txt")
        return main { checkNotNull(h.files.clipboard) }
    }
    private fun target(name:String="target")=FileLocation(FileEndpoint.APP,"$unique/$name")
    @Test fun clipboardSurvivesFinishedActivityAndRejectsOtherPagesStaleDecisions() {
        val a=launch();val snapshot=source(a);val b=launch()
        main {
            assertNotSame(a.files,b.files);assertSame(snapshot,b.files.clipboard)
            assertFalse(a.tasks.presentsAuthentication);assertTrue(b.tasks.presentsAuthentication)
        }
        browse(b,"$unique/source");copy(b,"b.txt")
        main { b.files.requestClipboardClear() }
        copy(a,"b.txt");main { a.files.confirmClipboardReplacement() }
        val replacement=main { a.files.clipboard!! };assertNotEquals(snapshot.id,replacement.id)
        main {
            b.files.confirmClipboardReplacement();assertSame(replacement,b.files.clipboard);assertNotNull(b.files.clipboardError)
            b.files.confirmClipboardClear();assertSame(replacement,b.files.clipboard);assertNotNull(b.files.clipboardError)
        }
        close(a);assertSame(replacement,main { b.files.clipboard });close(b)
        val c=launch();assertSame(replacement,main { c.files.clipboard })
        main { c.files.requestClipboardClear();c.files.confirmClipboardClear() };assertNull(main { c.files.clipboard })
        assertEquals("process-owned bytes ☃\n",File(root,"source/a.txt").readText())
    }
    @Test fun confirmationAndResultOutliveOriginAndAreSharedWithoutSharingPageRoute() {
        val a=launch();val snapshot=source(a)
        main { a.tasks.start(snapshot,target(),"target") };await { work.tasks.tasks.first().phase==FileTaskPhase.CONFIRMING }
        val task=main { work.tasks.tasks.first() };val b=launch()
        main { assertNotSame(a.tasks,b.tasks);assertSame(task,b.tasks.tasks.first());assertFalse(b.tasks.visible) }
        close(a)
        main { assertEquals(FileTaskPhase.CONFIRMING,task.phase);b.tasks.show();b.tasks.confirm(task,task.decisions!!.reference) }
        await { !work.tasks.busy }
        assertEquals(FileCopyState.COMPLETE,main { task.result?.state })
        assertEquals("process-owned bytes ☃\n",File(root,"target/a.txt").readText())
        close(b);val c=launch()
        main { assertSame(task,c.tasks.tasks.first());assertSame(snapshot,c.files.clipboard);c.tasks.remove(task) }
        assertTrue(main { c.tasks.tasks.isEmpty() });assertTrue(File(root,"target/a.txt").isFile)
    }
    @Test fun closedOriginDoesNotCancelScanAndCancelledGlobalSlotWaitsForActualRelease() {
        val a=launch();val snapshot=source(a)
        val entered=CountDownLatch(1);val release=CountDownLatch(1);releases+=release
        main { a.tasks.start(snapshot,target(),"target",{ request ->
            entered.countDown();check(release.await(20,TimeUnit.SECONDS))
            FileTaskConnections(request.source.endpoint,request.destination.endpoint,{ _,_->error("local") },{ _,_,_->error("local") })
        }) }
        assertTrue(entered.await(5,TimeUnit.SECONDS));val task=main { work.tasks.tasks.first() }
        close(a);val b=launch()
        main {
            assertEquals(FileTaskPhase.SCANNING,task.phase);assertFalse(task.cancelled)
            b.tasks.start(snapshot,target("other"),"other");assertEquals(1,b.tasks.tasks.size);assertSame(task,b.tasks.tasks.first())
            b.tasks.cancel(task);assertEquals(FileTaskPhase.CANCELLING,task.phase);assertTrue(b.tasks.busy)
            b.tasks.remove(task);assertTrue(task in b.tasks.tasks)
            b.tasks.start(snapshot,target("other"),"other");assertEquals(1,b.tasks.tasks.size)
        }
        release.countDown();await { !work.tasks.busy };assertFalse(File(root,"target/a.txt").exists())
        main { b.tasks.start(snapshot,target("other"),"other") }
        await { work.tasks.tasks.first().phase==FileTaskPhase.CONFIRMING }
        main { val next=b.tasks.tasks.first();assertNotEquals(task.request.id,next.request.id);b.tasks.confirm(next,next.decisions!!.reference) }
        await { !work.tasks.busy };assertEquals("process-owned bytes ☃\n",File(root,"other/a.txt").readText())
    }
    @Test fun completionRefreshesEveryLiveBrowserWithoutOwningItsViewModel() {
        val a=launch();val snapshot=source(a);browse(a,"$unique/target")
        val b=launch();browse(b,"$unique/target")
        main { a.tasks.start(snapshot,target(),"target") };await { work.tasks.tasks.first().phase==FileTaskPhase.CONFIRMING }
        main { val task=b.tasks.tasks.first();b.tasks.confirm(task,task.decisions!!.reference) }
        await("both browser subscribers observe actual result") {
            !work.tasks.busy && !a.files.loading && !b.files.loading &&
                a.files.entries.any { it.name=="a.txt" } && b.files.entries.any { it.name=="a.txt" }
        }
        assertEquals("process-owned bytes ☃\n",File(root,"target/a.txt").readText())
    }
    @Test fun backgroundWithdrawsAuthenticationPresenterWithoutCancellingPendingTask() {
        val a=launch();val snapshot=source(a)
        main { a.tasks.start(snapshot,target(),"target") };await { work.tasks.tasks.first().phase==FileTaskPhase.CONFIRMING }
        val task=main { work.tasks.tasks.first() }
        assertTrue(instrumentation.uiAutomation.performGlobalAction(AccessibilityService.GLOBAL_ACTION_HOME))
        await("Activity really stopped in background") { a.activity.lifecycle.currentState==Lifecycle.State.CREATED }
        main { assertNull(work.presenter);assertFalse(a.tasks.presentsAuthentication);assertEquals(FileTaskPhase.CONFIRMING,task.phase);assertFalse(task.cancelled) }
        val b=launch()
        main { assertSame(task,b.tasks.tasks.first());assertTrue(b.tasks.presentsAuthentication);assertFalse(a.tasks.presentsAuthentication) }
        close(a)
        main { assertEquals(FileTaskPhase.CONFIRMING,task.phase);b.tasks.cancel(task) }
        assertFalse(File(root,"target/a.txt").exists())
    }
    private fun configureRemote() {
        configBefore=config.takeIf(File::exists)?.readBytes();knownBefore=known.takeIf(File::exists)?.readBytes()
        oldIds=vault.list().map { it.id }.toSet();remoteConfigured=true
        val material=fixture.getString("taskFilePem").toByteArray()
        try { provider("configure",material);key=vault.import(unique,material) } finally { material.fill(0) }
        config.parentFile!!.mkdirs();known.delete()
        config.writeText("Host $unique\n HostName 127.0.0.1\n Port ${fixture.getInt("port")}\n User ${fixture.getString("user")}\n IdentityFile ghosterm-identity:${key.id}\n")
    }
    private fun authenticate(model:SessionsViewModel,done:()->Boolean) {
        val replied=mutableSetOf<UUID>();val end=SystemClock.elapsedRealtime()+20000
        while(SystemClock.elapsedRealtime()<end && !main(done)) {
            main {
                model.connecting?.let { attempt ->
                    assertFalse("authentication failed: ${attempt.message}",attempt.failed)
                    attempt.auth?.let { prompt -> when(prompt.phase) {
                        SSHAuthPhase.HOSTKEY -> model.decide(attempt.id,prompt,true)
                        SSHAuthPhase.CREDENTIALS -> if(!attempt.credentialBusy && attempt.identities.any { it.id==key.id } && replied.add(attempt.id))
                            model.useIdentity(attempt.id,prompt,key,ByteArray(0))
                        else -> Unit
                    } }
                }
            };SystemClock.sleep(40)
        }
        assertTrue("authentication did not complete",main(done))
    }
    private fun startRemote(a:Holder):FileTask {
        configureRemote();val repo=ConfigRepository(config.parentFile!!);val prepared=repo.prepareConnection(repo.load(),unique)
        main {
            a.files.enter();val token=a.files.beginRemoteConnection()
            a.sessions.connectFiles(prepared) { connection,user ->
                a.files.remoteConnected(RemoteFileIdentity(prepared.alias,prepared.host,user,prepared.port),connection,token)
            }
        }
        authenticate(a.sessions) { a.sessions.connecting==null && !a.files.loading && a.files.location.endpoint.remote!=null }
        main { assertNull(a.files.error);a.files.longPress(a.files.entries.single { it.name=="文本.txt" });a.files.copySelected()
            a.tasks.start(a.files.clipboard!!,target(),"target") }
        await("task credentials") { work.authentication.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        return main { work.tasks.tasks.first() }
    }
    @Test fun independentTaskAuthenticationTransfersToAnotherActivityAfterOriginFinishes() {
        val a=launch();val task=startRemote(a);val attempt=main { work.authentication.connecting!!.id }
        val b=launch()
        main { assertSame(a.tasks.authentication,b.tasks.authentication);assertEquals(attempt,b.tasks.authentication.connecting!!.id) }
        close(a)
        systemAwait { nodes().any { it.isVisibleToUser && it.text?.toString()=="复制任务 · 读取来源" } }
        authenticate(b.tasks.authentication) { task.phase!=FileTaskPhase.SCANNING }
        main { assertEquals(FileTaskPhase.CONFIRMING,task.phase);b.tasks.show();b.tasks.confirm(task,task.decisions!!.reference) }
        await { work.authentication.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
        val second=main { work.authentication.connecting!!.id };assertNotEquals(attempt,second)
        val c=launch();close(b)
        systemAwait { nodes().any { it.isVisibleToUser && it.text?.toString()=="复制任务 · 读取来源" } }
        authenticate(c.tasks.authentication) { !work.tasks.busy }
        assertEquals(FileCopyState.COMPLETE,main { task.result?.state })
        assertEquals("远端 UTF-8\nline two\n",File(root,"target/文本.txt").readText())
        assertNull(main { c.sessions.connecting });assertTrue(main { c.sessions.sessions.isEmpty() })
    }
    @Test fun closedPresenterReleasesPickerAndItsLateResultCannotConsumeAnotherPagesRequest() {
        val a=launch();val task=startRemote(a);val auth=main { work.authentication };val oldId=a.tasks.presenterId
        main { val attempt=auth.connecting!!;assertTrue(auth.beginIdentityImport(attempt.id,attempt.auth!!,oldId)) }
        val b=launch();close(a)
        main {
            assertFalse("closing picker host releases only its pending result",auth.identityPickerPending)
            val attempt=auth.connecting!!;assertTrue(auth.beginIdentityImport(attempt.id,attempt.auth!!,b.tasks.presenterId))
            auth.completeIdentityImport(null,oldId);assertTrue("late old Activity result must not consume the new picker",auth.identityPickerPending)
            auth.completeIdentityImport(null,b.tasks.presenterId);assertFalse(auth.identityPickerPending)
            assertEquals(FileTaskPhase.SCANNING,task.phase)
        }
        authenticate(auth) { task.phase!=FileTaskPhase.SCANNING }
        assertEquals(FileTaskPhase.CONFIRMING,main { task.phase })
    }
    @Test fun realTaskIdentityPickerSurvivesActivityRecreationAndAuthenticates() {
        val a=launch()
        rotationToRestore=a.activity.display?.rotation ?: 0
        autoRotation=Settings.System.getInt(context.contentResolver,Settings.System.ACCELEROMETER_ROTATION,0)==1
        assertTrue(instrumentation.uiAutomation.setRotation(UiAutomation.ROTATION_FREEZE_0))
        systemAwait { val bounds=Rect();instrumentation.uiAutomation.rootInActiveWindow?.getBoundsInScreen(bounds);bounds.height()>bounds.width() }
        a.refresh();val task=startRemote(a);val auth=main { work.authentication }
        val page=a.tasks;val owner=page.presenterId;val attempt=main { auth.connecting!!.id }
        systemAwait {
            if(click("导入密钥")) true else {
                nodes().firstOrNull { it.isScrollable }?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD);false
            }
        }
        systemAwait { nodes().any { it.packageName?.toString()?.contains("documentsui")==true } }
        assertTrue(main { auth.identityPickerPending })
        val activity=a.activity
        assertTrue(instrumentation.uiAutomation.setRotation(UiAutomation.ROTATION_FREEZE_90))
        systemAwait {
            val root=instrumentation.uiAutomation.rootInActiveWindow;val bounds=Rect();root?.getBoundsInScreen(bounds)
            root?.packageName?.toString()?.contains("documentsui")==true && bounds.width()>bounds.height()
        }
        systemAwait {
            val tree=nodes()
            val roots=tree.firstOrNull { it.isVisibleToUser && it.viewIdResourceName?.endsWith(":id/roots_list")==true }
            if(roots!=null) {
                val row=nodes(roots).firstOrNull { it.isVisibleToUser && it.text?.toString()=="Ghosterm Test Keys" }
                if(row!=null) {
                    var target:AccessibilityNodeInfo?=row
                    while(target!=null && !target.isClickable) target=target.parent
                    target?.performAction(AccessibilityNodeInfo.ACTION_CLICK)
                } else roots.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                false
            } else if(click("ssh-key.pem")) true else {
                if(tree.any { it.isVisibleToUser && it.text?.toString()=="Ghosterm Test Keys" })
                    tree.firstOrNull { it.isScrollable && it.viewIdResourceName?.endsWith(":id/dir_list")==true }?.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)
                else click("Show roots","Open navigation drawer","显示根目录","显示位置")
                false
            }
        }
        systemAwait { val tree=nodes();tree.any { it.isVisibleToUser && it.packageName?.toString()=="com.hanxi.ghosterm" } && tree.none { it.packageName?.toString()?.contains("documentsui")==true } }
        await("origin Activity recreated") { activity.isDestroyed };a.refresh()
        assertNotSame(activity,a.activity);assertSame(page,a.tasks);assertEquals(owner,a.tasks.presenterId)
        await("task picker result after recreation") { !auth.identityPickerPending && auth.connecting?.identities?.any { it.id!=key.id && it.id !in oldIds }==true }
        main { assertEquals(attempt,auth.connecting!!.id);assertSame(task,a.tasks.tasks.first()) }
        val imported=vault.list().single { it.id!=key.id && it.id !in oldIds }
        val actual=vault.unlock(imported);val expected=fixture.getString("taskFilePem").toByteArray()
        try { assertTrue(expected.contentEquals(actual)) } finally { expected.fill(0);actual.fill(0) }
        key=imported
        authenticate(auth) { task.phase!=FileTaskPhase.SCANNING }
        main { assertEquals(FileTaskPhase.CONFIRMING,task.phase);a.tasks.confirm(task,task.decisions!!.reference) }
        authenticate(auth) { !work.tasks.busy }
        assertEquals(FileCopyState.COMPLETE,main { task.result?.state })
        assertEquals("远端 UTF-8\nline two\n",File(root,"target/文本.txt").readText())
        assertEquals(1,provider("status").getInt("reads"));assertEquals(0,provider("status").getInt("writes"))
    }
    private fun click(vararg labels:String):Boolean {
        for(match in nodes().filter { it.isVisibleToUser && (it.text?.toString() in labels || it.contentDescription?.toString() in labels) }) {
            var target:AccessibilityNodeInfo?=match
            while(target!=null) { if(target.isClickable && target.performAction(AccessibilityNodeInfo.ACTION_CLICK))return true;target=target.parent }
        }
        return false
    }
    private fun provider(method:String,bytes:ByteArray?=null):Bundle=checkNotNull(context.contentResolver.call(
        Uri.parse("content://com.hanxi.ghosterm.test.keycontrol"),"fixture-$method",null,Bundle().apply { if(bytes!=null)putByteArray("material",bytes) }))
    private fun nodes(root:AccessibilityNodeInfo?=instrumentation.uiAutomation.rootInActiveWindow):List<AccessibilityNodeInfo> {
        val result=mutableListOf<AccessibilityNodeInfo>()
        fun visit(node:AccessibilityNodeInfo?) { if(node==null)return;result+=node;for(i in 0 until node.childCount)visit(node.getChild(i)) }
        visit(root);return result
    }
    private fun systemAwait(condition:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+15000
        while(SystemClock.elapsedRealtime()<end) { if(condition())return;SystemClock.sleep(50) }
        fail("UI state did not arrive: "+nodes().map { it.text to it.contentDescription })
    }
}
