package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionChannelException
import com.hanxi.ghosterm.bridge.ExtensionSession
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
class ExtensionSessionTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun config(peer: ExtensionSession.PeerKind = ExtensionSession.PeerKind.LOCAL) = ExtensionSession.Config(
        "org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0", required = listOf("ui.form"),
        available = listOf("ui.form", "ui.chat"), peerKind = peer, peerIdentity = "fixture-native-owner")
    private fun channels(): Pair<ExtensionChannel, ExtensionChannel> {
        val f = ParcelFileDescriptor.createSocketPair(); val a = f[0].detachFd(); val b = f[1].detachFd()
        return ExtensionChannel.adoptDescriptors(a, a) to ExtensionChannel.adoptDescriptors(b, b)
    }
    private fun pair(): Pair<ExtensionSession, ExtensionSession> {
        val (a, b) = channels(); val old = id(a)
        val h = ExtensionSession.adopt(a, ExtensionSession.Role.HOST, config())
        val e = ExtensionSession.adopt(b, ExtensionSession.Role.EXECUTOR, config())
        assertThrows(IllegalStateException::class.java) { a.info() }
        assertThrows(InvocationTargetException::class.java) { native(false, "info", arrayOf(java.lang.Long.TYPE), old) }
        assertNotEquals(old, id(h)); a.stop(); b.stop(); return h to e
    }
    private fun close(s: ExtensionSession) { repeat(64) { if (s.closeStep()) return }; error("Session remains draining") }
    private fun wait(h: ExtensionSession, e: ExtensionSession, phase: ExtensionSession.Phase) {
        repeat(2000) { h.step(); e.step(); if (h.info().phase == phase) return }; error("Session did not reach $phase")
    }
    private fun id(value: Any): Long = value.javaClass.getDeclaredField("handle").let { it.isAccessible = true; it.getLong(value) }
    private fun native(session: Boolean, name: String, types: Array<Class<*>>, vararg values: Any?): Any? {
        val cls = Class.forName("com.hanxi.ghosterm.bridge.NativeExtension" + if (session) "Session" else "Channel")
        return cls.getDeclaredMethod(name, *types).invoke(cls.getField("INSTANCE").get(null), *values)
    }
    @Test fun consentLeaseAndRevocationRemainScoped() {
        val (h, e) = pair()
        try {
            wait(h, e, ExtensionSession.Phase.CONSENT); val i = h.info()
            assertEquals(ExtensionSession.Decision.UNKNOWN, i.decision); assertNull(h.lease()); assertEquals("fixture-native-owner", i.peerIdentity)
            assertEquals(0, h.decide(i.consentToken, ExtensionSession.Decision.UNKNOWN)); assertNull(h.lease())
            assertEquals(1, h.decide("0".repeat(32), ExtensionSession.Decision.ALLOW)); assertEquals(ExtensionSession.Decision.UNKNOWN, h.info().decision)
            assertEquals(0, h.decide(i.consentToken, ExtensionSession.Decision.ALLOW)); assertNull(h.lease())
            wait(h, e, ExtensionSession.Phase.ACTIVE); val l = h.lease()!!; val other = e.lease()!!
            assertEquals(ExtensionSession.Access.ALLOWED, h.access(l, "ui.form")); assertEquals(ExtensionSession.Access.UNSUPPORTED, h.access(l, "ui.chat"))
            assertEquals(ExtensionSession.Access.DENIED, h.access(other, "ui.form")); assertEquals(1, h.decide(i.consentToken, ExtensionSession.Decision.ALLOW))
            h.revoke(); assertEquals(ExtensionSession.Access.REVOKED, h.access(l, "ui.form")); assertNull(h.lease())
            wait(h, e, ExtensionSession.Phase.ENDED); assertEquals(ExtensionSession.End.REVOKED, h.info().end); assertEquals(ExtensionSession.End.PEER, e.info().end)
        } finally { close(h); close(e) }
    }
    @Test fun deniedSessionPreservesItsDecision() {
        val (h, e) = pair()
        try { wait(h, e, ExtensionSession.Phase.CONSENT); assertEquals(0, h.decide(h.info().consentToken, ExtensionSession.Decision.DENY))
            wait(h, e, ExtensionSession.Phase.ENDED); assertEquals(ExtensionSession.Decision.DENY, h.info().decision)
            assertEquals(ExtensionSession.End.DENIED, e.info().end); assertNull(h.lease()); h.stop(); assertEquals(ExtensionSession.End.DENIED, h.info().end)
        } finally { close(h); close(e) }
    }
    @Test fun failedAdoptionRetainsOriginalChannel() {
        val (a, b) = channels()
        try {
            assertThrows(ExtensionChannelException::class.java) { ExtensionSession.adopt(a, ExtensionSession.Role.HOST, config(ExtensionSession.PeerKind.SSH)) }
            assertThrows(ExtensionChannelException::class.java) { ExtensionSession.adopt(a, ExtensionSession.Role.HOST, config().copy(sdkVersion = "2.0.0")) }
            assertThrows(IllegalStateException::class.java) { ExtensionSession.adopt(a, ExtensionSession.Role.HOST, config().copy(pluginId = "bad\u0000id")) }
            assertEquals(1, a.info().phase); assertEquals(0L, a.info().sent); assertEquals(1, a.write(byteArrayOf(1)).count)
            assertThrows(ExtensionChannelException::class.java) { ExtensionSession.adopt(a, ExtensionSession.Role.HOST, config()) }; assertEquals(1, a.info().buffered)
        } finally { a.close(); b.close() }
    }
    @Test fun crossInstanceTicketIsRefused() {
        val (h, e) = pair(); val (h2, e2) = pair()
        try { wait(h, e, ExtensionSession.Phase.CONSENT); wait(h2, e2, ExtensionSession.Phase.CONSENT)
            assertEquals(1, h2.decide(h.info().consentToken, ExtensionSession.Decision.ALLOW)); assertEquals(ExtensionSession.Decision.UNKNOWN, h2.info().decision)
        } finally { close(h); close(e); close(h2); close(e2) }
    }
    @Test fun workerOwnershipAndCrossThreadStop() {
        val (h, e) = pair()
        try {
            val error = AtomicReference<Throwable?>(); val t = Thread {
                try { native(true, "step", arrayOf(java.lang.Long.TYPE, Integer.TYPE), id(h), 0) } catch (x: Throwable) { error.set(x) }
            }; t.start(); t.join(3000); assertFalse(t.isAlive); assertTrue(error.get() is InvocationTargetException)
            instrumentation.runOnMainSync { assertThrows(IllegalStateException::class.java) { h.info() }; h.stop() }
            assertEquals(ExtensionSession.End.REVOKED, h.info().end); assertNull(h.lease())
        } finally { close(h); close(e) }
    }
    @Test fun staleChannelAndSessionGenerationsCannotStopNewOwner() {
        val (h, e) = pair(); val old = id(h); close(h); close(e)
        val (h2, e2) = pair()
        try {
            assertNotEquals(old, id(h2)); native(true, "stop", arrayOf(java.lang.Long.TYPE), old)
            assertEquals(ExtensionSession.End.NONE, h2.info().end)
            assertThrows(InvocationTargetException::class.java) { native(true, "info", arrayOf(java.lang.Long.TYPE), old) }
        } finally { close(h2); close(e2) }
    }
    @Test fun adoptedSessionsRetainQuotaUntilActualRelease() {
        val owners = mutableListOf<ExtensionSession>()
        try {
            repeat(8) { val (h, e) = pair(); owners += h; owners += e }
            val f = ParcelFileDescriptor.createSocketPair()
            try {
                assertThrows(IllegalStateException::class.java) { ExtensionChannel.adoptDescriptors(f[0].fd, f[0].fd) }
                assertTrue(f[0].fd >= 0); android.system.Os.fstat(f[0].fileDescriptor)
            } finally { f.forEach { it.close() } }
        } finally { owners.forEach { close(it) }; owners.clear() }
        try { repeat(8) { val (h, e) = pair(); owners += h; owners += e }; assertEquals(16, owners.size) }
        finally { owners.forEach { close(it) } }
    }
    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000; socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray())
            val stream = DataInputStream(socket.getInputStream()); val n = stream.readInt(); check(n in 1..1048576)
            val bytes = ByteArray(n); stream.readFully(bytes); JSONObject(bytes.toString(Charsets.UTF_8))
        }
    }
    private fun network(label: String, deny: Boolean) {
        val payload = fixture; val entry = payload.getJSONObject(label); val ports = entry.getJSONArray("ports")
        val dir = File(instrumentation.targetContext.cacheDir, "extension-session-${System.nanoTime()}"); check(dir.mkdir())
        val known = File(dir, "known_hosts"); known.writeText(entry.getString("known"))
        val hops = (0 until ports.length()).map { i -> ExtensionChannel.Hop("route-hop-$i.invalid", ports.getInt(i), payload.getString("user"), known.path, if (i == 0) null else "127.0.0.1") }
        val channel = ExtensionChannel.ssh(hops, 4, byteArrayOf(127, 0, 0, 1) + ByteArray(12), bootstrap = payload.getString("bootstrap"))
        var session: ExtensionSession? = null
        try {
            val end = SystemClock.elapsedRealtime() + 25000
            while (channel.step(5) != 1) { val i = channel.info(); check(i.phase == 0 && SystemClock.elapsedRealtime() < end)
                if (i.challenge != 0L) assertEquals(0, channel.supply(i.hop, i.challenge, 2, payload.getString("pem").toByteArray())) }
            val old = id(channel); val s = ExtensionSession.adopt(channel, ExtensionSession.Role.HOST, config(ExtensionSession.PeerKind.SSH)); session = s
            assertThrows(IllegalStateException::class.java) { channel.info() }; assertNotEquals(old, id(s)); var decided = false; var active = false
            while (s.info().phase != ExtensionSession.Phase.ENDED) {
                s.step(5); val i = s.info(); check(SystemClock.elapsedRealtime() < end)
                if (i.phase == ExtensionSession.Phase.CONSENT) { assertFalse(decided); assertNull(s.lease()); assertEquals(ExtensionSession.PeerKind.SSH, i.peerKind)
                    assertEquals(0, s.decide(i.consentToken, if (deny) ExtensionSession.Decision.DENY else ExtensionSession.Decision.ALLOW)); decided = true }
                if (i.phase == ExtensionSession.Phase.ACTIVE) { assertEquals(ExtensionSession.Access.ALLOWED, s.access(s.lease()!!, "ui.form")); active = true; s.revoke() }
            }
            assertTrue(decided); assertEquals(!deny, active); assertEquals(if (deny) ExtensionSession.End.DENIED else ExtensionSession.End.REVOKED, s.info().end); assertNull(s.lease())
        } finally { if (session != null) close(session) else channel.close(); check(dir.deleteRecursively()) }
    }
    @Test fun twoHopActualConsent() = network("two", false)
    @Test fun threeHopActualConsent() = network("three", false)
    @Test fun twoHopActualDenial() = network("two", true)
}
