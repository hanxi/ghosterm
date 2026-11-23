package com.hanxi.ghosterm

import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.ExtensionPackage
import org.json.JSONObject
import org.json.JSONArray
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

/** Native Runtime ABI tests for the Lua build profile. */
@RunWith(AndroidJUnit4::class)
class ExtensionRuntimeTest {
    private data class PairOwner(val h: ExtensionSession, val e: ExtensionSession, val hl: ExtensionSession.Lease, val el: ExtensionSession.Lease)
    private fun pair(hs: ExtensionResourceStore? = null, es: ExtensionResourceStore? = null, custom: ExtensionSession.Config? = null): PairOwner {
        assertEquals(0, ExtensionRuntime.supportStatus())
        val f = ParcelFileDescriptor.createSocketPair(); val a = f[0].detachFd(); val b = f[1].detachFd()
        val grants = if (hs != null) listOf("ui.form", "resource.receive", "file.readScoped") else listOf("ui.form")
        val config = custom ?: ExtensionSession.Config("org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0", required = grants, available = grants, peerKind = ExtensionSession.PeerKind.LOCAL, peerIdentity = "runtime-worker", methods = listOf(
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
    private fun config(plugin: String = echo, role: ExtensionSession.Role = ExtensionSession.Role.EXECUTOR,
                       cpu: Int = 500): ExtensionRuntime.Config {
        val entry = "plugin:/entry.lua"
        return ExtensionRuntime.Config(listOf(module(entry, plugin.toByteArray())), entry, role, cpuMilliseconds = cpu)
    }
    @Test fun fixedPackageAdoptionAndRPC() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val dir = File(context.cacheDir, "package-${java.util.UUID.randomUUID()}"); check(dir.mkdir())
        val entry = "executor/中文-" + "long-component/".repeat(20) + "entry.lua"
        fun array(vararg values: Any) = JSONArray(values.toList())
        val manifest = JSONObject().put("schemaVersion", 2).put("id", "org.ghosterm.fixture").put("pluginVersion", "0.1.0")
            .put("sdkApiRange", "^1.0.0").put("bridgeProtocolRange", array(1)).put("publisher", "测试 publisher").put("license", "MIT")
            .put("commands", array(JSONObject().put("path", array("fixture")).put("title", "Fixture").put("argsSchema", "schemas/input.json")))
            .put("executor", JSONObject().put("entry", entry).put("requires", array("session.rpc"))
                .put("rpc", array("fixture.echo")))
        val invokedEcho = """
            local ctx = ...
            local args = assert(ctx.arguments)
            assert(args.first == "中文" and args.empty == "" and args.literal == "';throw Error('injected');//")
            $echo
        """.trimIndent()
        val stringSchema = JSONObject().put("type", "string").put("maxLength", 100)
        val argumentSchema = JSONObject().put("type", "object")
            .put("properties", JSONObject().put("first", stringSchema).put("empty", stringSchema).put("literal", stringSchema))
            .put("required", array("first", "empty", "literal")).put("additionalProperties", false)
            .put("x-ghosterm-cli", JSONObject().put("version", 1).put("fields", JSONObject()
                .put("first", JSONObject().put("position", 0))
                .put("empty", JSONObject().put("position", 1))
                .put("literal", JSONObject().put("position", 2))))
        val files = mapOf("manifest.json" to manifest.toString(), "README" to "fixture", "LICENSES/MIT.txt" to "fixture", "schemas/input.json" to argumentSchema.toString(), entry to invokedEcho)
        val members = JSONArray()
        for ((path, text) in files) {
            val bytes = text.toByteArray(); val file = File(dir, path); file.parentFile!!.mkdirs(); file.writeBytes(bytes)
            members.put(JSONObject().put("path", path).put("bytes", bytes.size).put("sha256", MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }))
        }
        File(dir, "integrity.json").writeText(JSONObject().put("schemaVersion", 1).put("members", members).toString())
        val fd = Os.open(dir.path, OsConstants.O_RDONLY or OsConstants.O_CLOEXEC, 0)
        val descriptor = ParcelFileDescriptor.dup(fd); Os.close(fd)
        try {
            val packageBytes = ExtensionPackage.capture(descriptor.fd)
            try {
                for (i in 0 until 1000) { if (packageBytes.step() != ExtensionPackage.Phase.CAPTURING) break }
                val host = "{\"side\":\"executor\",\"sdkVersion\":\"1.0.0\",\"protocols\":[1],\"runtimes\":[\"lua\"],\"capabilities\":[\"session.rpc\"]}"
                assertEquals(ExtensionPackage.Phase.CHECKED, packageBytes.check(host))
                val m = packageBytes.metadata(); assertEquals(entry, m.entry); assertEquals("测试 publisher", m.publisher)
                assertEquals(listOf("session.rpc"), m.requires); assertEquals(listOf(1), m.protocols)
                assertFalse(m.authorizationGranted); assertFalse(m.publisherAuthenticated)
                assertEquals(listOf(ExtensionPackage.Command("fixture", "Fixture", "schemas/input.json", emptyList())), packageBytes.commands())
                val registry = ExtensionRegistry()
                val selected = try {
                    assertEquals(ExtensionRegistry.Status.UNKNOWN, registry.put(packageBytes, ExtensionRegistry.Source.UNKNOWN))
                    assertEquals(ExtensionRegistry.Status.DENIED, registry.put(packageBytes, ExtensionRegistry.Source.DENIED))
                    assertEquals(ExtensionRegistry.Status.OK, registry.put(packageBytes, ExtensionRegistry.Source.ALLOWED))
                    assertEquals(ExtensionRegistry.Status.CONFLICT, registry.put(packageBytes, ExtensionRegistry.Source.ALLOWED))
                    assertEquals(ExtensionRegistry.Status.STALE, registry.resolve("fixture", true, m.id, "0".repeat(64)).status)
                    packageBytes.stop()
                    assertEquals(ExtensionRegistry.Status.CANCELLED, registry.put(packageBytes, ExtensionRegistry.Source.ALLOWED, m.packageDigest))
                    val selection = registry.resolve("fixture", true, m.id, m.packageDigest)
                    assertEquals(ExtensionRegistry.Status.OK, selection.status)
                    assertEquals(packageBytes.commands().single(), selection.command)
                    selection.packageBytes!!
                } catch (error: Throwable) { registry.close(); throw error }
                try {
                    assertEquals(packageBytes.info().members, selected.info().members)
                    assertEquals(packageBytes.info().bytes, selected.info().bytes)
                    assertEquals(ExtensionRegistry.Status.OK, registry.remove(m.id, true, m.packageDigest))
                    registry.close()
                    try { registry.resolve("fixture", true); fail("closed registry survived") } catch (_: IllegalStateException) { }
                val config = ExtensionSession.Config(m.id, m.version, m.packageDigest, m.packageDigest, "0.1.0", required = m.requires, available = m.requires,
                    peerKind = ExtensionSession.PeerKind.LOCAL, peerIdentity = "package-worker", methods = listOf(ExtensionSession.Method("fixture.echo", "session.rpc", ExtensionSession.Role.EXECUTOR, ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT)))
                val p = pair(custom = config)
                try {
                    val runtime = ExtensionRuntime.adoptPackage(p.e, selected, ExtensionSession.Role.EXECUTOR, cpuMilliseconds = 500, invocation = ExtensionRuntime.Invocation("fixture", listOf("中文", "", "';throw Error('injected');//")))
                    try {
                        selected.close(); packageBytes.close(); assertTrue(File(dir, entry).delete()); p.e.stop()
                        val request = p.h.request(p.hl, "fixture.echo", "{\"text\":\"固定快照\",\"count\":17}".toByteArray(), 10000).handle!!
                        for (i in 0 until 5000) { p.h.step(); runtime.step(); if (p.h.requestResult(p.hl, request).second?.confirmed == true) break }
                        assertEquals(ExtensionRuntime.Phase.ACTIVE, runtime.info().phase)
                        val result = p.h.requestResult(p.hl, request).second!!
                        assertTrue(result.confirmed); assertEquals(ExtensionSession.RPCState.SUCCEEDED, result.state)
                        assertTrue(result.outcome.toString(Charsets.UTF_8).contains("固定快照"))
                        assertEquals(ExtensionSession.RPCStatus.OK, p.h.forgetRequest(p.hl, request))
                    } finally { runtime.close() }
                } finally { p.e.close(); p.h.close() }
                } finally { selected.close(); registry.close() }
            } finally { packageBytes.close() }
        } finally { descriptor.close(); dir.deleteRecursively() }
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
            assertEquals(ExtensionSession.RPCStatus.OK, p.h.forgetRequest(p.hl, handle)); val saved = r.info(); assertEquals(ExtensionRuntime.Phase.ACTIVE, saved.phase); assertTrue(saved.steps > 0uL && saved.moduleBytes > 0uL)
        } finally { r.close(); p.h.close(); p.e.close() }
    }
    @Test fun clientInvocationContextPreservesExactFieldsThroughJni() {
        val p = pair()
        val script = """
            return { fixture = function(ctx)
                assert(ctx.context.workspaceId == "workspace-a")
                assert(ctx.context.paneId == "pane-b")
                assert(ctx.context.directory == "/owned/中文")
                return true
            end }
        """.trimIndent()
        val entry = "plugin:/context.lua"
        val invocation = ExtensionRuntime.Invocation("fixture", emptyList(),
            ExtensionRuntime.Invocation.Context("workspace-a", "pane-b", "/owned/中文"))
        val runtime = ExtensionRuntime.adopt(p.h, ExtensionRuntime.Config(
            listOf(module(entry, script.toByteArray())), entry, ExtensionSession.Role.HOST, invocation = invocation))
        try {
            repeat(20) { p.e.step(); runtime.step() }
            assertEquals(ExtensionRuntime.Failure.NONE, runtime.info().error)
            assertEquals(ExtensionRuntime.Phase.ACTIVE, runtime.info().phase)
        } finally { runtime.close(); p.e.close(); p.h.close() }
    }
    @Test fun existingRequestRejectsAdoption() {
        val p = pair(); val request = p.e.request(p.el, "fixture.ask", "{}".toByteArray()).handle!!
        try {
            try { ExtensionRuntime.adopt(p.e, config()).close(); fail("request owner lost") } catch (_: Exception) { }
            assertEquals(ExtensionSession.Phase.ACTIVE, p.e.info().phase)
            assertEquals(ExtensionSession.RPCStatus.OK, p.e.cancelRequest(p.el, request)); assertEquals(ExtensionSession.RPCStatus.OK, p.e.forgetRequest(p.el, request))
        } finally { p.e.close(); p.h.close() }
    }
    @Test fun cpuAndScriptFailuresRemainDistinct() {
        for ((plugin, failure) in listOf("while true do end" to ExtensionRuntime.Failure.CPU,
            "require('host:/native')" to ExtensionRuntime.Failure.SCRIPT)) {
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
            val admission = r.requestExecutor("fixture.echo", "{}".toByteArray())
            assertEquals(ExtensionSession.RPCStatus.CLOSED, admission.status)
            assertNull(admission.handle)
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
        val plugin = """
            local ctx = ...
            local resource = assert(ctx.resources.publish("workspace", "file.bin", "application/octet-stream", 262144, 10000))
            assert(resource.size == 262144)
            return {}
        """.trimIndent()
        val c = config(plugin)
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
    @Test fun nativeHostCanReadIncomingResourceAfterRuntimeAdoption() {
        val (hs, hd) = store(); val (es, ed) = store(); val p = pair(hs, es)
        val plugin = "return {}"
        var runtime: ExtensionRuntime? = null
        try {
            runtime = ExtensionRuntime.adopt(p.h, config(plugin, role = ExtensionSession.Role.HOST))
            val activationDeadline = android.os.SystemClock.elapsedRealtime() + 10000
            while (runtime.info().phase in setOf(ExtensionRuntime.Phase.CREATED, ExtensionRuntime.Phase.STARTING)) {
                runtime.step(); p.e.step()
                assertTrue("Host Runtime activation timed out", android.os.SystemClock.elapsedRealtime() < activationDeadline)
            }
            assertEquals("Host Runtime activation failed: ${runtime.info()}",
                ExtensionRuntime.Phase.ACTIVE, runtime.info().phase)

            val payload = "abc".toByteArray()
            val descriptor = ExtensionResourceStore.Descriptor(
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "text/plain", "readme.txt", payload.size.toLong())
            val admission = p.e.sendResource(p.el, descriptor)
            assertEquals(ExtensionResourceStore.Status.OK, admission.status)
            val transfer = admission.handle!!
            var written = 0
            var finished = false
            var received: ExtensionResourceStore.Content? = null
            val transferDeadline = android.os.SystemClock.elapsedRealtime() + 10000
            while (received == null && android.os.SystemClock.elapsedRealtime() < transferDeadline) {
                if (written < payload.size) {
                    val (status, accepted) = p.e.writeResource(p.el, transfer, payload, written)
                    assertTrue(status == ExtensionResourceStore.Status.OK || status == ExtensionResourceStore.Status.WAIT)
                    written += accepted
                }
                if (written == payload.size && !finished) {
                    assertEquals(ExtensionResourceStore.Status.OK, p.e.finishResource(p.el, transfer, true))
                    finished = true
                }
                runtime.step(); p.e.step()
                val (status, content) = runtime.takeResourceData(payload.size.toLong())
                assertTrue(status == ExtensionResourceStore.Status.OK || status == ExtensionResourceStore.Status.WAIT)
                received = content
            }
            assertArrayEquals(payload, received?.bytes)
            assertTrue(received?.descriptor?.resourceId?.isNotEmpty() == true)
            assertEquals(descriptor.sha256, received?.descriptor?.sha256)
            assertEquals(descriptor.displayName, received?.descriptor?.displayName)
            val receiptDeadline = android.os.SystemClock.elapsedRealtime() + 10000
            while (p.e.resourceResult(p.el, transfer).second?.confirmed != true &&
                android.os.SystemClock.elapsedRealtime() < receiptDeadline) { runtime.step(); p.e.step() }
            assertTrue("resource receipt was not confirmed", p.e.resourceResult(p.el, transfer).second?.confirmed == true)
            assertEquals(ExtensionResourceStore.Status.OK, p.e.forgetResource(p.el, transfer))
        } finally {
            runtime?.close()
            p.e.close()
            hs.close(); es.close()
            check(hd.deleteRecursively() && ed.deleteRecursively())
        }
    }
    @Test fun rootsWithoutResourceSessionAreUnsupported() {
        val p = pair()
        try {
            try { ExtensionRuntime.adopt(p.e, config().copy(roots = listOf(ExtensionRuntime.WorkspaceRoot("workspace", -1, 3)))).close(); fail("root accepted without resource Session") }
            catch (error: com.hanxi.ghosterm.bridge.ExtensionChannelException) { assertEquals(7, error.status) }
            assertEquals(ExtensionSession.Phase.ACTIVE, p.e.info().phase)
        } finally { p.e.close(); p.h.close() }
    }
    companion object {
        private const val echo = "return {['fixture.echo'] = function(input) return input end}"
    }
}
