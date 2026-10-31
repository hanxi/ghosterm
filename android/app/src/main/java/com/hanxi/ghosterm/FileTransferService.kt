package com.hanxi.ghosterm

import android.app.*
import android.content.*
import android.content.pm.ServiceInfo
import android.os.IBinder
import androidx.compose.runtime.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.conflate
import java.util.UUID

enum class FileBackgroundState { IDLE, STARTING, ACTIVE, STOPPING, FAILED, TIMED_OUT }
internal data class FileNotificationSummary(val count: Int, val id: String?, val phase: FileTaskPhase?)

/** Owns only the service registration. FileTaskStore remains the sole execution owner. */
class FileTransferBackground(private val application: Application, private val work: FileWorkRuntime,
    scope: CoroutineScope) {
    val enabled = application is GhostermApplication
    var state by mutableStateOf(FileBackgroundState.IDLE); private set
    var error by mutableStateOf<String?>(null); private set
    var notificationsVisible by mutableStateOf(true); private set
    internal var generation: String? = null; private set
    private var binding: FileTransferService? = null
    init {
        if(enabled) scope.launch {
            snapshotFlow { summary() }.conflate().collect {
                if(state==FileBackgroundState.ACTIVE) render()
                delay(500)
            }
        }
    }
    private fun summary(): FileNotificationSummary {
        val active=work.tasks.tasks.filter { it.active || it.recording }
        val current=active.lastOrNull { it.phase!=FileTaskPhase.QUEUED } ?: active.lastOrNull()
        return FileNotificationSummary(active.size,current?.request?.id,current?.phase)
    }
    fun refreshNotifications() {
        if(!enabled) return
        val manager=application.getSystemService(NotificationManager::class.java)
        notificationsVisible=manager.areNotificationsEnabled() &&
            manager.getNotificationChannel(FileTransferService.CHANNEL)?.importance!=NotificationManager.IMPORTANCE_NONE
    }
    fun submitFromUser(presenter: UUID, submit: () -> Unit) {
        if(!enabled) { submit();return }
        if(work.presenter!=presenter) { error="请返回文件任务页面后重新操作";return }
        if(state==FileBackgroundState.ACTIVE) { submit();return }
        work.tasks.holdForBackground("正在启动后台传输")
        submit()
        if(work.tasks.busy) requestFromUser(presenter)
        else work.tasks.allowBackgroundScheduling()
    }
    fun requestFromUser(presenter: UUID) {
        if(!enabled || !work.tasks.busy || state==FileBackgroundState.ACTIVE || state==FileBackgroundState.STARTING) return
        if(work.presenter!=presenter) { error="请返回文件任务页面后重新操作";return }
        work.tasks.holdForBackground("正在启动后台传输")
        val token=UUID.randomUUID().toString()
        generation=token;binding=null;state=FileBackgroundState.STARTING;error=null
        try {
            // A malformed type/permission declaration can make Android kill even
            // a service stopped before promotion. Reject it before requesting FGS.
            val info=application.packageManager.getServiceInfo(ComponentName(application,FileTransferService::class.java),0)
            check(info.foregroundServiceType and ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC!=0)
            check(application.checkSelfPermission(android.Manifest.permission.FOREGROUND_SERVICE)==android.content.pm.PackageManager.PERMISSION_GRANTED)
            if(android.os.Build.VERSION.SDK_INT>=34)
                check(application.checkSelfPermission(android.Manifest.permission.FOREGROUND_SERVICE_DATA_SYNC)==android.content.pm.PackageManager.PERMISSION_GRANTED)
            checkNotNull(application.startForegroundService(Intent(application,FileTransferService::class.java)
                .putExtra(FileTransferService.GENERATION,token)))
        } catch(_: Exception) { failed(token) }
    }
    internal fun accepts(token: String?) = token!=null && token==generation &&
        (state==FileBackgroundState.STARTING || state==FileBackgroundState.ACTIVE)
    internal fun promoted(service: FileTransferService, token: String): Boolean {
        if(!accepts(token)) return false
        binding=service;state=FileBackgroundState.ACTIVE;error=null
        refreshNotifications();work.tasks.allowBackgroundScheduling();render()
        return true
    }
    internal fun failed(token: String) {
        if(token!=generation) return
        binding=null;generation=null;state=FileBackgroundState.FAILED
        error="系统未能启动后台传输，任务尚未继续。请重试或取消等待任务。"
        work.tasks.holdForBackground(error!!)
    }
    internal fun ended(service: FileTransferService, token: String?, timedOut: Boolean) {
        if(token==null || token!=generation || service!==binding) return
        generation=null;binding=null
        state=if(timedOut) FileBackgroundState.TIMED_OUT else FileBackgroundState.FAILED
        error=if(timedOut) "后台传输已达系统时限，正在取消并等待真实结果。" else "后台传输服务已停止，正在取消并等待真实结果。"
        work.tasks.stopForBackground(error!!)
    }
    internal fun destroyed(service: FileTransferService, token: String?) {
        if(state==FileBackgroundState.STOPPING && service===binding && generation==token) {
            binding=null;generation=null;state=FileBackgroundState.IDLE
        } else ended(service,token,false)
    }
    internal fun cancel(token: String?, id: String?) {
        if(accepts(token) && id!=null) work.tasks.cancelRequest(id)
    }
    private fun render() {
        val service=binding ?: return
        val token=generation ?: return
        val summary=summary()
        if(summary.count==0) {
            state=FileBackgroundState.STOPPING
            service.finishIdle()
        } else service.update(token,summary)
    }
}

