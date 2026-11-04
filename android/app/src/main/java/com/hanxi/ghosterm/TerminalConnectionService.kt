package com.hanxi.ghosterm

import android.app.*
import android.content.*
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.compose.runtime.*
import kotlinx.coroutines.*
import java.util.UUID

/** Process-local ownership: Activity recreation/finish never disposes registered terminals. */
internal class TerminalSessionStore {
    val sessions = mutableStateListOf<TerminalSession>()
    var selected by mutableStateOf<UUID?>(null)
    var sequence = 0
}

enum class TerminalBackgroundState { IDLE, STARTING, ACTIVE, FAILED }

/** Holds registration and observes native state; terminal bytes remain on their native owners. */
class TerminalConnectionBackground internal constructor(
    private val application: Application, private val store: TerminalSessionStore,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    var state by mutableStateOf(TerminalBackgroundState.IDLE); private set
    var error by mutableStateOf<String?>(null); private set
    var keepAwake by mutableStateOf(false); private set
    var notificationsVisible by mutableStateOf(true); private set
    internal var generation: String? = null; private set
    private var binding: TerminalConnectionService? = null
    private var pendingModel: SessionsViewModel? = null
    private var lastSummary: Pair<Int, Boolean>? = null
    private fun pending() = pendingModel?.let { model -> model.creating ||
        model.connecting?.let { !it.forFiles && it.tmuxReady == null && !it.failed } == true } == true
    internal val count get() = store.sessions.count { !it.state.terminal }
    init {
        scope.launch {
            while (isActive) {
                store.sessions.toList().forEach { it.state = it.connection.state() }
                val waiting = pending()
                if (!waiting) pendingModel = null
                if (count == 0 && !waiting) finishIdle()
                else if (state == TerminalBackgroundState.ACTIVE) {
                    val summary = count to waiting
                    if (summary != lastSummary) {
                        lastSummary = summary
                        try { binding?.update(count, waiting, keepAwake) }
                        catch (_: Exception) { generation?.let { failed(it) } }
                    }
                }
                delay(500)
            }
        }
    }
    fun startFromUser(model: SessionsViewModel) {
        pendingModel = model
        if (state == TerminalBackgroundState.ACTIVE || state == TerminalBackgroundState.STARTING) return
        if (count == 0 && !pending()) return
        val token = UUID.randomUUID().toString()
        generation = token; state = TerminalBackgroundState.STARTING; error = null; lastSummary = null
        try {
            val info = application.packageManager.getServiceInfo(ComponentName(application, TerminalConnectionService::class.java), 0)
            check(application.checkSelfPermission(android.Manifest.permission.FOREGROUND_SERVICE) == android.content.pm.PackageManager.PERMISSION_GRANTED)
            if (Build.VERSION.SDK_INT >= 34) {
                check(info.foregroundServiceType and ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE != 0)
                check(application.checkSelfPermission(android.Manifest.permission.FOREGROUND_SERVICE_SPECIAL_USE) == android.content.pm.PackageManager.PERMISSION_GRANTED)
            }
            checkNotNull(application.startForegroundService(Intent(application, TerminalConnectionService::class.java)
                .putExtra(TerminalConnectionService.GENERATION, token)))
        } catch (_: Exception) { failed(token) }
    }
    internal fun detached(model: SessionsViewModel) { if (pendingModel === model) pendingModel = null }
    internal fun accepts(token: String?) = token != null && generation == token &&
        state in listOf(TerminalBackgroundState.STARTING, TerminalBackgroundState.ACTIVE)
    internal fun promoted(service: TerminalConnectionService, token: String): Boolean {
        if (!accepts(token)) return false
        binding = service; state = TerminalBackgroundState.ACTIVE; error = null
        refreshNotifications()
        lastSummary = count to pending()
        service.update(count, pending(), keepAwake)
        return true
    }
    internal fun failed(token: String) {
        if (generation != token) return
        val service = binding
        binding = null; generation = null; state = TerminalBackgroundState.FAILED; keepAwake = false
        error = "后台连接保护未能启动。连接仍可在前台使用，请从更多菜单重试后台保护。"
        service?.finishIdle()
    }
    internal fun destroyed(service: TerminalConnectionService, token: String?) {
        if (binding !== service || generation != token) return
        binding = null; generation = null; state = TerminalBackgroundState.FAILED; keepAwake = false
        error = "后台连接保护已停止，请返回终端检查连接状态。"
    }
    fun setKeepAwakeFromUser(value: Boolean) {
        val service = binding ?: return
        if (state != TerminalBackgroundState.ACTIVE) return
        try {
            service.setAwake(value)
            keepAwake = value; error = null
            service.update(count, pending(), keepAwake)
        } catch (_: Exception) {
            keepAwake = false; service.setAwake(false)
            error = "无法保持锁屏唤醒，后台连接仍可能受系统休眠影响。"
        }
    }
    fun dismissError() { error = null }
    fun refreshNotifications() {
        val manager = application.getSystemService(NotificationManager::class.java)
        val previous = notificationsVisible
        notificationsVisible = manager.areNotificationsEnabled() &&
            manager.getNotificationChannel(TerminalConnectionService.CHANNEL)?.importance != NotificationManager.IMPORTANCE_NONE
        if (!previous && notificationsVisible) {
            try { binding?.update(count, pending(), keepAwake) }
            catch (_: Exception) { generation?.let { failed(it) } }
        }
    }
    private fun finishIdle() {
        if (state == TerminalBackgroundState.IDLE) return
        val service = binding
        binding = null; generation = null; keepAwake = false; lastSummary = null
        state = TerminalBackgroundState.IDLE; error = null
        service?.finishIdle()
    }
}

