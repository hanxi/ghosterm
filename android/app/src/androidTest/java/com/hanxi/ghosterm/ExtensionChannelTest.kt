package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.Os
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import java.io.DataInputStream
import java.io.File
import java.lang.reflect.InvocationTargetException
import java.net.Socket
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ExtensionChannelTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray())
            val stream = DataInputStream(socket.getInputStream()); val n = stream.readInt()
            check(n in 1..1048576); val bytes = ByteArray(n); stream.readFully(bytes)
            JSONObject(bytes.toString(Charsets.UTF_8))
        }
    }
    private fun close(channel: ExtensionChannel) {
        val deadline = SystemClock.elapsedRealtime() + 3000
        while (!channel.closeStep()) { check(SystemClock.elapsedRealtime() < deadline); Thread.yield() }
    }
    private fun pair(): Pair<ExtensionChannel, ExtensionChannel> {
        val fds = ParcelFileDescriptor.createSocketPair()
        val a = fds[0].detachFd(); val b = fds[1].detachFd()
        return ExtensionChannel.adoptDescriptors(a, a) to ExtensionChannel.adoptDescriptors(b, b)
    }
    private fun read(channel: ExtensionChannel, count: Int): ByteArray {
        val out = ByteArray(count); var at = 0; val deadline = SystemClock.elapsedRealtime() + 15000
        while (at < count) {
            val (io, bytes) = channel.read(minOf(count - at, 16384), 5)
            check(io.data || io.wait); bytes.copyInto(out, at); at += bytes.size
            check(SystemClock.elapsedRealtime() < deadline)
        }
        return out
    }
    private fun id(channel: ExtensionChannel): Long = channel.javaClass.getDeclaredField("handle").let {
        it.isAccessible = true; it.getLong(channel)
    }
    private fun native(name: String, types: Array<Class<*>>, vararg args: Any?): Any? {
        val cls = Class.forName("com.hanxi.ghosterm.bridge.NativeExtensionChannel")
        return cls.getDeclaredMethod(name, *types).invoke(cls.getField("INSTANCE").get(null), *args)
    }
    @Test fun temporaryArrayIsCopiedBeforeReturning() {
        val (a, b) = pair()
        try {
            val expected = ByteArray(16384) { (it % 251).toByte() }; val input = expected.copyOf()
            assertEquals(16384, a.write(input).count); input.fill(0x55)
            assertEquals(0L, a.info().sent); assertEquals(16384, a.info().buffered)
            assertTrue(a.write(input, 0, 1).wait)
            while (a.info().buffered > 0) assertEquals(1, a.step(5))
            assertArrayEquals(expected, read(b, expected.size)); assertEquals(16384L, a.info().sent)
            assertEquals(3, b.write(byteArrayOf(0, -1, 1)).count)
            while (b.info().buffered > 0) b.step(5)
            assertArrayEquals(byteArrayOf(0, -1, 1), read(a, 3)); assertNull(a.info().exitStatus)
        } finally { close(a); close(b) }
    }
    @Test fun closeDropsQueuedDataAndRejectsStaleNativeId() {
        val (a, b) = pair(); val old = id(a)
        try { assertEquals(3, a.write(byteArrayOf(1, 2, 3)).count); close(a); assertTrue(b.read(1, 20).first.eof)
            val error = assertThrows(InvocationTargetException::class.java) {
                native("info", arrayOf(java.lang.Long.TYPE), old)
            }
            assertTrue(error.cause is IllegalStateException)
            val (c, d) = pair()
            try {
                assertNotEquals(old, id(c)); native("stop", arrayOf(java.lang.Long.TYPE), old)
                assertEquals(1, c.info().phase)
                assertThrows(IllegalStateException::class.java) { a.read() }
            } finally { close(c); close(d) }
        } finally { close(b) }
    }
    @Test fun workerAndUiOwnershipAreEnforced() {
        val (a, b) = pair()
        try {
            val result = AtomicReference<Throwable?>()
            val other = Thread {
                try { native("step", arrayOf(java.lang.Long.TYPE, Integer.TYPE), id(a), 0) }
                catch (e: Throwable) { result.set(e) }
            }; other.start(); other.join(3000); assertFalse(other.isAlive)
            assertTrue(result.get() is InvocationTargetException)
            instrumentation.runOnMainSync {
                assertThrows(IllegalStateException::class.java) { a.read() }
                assertThrows(IllegalStateException::class.java) { ExtensionChannel.adoptDescriptors(-1, -1) }
                a.stop()
            }
            assertEquals(3, a.step()); assertEquals(3, a.info().phase); assertEquals(3, a.read(1).first.state)
        } finally { close(a); close(b) }
    }
    @Test fun failedParameterCopiesReleaseRegistryReservation() {
        for (i in 0 until 20) {
            val hops = listOf(ExtensionChannel.Hop("bad\u0000host", 22, "fixture"))
            assertThrows(IllegalStateException::class.java) {
                ExtensionChannel.ssh(hops, 4, byteArrayOf(127, 0, 0, 1) + ByteArray(12), bootstrap = "fixed")
            }
        }
        val result = runCatching { pair() }; assertTrue("JNI reservation must be reusable after rejected inputs", result.isSuccess)
        val (a, b) = result.getOrThrow(); close(a); close(b)
    }
    @Test fun rejectedCredentialArraysAreErased() {
        val (a, b) = pair()
        try {
            val key = byteArrayOf(1, 2, 3); val passphrase = byteArrayOf(4, 5)
            assertThrows(IllegalArgumentException::class.java) { a.supply(0, 1, 2, key, passphrase) }
            assertArrayEquals(ByteArray(3), key); assertArrayEquals(ByteArray(2), passphrase)
            assertEquals(7, a.decide(0, 1, ExtensionChannel.Decision.ACCEPT))
        } finally { close(a); close(b) }
    }
    @Test fun capacityRetainsFailedAndLiveOwnersUntilClose() {
        val channels = ArrayList<ExtensionChannel>(); val peers = ArrayList<ParcelFileDescriptor>()
        try {
            repeat(16) { val fds = ParcelFileDescriptor.createSocketPair(); val fd = fds[0].detachFd()
                channels += ExtensionChannel.adoptDescriptors(fd, fd); peers += fds[1] }
            val extra = ParcelFileDescriptor.createSocketPair()
            try {
                assertThrows(IllegalStateException::class.java) {
                    ExtensionChannel.adoptDescriptors(extra[0].fd, extra[0].fd)
                }
                assertTrue(Os.fstat(extra[0].fileDescriptor).st_mode != 0)
                close(channels.removeAt(0))
                val fd = extra[0].detachFd(); val accepted = ExtensionChannel.adoptDescriptors(fd, fd)
                close(accepted)
            } finally { extra.forEach { it.close() } }
        } finally { channels.forEach { close(it) }; peers.forEach { it.close() } }
    }
    private fun network(label: String, unknown: Boolean = false, deny: Boolean = false, forwardDenied: Boolean = false, retry: Boolean = false) {
        val payload = fixture; val entry = payload.getJSONObject(label); val ports = entry.getJSONArray("ports")
        val dir = File(instrumentation.targetContext.cacheDir, "extension-channel-${System.nanoTime()}")
        check(dir.mkdir())
        val known = File(dir, "known_hosts")
        var pins = entry.getString("known")
        if (unknown) pins = pins.trimEnd().lines().dropLast(1).joinToString("\n") + "\n"
        known.writeText(pins)
        val hops = (0 until ports.length()).map { i ->
            ExtensionChannel.Hop("route-hop-$i.invalid", ports.getInt(i), payload.getString("user"),
                known.path, if (i == 0) null else "127.0.0.1")
        }
        val channel = ExtensionChannel.ssh(hops, 4, byteArrayOf(127, 0, 0, 1) + ByteArray(12), bootstrap = payload.getString("bootstrap"))
        try {
            val deadline = SystemClock.elapsedRealtime() + 25000; var supplies = 0; var lastChallenge = 0L; var badChallenge = 0L
            while (true) {
                val phase = channel.step(5); val info = channel.info()
                check(SystemClock.elapsedRealtime() < deadline)
                if (info.challenge != 0L) {
                    assertTrue(info.challenge > lastChallenge); lastChallenge = info.challenge
                    assertEquals(1, channel.decide(info.hop + 1, info.challenge, ExtensionChannel.Decision.ACCEPT))
                    val auth = info.authentication[info.hop]
                    if (auth.phase == 2) {
                        assertEquals(2, auth.host); assertTrue(auth.fingerprint.startsWith("SHA256:"))
                        assertEquals(0, channel.decide(info.hop, info.challenge, ExtensionChannel.Decision.UNKNOWN))
                        assertEquals(info.challenge, channel.info().challenge)
                        assertEquals(if (deny) 5 else 0, channel.decide(info.hop, info.challenge, if (deny) ExtensionChannel.Decision.REJECT else ExtensionChannel.Decision.ACCEPT))
                        assertTrue(channel.decide(info.hop, info.challenge, ExtensionChannel.Decision.ACCEPT) != 0)
                    } else {
                        assertEquals(8, auth.phase)
                        assertTrue(auth.host == 1 || (auth.host == 2 && auth.decision == ExtensionChannel.Decision.ACCEPT.nativeValue))
                        val rejected = payload.getString("pem").toByteArray()
                        assertEquals(1, channel.supply(info.hop, info.challenge + 1, 2, rejected))
                        assertTrue(rejected.all { it == 0.toByte() })
                        val bad = retry && info.hop == 0 && badChallenge == 0L
                        if (bad) badChallenge = info.challenge
                        else if (retry && info.hop == 0) {
                            assertEquals(4, auth.error); assertTrue(info.challenge > badChallenge)
                            assertEquals(1, channel.supply(0, badChallenge, 2, payload.getString("pem").toByteArray()))
                        }
                        val key = payload.getString(if (bad) "wrongPem" else "pem").toByteArray()
                        assertEquals(0, channel.supply(info.hop, info.challenge, 2, key))
                        assertTrue(key.all { it == 0.toByte() }); supplies++
                    }
                }
                if (phase == 1 || phase == 2) break
            }
            if (deny || forwardDenied) {
                assertEquals(2, channel.info().phase); assertEquals(if (deny) 5 else 3, channel.info().error)
                if (forwardDenied) assertEquals(2, channel.info().failureStage)
                return
            }
            assertEquals(1, channel.info().phase); assertEquals(ports.length() + if (retry) 1 else 0, supplies)
            assertArrayEquals("ready".toByteArray(), read(channel, 5))
            repeat(16) { block ->
                val input = ByteArray(16384) { ((block * 16384 + it) % 251).toByte() }
                assertEquals(input.size, channel.write(input).count)
                while (channel.info().buffered > 0) { channel.step(5); check(SystemClock.elapsedRealtime() < deadline) }
                assertArrayEquals(input, read(channel, input.size))
            }
            assertEquals(262144L, channel.info().sent); assertEquals(262149L, channel.info().received)
            assertNull(channel.info().exitStatus)
        } finally { close(channel); check(dir.deleteRecursively()) }
    }
    @Test fun twoHopActualSsh() = network("two")
    @Test fun threeHopActualSsh() = network("three")
    @Test fun unknownHostBoundDecision() = network("two", unknown = true)
    @Test fun explicitHostRejection() = network("two", unknown = true, deny = true)
    @Test fun forwardingDeniedIsVisible() = network("denied", forwardDenied = true)
    @Test fun credentialRetryRejectsOldChallenge() = network("two", retry = true)
}
