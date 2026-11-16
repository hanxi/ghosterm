package com.hanxi.ghosterm

import android.net.Uri
import android.os.Bundle
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.*
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class PluginSignatureDocumentsTest {
    private val resolver = InstrumentationRegistry.getInstrumentation().targetContext.contentResolver
    private fun configure(bytes: ByteArray, mode: String = "regular") {
        val answer = resolver.call(PluginSignatureStreamProvider.URI, "set", null, Bundle().apply {
            putByteArray("bytes", bytes); putString("mode", mode)
        })!!
        assertTrue(answer.toString(), answer.getBoolean("ok"))
    }
    private fun release() { assertTrue(resolver.call(PluginSignatureStreamProvider.URI, "release", null, null)!!.getBoolean("ok")) }
    private suspend fun awaitOpen() = withTimeout(3000) {
        while (!resolver.call(PluginSignatureStreamProvider.URI, "state", null, null)!!.getBoolean("opening")) delay(10)
    }
    private suspend fun rejected(kind: PluginSignatureMaterial) {
        try { PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, kind); fail("invalid material accepted") }
        catch (_: IllegalArgumentException) { }
    }
    @Test fun actualBytesIgnoreMetadataAndReleaseFileDescriptors() = runBlocking {
        configure(ByteArray(64) { it.toByte() })
        try {
            val before = File("/proc/self/fd").list()!!.size
            repeat(24) {
                val document = PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.SIGNATURE)
                assertEquals(64, document.size)
                assertArrayEquals(ByteArray(64) { it.toByte() }, document.copyBytes())
            }
            assertTrue("content descriptors leaked", File("/proc/self/fd").list()!!.size <= before + 2)
            configure(ByteArray(65))
            rejected(PluginSignatureMaterial.SIGNATURE)
        } finally { release() }
    }
    @Test fun eachRoleEnforcesActualEofLimitsAndEmptySignature() = runBlocking {
        try {
            for (kind in PluginSignatureMaterial.entries) {
                configure(ByteArray(kind.limit) { 1 })
                assertEquals(kind.limit, PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, kind).size)
                configure(ByteArray(kind.limit + 1)); rejected(kind)
                configure(ByteArray(0)); rejected(kind)
            }
            configure(ByteArray(63)); rejected(PluginSignatureMaterial.SIGNATURE)
        } finally { release() }
    }
    @Test fun capturedBytesAndHashAreIndependentOfLaterProviderChanges() = runBlocking {
        configure("public policy".toByteArray())
        try {
            val first = PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.POLICY)
            val hash = first.sha256
            configure("new policy".toByteArray())
            val copy = first.copyBytes(); copy.fill(0)
            assertEquals("public policy", String(first.copyBytes()))
            assertEquals(hash, first.sha256)
            assertNotEquals(hash, PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.POLICY).sha256)
        } finally { release() }
    }
    @Test fun cancellationInterruptsOwnedPipeReadAndCooperativeProviderOpen() = runBlocking {
        try {
            for (mode in listOf("stall", "blocked-open")) {
                configure(ByteArray(0), mode)
                val reading = launch { PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.POLICY) }
                awaitOpen()
                reading.cancel()
                withTimeout(3000) { reading.join() }
                assertTrue(reading.isCancelled)
                if (mode == "blocked-open") assertTrue(resolver.call(PluginSignatureStreamProvider.URI, "state", null, null)!!.getInt("cancelled") > 0)
                release()
            }
        } finally { release() }
    }
    @Test fun outstandingReadsKeepTheirSlotsUntilActualRelease() = runBlocking {
        configure(ByteArray(0), "stall")
        fun opened() = resolver.call(PluginSignatureStreamProvider.URI, "state", null, null)!!.getInt("opened")
        try {
            supervisorScope {
                val jobs = mutableListOf<Deferred<Result<PluginSignatureDocument>>>()
                try {
                    repeat(2) { jobs += async { runCatching { PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.POLICY) } } }
                    withTimeout(3000) { while (opened() < 2) delay(10) }
                    val third = async { runCatching { PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.POLICY) } }
                    jobs += third
                    withTimeout(3000) { while (!third.isCompleted && opened() < 3) delay(10) }
                    assertTrue("a third request reached the provider while two reads retained their slots", third.isCompleted)
                    assertTrue(third.await().exceptionOrNull() is PluginSignatureReadFailure)
                    assertEquals(2, opened())
                } finally {
                    jobs.forEach { it.cancel() }; withTimeout(3000) { jobs.joinAll() }; release()
                }
            }
            configure(ByteArray(64))
            assertEquals(64, PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.SIGNATURE).size)
        } finally { release() }
    }
    @Test fun providerReportedErrorsRejectTheCapturedPrefix() = runBlocking {
        configure(ByteArray(64), "error-pipe")
        try {
            try { PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.SIGNATURE); fail("failed stream prefix accepted as complete") }
            catch (_: java.io.IOException) { }
        } finally { release() }
    }
    @Test fun readWriteProviderHandlesAreRejected() = runBlocking {
        configure(ByteArray(64), "read-write")
        try { rejected(PluginSignatureMaterial.SIGNATURE) } finally { release() }
    }
    @Test fun providerStreamTimeoutAndNonContentUriFailExplicitly() = runBlocking {
        configure(ByteArray(0), "stall")
        try {
            try { PluginSignatureDocuments.read(resolver, PluginSignatureStreamProvider.URI, PluginSignatureMaterial.POLICY); fail("stalled stream accepted") }
            catch (_: TimeoutCancellationException) { }
            val local = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "signature-local-uri-${java.util.UUID.randomUUID()}")
            try {
                local.writeText("public fixture")
                try { PluginSignatureDocuments.read(resolver, Uri.fromFile(local), PluginSignatureMaterial.POLICY); fail("file URI accepted") }
                catch (_: IllegalArgumentException) { }
            } finally { check(local.delete() || !local.exists()) }
        } finally { release() }
    }
}
