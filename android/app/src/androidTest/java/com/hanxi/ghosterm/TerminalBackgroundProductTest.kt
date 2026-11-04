package com.hanxi.ghosterm

import android.app.*
import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.io.FileInputStream

/** Actual promotion/notification/wake lock and ownership across real Activity transitions. */
@RunWith(AndroidJUnit4::class)
class TerminalBackgroundProductTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val app get() = instrumentation.targetContext.applicationContext as GhostermApplication
    private val activities = mutableListOf<MainActivity>()
    private lateinit var activity: MainActivity
    private var observedWake: android.os.PowerManager.WakeLock? = null
    private val callbacks = object : Application.ActivityLifecycleCallbacks {
        override fun onActivityCreated(value: Activity, state: Bundle?) { if (value is MainActivity) activities.add(value) }
        override fun onActivityDestroyed(value: Activity) { activities.remove(value) }
        override fun onActivityStarted(value: Activity) {}
        override fun onActivityResumed(value: Activity) {}
        override fun onActivityPaused(value: Activity) {}
        override fun onActivityStopped(value: Activity) {}
        override fun onActivitySaveInstanceState(value: Activity, state: Bundle) {}
    }
    private fun <T> main(block: () -> T): T {
        var result: T? = null
        var failure: Throwable? = null
        instrumentation.runOnMainSync { try { result = block() } catch (error: Throwable) { failure = error } }
        failure?.let { throw it }
        @Suppress("UNCHECKED_CAST") return result as T
    }
    private fun model() = ViewModelProvider(activity)[SessionsViewModel::class.java]
    private fun shell(command: String) = instrumentation.uiAutomation.executeShellCommand(command).use {
        FileInputStream(it.fileDescriptor).bufferedReader().use { reader -> reader.readText() }
    }
    private fun terminalWakeHeld(): Boolean {
        val power = shell("dumpsys power").replace("\r\n", "\n")
        val start = power.indexOf("Wake Locks: size=")
        check(start >= 0) { "Fixture: active wake-lock section is unavailable" }
        // New Android versions also print historical acquire/release events.
        // Only the active section proves a lock is still held.
        return "Ghosterm:terminal-connections" in power.substring(start).substringBefore("\n\n")
    }
    private fun await(message: String, predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 15_000
        while (SystemClock.elapsedRealtime() < end) { if (predicate()) return; SystemClock.sleep(50) }
        fail(message)
    }
    private fun text(session: TerminalSession): String = session.connection.frame(-1)?.let { frame ->
        (0 until frame.rows).joinToString("\n", transform = frame::line)
    }.orEmpty()
    @Before fun setup() {
        main { app.registerActivityLifecycleCallbacks(callbacks) }
        activity = instrumentation.startActivitySync(Intent(instrumentation.targetContext, MainActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) as MainActivity
        await("fixture Activity must be foreground") { main { activity.hasWindowFocus() } }
    }
    @After fun cleanup() {
        main {
            if (::activity.isInitialized) model().sessions.map { it.id }.forEach(model()::close)
            activities.toList().forEach { it.finish() }
            app.unregisterActivityLifecycleCallbacks(callbacks)
        }
        await("last terminal must release its service") { app.terminalBackground.state == TerminalBackgroundState.IDLE }
        try {
            await("last terminal must release its wake lock") { !terminalWakeHeld() && observedWake?.isHeld != true }
        } finally {
            observedWake?.let { if (it.isHeld) it.release() }
            observedWake = null
        }
    }
    @Test fun realServiceAndOptionalWakeLockTrackSessionsAndBackgroundIoContinues() {
        assertTrue("Fixture: enable notifications through the host runner before this notification test",
            app.getSystemService(NotificationManager::class.java).areNotificationsEnabled())
        main { model().newLocal() }
        await("terminal must connect with foreground protection") {
            main { model().active?.state?.connected == true && app.terminalBackground.state == TerminalBackgroundState.ACTIVE }
        }
        val session = main { model().active!! }
        val services = app.getSystemService(ActivityManager::class.java).getRunningServices(100)
        assertTrue(services.any { it.service.className == TerminalConnectionService::class.java.name && it.foreground })
        if (android.os.Build.VERSION.SDK_INT >= 34) main {
            val field = TerminalConnectionBackground::class.java.getDeclaredField("binding").also { it.isAccessible = true }
            val service = field.get(app.terminalBackground) as TerminalConnectionService
            assertTrue(service.foregroundServiceType and android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE != 0)
        }
        assertFalse(app.terminalBackground.keepAwake)
        main { model().toggleBackgroundAwake() }
        await("explicit lock-screen option must acquire a real partial wake lock") {
            app.terminalBackground.keepAwake && terminalWakeHeld()
        }
        main {
            val service = TerminalConnectionBackground::class.java.getDeclaredField("binding").also { it.isAccessible = true }
                .get(app.terminalBackground) as TerminalConnectionService
            observedWake = TerminalConnectionService::class.java.getDeclaredField("wake").also { it.isAccessible = true }
                .get(service) as android.os.PowerManager.WakeLock
        }
        val notification = app.getSystemService(NotificationManager::class.java).activeNotifications
            .single { it.id == TerminalConnectionService.NOTIFICATION }.notification
        assertTrue(notification.extras.getCharSequence("android.text").toString().contains("1 个终端连接"))
        main { session.connection.commit("sleep 1; printf '\\r\\nBACKGROUND_IO_PROOF\\r\\n'\r") }
        shell("input keyevent KEYCODE_HOME")
        await("native IO must continue with the Activity in the background") { text(session).contains("BACKGROUND_IO_PROOF") }
        assertTrue(session.connection.state().connected)
        instrumentation.targetContext.startActivity(Intent(instrumentation.targetContext, MainActivity::class.java)
            .setAction(TerminalConnectionService.OPEN_TERMINALS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP))
        await("notification must restore the actual terminal presenter") {
            main {
                activities.lastOrNull { it.hasWindowFocus() }?.let { returned ->
                    val presenter = ViewModelProvider(returned)[SessionsViewModel::class.java]
                    presenter.visible && presenter.active === session
                } == true
            }
        }
    }
    @Test fun clearingAPresenterKeepsTheNativeOwnerForTheNextPresenter() {
        val firstStore = ViewModelStore()
        val secondStore = ViewModelStore()
        lateinit var first: SessionsViewModel
        lateinit var next: SessionsViewModel
        try {
            main { first = SessionsViewModel(app); firstStore.put("terminal", first); first.newLocal() }
            await("first owner did not connect") { main { first.active?.state?.connected == true && app.terminalBackground.state == TerminalBackgroundState.ACTIVE } }
            val session = main { first.active!! }
            main { firstStore.clear() }
            assertTrue(session.connection.state().connected)
            main {
                next = SessionsViewModel(app); secondStore.put("terminal", next)
                assertSame(session, next.active)
                assertSame(session.connection, next.active!!.connection)
                next.active!!.connection.commit("printf '\\r\\nNEXT_PRESENTER_PROOF\\r\\n'\r")
            }
            await("new presenter must operate the original native owner") { text(session).contains("NEXT_PRESENTER_PROOF") }
            main { next.close(session.id) }
        } finally { main { firstStore.clear(); secondStore.clear() } }
    }
}
