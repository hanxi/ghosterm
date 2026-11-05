package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.security.MessageDigest

/** Compiled only in the explicit enabled profile; never an empty/skipped test
 * in default discovery. SDK assets belong to the test APK, not ordinary App. */
@RunWith(AndroidJUnit4::class)
class ExtensionRuntimeTest {
    private data class PairOwner(val h: ExtensionSession, val e: ExtensionSession, val hl: ExtensionSession.Lease, val el: ExtensionSession.Lease)
    private fun pair(): PairOwner {
        assertEquals(0, ExtensionRuntime.supportStatus())
        val f = ParcelFileDescriptor.createSocketPair(); val a = f[0].detachFd(); val b = f[1].detachFd()
        val config = ExtensionSession.Config("org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0", required = listOf("ui.form"), available = listOf("ui.form"), peerKind = ExtensionSession.PeerKind.LOCAL, peerIdentity = "runtime-worker", methods = listOf(
            ExtensionSession.Method("fixture.echo", "ui.form", ExtensionSession.Role.EXECUTOR, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
            ExtensionSession.Method("fixture.ask", "ui.form", ExtensionSession.Role.HOST, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT)))
        val h = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(a, a), ExtensionSession.Role.HOST, config)
        val e = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(b, b), ExtensionSession.Role.EXECUTOR, config)
        repeat(2000) { h.step(); e.step(); if (h.info().phase == ExtensionSession.Phase.CONSENT) assertEquals(0, h.decide(h.info().consentToken, ExtensionSession.Decision.ALLOW))
            if (h.info().phase == ExtensionSession.Phase.ACTIVE) return PairOwner(h, e, h.lease()!!, e.lease()!!) }
        error("runtime handshake")
    }
    private fun module(name: String, source: ByteArray): ExtensionRuntime.Module = ExtensionRuntime.Module(name, source,
        MessageDigest.getInstance("SHA-256").digest(source).joinToString("") { "%02x".format(it.toInt() and 255) })
    private fun config(plugin: String = echo, role: ExtensionSession.Role = ExtensionSession.Role.EXECUTOR, cpu: Int = 500): ExtensionRuntime.Config {
        val assets = InstrumentationRegistry.getInstrumentation().context.assets
        val modules = listOf("index.js", "codec.js", "session.js", "wire-contract.js").map { name ->
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
    companion object { private const val echo = "import {object,text,integer} from '@ghosterm/plugin-sdk';export function activate(ctx){const codec=object({text:text(40),count:integer(0,100)});ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{await Promise.resolve();c.check();return v});return {dispose(){}}}" }
}
