package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionChannelException
import com.hanxi.ghosterm.bridge.ExtensionSession
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.UUID
import java.util.concurrent.atomic.AtomicReference
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ExtensionProcessChannelTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun fixture(body: (String, Int, File) -> Unit) {
        val helper = File(instrumentation.context.applicationInfo.nativeLibraryDir, "libghosterm_test_process_channel.so")
        check(helper.canExecute()) { "compiled test-only process fixture missing" }
        val cwd = File(instrumentation.targetContext.cacheDir, "process-channel-${UUID.randomUUID()}")
        check(cwd.mkdir())
        val fd = ParcelFileDescriptor.open(cwd, ParcelFileDescriptor.MODE_READ_ONLY)
        try { body(helper.canonicalPath, fd.fd, cwd) }
        finally { fd.close(); check(cwd.deleteRecursively()) }
    }
    private fun config(helper: String, fd: Int, mode: String) = ExtensionChannel.ProcessConfiguration(
        helper, listOf("raw argv zero", mode), fd, listOf("GT_SELECTED=only"), executionAuthorized = true)
    private fun close(operation: () -> Boolean) {
        val end = SystemClock.uptimeMillis() + 2000
        while (SystemClock.uptimeMillis() < end) { if (operation()) return; SystemClock.sleep(1) }
        fail("process owner not released within existing two-second cleanup budget")
    }
    private fun ready(channel: ExtensionChannel) {
        val end = SystemClock.uptimeMillis() + 2000
        while (SystemClock.uptimeMillis() < end) {
            val (io, bytes) = channel.read(1, 5)
            if (io.data) { assertArrayEquals(byteArrayOf(82), bytes); return }
            assertTrue(io.wait)
        }
        fail("fixture not ready")
    }
    @Test fun rawArgumentsEnvironmentAndFinalExit() = fixture { helper, fd, cwd ->
        val args = listOf("raw argv zero", "context", "", "目录⚠️", "\$(touch nope); ' \\")
        val channel = ExtensionChannel.process(config(helper, fd, "context").copy(arguments = args))
        var released = false
        try {
            val output = ByteArrayOutputStream(); val end = SystemClock.uptimeMillis() + 2000
            while (SystemClock.uptimeMillis() < end) {
                val (io, bytes) = channel.read(milliseconds = 5); output.write(bytes)
                if (io.eof && channel.processObservation().reaped) break
                assertTrue(io.data || io.wait || io.eof)
            }
            val expected = (args + listOf(cwd.canonicalPath, "only", "no-home", "no-path")).joinToString("\u0000") + "\u0000"
            assertArrayEquals(expected.toByteArray(), output.toByteArray())
            val i = channel.processObservation()
            assertTrue(i.childExited && i.reaped && i.stdoutEof); assertEquals(23, i.exitCode); assertNull(i.termSignal)
            assertArrayEquals(byteArrayOf(68, 0, -1, 10), i.diagnostic); assertEquals(23, channel.info().exitStatus)
            close { channel.closeStep(1) }; released = true
            assertEquals(23, channel.processObservation().exitCode)
            assertThrows(IllegalStateException::class.java) { channel.read() }
        } finally { if (!released) close { channel.closeStep(1) } }
    }
    @Test fun duplex256KiBAndFinalSnapshot() = fixture { helper, fd, _ ->
        val channel = ExtensionChannel.process(config(helper, fd, "echo"))
        var released = false
        try {
            ready(channel)
            repeat(16) { block ->
                val input = ByteArray(16384) { ((it + block) % 251).toByte() }
                assertEquals(input.size, channel.write(input.copyOf()).count)
                val output = ByteArrayOutputStream(); val end = SystemClock.uptimeMillis() + 2000
                while (output.size() < input.size && SystemClock.uptimeMillis() < end) {
                    assertEquals(1, channel.step(1)); val (io, bytes) = channel.read(milliseconds = 1)
                    assertTrue(io.data || io.wait); output.write(bytes)
                }
                assertArrayEquals(input, output.toByteArray())
            }
            assertEquals(262145UL, channel.processObservation().stdoutBytes)
            close { channel.closeStep(1) }; released = true; assertTrue(channel.processObservation().reaped)
            assertArrayEquals(byteArrayOf(68, 0, -1, 10), channel.processObservation().diagnostic)
        } finally { if (!released) close { channel.closeStep(1) } }
    }
    @Test fun eofRemainsDistinctFromExit() = fixture { helper, fd, _ ->
        val channel = ExtensionChannel.process(config(helper, fd, "eof-hold"))
        var released = false
        try {
            val end = SystemClock.uptimeMillis() + 2000; var eof = false
            while (!eof && SystemClock.uptimeMillis() < end) { eof = channel.read(milliseconds = 5).first.eof }
            assertTrue(eof); assertFalse(channel.processObservation().childExited); assertFalse(channel.processObservation().reaped)
            close { channel.closeStep(1) }; released = true; assertTrue(channel.processObservation().reaped)
        } finally { if (!released) close { channel.closeStep(1) } }
    }
    @Test fun stopWakesAndWorkerOwnershipRemains() = fixture { helper, fd, _ ->
        val channel = ExtensionChannel.process(config(helper, fd, "hold"))
        var released = false
        try {
            ready(channel)
            val failure = AtomicReference<Throwable?>()
            val other = Thread { try { channel.processObservation() } catch (e: Throwable) { failure.set(e) } }
            other.start(); other.join(2000); assertFalse(other.isAlive); assertTrue(failure.get() is IllegalStateException)
            val nativeFailure = AtomicReference<Throwable?>()
            val raw = channel.javaClass.getDeclaredField("handle").let { it.isAccessible = true; it.getLong(channel) }
            val native = Thread {
                try {
                    val cls = Class.forName("com.hanxi.ghosterm.bridge.NativeExtensionChannel")
                    cls.getDeclaredMethod("processObservation", java.lang.Long.TYPE).invoke(cls.getField("INSTANCE").get(null), raw)
                } catch (e: Throwable) { nativeFailure.set(e) }
            }
            native.start(); native.join(2000); assertFalse(native.isAlive)
            assertTrue(nativeFailure.get() is java.lang.reflect.InvocationTargetException)
            val stopper = Thread { SystemClock.sleep(20); channel.stop() }; stopper.start()
            val start = SystemClock.uptimeMillis(); assertEquals(3, channel.read(milliseconds = 1000).first.state)
            stopper.join(2000); assertFalse(stopper.isAlive); assertTrue(SystemClock.uptimeMillis() - start < 500)
            close { channel.closeStep(1) }; released = true; assertTrue(channel.processObservation().stopping && channel.processObservation().reaped)
        } finally { if (!released) close { channel.closeStep(1) } }
    }
    @Test fun sessionAdoptionKeepsDiagnosticAndFinalExit() = fixture { helper, fd, cwd ->
        val channel = ExtensionChannel.process(config(helper, fd, "hold-silent"))
        val end = SystemClock.uptimeMillis() + 2000
        while (File(cwd, "ready").length() != 1L && SystemClock.uptimeMillis() < end) SystemClock.sleep(1)
        assertEquals("R", File(cwd, "ready").readText())
        val config = ExtensionSession.Config("org.ghosterm.process-fixture", "0.1.0", "d".repeat(64), "e".repeat(64),
            "0.1.0", required = listOf("ui.form"), available = listOf("ui.form"),
            peerKind = ExtensionSession.PeerKind.LOCAL, peerIdentity = "process-fixture")
        assertThrows(ExtensionChannelException::class.java) { ExtensionSession.adopt(channel, ExtensionSession.Role.HOST,
            config.copy(peerKind = ExtensionSession.PeerKind.SSH)) }
        val session = ExtensionSession.adopt(channel, ExtensionSession.Role.HOST, config)
        var released = false
        try {
            assertThrows(IllegalStateException::class.java) { channel.processObservation() }
            assertFalse(session.processObservation().childExited)
            close { session.closeStep(1) }; released = true; val i = session.processObservation()
            assertTrue(i.reaped && i.childExited); assertEquals(37, i.exitCode)
            assertArrayEquals(byteArrayOf(68, 0, -1, 10), i.diagnostic)
        } finally { if (!released) close { session.closeStep(1) } }
    }
    @Test fun missingApprovalMalformedTextAndNonProcessAreExplicit() = fixture { helper, fd, _ ->
        assertThrows(ExtensionChannelException::class.java) { ExtensionChannel.process(config(helper, fd, "hold").copy(executionAuthorized = false)) }
        assertThrows(IllegalArgumentException::class.java) { ExtensionChannel.process(config(helper, fd, "hold").copy(arguments = listOf(helper, "bad\u0000argument"))) }
        assertThrows(java.nio.charset.CharacterCodingException::class.java) { ExtensionChannel.process(config(helper, fd, "hold").copy(arguments = listOf(helper, "\ud800"))) }
        val descriptors = ParcelFileDescriptor.createSocketPair(); val raw = descriptors[0].detachFd()
        val channel = ExtensionChannel.adoptDescriptors(raw, raw)
        try {
            val e = assertThrows(ExtensionChannelException::class.java) { channel.processObservation() }
            assertEquals(7, e.status)
        } finally { channel.close(); descriptors[1].close() }
    }
}
