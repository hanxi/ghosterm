package com.hanxi.ghosterm

import android.net.Uri
import android.os.Bundle
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.FileDescriptorFlags
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.ConcurrentLinkedQueue
import kotlin.coroutines.CoroutineContext

@RunWith(AndroidJUnit4::class)
class PluginArchiveCaptureTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val resolver get() = context.contentResolver
    private fun configure(bytes: ByteArray = ByteArray(32), mode: String = "regular", size: Long? = null) {
        val result = resolver.call(PluginSignatureStreamProvider.URI, "set", null, Bundle().apply {
            putByteArray("bytes", bytes); putString("mode", mode); size?.let { putLong("size", it) }
        })!!
        assertTrue(result.toString(), result.getBoolean("ok"))
    }
    private fun state() = resolver.call(PluginSignatureStreamProvider.URI, "state", null, null)!!
    private fun release() { assertTrue(resolver.call(PluginSignatureStreamProvider.URI, "release", null, null)!!.getBoolean("ok")) }
    private fun files() = context.cacheDir.list()!!.filter { it.startsWith("plugin-archive-capture-") }.sorted()
    private fun finish(before: List<String>) {
        val after = files()
        try { assertEquals(before, after) }
        finally { for (name in after - before.toSet()) assertTrue(File(context.cacheDir, name).delete()) }
    }
    private suspend fun read() = PluginArchiveCapture.read(context, resolver, PluginSignatureStreamProvider.URI)
    private suspend fun reject() {
        try { read().use { }; fail("invalid ZIP capture accepted") }
        catch (_: PluginArchiveCapture.Rejected) { }
    }
    private fun zerosHash(count: Long): String {
        val hash = MessageDigest.getInstance("SHA-256"); val bytes = ByteArray(16384); var left = count
        while (left > 0) { val size = minOf(bytes.size.toLong(), left).toInt(); hash.update(bytes, 0, size); left -= size }
        return hash.digest().joinToString("") { "%02x".format(it.toInt() and 255) }
    }
    @Test fun streamUsesActualEofAndReturnsUnlinkedReadOnlySnapshot() = runBlocking {
        val before = files(); configure(byteArrayOf(), size = 4L * 1024 * 1024)
        try { read().use { captured ->
            assertEquals(4L * 1024 * 1024, captured.bytes); assertEquals(zerosHash(captured.bytes), captured.sha256)
            captured.borrow().use { fd ->
                val info = Os.fstat(fd.fileDescriptor)
                assertEquals(captured.bytes, info.st_size); assertEquals(0L, info.st_nlink)
                assertEquals(OsConstants.O_RDONLY, FileDescriptorFlags.status(fd.fileDescriptor) and OsConstants.O_ACCMODE)
                assertEquals(256, info.st_mode and 511)
            }
            assertEquals(before, files())
        } } finally { release(); finish(before) }
    }
    @Test fun actualLimitAccepts72MiBAndRejectsExtraByteAndShortInput() = runBlocking {
        val before = files()
        try {
            configure(byteArrayOf(), size = PluginReleaseArchive.LIMIT)
            read().use { assertEquals(PluginReleaseArchive.LIMIT, it.bytes) }
            configure(byteArrayOf(), size = PluginReleaseArchive.LIMIT + 1); reject()
            configure(ByteArray(21)); reject(); configure(byteArrayOf()); reject()
        } finally { release(); finish(before) }
    }
    @Test fun capturedBytesSurviveProviderChangeAndBorrowedLeaseSurvivesClose() = runBlocking {
        configure(ByteArray(32) { 42 })
        try {
            val captured = read(); val hash = captured.sha256; val borrowed = captured.borrow()
            try {
                configure(ByteArray(32) { 7 }); captured.close(); captured.close()
                val bytes = ByteArray(32); assertEquals(32, Os.pread(borrowed.fileDescriptor, bytes, 0, 32, 0))
                assertArrayEquals(ByteArray(32) { 42 }, bytes)
                assertEquals(hash, MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) })
                try { captured.borrow().close(); fail("closed capture handed out fd") } catch (_: IllegalStateException) { }
            } finally { borrowed.close(); captured.close() }
        } finally { release() }
    }
    @Test fun providerFailureAndWritableHandlesDoNotKeepNamedCacheOrDescriptors() = runBlocking {
        val before = files(); val fds = File("/proc/self/fd").list()!!.size
        try {
            configure(mode = "read-write"); reject()
            configure(mode = "error-pipe")
            try { read().close(); fail("provider error prefix accepted") } catch (_: java.io.IOException) { }
            configure()
            repeat(20) { read().close() }
            assertTrue("capture fd leaked", File("/proc/self/fd").list()!!.size <= fds + 2)
        } finally { release(); finish(before) }
    }
    @Test fun sharedInputStatusFlagsAreRestoredAfterCapture() = runBlocking {
        configure(mode = "shared")
        try { val flags = state().getLong("flags"); read().close(); assertEquals(flags, state().getLong("flags")) }
        finally { release() }
    }
    @Test fun cancellationReachesProviderAndReleasesOwnedOutput() = runBlocking {
        val before = files()
        try {
            for (mode in listOf("stall", "blocked-open")) {
                configure(mode = mode)
                val job = launch { read().close() }
                withTimeout(3000) { while (!state().getBoolean("opening")) delay(10) }
                job.cancel(); withTimeout(3000) { job.join() }; assertTrue(job.isCancelled)
                if (mode == "blocked-open") assertTrue(state().getInt("cancelled") > 0)
                assertEquals(before, files()); release()
            }
        } finally { release() }
    }
    @Test fun archiveAndSmallMaterialsShareTwoSlotsUntilActualIoExit() = runBlocking {
        configure(mode = "stall")
        val jobs = mutableListOf<Job>()
        try { supervisorScope {
            try {
            repeat(2) { jobs += launch { read().close() } }
            withTimeout(3000) { while (state().getInt("opened") < 2) delay(10) }
            val third = async { runCatching { PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.POLICY) } }
            jobs += third
            withTimeout(3000) { while (!third.isCompleted && state().getInt("opened") < 3) delay(10) }
            assertTrue("third request reached provider while two ZIP reads held slots", third.isCompleted)
            assertTrue(third.await().exceptionOrNull() is PluginSignatureReadFailure); assertEquals(2, state().getInt("opened"))
            } finally { jobs.forEach { it.cancel() }; withTimeout(3000) { jobs.joinAll() } }
        } } finally {
            release()
        }
        configure()
        try { assertTrue("slots were not returned after actual IO exit", runCatching { read().close() }.isSuccess) } finally { release() }
    }
    @Test fun nonContentUrisAreRejectedWithoutOpeningProvider() = runBlocking {
        configure()
        val local = File(context.cacheDir, "archive-file-uri-test").apply { writeBytes(ByteArray(32)) }
        try {
            for (uri in listOf(Uri.fromFile(local), Uri.parse("content:/missing-authority"))) {
                try { PluginArchiveCapture.read(context, resolver, uri).close(); fail("non-content source accepted") }
                catch (_: PluginArchiveCapture.Rejected) { }
            }
            assertEquals(0, state().getInt("opened"))
        } finally { assertTrue(local.delete()); release() }
    }
    @Test fun cancelledDispatchOfCompletedCaptureClosesUndeliveredOwner() = runBlocking {
        configure()
        fun captures() = File("/proc/self/fd").list()!!.count { name ->
            runCatching { Os.readlink("/proc/self/fd/$name").contains("/plugin-archive-capture-") }.getOrDefault(false)
        }
        val queue = ConcurrentLinkedQueue<Runnable>()
        val dispatcher = object : CoroutineDispatcher() {
            override fun dispatch(context: CoroutineContext, block: Runnable) { queue.add(block) }
        }
        val scope = CoroutineScope(SupervisorJob() + dispatcher)
        try {
            val before = captures()
            val reading = scope.async { read() }
            checkNotNull(queue.poll()).run()
            withTimeout(3000) { while (queue.isEmpty() || captures() <= before) delay(5) }
            // The IO worker has produced an fd but its caller cannot receive
            // the value until we dispatch this queued continuation.
            reading.cancel()
            withTimeout(3000) { while (!reading.isCompleted) { queue.poll()?.run(); delay(5) } }
            assertTrue(reading.isCancelled)
            assertEquals("cancelled result dispatch retained a capture fd", before, captures())
        } finally { scope.cancel(); while (queue.isNotEmpty()) queue.poll()?.run(); release() }
    }
}
