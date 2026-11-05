package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.ExtensionResourceStore
import com.hanxi.ghosterm.bridge.ExtensionFileSource
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.DataInputStream
import java.net.Socket
import java.nio.charset.CharacterCodingException
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONObject

@RunWith(AndroidJUnit4::class)
class ExtensionSourceTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val ok = ExtensionFileSource.Status.OK
    private fun directory(): File = File(instrumentation.targetContext.cacheDir, "source-${System.nanoTime()}").also { check(it.mkdir()); Os.chmod(it.path, 448) }
    private fun store(): Pair<ExtensionResourceStore, File> {
        val dir = directory(); val fd = Os.open(dir.path, OsConstants.O_RDONLY or OsConstants.O_CLOEXEC, 0)
        try { return ParcelFileDescriptor.dup(fd).use { ExtensionResourceStore(it.fd) } to dir } finally { Os.close(fd) }
    }
    private fun config(peer: ExtensionSession.PeerKind = ExtensionSession.PeerKind.LOCAL, fileGrant: Boolean = true) = ExtensionSession.Config(
        "org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0", required = if (fileGrant) listOf("ui.form", "resource.receive", "file.readScoped") else listOf("ui.form", "resource.receive"),
        available = listOf("ui.form", "ui.chat", "resource.receive", "file.readScoped"), peerKind = peer, peerIdentity = "native-scoped-source", methods = listOf(
            ExtensionSession.Method("fixture.echo", "ui.form", ExtensionSession.Role.EXECUTOR, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.ask", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.done", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.denied", "ui.chat", ExtensionSession.Role.EXECUTOR)))
    private data class PairOwner(val h: ExtensionSession, val e: ExtensionSession, val hl: ExtensionSession.Lease, val el: ExtensionSession.Lease)
    private fun pair(hs: ExtensionResourceStore, es: ExtensionResourceStore, fileGrant: Boolean = true): PairOwner {
        val fd = ParcelFileDescriptor.createSocketPair(); val a = fd[0].detachFd(); val b = fd[1].detachFd()
        val h = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(a, a), ExtensionSession.Role.HOST, config(fileGrant = fileGrant), hs)
        val e = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(b, b), ExtensionSession.Role.EXECUTOR, config(fileGrant = fileGrant), es)
        repeat(2000) { h.step(); e.step(); if (h.info().phase == ExtensionSession.Phase.CONSENT) assertEquals(0, h.decide(h.info().consentToken, ExtensionSession.Decision.ALLOW))
            if (h.info().phase == ExtensionSession.Phase.ACTIVE) return PairOwner(h, e, h.lease()!!, e.lease()!!) }; error("source handshake")
    }
    private fun close(s: ExtensionSession) { s.close() }
    private fun pump(p: PairOwner) { p.h.step(); p.e.step() }
    private fun file(dir: File, size: Int = 3) { File(dir, "file.bin").writeBytes(if (size == 3) "abc".toByteArray() else ByteArray(size) { (it % 251).toByte() }) }
    private fun source(s: ExtensionSession, l: ExtensionSession.Lease, dir: File, path: String = "file.bin", limit: Long = 262144): ExtensionFileSource.Admission {
        val fd = Os.open(dir.path, OsConstants.O_RDONLY or OsConstants.O_CLOEXEC, 0)
        try { return ParcelFileDescriptor.dup(fd).use { s.openSource(l, it.fd, path, byteLimit = limit) } } finally { Os.close(fd) }
    }
    private fun ready(s: ExtensionFileSource) { repeat(100) { val phase = s.step(); assertTrue(phase == ExtensionFileSource.Phase.HASHING || phase == ExtensionFileSource.Phase.READY); if (phase == ExtensionFileSource.Phase.READY) return }; error("source hash") }
    private fun verify(snap: ExtensionResourceStore.Snapshot, size: Int) {
        assertEquals("file.bin", snap.descriptor.displayName); assertEquals(size.toLong(), snap.descriptor.size); assertTrue(snap.info().second!!.published)
        var at = 0
        while (true) { val (st, bytes) = snap.read(at.toLong()); assertEquals(ExtensionResourceStore.Status.OK, st); if (bytes.isEmpty()) break
            assertArrayEquals(if (size == 3) "abc".toByteArray().copyOfRange(at, at + bytes.size) else ByteArray(bytes.size) { ((at + it) % 251).toByte() }, bytes); at += bytes.size }; assertEquals(size, at)
    }
    @Test fun fileReceiptAndBorrowPinSurviveParentClose() {
        val (hs, hd) = store(); val (es, ed) = store(); val input = directory(); file(input); val p = pair(hs, es)
        var src: ExtensionFileSource? = null; var snapshot: ExtensionResourceStore.Snapshot? = null
        try {
            val admission = source(p.h, p.hl, input); assertEquals(ok, admission.status); val s = admission.source!!; src = s
            assertEquals(ExtensionFileSource.Status.WAIT, s.send()); ready(s); assertEquals(3L, s.info().hashed); assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", s.info().descriptor.sha256)
            assertEquals(ok, s.send()); var completed = false
            for (i in 0 until 5000) { s.step(); pump(p); if (s.info().phase == ExtensionFileSource.Phase.SUCCEEDED) { completed = true; break } }; assertTrue(completed)
            val saved = s.info(); assertTrue(saved.hasTransfer && saved.transfer.confirmed); assertEquals(3L, saved.readBytes); assertEquals(3L, saved.accepted); assertEquals(ExtensionSession.TransferState.SUCCEEDED, saved.transfer.state)
            val (status, snap) = p.e.takeResource(p.el); assertEquals(ExtensionResourceStore.Status.OK, status); snapshot = snap!!; verify(snap, 3)
            assertFalse(p.h.closeStep()); assertEquals(ok, s.closeStep()); close(p.h); close(p.e); assertTrue(saved.transfer.confirmed); verify(snap, 3)
        } finally { src?.close(); snapshot?.close(); close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively() && input.deleteRecursively()) }
    }
    @Test fun changeAndCancelBeforeSendDoNotPublish() {
        val (hs, hd) = store(); val (es, ed) = store(); val input = directory(); file(input); val p = pair(hs, es)
        try {
            source(p.h, p.hl, input).source!!.use { s -> ready(s); File(input, "file.bin").writeText("changed"); assertEquals(ExtensionFileSource.Status.CHANGED, s.send()); assertEquals(ExtensionFileSource.Status.CHANGED, s.info().error); assertEquals(ok, s.info().cleanupError); assertFalse(s.info().hasTransfer) }
            source(p.h, p.hl, input).source!!.use { s -> s.cancel(); assertEquals(ExtensionFileSource.Phase.STOPPED, s.info().phase); assertEquals(ExtensionFileSource.Status.CANCELLED, s.info().error); assertFalse(s.info().hasTransfer) }
            assertEquals(0L, es.usage().resources)
        } finally { close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively() && input.deleteRecursively()) }
    }
    @Test fun scopePathsAndMissingFileGrantAreExplicit() {
        val (hs, hd) = store(); val (es, ed) = store(); val input = directory(); file(input); val p = pair(hs, es)
        try {
            assertEquals(ExtensionFileSource.Status.DENIED, source(p.h, p.el, input).status); assertEquals(ExtensionFileSource.Status.DENIED, source(p.h, p.hl, input, "../file.bin").status)
            assertEquals(ExtensionFileSource.Status.INVALID, source(p.h, p.hl, input, File(input, "file.bin").absolutePath).status)
            assertEquals(ExtensionFileSource.Status.NOT_FOUND, source(p.h, p.hl, input, "missing").status); assertEquals(ExtensionFileSource.Status.LIMIT, source(p.h, p.hl, input, limit = 2).status)
        } finally { close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively()) }
        val (other, od) = store(); val (remote, rd) = store(); val missing = pair(other, remote, fileGrant = false)
        try { assertEquals(ExtensionFileSource.Status.UNSUPPORTED, source(missing.h, missing.hl, input).status) }
        finally { close(missing.h); close(missing.e); other.close(); remote.close(); check(od.deleteRecursively() && rd.deleteRecursively() && input.deleteRecursively()) }
    }
    @Test fun closedSessionSettlesUnknownWithoutFreeingSourceEarly() {
        val (hs, hd) = store(); val (es, ed) = store(); val input = directory(); file(input, 262144); val p = pair(hs, es); val s = source(p.h, p.hl, input).source!!
        try {
            ready(s); assertEquals(ok, s.send()); repeat(40) { s.step(); pump(p) }; assertFalse(p.h.closeStep()); s.step()
            assertEquals(ExtensionFileSource.Phase.UNCERTAIN, s.info().phase); assertEquals(ExtensionFileSource.Status.UNKNOWN, s.info().error); assertFalse(s.info().transfer.confirmed)
            assertEquals(ok, s.closeStep()); close(p.h)
        } finally { s.close(); close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively() && input.deleteRecursively()) }
    }
    @Test fun malformedUtf16AndNulDoNotSelectReplacementFile() {
        val (hs, hd) = store(); val (es, ed) = store(); val input = directory(); file(input); File(input, "?").writeText("replacement"); val p = pair(hs, es)
        var unexpected: ExtensionFileSource? = null
        try {
            try { assertThrows(CharacterCodingException::class.java) { unexpected = source(p.h, p.hl, input, "\uD800").source } } finally { unexpected?.close() }
            assertThrows(IllegalArgumentException::class.java) { source(p.h, p.hl, input, "file\u0000.bin") }
            assertTrue(p.h.closeStep())
        } finally { close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively() && input.deleteRecursively()) }
    }
    @Test fun sourceRejectsOtherWorkerAndUi() {
        val (hs, hd) = store(); val (es, ed) = store(); val input = directory(); file(input); val p = pair(hs, es); val s = source(p.h, p.hl, input).source!!
        try {
            val error = AtomicReference<Throwable?>(); val thread = Thread { try { s.info() } catch (t: Throwable) { error.set(t) } }; thread.start(); thread.join(3000); assertFalse(thread.isAlive); assertTrue(error.get() is IllegalStateException)
            instrumentation.runOnMainSync { assertThrows(IllegalStateException::class.java) { s.step() } }; assertEquals(0L, s.info().hashed)
        } finally { s.close(); close(p.h); close(p.e); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively() && input.deleteRecursively()) }
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
            var source: ExtensionFileSource? = null; var sourceDone = false; var resourceReceived = false; var resourceReady = false
            val input = directory(); file(input, 262144)
            while (s.info().phase != ExtensionSession.Phase.ENDED) {
                s.step(5); val i = s.info(); check(SystemClock.elapsedRealtime() < end)
                if (i.phase == ExtensionSession.Phase.CONSENT) assertEquals(0, s.decide(i.consentToken, ExtensionSession.Decision.ALLOW))
                if (i.phase != ExtensionSession.Phase.ACTIVE) continue
                val l = s.lease()!!
                if (source == null && !sourceDone) source = source(s, l, input).source!!
                val active = source
                if (active != null) {
                    val phase = active.step()
                    if (phase == ExtensionFileSource.Phase.READY) assertEquals(ok, active.send())
                    if (phase == ExtensionFileSource.Phase.SUCCEEDED) {
                        val value = active.info(); assertTrue(value.transfer.confirmed); assertEquals(ExtensionSession.TransferState.SUCCEEDED, value.transfer.state)
                        assertEquals(262144L, value.hashed); assertEquals(262144L, value.readBytes); assertEquals(262144L, value.accepted)
                        assertEquals(ok, active.closeStep()); source = null; sourceDone = true; check(input.deleteRecursively())
                    }
                }
                if (!resourceReceived) { val (st, snap) = s.takeResource(l); assertTrue(st == ExtensionResourceStore.Status.OK || st == ExtensionResourceStore.Status.WAIT)
                    if (snap != null) { verify(snap, 262144); snap.close(); resourceReceived = true } }
                resourceReady = sourceDone && resourceReceived
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
    @Test fun twoHopBidirectionalSourcesAndRPC() = network("two")
    @Test fun threeHopBidirectionalSourcesAndRPC() = network("three")
}