/** Non-sticky foreground lifetime over the existing application queue; never replays intents. */
class FileTransferService : Service() {
    companion object {
        const val CHANNEL="file-transfers"
        const val GENERATION="generation"
        const val TASK="task"
        const val CANCEL="com.hanxi.ghosterm.CANCEL_FILE_TASK"
        const val OPEN_TASKS="com.hanxi.ghosterm.OPEN_FILE_TASKS"
        private const val NOTIFICATION=7101
    }
    private val owner get()=(application as GhostermApplication).fileWork.background
    private var token: String?=null
    private var lastStart=0
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Android counts even ignored intents when deciding stopSelfResult().
        lastStart=startId
        val requested=intent?.getStringExtra(GENERATION)
        if(!owner.accepts(requested)) {
            if(token==null) stopSelfResult(startId)
            return START_NOT_STICKY
        }
        token=requested
        try {
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL,"文件传输",NotificationManager.IMPORTANCE_LOW))
            startForeground(NOTIFICATION,notification(requested!!,null),ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
            if(!owner.promoted(this,requested)) finishIdle()
        } catch(_: Exception) {
            owner.failed(requested!!)
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
        return START_NOT_STICKY
    }
    internal fun finishIdle() {
        stopForeground(STOP_FOREGROUND_REMOVE);stopSelfResult(lastStart)
    }
    internal fun update(generation: String, summary: FileNotificationSummary) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION,notification(generation,summary))
    }
    private fun notification(generation: String, summary: FileNotificationSummary?): Notification {
        val open=PendingIntent.getActivity(this,0,Intent(this,MainActivity::class.java).setAction(OPEN_TASKS)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val text=when(summary?.phase) {
            FileTaskPhase.SCANNING -> "正在检查文件"
            FileTaskPhase.CONFIRMING -> "等待在任务页面确认"
            FileTaskPhase.RUNNING -> "正在复制文件"
            FileTaskPhase.CANCELLING -> "正在取消并等待结果"
            else -> "正在准备文件任务"
        }
        val builder=Notification.Builder(this,CHANNEL).setSmallIcon(R.drawable.ic_file_transfer)
            .setContentTitle("Ghosterm 文件任务").setContentText(text)
            .setSubText(summary?.let { "${it.count} 个待处理任务" }).setContentIntent(open)
            .setOngoing(true).setOnlyAlertOnce(true).setCategory(Notification.CATEGORY_PROGRESS)
        summary?.id?.let { id ->
            val cancel=PendingIntent.getBroadcast(this,0,Intent(this,FileTransferActions::class.java).setAction(CANCEL)
                .setData(android.net.Uri.parse("ghosterm://file-task/$generation/$id"))
                .putExtra(GENERATION,generation).putExtra(TASK,id),PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            builder.addAction(Notification.Action.Builder(null,"取消当前任务",cancel).build())
        }
        return builder.build()
    }
    override fun onTimeout(startId: Int, fgsType: Int) {
        owner.ended(this,token,true)
        stopForeground(STOP_FOREGROUND_REMOVE);stopSelf()
    }
    override fun onDestroy() { owner.destroyed(this,token);super.onDestroy() }
}

class FileTransferActions : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if(intent.action==FileTransferService.CANCEL) (context.applicationContext as GhostermApplication).fileWork.background
            .cancel(intent.getStringExtra(FileTransferService.GENERATION),intent.getStringExtra(FileTransferService.TASK))
    }
}
