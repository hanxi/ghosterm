package com.hanxi.ghosterm

import android.app.Activity
import android.app.Application
import android.os.Bundle
import android.os.SystemClock
import androidx.lifecycle.Lifecycle
import android.app.ActivityManager
import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.os.Handler
import android.os.Looper
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import android.content.pm.ServiceInfo
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.data.LocalFileBrowser
import com.hanxi.ghosterm.domain.*
import kotlinx.coroutines.runBlocking
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

/** Actual Android service promotion over the production Application and queue. */
@RunWith(AndroidJUnit4::class)
class FileForegroundServiceTest {
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private var activity:MainActivity?=null
    private var taskId:Int?=null
    private lateinit var model:FileTasksViewModel
    private val holder=UUID.randomUUID().toString()
    private val holderExtra="com.hanxi.ghosterm.test.FOREGROUND_HOLDER"
    private val context get()=InstrumentationRegistry.getInstrumentation().targetContext
    private val application get()=context.applicationContext as GhostermApplication
    private val lifecycle=object:Application.ActivityLifecycleCallbacks {
        override fun onActivityCreated(value:Activity,state:Bundle?) {
            if(value is MainActivity && (value.intent.getStringExtra(holderExtra)==holder || value.taskId==taskId)) {
                activity=value;taskId=value.taskId;model=ViewModelProvider(value)[FileTasksViewModel::class.java]
            }
        }
        override fun onActivityStarted(value:Activity) {}
        override fun onActivityResumed(value:Activity) {}
        override fun onActivityPaused(value:Activity) {}
        override fun onActivityStopped(value:Activity) {}
        override fun onActivitySaveInstanceState(value:Activity,state:Bundle) {}
        override fun onActivityDestroyed(value:Activity) {}
    }
    private fun <T> main(block:()->T):T {
        var result:T?=null;var failure:Throwable?=null
        instrumentation.runOnMainSync { try { result=block() } catch(error:Throwable) { failure=error } }
        failure?.let { throw it }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun waitFor(timeout:Long=15000,message:String="expected foreground state",check:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+timeout
        while(SystemClock.elapsedRealtime()<end) { if(main(check)) return;SystemClock.sleep(30) }
        fail("$message; state="+main { if(::model.isInitialized) model.background.state else null }+"; tasks="+main { own.map { it.phase } })
    }
    private val component get()=ComponentName(context,FileTransferService::class.java)
    private val unique="foreground-${UUID.randomUUID()}"
    private val root get()=File(context.filesDir,"home/$unique")
    private lateinit var snapshot: FileClipboardSnapshot
    private val own=mutableListOf<FileTask>()
    private var clipboardId:String?=null
    private var systemProvider=false
    private val systemArgs get()=InstrumentationRegistry.getArguments()
    private fun provider(action:String="state",mode:String="normal")=checkNotNull(context.contentResolver.call(
        android.net.Uri.parse("content://com.hanxi.ghosterm.test.publicationcontrol"),action,null,
        Bundle().apply { putString("mode",mode) }))
    private fun systemMarker(token:String,phase:String,value:org.json.JSONObject) {
        require(token.matches(Regex("[a-z0-9-]{1,60}")))
        val file=File(context.cacheDir,"foreground-system-$token-$phase.json")
        val temporary=File(file.path+".new")
        java.io.FileOutputStream(temporary).use { it.write(value.toString().toByteArray());it.fd.sync() }
        check(temporary.renameTo(file))
    }
    @Suppress("DEPRECATION")
    private fun servicePresent()=context.getSystemService(ActivityManager::class.java).getRunningServices(100).any { it.service==component }

    @Before fun setup() {
        main {
            application.registerActivityLifecycleCallbacks(lifecycle)
            context.startActivity(Intent(context,MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_MULTIPLE_TASK).putExtra(holderExtra,holder))
        }
        waitFor(message="real Activity must resume") { activity?.lifecycle?.currentState==Lifecycle.State.RESUMED }
        check(File(root,"source").mkdirs());check(File(root,"target").mkdir())
        File(root,"source/a.txt").writeText("foreground actual bytes")
        val source=FileLocation(FileEndpoint.APP,"$unique/source")
        snapshot=runBlocking { FileClipboardSnapshot.capture(source,"source",
            LocalFileBrowser(File(context.filesDir,"home"),context.contentResolver).browse(source).entries) }
        waitFor(10000) { !model.journalLoading }
    }
    @After fun cleanup() {
        if(systemProvider) provider("release")
        context.packageManager.setComponentEnabledSetting(component,PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,PackageManager.DONT_KILL_APP)
        main {
            own.forEach(model::cancel)
            if(clipboardId!=null && model.work.clipboard.snapshot?.id==clipboardId) model.work.clipboard.snapshot=null
        }
        waitFor(15000) { own.none { it.active || it.recording } }
        main { own.forEach(model::remove) }
        waitFor(15000) { own.none { it in model.tasks } }
        // Cleanup must survive mutants that deliberately break autonomous shutdown.
        context.stopService(Intent(context,FileTransferService::class.java))
        waitFor(10000) { !running() };root.deleteRecursively()
        val closing=main { activity?.also { it.finishAndRemoveTask() } }
        try { if(closing!=null) waitFor(message="real Activity must be destroyed") { closing.isDestroyed } }
        finally { main { application.unregisterActivityLifecycleCallbacks(lifecycle) } }
    }
    @Suppress("DEPRECATION")
    private fun running()=context.getSystemService(ActivityManager::class.java).getRunningServices(100)
        .any { it.service==component && it.foreground }
    private fun begin(checkStarting:Boolean=false):FileTask=main {
        val before=model.tasks.map { it.request.id }.toSet()
        model.startUserTask(snapshot,FileLocation(FileEndpoint.APP,"$unique/target"),"target")
        val captured=model.tasks.single { it.request.id !in before }.also { own+=it }
        if(checkStarting) assertEquals("request is not actual promotion",FileBackgroundState.STARTING,model.background.state)
        captured
    }
    private fun prepared(task: FileTask) {
        waitFor(15000) { task.phase==FileTaskPhase.CONFIRMING }
        assertEquals(FileBackgroundState.ACTIVE,model.background.state)
        assertTrue("actual Android service must be foreground",running())
    }
    private fun command(generation: String?, id: String?) {
        val delivered=CountDownLatch(1)
        context.sendOrderedBroadcast(Intent(context,FileTransferActions::class.java).setAction(FileTransferService.CANCEL)
            .putExtra(FileTransferService.GENERATION,generation).putExtra(FileTransferService.TASK,id),null,
            object:BroadcastReceiver() { override fun onReceive(context:Context,intent:Intent) { delivered.countDown() } },
            Handler(Looper.getMainLooper()),Activity.RESULT_OK,null,null)
        assertTrue("actual cancel broadcast must finish delivery",delivered.await(15,TimeUnit.SECONDS))
    }
    private fun send(action:PendingIntent) {
        val delivered=CountDownLatch(1)
        action.send(context,0,null,{ _,_,_,_,_ -> delivered.countDown() },Handler(Looper.getMainLooper()))
        assertTrue("actual PendingIntent must finish delivery",delivered.await(15,TimeUnit.SECONDS))
    }
    private fun await(message: String, check: () -> Boolean) {
        waitFor(message=message,check=check)
    }
    private fun awaitText(text:String) {
        fun contains(node:android.view.accessibility.AccessibilityNodeInfo?):Boolean {
            if(node==null) return false
            if(node.text?.toString()==text && node.isVisibleToUser) return true
            return (0 until node.childCount).any { contains(node.getChild(it)) }
        }
        val end=SystemClock.elapsedRealtime()+15000
        while(SystemClock.elapsedRealtime()<end) {
            if(contains(instrumentation.uiAutomation.rootInActiveWindow)) return
            SystemClock.sleep(30)
        }
        fail("actual visible page must contain: $text")
    }
    private fun tryClick(vararg names:String):Boolean {
        val matches=mutableListOf<android.view.accessibility.AccessibilityNodeInfo>()
        fun collect(node:android.view.accessibility.AccessibilityNodeInfo?) {
            if(node==null) return
            if(node.isVisibleToUser && (node.text?.toString() in names || node.contentDescription?.toString() in names)) matches+=node
            for(i in 0 until node.childCount) collect(node.getChild(i))
        }
        collect(instrumentation.uiAutomation.rootInActiveWindow)
        for(found in matches) {
            var node:android.view.accessibility.AccessibilityNodeInfo?=found
            while(node!=null) {
                if(node.isEnabled && node.isClickable && node.performAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_CLICK)) return true
                node=node.parent
            }
        }
        return false
    }
    private fun uiWait(message:String,check:()->Boolean) {
        val end=SystemClock.elapsedRealtime()+15000
        while(SystemClock.elapsedRealtime()<end) { if(check()) return;SystemClock.sleep(30) }
        fail(message)
    }
    private fun clickText(vararg names:String)=uiWait("actual enabled control must be clickable: ${names.toList()}") { tryClick(*names) }
    private fun systemDestination():FileLocation {
        systemProvider=true
        provider("reset")
        val files=main { ViewModelProvider(checkNotNull(activity))[FilesViewModel::class.java] }
        main { files.enter();files.chooseLocations() }
        clickText("选择设备文件夹")
        // Opening the drawer works even when DocumentsUI remembers a different provider.
        clickText("Show roots","Open navigation drawer","显示根目录","显示位置")
        clickText("Ghosterm Copy Destination")
        clickText("Use this folder","USE THIS FOLDER","使用此文件夹")
        uiWait("real SAF grant must be confirmed") { tryClick("Allow","ALLOW","允许") || main { files.location.endpoint.treeUri!=null } }
        waitFor { !files.loading && files.location.endpoint.treeUri!=null && !files.choosingLocation && activity?.lifecycle?.currentState==Lifecycle.State.RESUMED }
        val destination=main { files.location }
        assertEquals("com.hanxi.ghosterm.test.publication",android.net.Uri.parse(destination.endpoint.treeUri).authority)
        assertTrue(context.contentResolver.persistedUriPermissions.any { it.uri.toString()==destination.endpoint.treeUri && it.isReadPermission && it.isWritePermission })
        return destination
    }
    private fun channelSettings() {
        context.startActivity(Intent(android.provider.Settings.ACTION_CHANNEL_NOTIFICATION_SETTINGS)
            .putExtra(android.provider.Settings.EXTRA_APP_PACKAGE,context.packageName)
            .putExtra(android.provider.Settings.EXTRA_CHANNEL_ID,FileTransferService.CHANNEL)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        uiWait("real channel settings must show its notification switch") {
            fun switch(node:android.view.accessibility.AccessibilityNodeInfo?):Boolean {
                if(node==null) return false
                if(node.isVisibleToUser && node.isCheckable && node.className?.toString()?.endsWith("Switch")==true) return true
                return (0 until node.childCount).any { switch(node.getChild(it)) }
            }
            switch(instrumentation.uiAutomation.rootInActiveWindow)
        }
    }
    private fun toggleChannel(enabled:Boolean) {
        val manager=context.getSystemService(NotificationManager::class.java)
        fun current()=manager.getNotificationChannel(FileTransferService.CHANNEL).importance!=NotificationManager.IMPORTANCE_NONE
        if(current()==enabled) return
        channelSettings()
        fun toggle(node:android.view.accessibility.AccessibilityNodeInfo?):Boolean {
            if(node==null) return false
            if(node.isVisibleToUser && node.isCheckable && node.className?.toString()?.endsWith("Switch")==true && node.isChecked!=enabled) {
                var target:android.view.accessibility.AccessibilityNodeInfo?=node
                while(target!=null) {
                    if(target.isClickable && target.performAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_CLICK)) return true
                    target=target.parent
                }
            }
            return (0 until node.childCount).any { toggle(node.getChild(it)) }
        }
        uiWait("real Settings switch must be clickable") { toggle(instrumentation.uiAutomation.rootInActiveWindow) }
        uiWait("Android must persist the actual channel state") { current()==enabled }
        // Restore the existing task rather than creating a second execution owner.
        main {
            val ownTask=context.getSystemService(ActivityManager::class.java).appTasks.single { it.taskInfo.taskId==taskId }
            ownTask.moveToFront()
        }
        waitFor { activity?.lifecycle?.currentState==Lifecycle.State.RESUMED }
    }
    private fun closedChannelProbe() {
        val manager=context.getSystemService(NotificationManager::class.java)
        assertEquals(PackageManager.PERMISSION_GRANTED,context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS))
        assertTrue(manager.areNotificationsEnabled())
        val original=checkNotNull(manager.getNotificationChannel(FileTransferService.CHANNEL)).importance
        assertNotEquals("channel fixture must start enabled",NotificationManager.IMPORTANCE_NONE,original)
        try {
            toggleChannel(false)
            assertTrue("app notifications remain granted",manager.areNotificationsEnabled())
            assertEquals(NotificationManager.IMPORTANCE_NONE,manager.getNotificationChannel(FileTransferService.CHANNEL).importance)
            val task=begin();prepared(task)
            assertFalse("closed channel must be visible independently of permission",model.background.notificationsVisible)
            awaitText("后台传输已启动");awaitText("通知已关闭，可在任务页查看状态和取消任务。")
            val generation=model.background.generation
            toggleChannel(true)
            waitFor { model.background.notificationsVisible }
            await("reopening the channel must repost the same task notification") {
                notification()?.extras?.getCharSequence(Notification.EXTRA_TEXT)?.toString()=="等待在任务页面确认"
            }
            assertEquals(generation,model.background.generation)
            assertSame(task,model.tasks.single { it.request.id==task.request.id })
            main { model.confirm(task,task.decisions!!.reference) }
            waitFor { task.phase==FileTaskPhase.FINISHED && !servicePresent() }
            assertEquals("foreground actual bytes",File(root,"target/a.txt").readText())
            assertEquals(FileCopyState.COMPLETE,task.result?.state)
        } finally {
            toggleChannel(true)
            assertEquals("user channel importance must be restored",original,manager.getNotificationChannel(FileTransferService.CHANNEL).importance)
        }
        instrumentation.sendStatus(0,Bundle().apply { putString("stream","FOREGROUND_SYSTEM_VERIFIED mode=channel-closed\n") })
    }
    private fun restoredNotificationsProbe() {
        val manager=context.getSystemService(NotificationManager::class.java)
        assertFalse("host must revoke before starting instrumentation",manager.areNotificationsEnabled())
        val task=begin();prepared(task)
        assertFalse(model.background.notificationsVisible)
        assertNull("permission-denied service has no posted notification",notification())
        val generation=model.background.generation
        clickText("通知设置")
        clickText("All Ghosterm notifications","允许通知","允许通知消息")
        uiWait("system settings must actually grant app notifications") { manager.areNotificationsEnabled() }
        clickText("Navigate up","向上导航")
        waitFor(message="same Activity must resume with actual permission visible") {
            activity?.lifecycle?.currentState==Lifecycle.State.RESUMED && model.background.notificationsVisible
        }
        assertEquals(generation,model.background.generation);assertTrue(running())
        assertSame(task,model.tasks.single { it.request.id==task.request.id })
        assertEquals(FileTaskPhase.CONFIRMING,task.phase)
        await("returning from Settings must repost the existing task notification") {
            notification()?.extras?.getCharSequence(Notification.EXTRA_TEXT)?.toString()=="等待在任务页面确认"
        }
        main { model.hide() };awaitText("主机")
        send(notification()!!.contentIntent);awaitText("文件任务")
        assertSame(task,model.tasks.single { it.request.id==task.request.id });assertEquals(generation,model.background.generation)
        send(notification()!!.actions.single().actionIntent)
        await("restored notification must cancel its original request") { task.phase==FileTaskPhase.FINISHED && !running() }
        assertTrue(task.cancelled);assertFalse(File(root,"target/a.txt").exists())
        instrumentation.sendStatus(0,Bundle().apply { putString("stream","FOREGROUND_SYSTEM_VERIFIED mode=notifications-restored\n") })
    }
    private fun systemProbe(mode:String) {
        require(mode in listOf("timeout","stop-app","notifications-denied","channel-closed","notifications-restored"))
        if(mode=="channel-closed") { closedChannelProbe();return }
        if(mode=="notifications-restored") { restoredNotificationsProbe();return }
        if(mode=="notifications-denied") {
            assertEquals(PackageManager.PERMISSION_DENIED,context.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS))
            assertFalse(context.getSystemService(NotificationManager::class.java).areNotificationsEnabled())
            val task=begin();prepared(task)
            assertFalse("notification visibility is separate from foreground eligibility",model.background.notificationsVisible)
            awaitText("后台传输已启动")
            awaitText("通知已关闭，可在任务页查看状态和取消任务。")
            awaitText("通知设置")
            main { model.confirm(task,task.decisions!!.reference) }
            waitFor { task.phase==FileTaskPhase.FINISHED && !servicePresent() }
            assertEquals("foreground actual bytes",File(root,"target/a.txt").readText())
            assertEquals(FileCopyState.COMPLETE,task.result?.state)
            instrumentation.sendStatus(0,Bundle().apply { putString("stream","FOREGROUND_SYSTEM_VERIFIED mode=notifications-denied\n") })
            return
        }
        val token=checkNotNull(systemArgs.getString("foregroundSystemToken"))
        if(systemArgs.getString("foregroundSystemPhase")=="verify") {
            require(mode=="stop-app")
            val previous=org.json.JSONObject(File(context.cacheDir,"foreground-system-$token-ready.json").readText())
            assertNotEquals(previous.getInt("pid"),android.os.Process.myPid())
            assertTrue(model.tasks.isEmpty());assertFalse(servicePresent())
            val record=main { model.history.single { it.intent.id==previous.getString("request") } }
            assertTrue(record.interrupted);assertEquals(FileJournalPhase.RUNNING,record.phase)
            assertTrue(record.items.any { it.checkpoint==FileCopyCheckpoint.DOCUMENT_CREATE })
            assertEquals(previous.getString("document"),provider().getString("latest"))
            assertEquals(1,provider().getInt("creates"));assertEquals(0,provider().getInt("writes"))
            assertArrayEquals(byteArrayOf(),provider().getByteArray("bytes"))
            val old=File(context.filesDir,"home/"+previous.getString("fixture"))
            require(old.parentFile==File(context.filesDir,"home") && old.name.matches(Regex("foreground-[a-f0-9-]{36}")))
            assertEquals("foreground actual bytes",File(old,"source/a.txt").readText())
            systemProvider=true;provider("release")
            main { model.removeHistory(record) };waitFor { model.history.none { it.intent.id==record.intent.id } && !model.journalLoading }
            assertArrayEquals(byteArrayOf(),provider().getByteArray("bytes"));assertEquals(0,provider().getInt("deletes"))
            assertFalse(servicePresent());assertTrue(model.tasks.isEmpty());old.deleteRecursively()
            instrumentation.sendStatus(0,Bundle().apply { putString("stream","FOREGROUND_SYSTEM_VERIFIED mode=stop-app oldPid=${previous.getInt("pid")} newPid=${android.os.Process.myPid()}\n") })
            return
        }
        val destination=systemDestination()
        provider("reset","foreground-late-create")
        val task=main {
            val before=model.tasks.map { it.request.id }.toSet()
            model.startUserTask(snapshot,destination,"SAF")
            model.tasks.single { it.request.id !in before }.also { own+=it }
        }
        prepared(task);main { model.confirm(task,task.decisions!!.reference) }
        uiWait("real document must exist while create reply is blocked") { provider().getString("latest")!=null && main { task.phase==FileTaskPhase.RUNNING } }
        assertTrue(running());assertEquals(0,provider().getInt("writes"))
        val ready=org.json.JSONObject().put("pid",android.os.Process.myPid()).put("request",task.request.id)
            .put("fixture",unique).put("document",provider().getString("latest")).put("mode",mode)
        systemMarker(token,"ready",ready)
        if(mode=="stop-app") {
            // The host must actually stop this process. Returning would falsely count a pass.
            SystemClock.sleep(45000);fail("host did not stop the foreground app")
        }
        waitFor(20000,"actual system onTimeout must run") { model.background.state==FileBackgroundState.TIMED_OUT }
        waitFor(2000,"service record must disappear without waiting for provider reply") { !servicePresent() }
        main { assertEquals(FileTaskPhase.CANCELLING,task.phase);assertTrue(model.work.tasks.busy);assertNotNull(model.work.tasks.backgroundHold) }
        assertEquals(0,provider().getInt("writes"));assertArrayEquals(byteArrayOf(),provider().getByteArray("bytes"))
        systemMarker(token,"timed-out",ready)
        uiWait("host must acknowledge the stopped service") { File(context.cacheDir,"foreground-system-$token-release").exists() }
        provider("release")
        waitFor { task.phase==FileTaskPhase.FINISHED && !task.recording }
        assertTrue(task.cancelled);assertNotEquals(FileCopyState.COMPLETE,task.result?.state)
        val actual=checkNotNull(task.result).items.single().documentUri
        assertEquals(ready.getString("document"),android.provider.DocumentsContract.getDocumentId(android.net.Uri.parse(actual)))
        assertEquals(0,provider().getInt("writes"));assertArrayEquals(byteArrayOf(),provider().getByteArray("bytes"))
        val record=runBlocking { com.hanxi.ghosterm.data.FileTaskJournal(File(context.filesDir,"file-history/tasks.db")).load().single { it.intent.id==task.request.id } }
        assertFalse(record.interrupted);assertEquals(actual,record.items.single().value.documentUri)
        assertEquals("foreground actual bytes",File(root,"source/a.txt").readText());assertFalse(servicePresent())
        instrumentation.sendStatus(0,Bundle().apply { putString("stream","FOREGROUND_SYSTEM_VERIFIED mode=timeout pid=${android.os.Process.myPid()}\n") })
    }
    @Test fun actualFilePagePasteStartsForegroundAndFreezesItsDestination() {
        val files=main { ViewModelProvider(checkNotNull(activity))[FilesViewModel::class.java] }
        main { assertNull(files.clipboard);files.enter() }
        fun folder(name:String) {
            waitFor { !files.loading }
            main { files.tap(files.visibleEntries.single { it.name==name }) }
            waitFor { !files.loading }
        }
        folder(unique);folder("source")
        main {
            files.longPress(files.visibleEntries.single { it.name=="a.txt" });files.copySelected()
            clipboardId=files.clipboard?.id
            assertNotNull(clipboardId)
            files.back()
        }
        folder("target")
        val previous=main { model.tasks.map { it.request.id }.toSet() }
        clickText("粘贴到当前目录")
        waitFor(message="actual paste must admit its task") {
            val found=model.tasks.firstOrNull { it.request.id !in previous }
            if(found!=null && own.none { it===found }) own+=found
            found!=null
        }
        val task=own.single();prepared(task);awaitText("文件任务")
        assertEquals(FileLocation(FileEndpoint.APP,"$unique/target"),task.request.destination)
        assertEquals(clipboardId,task.request.clipboardId)
        assertFalse(File(root,"target/a.txt").exists())
    }
    private fun notification():Notification?=context.getSystemService(NotificationManager::class.java).activeNotifications
        .singleOrNull { it.notification.channelId==FileTransferService.CHANNEL }?.notification
    @Test fun postedNotificationOpensTasksAndCancelsOnlyItsOriginalRequest() {
        val first=begin();prepared(first)
        await("notification must be posted with actual task status") {
            notification()?.extras?.getCharSequence(Notification.EXTRA_TEXT)?.toString()=="等待在任务页面确认"
        }
        val posted=notification()!!
        assertTrue(model.background.notificationsVisible)
        assertEquals("Ghosterm 文件任务",posted.extras.getCharSequence(Notification.EXTRA_TITLE).toString())
        val text=posted.extras.toString()
        assertFalse(text.contains(unique));assertFalse(text.contains(context.filesDir.path));assertFalse(text.contains("a.txt"))
        main { model.hide() };assertFalse(model.visible)
        send(posted.contentIntent)
        await("notification must open the existing task page") { model.visible }
        awaitText("文件任务")
        val original=main { checkNotNull(activity) }
        val originalPage=model;val generation=model.background.generation
        main { original.recreate() }
        waitFor(message="notification-opened Activity must recreate") {
            activity!==original && activity?.lifecycle?.currentState==Lifecycle.State.RESUMED
        }
        assertSame(originalPage,model);assertEquals(generation,model.background.generation)
        assertSame(first,model.tasks.single { it.request.id==first.request.id });assertTrue(running())
        awaitText("文件任务")
        main { model.hide() };awaitText("主机")
        val returned=main { checkNotNull(activity) }
        main { returned.recreate() }
        waitFor(message="home Activity must recreate") { activity!==returned && activity?.lifecycle?.currentState==Lifecycle.State.RESUMED }
        assertFalse("handled notification must not replay navigation after returning home",model.visible)
        awaitText("主机")
        val cancel=posted.actions.single().actionIntent
        assertEquals(context.packageName,cancel.creatorPackage)
        send(cancel)
        await("notification cancellation must finish its task") { first.phase==FileTaskPhase.FINISHED && model.background.state==FileBackgroundState.IDLE }
        assertTrue(first.cancelled);assertFalse(File(root,"target/a.txt").exists())
        val second=begin();prepared(second)
        send(cancel);main { }
        assertEquals(FileTaskPhase.CONFIRMING,second.phase);assertFalse(second.cancelled)
    }
    @Test fun finishingActivityKeepsForegroundRegistrationAndNotificationCanCancel() {
        val task=begin();prepared(task)
        await("actual notification must be available") { notification()?.actions?.isNotEmpty()==true }
        val cancel=notification()!!.actions.single().actionIntent
        val generation=model.background.generation
        val closing=main { checkNotNull(activity).also { it.finishAndRemoveTask() } }
        waitFor(message="Activity must finish independently of service") { closing.isDestroyed }
        assertEquals(generation,model.background.generation);assertEquals(FileTaskPhase.CONFIRMING,task.phase)
        assertTrue(running());assertSame(task,model.work.tasks.tasks.single { it.request.id==task.request.id })
        send(cancel)
        await("notification must cancel after Activity destruction") { task.phase==FileTaskPhase.FINISHED && !running() }
        assertTrue(task.cancelled);assertFalse(File(root,"target/a.txt").exists())
    }
    @Test fun actualServiceStopHoldsQueueBeforeCancellingAndNeverWritesWaitingTargets() {
        systemArgs.getString("foregroundSystemMode")?.let { systemProbe(it);return }

        val first=begin();prepared(first);val second=begin()
        assertTrue(context.stopService(Intent(context,FileTransferService::class.java)))
        await("service destruction must cancel and hold the queue") {
            model.background.state==FileBackgroundState.FAILED && own.all { it.phase==FileTaskPhase.FINISHED }
        }
        assertNotNull(model.work.tasks.backgroundHold)
        assertTrue(first.cancelled);assertTrue(second.cancelled)
        assertFalse(File(root,"target/a.txt").exists());assertFalse(running())
    }
    @Test fun promotedServiceCopiesRealBytesAndStopsOnlyAfterFinalResult() {
        val task=begin(checkStarting=true);prepared(task)
        val service=context.packageManager.getServiceInfo(component,0)
        assertFalse(service.exported)
        assertEquals(ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC,service.foregroundServiceType)
        assertFalse(context.packageManager.getReceiverInfo(ComponentName(context,FileTransferActions::class.java),0).exported)
        main { model.confirm(task,task.decisions!!.reference) }
        waitFor(15000) { task.phase==FileTaskPhase.FINISHED }
        assertEquals("foreground actual bytes",File(root,"target/a.txt").readText())
        assertNull(task.error);assertNull(task.persistenceError)
        waitFor(10000) { model.background.state==FileBackgroundState.IDLE && !running() }
    }
    @Test fun rejectedPlatformStartHoldsSameRequestUntilVisibleUserRetry() {
        context.packageManager.setComponentEnabledSetting(component,PackageManager.COMPONENT_ENABLED_STATE_DISABLED,PackageManager.DONT_KILL_APP)
        val task=begin()
        waitFor(10000) { model.background.state==FileBackgroundState.FAILED && !task.recording }
        main {
            assertEquals(FileTaskPhase.QUEUED,task.phase)
            assertNotNull(model.work.tasks.backgroundHold)
            assertFalse(File(root,"target/a.txt").exists());assertFalse(running())
        }
        context.packageManager.setComponentEnabledSetting(component,PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,PackageManager.DONT_KILL_APP)
        main { model.retryBackground() };prepared(task)
        assertEquals(1,own.size)
        main { model.confirm(task,task.decisions!!.reference) }
        waitFor(15000) { task.phase==FileTaskPhase.FINISHED }
        assertEquals("foreground actual bytes",File(root,"target/a.txt").readText())
    }
    @Test fun privateCancelReceiverUsesGenerationAndRequestIdentity() {
        val first=begin();prepared(first)
        val second=begin()
        command("old-registration",first.request.id)
        command(model.background.generation,"missing-request")
        main { assertEquals(FileTaskPhase.CONFIRMING,first.phase);assertEquals(FileTaskPhase.QUEUED,second.phase) }
        command(model.background.generation,first.request.id)
        waitFor(15000) { first.phase==FileTaskPhase.FINISHED && second.phase==FileTaskPhase.CONFIRMING }
        assertTrue(first.cancelled);assertFalse(second.cancelled);assertTrue(running())
        assertFalse(File(root,"target/a.txt").exists())
        command(model.background.generation,second.request.id)
        waitFor(15000) { second.phase==FileTaskPhase.FINISHED && !running() }
        assertTrue(second.cancelled)
    }
    @Test fun staleAndEmptyServiceIntentsCannotStopCurrentRegistrationOrReplayWork() {
        val task=begin();prepared(task)
        val generation=model.background.generation
        context.startService(Intent(context,FileTransferService::class.java))
        context.startService(Intent(context,FileTransferService::class.java).putExtra(FileTransferService.GENERATION,"stale"))
        main { }
        assertEquals(generation,model.background.generation);assertTrue(running())
        assertEquals(FileTaskPhase.CONFIRMING,task.phase)
        main { model.cancel(task) }
        waitFor(15000) { task.phase==FileTaskPhase.FINISHED && !running() && model.background.state==FileBackgroundState.IDLE }
        context.startService(Intent(context,FileTransferService::class.java))
        main { }
        assertEquals(FileTaskPhase.FINISHED,task.phase);assertTrue(task.cancelled)
        assertFalse(File(root,"target/a.txt").exists())
        assertNull(model.background.generation)
    }
}
