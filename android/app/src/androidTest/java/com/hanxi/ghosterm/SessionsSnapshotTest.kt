package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import androidx.lifecycle.ViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.LocalSession
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread

@RunWith(AndroidJUnit4::class)
class SessionsSnapshotTest {
    private lateinit var model: SessionsViewModel
    @Test fun activeSelectionSurvivesConcurrentMainThreadListChanges() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val app = instrumentation.targetContext.applicationContext as Application
        val dir = File(app.cacheDir, "active-snapshot-${UUID.randomUUID()}").apply { mkdirs() }
        val connection = LocalSession(dir.absolutePath)
        val store = ViewModelStore()
        // Selection metadata is the subject; all rows share one actual native
        // connection, so this does not create dozens of unused real shells.
        val rows = List(32) { TerminalSession(UUID.randomUUID(), "row-$it", connection) }
        val target = rows.last()
        val transient = TerminalSession(UUID.randomUUID(), "transient", connection)
        val finished = AtomicBoolean(false)
        val stop = AtomicBoolean(false)
        val samples = AtomicInteger()
        val failure = AtomicReference<Throwable?>()
        val start = CountDownLatch(1)
        var reader: Thread? = null
        try {
            instrumentation.runOnMainSync {
                model = ViewModelProvider(store, object : ViewModelProvider.Factory {
                    @Suppress("UNCHECKED_CAST")
                    override fun <T : ViewModel> create(modelClass: Class<T>): T =
                        SessionsViewModel(app, monitorSessions = false, settings = null) as T
                })[SessionsViewModel::class.java]
                model.sessions.addAll(rows)
                model.show(target.id)
            }
            assertSame(target, model.active)
            reader = thread(name = "active-snapshot-reader") {
                try {
                    check(start.await(5, TimeUnit.SECONDS))
                    while (!stop.get() && (!finished.get() || samples.get() < 2048)) {
                        assertSame(target, model.active)
                        samples.incrementAndGet()
                    }
                } catch (error: Throwable) { failure.set(error) }
            }
            instrumentation.runOnMainSync {
                start.countDown()
                repeat(6000) {
                    model.sessions.add(0, transient)
                    if (it % 8 == 0) Thread.yield()
                    model.sessions.removeAt(0)
                }
            }
            finished.set(true)
            reader.join(10_000)
            assertFalse("active reader must finish", reader.isAlive)
            failure.get()?.let { throw AssertionError("active selection raced with list changes", it) }
            assertTrue("reader exercised snapshots", samples.get() >= 2048)
            instrumentation.runOnMainSync { model.show(rows.first().id) }
            assertSame(rows.first(), model.active)
            instrumentation.runOnMainSync { model.sessions.remove(rows.first()) }
            assertNull("removed selection must not choose a different session", model.active)
        } finally {
            stop.set(true); finished.set(true); start.countDown(); reader?.join(10_000)
            instrumentation.runOnMainSync {
                if (::model.isInitialized) model.sessions.clear()
                store.clear()
            }
            connection.close()
            val owner = Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
            val end = SystemClock.uptimeMillis() + 5000
            while ((owner.getDeclaredMethod("ownerCount").invoke(owner.getField("INSTANCE").get(null)) as Int) != 0 && SystemClock.uptimeMillis() < end) SystemClock.sleep(10)
            assertEquals(0, owner.getDeclaredMethod("ownerCount").invoke(owner.getField("INSTANCE").get(null)))
            dir.deleteRecursively()
        }
    }
}
