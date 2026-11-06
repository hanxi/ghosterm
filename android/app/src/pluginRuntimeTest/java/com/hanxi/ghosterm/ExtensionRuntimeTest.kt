package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import com.hanxi.ghosterm.bridge.ExtensionResourceStore
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.security.MessageDigest
import java.io.File

/** Compiled only in the explicit enabled profile; never an empty/skipped test
 * in default discovery. SDK assets belong to the test APK, not ordinary App. */
@RunWith(AndroidJUnit4::class)
class ExtensionRuntimeTest {
    private data class PairOwner(val h: ExtensionSession, val e: ExtensionSession, val hl: ExtensionSession.Lease, val el: ExtensionSession.Lease)
    private fun pair(hs: ExtensionResourceStore? = null, es: ExtensionResourceStore? = null): PairOwner {
        assertEquals(0, ExtensionRuntime.supportStatus())
        val f = ParcelFileDescriptor.createSocketPair(); val a = f[0].detachFd(); val b = f[1].detachFd()
        val grants = if (hs != null) listOf("ui.form", "resource.receive", "file.readScoped") else listOf("ui.form")
        val config = ExtensionSession.Config("org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0", required = grants, available = grants, peerKind = ExtensionSession.PeerKind.LOCAL, peerIdentity = "runtime-worker", methods = listOf(
            ExtensionSession.Method("fixture.echo", "ui.form", ExtensionSession.Role.EXECUTOR, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.ask", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT)))
        val h = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(a, a), ExtensionSession.Role.HOST, config, hs)
        val e = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(b, b), ExtensionSession.Role.EXECUTOR, config, es)
        repeat(2000) { h.step(); e.step(); if (h.info().phase == ExtensionSession.Phase.CONSENT) assertEquals(0, h.decide(h.info().consentToken, ExtensionSession.Decision.ALLOW))
            if (h.info().phase == ExtensionSession.Phase.ACTIVE) return PairOwner(h, e, h.lease()!!, e.lease()!!) }
        error("runtime handshake")
    }
    private fun module(name: String, source: ByteArray): ExtensionRuntime.Module = ExtensionRuntime.Module(name, source,
        MessageDigest.getInstance("SHA-256").digest(source).joinToString("") { "%02x".format(it.toInt() and 255) })
    private fun config(plugin: String = echo, role: ExtensionSession.Role = ExtensionSession.Role.EXECUTOR, cpu: Int = 500, resources: Boolean = false): ExtensionRuntime.Config {
        val assets = InstrumentationRegistry.getInstrumentation().context.assets
        val names = listOf("index.js", "codec.js", "session.js", "wire-contract.js") + if (resources) listOf("resources.js") else emptyList()
        val modules = names.map { name ->
            module("sdk:/$name", assets.open("runtime-sdk/$name").use { it.readBytes() }) }.toMutableList()
        modules += module("plugin:/entry.js", plugin.toByteArray())
        return ExtensionRuntime.Config(modules, "plugin:/entry.js", role, cpuMilliseconds = cpu)
    }
    @Test fun adoptionAndActualPluginRPC() {
        val p = pair(); val r = ExtensionRuntime.adopt(p.e, config())
        try {
            try { p.e.info(); fail("old Session ID survived") } catch (_: IllegalStateException) { }
            p.e.stop(); assertEquals(ExtensionRuntime.Phase.CREATED, r.info().phase)
            val handle = p.h.request(p.hl, "fixture.echo", "{\"text\":\"中文\",\"count\":17}".toByteArray(), 10000).handle!!
            var complete = false
            for (i in 0 until 5000) { p.h.step(); r.step(); if (p.h.requestResult(p.hl, handle).second?.confirmed == true) { complete = true; break } }
            assertTrue(complete); val result = p.h.requestResult(p.hl, handle).second!!; assertEquals(ExtensionSession.RPCState.SUCCEEDED, result.state); assertTrue(result.outcome.toString(Charsets.UTF_8).contains("中文"))
            assertEquals(ExtensionSession.RPCStatus.OK, p.h.forgetRequest(p.hl, handle)); val saved = r.info(); assertEquals(ExtensionRuntime.Phase.ACTIVE, saved.phase); assertTrue(saved.jobs > 0uL && saved.moduleBytes > 0uL)
        } finally { r.close(); p.h.close(); p.e.close() }
    }
    @Test fun existingRequestRejectsAdoption() {
        val p = pair(); val request = p.e.request(p.el, "fixture.ask", "{}".toByteArray()).handle!!
        try {
            try { ExtensionRuntime.adopt(p.e, config()).close(); fail("request owner lost") } catch (_: Exception) { }
            assertEquals(ExtensionSession.Phase.ACTIVE, p.e.info().phase)
            assertEquals(ExtensionSession.RPCStatus.OK, p.e.cancelRequest(p.el, request)); assertEquals(ExtensionSession.RPCStatus.OK, p.e.forgetRequest(p.el, request))
        } finally { p.e.close(); p.h.close() }
    }
    @Test fun cpuAndImportFailuresRemainDistinct() {
        for ((plugin, failure) in listOf("export function activate(){let x=0;for(let i=0;i<20000000;i++)x+=i;return {dispose(){}}}" to ExtensionRuntime.Failure.CPU,
            "import 'host:/native';export function activate(){return {dispose(){}}}" to ExtensionRuntime.Failure.SCRIPT)) {
            val p = pair(); val r = ExtensionRuntime.adopt(p.e, config(plugin, cpu = if (failure == ExtensionRuntime.Failure.CPU) 10 else 500))
            try { repeat(100) { if (r.step() == ExtensionRuntime.Phase.FAILED) return@repeat }; assertEquals(ExtensionRuntime.Phase.FAILED, r.info().phase); assertEquals(failure, r.info().error) }
            finally { r.close(); p.h.close() }
        }
    }
    @Test fun stopFromMainAndOtherWorkerRejected() {
        val p = pair(); val r = ExtensionRuntime.adopt(p.e, config())
        try {
            repeat(100) { r.step() }
            var unexpected = false
            InstrumentationRegistry.getInstrumentation().runOnMainSync {
                try { r.info(); unexpected = true } catch (_: IllegalStateException) { }
                r.stop()
            }
            assertFalse("UI accessed runtime IO", unexpected)
            assertTrue(r.step() == ExtensionRuntime.Phase.STOPPED && r.info().error == ExtensionRuntime.Failure.CANCELLED)
        } finally { r.close(); p.h.close() }
    }
    @Test fun digestAndRoleFailuresKeepSession() {
        val p = pair()
        try {
            try { ExtensionRuntime.adopt(p.e, config(role = ExtensionSession.Role.HOST)).close(); fail("wrong role") } catch (_: Exception) { }
            val c = config(); val m = c.modules.toMutableList(); m[m.lastIndex] = m.last().copy(sha256 = "0".repeat(64))
            try { ExtensionRuntime.adopt(p.e, c.copy(modules = m)).close(); fail("wrong digest") } catch (_: Exception) { }
            assertEquals(ExtensionSession.Phase.ACTIVE, p.e.info().phase)
        } finally { p.e.close(); p.h.close() }
    }
    @Test fun generationalIdTransfer() {
        val p = pair()
        fun handle(value: Any): Long { val field = value.javaClass.getDeclaredField("handle"); field.isAccessible = true; return field.getLong(value) }
        val old = handle(p.e); val r = ExtensionRuntime.adopt(p.e, config())
        try { assertNotEquals("Session generation reused for runtime", old, handle(r)) }
        finally { r.close(); p.h.close(); p.e.close() }
    }
    @Test fun explicitCloseReleasesAndCopiedInfoSurvives() {
        val p = pair(); val r = ExtensionRuntime.adopt(p.e, config())
        repeat(100) { r.step() }; val saved = r.info(); r.close(); r.close()
        assertEquals(ExtensionRuntime.Phase.ACTIVE, saved.phase); assertTrue(saved.steps > 0uL)
        try { r.info(); fail("released runtime was readable") } catch (_: IllegalStateException) { }
        p.h.close(); p.e.close()
    }
    private fun directory(): File {
        val dir = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "runtime-resource-${System.nanoTime()}")
        check(dir.mkdir()); Os.chmod(dir.path, 448); return dir
    }
    private fun store(): Pair<ExtensionResourceStore, File> {
        val dir = directory(); val fd = Os.open(dir.path, OsConstants.O_RDONLY or OsConstants.O_CLOEXEC, 0)
        return try { ParcelFileDescriptor.dup(fd).use { ExtensionResourceStore(it.fd) } to dir } finally { Os.close(fd) }
    }
    @Test fun scopedPublicationCopiesRootAndWaitsForReceipt() {
        val (hs, hd) = store(); val (es, ed) = store(); val dir = directory(); val expected = ByteArray(262144) { (it % 251).toByte() }; File(dir, "file.bin").writeBytes(expected)
        val fd = Os.open(dir.path, OsConstants.O_RDONLY or OsConstants.O_CLOEXEC, 0); val p = pair(hs, es)
        val plugin = "export async function activate(ctx){if(ctx.resources.workspaces[0]!=='workspace')throw Error('root');const c=ctx.resources.publish({root:'workspace',relativePath:'file.bin',mediaType:'application/octet-stream'},{byteLimit:'262144',timeoutMs:10000});const o=await c.outcome;if(o.kind!=='success'||o.value.size.bytes!=='262144')throw Error('receipt');return {dispose(){}}}"
        val c = config(plugin, resources = true)
        try { ExtensionRuntime.adopt(p.e, c.copy(roots = listOf(ExtensionRuntime.WorkspaceRoot("workspace", -1, 262144)))).close(); fail("invalid root accepted") } catch (_: Exception) { }
        assertEquals(ExtensionSession.Phase.ACTIVE, p.e.info().phase)
        val r = ParcelFileDescriptor.dup(fd).use { ExtensionRuntime.adopt(p.e, c.copy(roots = listOf(ExtensionRuntime.WorkspaceRoot("workspace", it.fd, 262144)))) }; Os.close(fd)
        try {
            for (i in 0 until 10000) { p.h.step(); if (r.step() == ExtensionRuntime.Phase.ACTIVE) break }
            assertEquals(ExtensionRuntime.Phase.ACTIVE, r.info().phase)
            val (status, value) = p.h.takeResource(p.hl); assertEquals(ExtensionResourceStore.Status.OK, status); val snap = value!!
            assertEquals(262144L, snap.descriptor.size); var at = 0
            while (true) { val (st, block) = snap.read(at.toLong()); assertEquals(ExtensionResourceStore.Status.OK, st); if (block.isEmpty()) break; assertArrayEquals(expected.copyOfRange(at, at + block.size), block); at += block.size }
            assertEquals(expected.size, at); snap.close()
        } finally { r.close(); p.h.close(); p.e.close(); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively() && dir.deleteRecursively()) }
    }
    @Test fun resourceReadRejectsCopiedReferenceAndReleasesPin() {
        val (hs, hd) = store(); val (es, ed) = store(); val p = pair(hs, es)
        val plugin = "import {object,text,integer} from '@ghosterm/plugin-sdk';export function activate(ctx){const codec=object({text:text(40),count:integer(0,100)});ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{const t=ctx.resources.take();if(t.status!=='ok'||ctx.resources.open({...t.ref},{byteLimit:'3',timeoutMs:1000}).status!=='invalid')throw Error('ref');const o=ctx.resources.open(t.ref,{byteLimit:'3',timeoutMs:1000});if(o.status!=='ok')throw Error('open');const a=await o.stream.read();c.check();const b=await o.stream.read();c.check();if(a.kind!=='success'||String(a.value.bytes)!=='97,98,99'||b.kind!=='success'||!b.value.done||ctx.resources.release(t.ref)!=='ok')throw Error('read');return v});return {dispose(){}}}"
        val r = ExtensionRuntime.adopt(p.e, config(plugin, resources = true))
        try {
            repeat(100) { p.h.step(); r.step() }
            val d = ExtensionResourceStore.Descriptor("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "application/octet-stream", "file.bin", 3)
            val transfer = p.h.sendResource(p.hl, d).handle!!; assertEquals(3, p.h.writeResource(p.hl, transfer, "abc".toByteArray()).second); assertEquals(ExtensionResourceStore.Status.OK, p.h.finishResource(p.hl, transfer, true))
            for (i in 0 until 1000) { p.h.step(); r.step(); if (p.h.resourceResult(p.hl, transfer).second?.confirmed == true) break }
            assertEquals(ExtensionResourceStore.Status.OK, p.h.forgetResource(p.hl, transfer))
            val handle = p.h.request(p.hl, "fixture.echo", "{\"text\":\"read\",\"count\":17}".toByteArray()).handle!!
            for (i in 0 until 1000) { p.h.step(); r.step(); if (p.h.requestResult(p.hl, handle).second?.confirmed == true) break }
            assertEquals(ExtensionSession.RPCState.SUCCEEDED, p.h.requestResult(p.hl, handle).second?.state); assertEquals(ExtensionSession.RPCStatus.OK, p.h.forgetRequest(p.hl, handle)); assertEquals(0L, es.usage().resources)
        } finally { r.close(); p.h.close(); p.e.close(); hs.close(); es.close(); check(hd.deleteRecursively() && ed.deleteRecursively()) }
    }
    @Test fun rootsWithoutResourceSessionAreUnsupported() {
        val p = pair()
        try {
            try { ExtensionRuntime.adopt(p.e, config().copy(roots = listOf(ExtensionRuntime.WorkspaceRoot("workspace", -1, 3)))).close(); fail("root accepted without resource Session") }
            catch (error: com.hanxi.ghosterm.bridge.ExtensionChannelException) { assertEquals(7, error.status) }
            assertEquals(ExtensionSession.Phase.ACTIVE, p.e.info().phase)
        } finally { p.e.close(); p.h.close() }
    }
    companion object { private const val echo = "import {object,text,integer} from '@ghosterm/plugin-sdk';export function activate(ctx){const codec=object({text:text(40),count:integer(0,100)});ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{await Promise.resolve();c.check();return v});return {dispose(){}}}" }
}
