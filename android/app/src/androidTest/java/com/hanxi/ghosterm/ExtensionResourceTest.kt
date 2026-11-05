package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.runner.RunWith
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.ExtensionResourceStore
import org.junit.Assert.*
import org.junit.Test
import java.io.File
import java.io.DataInputStream
import java.net.Socket
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONObject

@RunWith(AndroidJUnit4::class)
class ExtensionResourceTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val ok = ExtensionResourceStore.Status.OK
    private fun store(): Pair<ExtensionResourceStore, File> {
        val dir = File(instrumentation.targetContext.cacheDir, "resource-${System.nanoTime()}"); check(dir.mkdir()); Os.chmod(dir.path, 448)
        val fd = Os.open(dir.path, OsConstants.O_RDONLY or OsConstants.O_CLOEXEC, 0)
        try { return ParcelFileDescriptor.dup(fd).use { ExtensionResourceStore(it.fd) } to dir } finally { Os.close(fd) }
    }
    private fun config(peer: ExtensionSession.PeerKind = ExtensionSession.PeerKind.LOCAL, granted: Boolean = true) = ExtensionSession.Config(
        "org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0", required = if (granted) listOf("ui.form", "resource.receive") else listOf("ui.form"),
        available = listOf("ui.form", "ui.chat", "resource.receive"), peerKind = peer, peerIdentity = "native-resource-owner", methods = listOf(
            ExtensionSession.Method("fixture.echo", "ui.form", ExtensionSession.Role.EXECUTOR, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.ask", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.done", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.denied", "ui.chat", ExtensionSession.Role.EXECUTOR)))
    private fun descriptor(size: Int, unknown: Boolean = false) = ExtensionResourceStore.Descriptor(
        when (size) { 3 -> "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"; 0 -> "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"; else -> "31a1f9dea0169551092d05e8bf4a446228c8c3eb4c9b713c66adcb7fd53c89be" },
        "application/octet-stream", "../展示/\"snapshot\\.bin", if (unknown) 0 else size.toLong(), !unknown)
    private data class PairOwner(val h: ExtensionSession, val e: ExtensionSession, val hl: ExtensionSession.Lease, val el: ExtensionSession.Lease)
    private fun pair(hs: ExtensionResourceStore, es: ExtensionResourceStore, granted: Boolean = true): PairOwner {
        val fd = ParcelFileDescriptor.createSocketPair(); val a = fd[0].detachFd(); val b = fd[1].detachFd()
        val h = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(a, a), ExtensionSession.Role.HOST, config(granted = granted), hs)
        val e = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(b, b), ExtensionSession.Role.EXECUTOR, config(granted = granted), es)
        repeat(2000) { h.step(); e.step(); if (h.info().phase == ExtensionSession.Phase.CONSENT) assertEquals(0, h.decide(h.info().consentToken, ExtensionSession.Decision.ALLOW))
            if (h.info().phase == ExtensionSession.Phase.ACTIVE) return PairOwner(h, e, h.lease()!!, e.lease()!!) }; error("resource handshake")
    }
    private fun close(s: ExtensionSession) { repeat(64) { if (s.closeStep()) return }; error("Session still draining") }
    private fun pump(p: PairOwner) { p.h.step(); p.e.step() }
    private fun expected(at: Int, size: Int, total: Int): ByteArray = if (total == 3) "abc".toByteArray().copyOfRange(at, at + size) else ByteArray(size) { ((at + it) % 251).toByte() }
    private fun send(p: PairOwner, reverse: Boolean = false, size: Int = 3, unknown: Boolean = false): ExtensionResourceStore.Snapshot {
        val s = if (reverse) p.e else p.h; val l = if (reverse) p.el else p.hl; val receiver = if (reverse) p.h else p.e; val rl = if (reverse) p.hl else p.el
        val a = s.sendResource(l, descriptor(size, unknown)); assertEquals(ok, a.status); val id = a.handle!!; var at = 0
        repeat(50000) { if (at < size) { val bytes = expected(at, minOf(768, size - at), size); val (st, accepted) = s.writeResource(l, id, bytes); assertTrue(st == ok || st == ExtensionResourceStore.Status.WAIT); at += accepted; bytes.fill(0); pump(p) } }
        assertEquals(size, at); assertEquals(ok, s.finishResource(l, id, true)); var confirmed = false
        for (i in 0 until 50000) { pump(p); val r = s.resourceResult(l, id).second!!; if (r.confirmed) { assertEquals(ExtensionSession.TransferState.SUCCEEDED, r.state); assertEquals(size.toLong(), r.accepted); confirmed = true; break } }
        assertTrue(confirmed); assertEquals(ok, s.forgetResource(l, id)); val (st, snap) = receiver.takeResource(rl); assertEquals(ok, st); return snap!!
    }
    private fun verify(snap: ExtensionResourceStore.Snapshot, total: Int = 3) {
        val (st, info) = snap.info(); assertEquals(ok, st); assertTrue(info!!.published); assertEquals(total.toLong(), info.received); assertEquals(descriptor(total).sha256, snap.descriptor.sha256); assertEquals("../展示/\"snapshot\\.bin", snap.descriptor.displayName)
        var at = 0
        while (true) { val (status, bytes) = snap.read(at.toLong()); assertEquals(ok, status); if (bytes.isEmpty()) break
            assertArrayEquals(expected(at, bytes.size, total), bytes); at += bytes.size }; assertEquals(total, at)
    }
    @Test fun retainedSnapshotOutlivesSessionAndRemainsCharged() {
        val (hs, hd) = store(); val (es, ed) = store(); val p = pair(hs, es)
        var snapshot: ExtensionResourceStore.Snapshot? = null; var retained: ExtensionResourceStore.Snapshot? = null; var original: Throwable? = null
        try {
            assertEquals(ExtensionResourceStore.Status.WAIT, hs.closeStep()); assertEquals(1L, hs.usage().sessions)
            val snap = send(p); snapshot = snap; val (st, copy) = snap.retain(); retained = copy
            assertEquals(ok, st); assertEquals(2L, snap.info().second!!.references); snap.close()
            close(p.h); close(p.e); assertEquals(0L, es.usage().sessions); assertEquals(ExtensionResourceStore.Status.WAIT, es.closeStep()); assertEquals(3L, es.usage().reservedBytes)
            verify(copy!!); copy.close(); assertEquals(ok, es.closeStep()); assertEquals(ok, hs.closeStep())
        } catch (error: Throwable) { original = error; throw error }
        finally {
            // Preserve the actual target assertion if a deliberately corrupted
            // JNI ref also prevents cleanup; surface cleanup as suppressed data.
            listOf<() -> Unit>({ snapshot?.close() }, { retained?.close() }, { p.h.close() }, { p.e.close() }, { hs.close() }, { es.close() },
                { check(hd.deleteRecursively() && ed.deleteRecursively()) }).forEach { action ->
                try { action() } catch (cleanup: Throwable) { val first = original; if (first != null) first.addSuppressed(cleanup) else throw cleanup }
            }
        }
    }
    @Test fun largeUnknownAndReverseEmpty() {
        val (hs, hd) = store(); val (es, ed) = store(); val p = pair(hs, es)
        try { send(p, size = 262144, unknown = true).use { verify(it, 262144) }; send(p, reverse = true, size = 0).use { verify(it, 0) } }
        finally { close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively()) }
    }
    @Test fun queuedCancellationAndLostAreDifferent() {
        val (hs, hd) = store(); val (es, ed) = store(); val p = pair(hs, es)
        try {
            val id = p.h.sendResource(p.hl, descriptor(3)).handle!!; val extra = p.h.sendResource(p.hl, descriptor(3)); assertEquals(ExtensionResourceStore.Status.WAIT, extra.status); assertNull(extra.handle)
            assertEquals(ok, p.h.cancelResource(p.hl, id)); val r = p.h.resourceResult(p.hl, id).second!!; assertEquals(ExtensionSession.TransferState.CANCELLED, r.state); assertFalse(r.confirmed); assertEquals(ok, p.h.forgetResource(p.hl, id))
            val pending = p.h.sendResource(p.hl, descriptor(3)).handle!!; repeat(100) { pump(p) }; p.h.stop(); p.h.step(); assertEquals(ExtensionSession.TransferState.UNKNOWN, p.h.resourceResult(p.hl, pending).second!!.state)
        } finally { close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively()) }
    }
    @Test fun scopeAndCapabilityDenialDoesNotReserve() {
        val (hs, hd) = store(); val (es, ed) = store(); val p = pair(hs, es, granted = false)
        try { assertEquals(ExtensionResourceStore.Status.DENIED, p.h.sendResource(p.hl, descriptor(3)).status); assertEquals(ExtensionResourceStore.Status.DENIED, p.h.takeResource(p.el).first); assertEquals(0L, es.usage().resources) }
        finally { close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively()) }
    }
    @Test fun resourceStoreRejectsOtherWorkerAndUi() {
        val (store, dir) = store()
        try {
            val error = AtomicReference<Throwable?>(); val t = Thread { try { store.usage() } catch (e: Throwable) { error.set(e) } }; t.start(); t.join(3000); assertFalse(t.isAlive); assertTrue(error.get() is IllegalStateException)
            instrumentation.runOnMainSync { assertThrows(IllegalStateException::class.java) { store.usage() } }; assertEquals(0L, store.usage().sessions)
        } finally { store.close(); check(dir.deleteRecursively()) }
    }
    private fun reply(s: ExtensionSession, l: ExtensionSession.Lease, q: ExtensionSession.RPCRequest) {
        assertEquals(ExtensionSession.RPCStatus.OK, s.checkRequest(l, q.ticket))
        assertEquals(ExtensionSession.RPCStatus.OK, s.respond(l, q.ticket, "{\"kind\":\"success\",\"value\":".toByteArray() + q.input + "}".toByteArray()))
    }
    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000; socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray()); val stream = DataInputStream(socket.getInputStream())
            val n = stream.readInt(); check(n in 1..1048576); val bytes = ByteArray(n); stream.readFully(bytes); JSONObject(bytes.toString(Charsets.UTF_8))
        }
    }
    private fun network(label: String) {
        val (resourceStore, resourceDir) = store()
        val f = fixture; val route = f.getJSONObject(label); val ports = route.getJSONArray("ports")
        val dir = File(instrumentation.targetContext.cacheDir, "rpc-${System.nanoTime()}"); check(dir.mkdir()); val known = File(dir, "known_hosts"); known.writeText(route.getString("known"))
        val hops = (0 until ports.length()).map { i -> ExtensionChannel.Hop("route-hop-$i.invalid", ports.getInt(i), f.getString("user"), known.path, if (i == 0) null else "127.0.0.1") }
        val c = ExtensionChannel.ssh(hops, 4, byteArrayOf(127, 0, 0, 1) + ByteArray(12), bootstrap = f.getString("bootstrap")); var session: ExtensionSession? = null
        try {
            val end = SystemClock.elapsedRealtime() + 30000
            while (c.step(5) != 1) { val i = c.info(); check(i.phase == 0 && SystemClock.elapsedRealtime() < end); if (i.challenge != 0L) assertEquals(0, c.supply(i.hop, i.challenge, 2, f.getString("pem").toByteArray())) }
            val s = ExtensionSession.adopt(c, ExtensionSession.Role.HOST, config(ExtensionSession.PeerKind.SSH), resourceStore); session = s
            var completed = 0; var handled = 0; var pending: Long? = null; var done: ExtensionSession.RPCRequest? = null; var answered = false
            var resourceId: Long? = null; var written = 0; var resourceFinished = false; var resourceReceived = false; var resourceReady = false
            while (s.info().phase != ExtensionSession.Phase.ENDED) {
                s.step(5); val i = s.info(); check(SystemClock.elapsedRealtime() < end)
                if (i.phase == ExtensionSession.Phase.CONSENT) assertEquals(0, s.decide(i.consentToken, ExtensionSession.Decision.ALLOW))
                if (i.phase != ExtensionSession.Phase.ACTIVE) continue
                val l = s.lease()!!
                if (resourceId == null) resourceId = s.sendResource(l, descriptor(262144)).handle!!
                val resource = resourceId
                if (written < 262144) {
                    val bytes = expected(written, minOf(768, 262144 - written), 262144)
                    val (st, accepted) = s.writeResource(l, resource, bytes); assertTrue(st == ok || st == ExtensionResourceStore.Status.WAIT); written += accepted
                }
                if (written == 262144 && !resourceFinished) { assertEquals(ok, s.finishResource(l, resource, true)); resourceFinished = true }
                if (!resourceReceived) { val (st, snap) = s.takeResource(l); assertTrue(st == ok || st == ExtensionResourceStore.Status.WAIT)
                    if (snap != null) { verify(snap, 262144); snap.close(); resourceReceived = true } }
                val resourceResult = s.resourceResult(l, resource).second!!
                if (resourceResult.confirmed) { assertEquals(ExtensionSession.TransferState.SUCCEEDED, resourceResult.state); resourceReady = resourceReceived }
                if (completed < 80 && pending == null) pending = s.request(l, "fixture.echo", "{\"text\":\"bridge\"}".toByteArray()).handle!!
                val (status, q) = s.takeRequest(l); assertTrue(status == ExtensionSession.RPCStatus.OK || status == ExtensionSession.RPCStatus.WAIT)
                if (q != null) { if (q.method == "fixture.done") { assertNull(done); done = q } else { assertEquals("fixture.ask", q.method); handled++; reply(s, l, q) } }
                val id = pending
                if (id != null) { val r = s.requestResult(l, id).second!!; if (r.confirmed) { assertEquals(ExtensionSession.RPCState.SUCCEEDED, r.state); assertEquals(ExtensionSession.RPCStatus.OK, s.forgetRequest(l, id)); pending = null; completed++ } }
                val terminal = done
                if (terminal != null && !answered && completed == 80 && handled == 80 && resourceReady) { reply(s, l, terminal); answered = true }
            }
            assertEquals(80, completed); assertEquals(80, handled); assertTrue(answered); assertTrue(resourceReady); assertEquals(ExtensionSession.End.LOST, s.info().end)
            s.revoke(); assertEquals(ExtensionSession.End.LOST, s.info().end)
        } finally { if (session == null) c.close() else close(session); resourceStore.close(); check(resourceDir.deleteRecursively()); check(dir.deleteRecursively()) }
    }
    @Test fun twoHopBidirectionalResourcesAndRPC() = network("two")
    @Test fun threeHopBidirectionalResourcesAndRPC() = network("three")
}
