package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionSession
import java.io.DataInputStream
import java.io.File
import java.net.Socket
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ExtensionRPCTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun methods() = listOf(
        ExtensionSession.Method("fixture.echo", "ui.form", ExtensionSession.Role.EXECUTOR, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
        ExtensionSession.Method("fixture.ask", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
        ExtensionSession.Method("fixture.done", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
        ExtensionSession.Method("fixture.denied", "ui.chat", ExtensionSession.Role.EXECUTOR))
    private fun config(peer: ExtensionSession.PeerKind = ExtensionSession.PeerKind.LOCAL, enabled: Boolean = true) = ExtensionSession.Config(
        "org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0", required = listOf("ui.form"),
        available = listOf("ui.form", "ui.chat"), peerKind = peer, peerIdentity = "fixture-native-rpc", methods = if (enabled) methods() else null)
    private data class PairOwner(val h: ExtensionSession, val e: ExtensionSession, val hl: ExtensionSession.Lease, val el: ExtensionSession.Lease)
    private fun pair(enabled: Boolean = true): PairOwner {
        val f = ParcelFileDescriptor.createSocketPair(); val a = f[0].detachFd(); val b = f[1].detachFd()
        val h = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(a, a), ExtensionSession.Role.HOST, config(enabled = enabled))
        val e = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(b, b), ExtensionSession.Role.EXECUTOR, config(enabled = enabled))
        repeat(2000) { h.step(); e.step(); if (h.info().phase == ExtensionSession.Phase.CONSENT) { assertEquals(0, h.decide(h.info().consentToken, ExtensionSession.Decision.ALLOW)); return@repeat }
            if (h.info().phase == ExtensionSession.Phase.ACTIVE) return PairOwner(h, e, h.lease()!!, e.lease()!!) }
        error("RPC handshake")
    }
    private fun close(s: ExtensionSession) { repeat(64) { if (s.closeStep()) return }; error("RPC remains draining") }
    private fun pump(p: PairOwner) { p.h.step(); p.e.step() }
    private fun take(receiver: ExtensionSession, lease: ExtensionSession.Lease, p: PairOwner): ExtensionSession.RPCRequest {
        repeat(2000) { pump(p); val (status, q) = receiver.takeRequest(lease); assertTrue(status == ExtensionSession.RPCStatus.OK || status == ExtensionSession.RPCStatus.WAIT); if (q != null) return q }; error("RPC take")
    }
    private fun reply(s: ExtensionSession, l: ExtensionSession.Lease, q: ExtensionSession.RPCRequest) {
        assertEquals(ExtensionSession.RPCStatus.OK, s.checkRequest(l, q.ticket))
        assertEquals(ExtensionSession.RPCStatus.OK, s.respond(l, q.ticket, "{\"kind\":\"success\",\"value\":".toByteArray() + q.input + "}".toByteArray()))
    }
    private fun complete(s: ExtensionSession, l: ExtensionSession.Lease, id: Long, p: PairOwner): ExtensionSession.RPCResult {
        repeat(2000) { pump(p); val (status, r) = s.requestResult(l, id); assertEquals(ExtensionSession.RPCStatus.OK, status); if (r != null && r.confirmed) return r }; error("RPC result")
    }
    @Test fun bidirectionalSnapshotsSurviveForgetAndClose() {
        val p = pair()
        try {
            val expected = "{\"text\":\"中文\",\"n\":17}".toByteArray(); val input = expected.copyOf()
            val a = p.h.request(p.hl, "fixture.echo", input); assertEquals(ExtensionSession.RPCStatus.OK, a.status); val id = a.handle!!; input.fill(0)
            val q = take(p.e, p.el, p); assertArrayEquals(expected, q.input); reply(p.e, p.el, q)
            val r = complete(p.h, p.hl, id, p); assertTrue(r.confirmed); assertEquals(ExtensionSession.RPCState.SUCCEEDED, r.state)
            assertEquals(ExtensionSession.RPCStatus.OK, p.h.forgetRequest(p.hl, id)); assertEquals(ExtensionSession.RPCStatus.INVALID, p.h.requestResult(p.hl, id).first)
            val other = p.e.request(p.el, "fixture.ask", expected).handle!!; val reverse = take(p.h, p.hl, p); reply(p.h, p.hl, reverse)
            assertEquals(ExtensionSession.RPCState.SUCCEEDED, complete(p.e, p.el, other, p).state)
            close(p.h); close(p.e); assertArrayEquals(expected, q.input); assertTrue(r.outcome.toString(Charsets.UTF_8).contains("中文"))
        } finally { p.h.close(); p.e.close() }
    }
    @Test fun queuedCancellationAndBackpressureAreExplicit() {
        val p = pair()
        try {
            val handles = (0 until 64).map { p.h.request(p.hl, "fixture.echo", "{}".toByteArray()).handle!! }
            val extra = p.h.request(p.hl, "fixture.echo", "{}".toByteArray()); assertEquals(ExtensionSession.RPCStatus.WAIT, extra.status); assertNull(extra.handle)
            handles.forEach { id ->
                assertEquals(ExtensionSession.RPCStatus.OK, p.h.cancelRequest(p.hl, id)); val r = p.h.requestResult(p.hl, id).second!!
                assertEquals(ExtensionSession.RPCState.CANCELLED, r.state); assertFalse(r.confirmed); assertTrue(r.cancelRequested); assertTrue(r.requestId.isEmpty())
                assertEquals(ExtensionSession.RPCStatus.OK, p.h.forgetRequest(p.hl, id)) }
        } finally { close(p.h); close(p.e) }
    }
    @Test fun lostPendingRequestRemainsUnknown() {
        val p = pair()
        try { val id = p.h.request(p.hl, "fixture.echo", "{}".toByteArray()).handle!!; take(p.e, p.el, p); p.h.stop()
            val r = p.h.requestResult(p.hl, id).second!!; assertEquals(ExtensionSession.RPCState.UNKNOWN, r.state); assertFalse(r.confirmed)
            assertTrue(r.requestId.isNotEmpty()); assertTrue(r.outcome.toString(Charsets.UTF_8).contains("unknown"))
        } finally { close(p.h); close(p.e) }
    }
    @Test fun confirmedResultSurvivesStop() {
        val p = pair()
        try { val id = p.h.request(p.hl, "fixture.echo", "{}".toByteArray()).handle!!; val q = take(p.e, p.el, p); reply(p.e, p.el, q)
            val before = complete(p.h, p.hl, id, p); p.h.stop(); val after = p.h.requestResult(p.hl, id).second!!
            assertEquals(ExtensionSession.RPCState.SUCCEEDED, after.state); assertTrue(after.confirmed); assertArrayEquals(before.outcome, after.outcome)
        } finally { close(p.h); close(p.e) }
    }
    @Test fun validationScopeAndDisabledFeature() {
        val p = pair()
        try {
            for (bytes in listOf("[]".toByteArray(), "{\"x\":1,\"x\":2}".toByteArray(), byteArrayOf(-1))) assertEquals(ExtensionSession.RPCStatus.INVALID, p.h.request(p.hl, "fixture.echo", bytes).status)
            assertThrows(IllegalStateException::class.java) { p.h.request(p.hl, "fixture.echo", byteArrayOf(123, 125, 0)) }
            assertEquals(ExtensionSession.RPCStatus.DENIED, p.h.request(p.hl, "fixture.denied", "{}".toByteArray()).status)
            assertEquals(ExtensionSession.RPCStatus.UNSUPPORTED, p.h.request(p.hl, "fixture.ask", "{}".toByteArray()).status)
            assertEquals(ExtensionSession.RPCStatus.DENIED, p.h.takeRequest(p.el).first)
            assertEquals(ExtensionSession.RPCStatus.DENIED, p.h.requestResult(p.el, 1).first)
            val other = pair(false)
            try { assertEquals(ExtensionSession.RPCStatus.UNSUPPORTED, other.h.request(other.hl, "fixture.echo", "{}".toByteArray()).status) }
            finally { close(other.h); close(other.e) }
        } finally { close(p.h); close(p.e) }
    }
    @Test fun rpcRejectsUiAndOtherWorkers() {
        val p = pair()
        try {
            val error = AtomicReference<Throwable?>(); val thread = Thread { try { p.h.takeRequest(p.hl) } catch (e: Throwable) { error.set(e) } }
            thread.start(); thread.join(3000); assertFalse(thread.isAlive); assertTrue(error.get() is IllegalStateException)
            instrumentation.runOnMainSync { assertThrows(IllegalStateException::class.java) { p.h.requestResult(p.hl, 1) } }
            assertEquals(ExtensionSession.End.NONE, p.h.info().end)
        } finally { close(p.h); close(p.e) }
    }
    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000; socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray()); val stream = DataInputStream(socket.getInputStream())
            val n = stream.readInt(); check(n in 1..1048576); val bytes = ByteArray(n); stream.readFully(bytes); JSONObject(bytes.toString(Charsets.UTF_8))
        }
    }
    private fun network(label: String) {
        val f = fixture; val route = f.getJSONObject(label); val ports = route.getJSONArray("ports")
        val dir = File(instrumentation.targetContext.cacheDir, "rpc-${System.nanoTime()}"); check(dir.mkdir()); val known = File(dir, "known_hosts"); known.writeText(route.getString("known"))
        val hops = (0 until ports.length()).map { i -> ExtensionChannel.Hop("route-hop-$i.invalid", ports.getInt(i), f.getString("user"), known.path, if (i == 0) null else "127.0.0.1") }
        val c = ExtensionChannel.ssh(hops, 4, byteArrayOf(127, 0, 0, 1) + ByteArray(12), bootstrap = f.getString("bootstrap")); var session: ExtensionSession? = null
        try {
            val end = SystemClock.elapsedRealtime() + 30000
            while (c.step(5) != 1) { val i = c.info(); check(i.phase == 0 && SystemClock.elapsedRealtime() < end); if (i.challenge != 0L) assertEquals(0, c.supply(i.hop, i.challenge, 2, f.getString("pem").toByteArray())) }
            val s = ExtensionSession.adopt(c, ExtensionSession.Role.HOST, config(ExtensionSession.PeerKind.SSH)); session = s
            var completed = 0; var handled = 0; var pending: Long? = null; var done: ExtensionSession.RPCRequest? = null; var answered = false
            while (s.info().phase != ExtensionSession.Phase.ENDED) {
                s.step(5); val i = s.info(); check(SystemClock.elapsedRealtime() < end)
                if (i.phase == ExtensionSession.Phase.CONSENT) assertEquals(0, s.decide(i.consentToken, ExtensionSession.Decision.ALLOW))
                if (i.phase != ExtensionSession.Phase.ACTIVE) continue
                val l = s.lease()!!
                if (completed < 80 && pending == null) pending = s.request(l, "fixture.echo", "{\"text\":\"bridge\"}".toByteArray()).handle!!
                val (status, q) = s.takeRequest(l); assertTrue(status == ExtensionSession.RPCStatus.OK || status == ExtensionSession.RPCStatus.WAIT)
                if (q != null) { if (q.method == "fixture.done") { assertNull(done); done = q } else { assertEquals("fixture.ask", q.method); handled++; reply(s, l, q) } }
                val id = pending
                if (id != null) { val r = s.requestResult(l, id).second!!; if (r.confirmed) { assertEquals(ExtensionSession.RPCState.SUCCEEDED, r.state); assertEquals(ExtensionSession.RPCStatus.OK, s.forgetRequest(l, id)); pending = null; completed++ } }
                val terminal = done
                if (terminal != null && !answered && completed == 80 && handled == 80) { reply(s, l, terminal); answered = true }
            }
            assertEquals(80, completed); assertEquals(80, handled); assertTrue(answered); assertEquals(ExtensionSession.End.LOST, s.info().end)
            s.revoke(); assertEquals(ExtensionSession.End.LOST, s.info().end)
        } finally { if (session == null) c.close() else close(session); check(dir.deleteRecursively()) }
    }
    @Test fun twoHopBidirectionalRPC() = network("two")
    @Test fun threeHopBidirectionalRPC() = network("three")
}