/** Non-sticky: process death never replays credentials, commands or connections. */
class TerminalConnectionService : Service() {
    private val owner get() = (application as GhostermApplication).terminalBackground
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var token: String? = null
    private var lastStart = 0
    private var wake: PowerManager.WakeLock? = null
    private var renew: Job? = null
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        lastStart = startId
        val requested = intent?.getStringExtra(GENERATION)
        if (!owner.accepts(requested)) { if (token == null) stopSelfResult(startId); return START_NOT_STICKY }
        token = requested
        try {
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL, "终端连接", NotificationManager.IMPORTANCE_LOW))
            val notification = notification(0, true, false)
            if (Build.VERSION.SDK_INT >= 34) startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
            else startForeground(NOTIFICATION, notification)
            if (!owner.promoted(this, requested!!)) finishIdle()
        } catch (_: Exception) { owner.failed(requested!!); finishIdle() }
        return START_NOT_STICKY
    }
    internal fun setAwake(enabled: Boolean) {
        renew?.cancel(); renew = null
        if (!enabled) { wake?.let { if (it.isHeld) it.release() }; wake = null; return }
        val lock = wake ?: getSystemService(PowerManager::class.java).newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK, "Ghosterm:terminal-connections").also { it.setReferenceCounted(false); wake = it }
        lock.acquire(120_000)
        renew = scope.launch {
            while (isActive) { delay(60_000); if (owner.count > 0) lock.acquire(120_000) }
        }
    }
    internal fun update(count: Int, waiting: Boolean, awake: Boolean) {
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION, notification(count, waiting, awake))
    }
    private fun notification(count: Int, waiting: Boolean, awake: Boolean): Notification {
        val open = PendingIntent.getActivity(this, 1, Intent(this, MainActivity::class.java).setAction(OPEN_TERMINALS)
            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL).setSmallIcon(R.drawable.ic_gt_terminal)
            .setContentTitle("Ghosterm 终端连接")
            .setContentText(if (count == 0 && waiting) "正在建立终端连接" else "$count 个终端连接 · 点击返回")
            .setSubText(if (awake) "锁屏保活已开启 · 耗电增加" else "后台连接保护已开启")
            .setContentIntent(open).setVisibility(Notification.VISIBILITY_PRIVATE)
            .setOngoing(true).setOnlyAlertOnce(true).setCategory(Notification.CATEGORY_SERVICE).build()
    }
    internal fun finishIdle() { setAwake(false); stopForeground(STOP_FOREGROUND_REMOVE); stopSelfResult(lastStart) }
    override fun onTimeout(startId: Int, fgsType: Int) { finishIdle() }
    override fun onDestroy() { setAwake(false); scope.cancel(); owner.destroyed(this, token); super.onDestroy() }
    companion object {
        const val GENERATION = "terminal-generation"
        const val OPEN_TERMINALS = "com.hanxi.ghosterm.OPEN_TERMINALS"
        const val CHANNEL = "terminal-connections"
        const val NOTIFICATION = 4103
    }
}
