package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.*

@RunWith(AndroidJUnit4::class)
class AboutModelTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val store = ViewModelStore()
    private fun <T> main(block: () -> T): T {
        var value: T? = null; instrumentation.runOnMainSync { value = block() }
        @Suppress("UNCHECKED_CAST") return value as T
    }
    private fun await(predicate: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 10_000
        while (SystemClock.elapsedRealtime() < end) { if (main(predicate)) return; SystemClock.sleep(20) }
        fail("About operation did not settle")
    }
    private fun model(reader: (String) -> String): AboutViewModel = main {
        AboutViewModel(Application(), reader).also { store.put("about", it) }
    }
    @After fun close() { main { store.clear() } }
    @Test fun missingMalformedAndUnknownBuildInfoNeverBecomeClean() {
        val sha = "a".repeat(40)
        listOf("clean", "dirty", "unknown").forEach { state ->
            val parsed = AboutBuild.decode("""{"schema":1,"git_commit":"$sha","source_state":"$state"}""")
            assertEquals(state, parsed.sourceState); assertEquals(sha, parsed.commit)
        }
        val unknown = AboutBuild.decode("""{"schema":1,"git_commit":null,"source_state":"unknown"}""")
        assertNull(unknown.commit); assertEquals("工作区状态无法确认", unknown.statusText)
        assertEquals("包含未提交的修改", AboutBuild(sha, "dirty").statusText)
        for (bad in listOf("{}", """{"schema":1,"git_commit":"$sha"}""",
            """{"schema":1,"git_commit":null,"source_state":"clean"}""",
            """{"schema":2,"git_commit":"$sha","source_state":"clean"}""",
            """{"schema":1,"git_commit":"main","source_state":"clean"}""",
            """{"schema":1,"git_commit":"$sha","source_state":"unsupported"}""",
            """{"schema":1,"schema":1,"git_commit":"$sha","source_state":"clean"}""")) {
            var rejected = false
            try { AboutBuild.decode(bad) } catch (_: Exception) { rejected = true }
            assertTrue("Malformed build metadata must be rejected", rejected)
        }
    }
    @Test fun failedReadsRemainVisibleAndCanBeRetried() {
        var reject = true
        val model = model { if (reject) throw java.io.IOException("test read failure") else "Apache License" }
        main { model.enter(); model.open(AboutLicense.OPENSSL) }
        await { model.buildFailed && model.document is AboutDocument.Failed }
        assertNull(main { model.build }); assertEquals(AboutDocument.Failed, main { model.document })
        reject = false
        main { model.open(AboutLicense.OPENSSL) }
        await { model.document is AboutDocument.Ready }
        assertEquals("Apache License", (main { model.document } as AboutDocument.Ready).text)
        main { assertTrue(model.back()); assertFalse(model.back()) }
    }
    @Test fun aLateLicenseReadCannotReplaceTheNewPageOrReopenAfterBack() {
        val entered = CountDownLatch(1); val release = CountDownLatch(1); val ended = CountDownLatch(1)
        val model = model { path ->
            if (path.endsWith("Ghosterm-MIT.txt")) {
                entered.countDown(); check(release.await(10, TimeUnit.SECONDS)); ended.countDown(); "old document"
            } else "new document"
        }
        try {
            main { model.open(AboutLicense.GHOSTERM) }; assertTrue(entered.await(5, TimeUnit.SECONDS))
            val oldJob = main { AboutViewModel::class.java.getDeclaredField("documentJob").apply { isAccessible = true }.get(model) as Job }
            main { assertTrue(model.back()); model.open(AboutLicense.OPENSSL) }
            await { model.document is AboutDocument.Ready }
            release.countDown(); assertTrue(ended.await(5, TimeUnit.SECONDS))
            runBlocking { withTimeout(5_000) { oldJob.join() } }
            assertEquals(AboutLicense.OPENSSL, main { model.selected })
            assertEquals("new document", (main { model.document } as AboutDocument.Ready).text)
            main { assertTrue(model.back()) }; SystemClock.sleep(100); assertNull(main { model.selected })
        } finally { release.countDown() }
    }
}
