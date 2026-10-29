package com.hanxi.ghosterm

import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Rect
import android.os.*
import android.view.*
import android.view.inputmethod.EditorInfo
import androidx.test.ext.junit.rules.ActivityScenarioRule
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModelProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.terminal.TerminalView
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** A reproducible debug/instrumentation baseline, not a device-independent speed gate. */
@RunWith(AndroidJUnit4::class)
class TerminalPerformanceTest {
    @get:Rule val ui = ActivityScenarioRule(MainActivity::class.java)
    private lateinit var activity: MainActivity
    private val instrumentation get() = InstrumentationRegistry.getInstrumentation()
    private fun main(action: () -> Unit) = instrumentation.runOnMainSync(action)
    private fun model() = ViewModelProvider(activity)[SessionsViewModel::class.java]
    private fun find(view: View): TerminalView? {
        if (view is TerminalView) return view
        if (view is ViewGroup) for (i in 0 until view.childCount) find(view.getChildAt(i))?.let { return it }
        return null
    }
    private fun view() = checkNotNull(find(activity.window.decorView))
    private fun await(description: String, timeout: Long = 30_000, predicate: () -> Boolean) {
        val deadline = SystemClock.elapsedRealtime() + timeout
        while (SystemClock.elapsedRealtime() < deadline) {
            if (predicate()) return
            SystemClock.sleep(10)
        }
        fail("performance probe incomplete: $description")
    }
    private fun send(text: String) = main { assertTrue(view().commitText(text)) }
    private fun output(marker: String) = await(marker) {
        var found = false
        main { view().currentFrame?.let { f -> found = (0 until f.rows).any { f.line(it) == marker } } }
        found
    }
    private fun memory() = JSONObject().apply {
        val info = Debug.MemoryInfo(); Debug.getMemoryInfo(info)
        put("elapsed_ms", SystemClock.elapsedRealtime())
        put("pss_kib", info.totalPss)
        put("native_allocated_bytes", Debug.getNativeHeapAllocatedSize())
        put("java_used_bytes", Runtime.getRuntime().totalMemory() - Runtime.getRuntime().freeMemory())
    }
    private fun idle(): JSONObject {
        // No sampling thread or UI polling during this interval. CPU includes the
        // app, instrumentation and native owner, but not the child shell process.
        val cpu = android.os.Process.getElapsedCpuTime()
        val wall = SystemClock.elapsedRealtime()
        SystemClock.sleep(3_000)
        val elapsed = SystemClock.elapsedRealtime() - wall
        val used = android.os.Process.getElapsedCpuTime() - cpu
        return JSONObject().put("wall_ms", elapsed).put("process_cpu_ms", used)
            .put("one_core_percent", used * 100.0 / elapsed)
    }
    private fun pixel(): Int {
        val ready = CountDownLatch(1)
        val bitmap = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888)
        var status = -1
        main {
            val xy = IntArray(2); view().getLocationInWindow(xy)
            PixelCopy.request(activity.window, Rect(xy[0] + 1, xy[1] + 1, xy[0] + 2, xy[1] + 2), bitmap,
                { result -> status = result; ready.countDown() }, Handler(Looper.getMainLooper()))
        }
        assertTrue("pixel capture timeout", ready.await(3, TimeUnit.SECONDS))
        assertEquals("pixel capture unavailable", PixelCopy.SUCCESS, status)
        return bitmap.getPixel(0, 0).also { bitmap.recycle() }
    }

    @Test fun recordRealTerminalBaseline() {
        val context = instrumentation.targetContext
        val work = File(context.cacheDir, "performance-${UUID.randomUUID()}").apply { mkdirs() }
        val reportFile = File(context.filesDir, "terminal-performance.json")
        reportFile.delete() // Never leave a previous success after an incomplete run.
        val metricsThread = HandlerThread("terminal-frame-metrics").apply { start() }
        val lock = Any()
        var phase = "setup"
        val timings = mutableMapOf<String, MutableList<Long>>()
        val drawTimings = mutableMapOf<String, MutableList<Long>>()
        val dropped = mutableMapOf<String, Int>()
        val listener = Window.OnFrameMetricsAvailableListener { _, metric, lost ->
            synchronized(lock) {
                timings.getOrPut(phase) { mutableListOf() }.add(metric.getMetric(FrameMetrics.TOTAL_DURATION))
                drawTimings.getOrPut(phase) { mutableListOf() }.add(metric.getMetric(FrameMetrics.DRAW_DURATION))
                dropped[phase] = (dropped[phase] ?: 0) + lost
            }
        }
        fun phase(name: String) { synchronized(lock) { phase = name }; android.util.Log.i("GhostermPerformance", "phase=$name") }
        var observedWindow: Window? = null
        fun detachMetrics() { main { observedWindow?.removeOnFrameMetricsAvailableListener(listener); observedWindow = null } }
        fun attachMetrics() { main { activity.window.addOnFrameMetricsAvailableListener(listener, Handler(metricsThread.looper)); observedWindow = activity.window } }
        var failure: Throwable? = null
        val memorySamples = JSONArray()
        val report = JSONObject().put("schema", 1).put("build_type", "debug-instrumentation")
            .put("device", Build.DEVICE).put("model", Build.MODEL).put("sdk", Build.VERSION.SDK_INT)
            .put("abis", JSONArray(Build.SUPPORTED_ABIS.toList())).put("fingerprint", Build.FINGERPRINT)
            .put("page_size", android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE))
            .put("memory_samples", memorySamples)
        try {
            ui.scenario.onActivity { activity = it; model().local() }
            await("local session") { var ready = false; main { ready = model().active?.state?.connected == true && find(activity.window.decorView)?.currentFrame != null }; ready }
            attachMetrics()
            main {
                val v = view(); val f = v.currentFrame!!
                report.put("viewport", JSONObject().put("width_px", v.width).put("height_px", v.height)
                    .put("cols", f.cols).put("rows", f.rows).put("density", v.resources.displayMetrics.density))
            }
            send("stty -echo; PS1=''; printf '\\033[0m\\033[?25l\\033[2J\\033[HPROFILE_READY\\r\\n'\r")
            output("PROFILE_READY")
            // File generated before measurement; payload bytes include ANSI and UTF-8.
            val line = "\u001b[32m0123456789 abcdefghijklmnopqrstuvwxyz 中文 é\u001b[0m\r\n"
            val payload = File(work, "payload").apply { writeText(line.repeat(2048), Charsets.UTF_8) }
            val sample = File(work, "run.sh")
            fun workload(name: String, repeats: Int, paced: Boolean): JSONObject {
                sample.writeText("#!/system/bin/sh\nset -e\ni=0\nwhile [ \"\u0024i\" -lt $repeats ]; do\ncat '${payload.absolutePath}'\ni=\u0024((i+1))\n" +
                    (if (paced) "sleep 0.05\n" else "") + "done\nprintf '\\033[0m\\033[2J\\033[H${name}_DONE\\r\\n'\n")
                phase(name)
                memorySamples.put(memory())
                val cpuStart = android.os.Process.getElapsedCpuTime()
                val start = SystemClock.elapsedRealtimeNanos()
                send("sh '${sample.absolutePath}'\r")
                var nextSample = SystemClock.elapsedRealtime() + 1_000
                var finished = false
                await("$name final output", 60_000) {
                    main { view().currentFrame?.let { f -> finished = f.line(0) == "${name}_DONE" } }
                    if (SystemClock.elapsedRealtime() >= nextSample) { memorySamples.put(memory()); nextSample += 1_000 }
                    finished
                }
                val duration = (SystemClock.elapsedRealtimeNanos() - start) / 1e6
                val cpuUsed = android.os.Process.getElapsedCpuTime() - cpuStart
                memorySamples.put(memory())
                return JSONObject().put("payload_bytes", payload.length() * repeats).put("elapsed_ms", duration)
                    .put("mib_per_second", payload.length() * repeats / 1048576.0 / (duration / 1000))
                    .put("process_cpu_ms", cpuUsed)
                    .put("paced_sleep_ms_per_chunk", if (paced) 50 else 0)
            }
            report.put("burst", workload("BURST", 64, false))
            report.put("sustained", workload("SUSTAINED", 128, true))
            phase("scroll")
            val scrollStart = SystemClock.elapsedRealtime()
            repeat(40) { index -> main { model().active!!.surface.scroll(if (index < 20) -10 else 10) }; SystemClock.sleep(50) }
            report.put("scroll_wall_ms", SystemClock.elapsedRealtime() - scrollStart)
            send("printf '\\033[0m\\033[48;2;0;0;0m\\033[2J\\033[HLATENCY_READY\\r\\n'\r")
            output("LATENCY_READY")
            // Calibrate the same capture/poll path before taking latency timestamps.
            // Keep the absolute floor; slow runners affect control and target alike.
            val calibrationStart = SystemClock.elapsedRealtimeNanos()
            repeat(20) { pixel(); SystemClock.sleep(10) }
            val calibrationMs = (SystemClock.elapsedRealtimeNanos() - calibrationStart) / 1e6
            val inputBudget = maxOf(1_000L, (calibrationMs * 1.5 + 250).toLong())
            report.put("pixel_poll_calibration_ms", calibrationMs).put("input_timeout_ms", inputBudget)
            phase("input_pixel")
            val latency = JSONArray()
            repeat(20) { index ->
                val green = index % 2 == 0
                val start = SystemClock.elapsedRealtimeNanos()
                main {
                    val connection = view().onCreateInputConnection(EditorInfo())
                    assertTrue(connection.commitText("printf '\\033[48;2;0;${if (green) 255 else 0};0m\\033[2J\\033[H'\r", 1))
                }
                await("input color $index", inputBudget) {
                    val color = pixel()
                    if (green) Color.green(color) > Color.red(color) + 40
                    else Color.red(color) < 10 && Color.green(color) < 10 && Color.blue(color) < 10
                }
                latency.put((SystemClock.elapsedRealtimeNanos() - start) / 1e6)
            }
            report.put("input_to_window_pixel_ms", latency)
            phase("foreground_idle"); SystemClock.sleep(500)
            report.put("foreground_idle", idle())
            phase("background_idle")
            detachMetrics()
            ui.scenario.moveToState(Lifecycle.State.CREATED)
            SystemClock.sleep(500)
            report.put("background_idle", idle())
            ui.scenario.moveToState(Lifecycle.State.RESUMED)
            ui.scenario.onActivity { activity = it }
            await("resumed view") { var ready = false; main { ready = find(activity.window.decorView)?.currentFrame != null }; ready }
            attachMetrics()
            phase("close_under_output")
            sample.writeText("#!/system/bin/sh\nwhile :; do cat '${payload.absolutePath}' || exit; done\n")
            send("sh '${sample.absolutePath}'\r")
            SystemClock.sleep(500)
            val closeStart = SystemClock.elapsedRealtimeNanos()
            main { model().close(model().active!!.id) }
            report.put("close_request_ms", (SystemClock.elapsedRealtimeNanos() - closeStart) / 1e6)
            await("native owner released", 10_000) { NativeTerminalProbe.owners() == 0 }
            report.put("close_owner_released_ms", (SystemClock.elapsedRealtimeNanos() - closeStart) / 1e6)
            phase("done")
            memorySamples.put(memory())
            val frames = JSONObject()
            synchronized(lock) {
                for ((name, values) in timings) frames.put(name, JSONObject()
                    .put("total_duration_ns", JSONArray(values)).put("draw_duration_ns", JSONArray(drawTimings[name]))
                    .put("callback_dropped", dropped[name] ?: 0))
            }
            // Missing metrics are a failed measurement, never zero-cost rendering.
            assertTrue("no sustained window frames", (frames.optJSONObject("SUSTAINED")?.optJSONArray("total_duration_ns")?.length() ?: 0) > 0)
            report.put("window_frames", frames).put("complete", true)
            reportFile.writeText(report.toString(2))
            instrumentation.sendStatus(0, Bundle().apply { putString("ghosterm_profile", report.toString()) })
        } catch (error: Throwable) {
            failure = error
            android.util.Log.e("GhostermPerformance", "measurement failed during $phase", error)
            throw error
        } finally {
            val cleanup = runCatching {
                detachMetrics()
                main { model().sessions.map { it.id }.forEach(model()::close) }
            }.exceptionOrNull()
            metricsThread.quitSafely(); metricsThread.join(3_000)
            work.deleteRecursively()
            if (cleanup != null) { if (failure != null) failure.addSuppressed(cleanup) else throw cleanup }
        }
    }
}
