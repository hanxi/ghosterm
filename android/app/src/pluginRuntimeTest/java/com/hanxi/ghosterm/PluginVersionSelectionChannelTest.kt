package com.hanxi.ghosterm

import android.os.SystemClock
import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ExtensionRegistry
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.LocalPublicationFile
import com.hanxi.ghosterm.bridge.PluginInstallFiles
import com.hanxi.ghosterm.bridge.PreparedSSH
import com.hanxi.ghosterm.bridge.SSHAuthPhase
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.DataInputStream
import java.io.File
import java.net.Socket
import java.util.UUID
import java.util.Base64
import java.util.zip.ZipInputStream

/** Actual signed versions, native Session/QuickJS and two-hop isolated SSH. */
@RunWith(AndroidJUnit4::class)
class PluginVersionSelectionChannelTest {
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
    private fun shellQuote(value: String) = "'" + value.replace("'", "'\\''") + "'"
    private fun remoteCompletionProof(): Boolean {
        val args = InstrumentationRegistry.getArguments()
        return Socket("127.0.0.1", args.getString("fixturePort")!!.toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((args.getString("fixtureToken") + " proof\n").toByteArray())
            val stream = DataInputStream(socket.getInputStream()); val size = stream.readInt()
            check(size in 1..128); val bytes = ByteArray(size); stream.readFully(bytes)
            JSONObject(String(bytes)).getBoolean("complete")
        }
    }
    private fun lock(root: File, slot: String) = LocalPublicationFile.directory(root, slot).use { PluginInstallFiles.lock(it.fd) }
    private suspend fun select(owner: ExtensionController, root: File, slot: String): ExtensionController.VersionSelectionResult {
        val revision = checkNotNull(PluginReleaseCatalog.open(root)).use { it.selection().revision }
        val view = owner.previewInstalledVersion(UUID.randomUUID().toString(), slot, revision, PluginVersionSelectionFixture.policy)
        return owner.confirmInstalledVersion(view.id, true)
    }
    @Test fun anotherControllerSwitchPreservesSessionSnapshotAndRuntimeLease() = scenario(false)
    @Test fun sameControllerRegistryReplacementPreservesSessionSnapshotAndRuntimeLease() = scenario(true)
    @Test fun developmentSessionSelectingSameManagedDigestPinsVersionUntilActualRuntimeReleaseAndRemoval() = scenario(true, true)
    @Test fun developmentRuntimeSelectingSameManagedDigestPinsVersionUntilActualRuntimeReleaseAndRemoval() = scenario(true, true, true)
    private fun scenario(sameController: Boolean, developmentFirst: Boolean = false, manageAfterActivation: Boolean = false) = runBlocking {
        require(!manageAfterActivation || developmentFirst)

        val payload = fixture; val ports = payload.getJSONObject("two").getJSONArray("ports")
        val packageDir = PluginInstallTestFiles.root()
        val development = if (developmentFirst) PluginInstallTestFiles.root() else null
        val knownHosts = File(packageDir.parentFile, "extension-client-known-${System.nanoTime()}")
            .apply { writeText(payload.getJSONObject("two").getString("known")) }
        val target = PreparedSSH("dest", "127.0.0.1", payload.getString("user"), ports.getInt(1), emptyList(),
            listOf(PreparedSSH("jump", "route-hop-0.invalid", payload.getString("user"), ports.getInt(0), emptyList())))
        val controller = ExtensionController(
            ExtensionController.HostProfile("1.0.0", listOf(1), listOf("portable-js"), listOf("session.rpc")),
            instrumentation.targetContext, packageDir)
        val changer = if (sameController) controller else PluginVersionSelectionFixture.controller(packageDir)
        try {
            assertEquals(0, ExtensionRuntime.supportStatus())
            var installed: ExtensionController.InstalledRelease? = null
            var next: ExtensionController.InstalledRelease? = null
            val reviewed = if (development != null) {
                val vector = PluginVersionSelectionFixture.vector("1.0.0")
                ZipInputStream(Base64.getDecoder().decode(vector.getString("archive")).inputStream()).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        require(!entry.name.startsWith('/') && entry.name.split('/').none { it == ".." })
                        File(development, entry.name).apply { parentFile!!.mkdirs(); writeBytes(zip.readBytes()) }
                    }
                }
                controller.review(development)
            } else {
                installed = PluginVersionSelectionFixture.install(controller, packageDir)
                next = PluginVersionSelectionFixture.install(controller, packageDir, "2.0.0")
                select(controller, packageDir, installed.publication.releaseID).review!!
            }
            assertEquals(ExtensionRegistry.Status.OK, controller.approve(reviewed.id, reviewed.digest))
            val command = "exec " + listOf(payload.getString("rpcExecutorBinary"), "--android-executor",
                payload.getString("rpcAndroidProofRoot"), reviewed.id, reviewed.digest,
                payload.getString("rpcExecutorDigest"), "0.1.0", target.alias, reviewed.version).joinToString(" ", transform = ::shellQuote) +
                " 2>" + shellQuote(File(payload.getString("rpcAndroidProofRoot"), "android-executor.stderr").path)
            var route = controller.openSSHChannel(target, java.net.InetAddress.getByAddress(byteArrayOf(127, 0, 0, 1)),
                knownHosts, command)
            val deadline = SystemClock.elapsedRealtime() + 90000
            while (route.info.phase != 1 && route.info.phase != 2) {
                when (route.authentication.phase) {
                    SSHAuthPhase.HOSTKEY -> controller.decideSSHHost(route.authentication, true)
                    SSHAuthPhase.CREDENTIALS -> controller.submitSSHPrivateKey(route.authentication, payload.getString("pem").toByteArray(), byteArrayOf())
                    SSHAuthPhase.FAILED, SSHAuthPhase.CANCELLED -> fail(route.authentication.description)
                    else -> Unit
                }
                assertTrue("two-hop Client Runtime route timed out", SystemClock.elapsedRealtime() < deadline)
                route = controller.stepSSHChannel(5)
            }
            assertEquals("${route.info.error}", 1, route.info.phase)
            val methods = listOf(
                ExtensionSession.Method("fixture.echo", "session.rpc", ExtensionSession.Role.EXECUTOR,
                    ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
                ExtensionSession.Method("fixture.ask", "session.rpc", ExtensionSession.Role.HOST,
                    ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
                ExtensionSession.Method("fixture.done", "session.rpc", ExtensionSession.Role.EXECUTOR,
                    ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT),
                ExtensionSession.Method("fixture.denied", "session.rpc", ExtensionSession.Role.EXECUTOR,
                    ExtensionSession.JSONShape.OBJECT, ExtensionSession.JSONShape.OBJECT))
            var session = controller.adoptSSHSession(reviewed.id, reviewed.digest, payload.getString("rpcExecutorDigest"),
                "0.1.0", target.alias, methods)
            var installedSlot = ""; var nextSlot = ""
            lateinit var upgraded: ExtensionController.Review
            suspend fun switchManagedVersion() {
                if (developmentFirst) {
                    installed = PluginVersionSelectionFixture.install(controller, packageDir)
                    next = PluginVersionSelectionFixture.install(controller, packageDir, "2.0.0")
                    val managed = select(controller, packageDir, installed.publication.releaseID)
                    assertEquals(reviewed.digest, managed.review!!.digest)
                }
                installedSlot = checkNotNull(installed).publication.releaseID
                nextSlot = checkNotNull(next).publication.releaseID
                assertEquals(OsConstants.EAGAIN, lock(packageDir, installedSlot))
                val replacement = select(changer, packageDir, nextSlot)
                upgraded = replacement.review!!
                assertEquals(ExtensionRegistry.Status.OK, changer.approve(upgraded.id, upgraded.digest))
                assertEquals(ExtensionRegistry.Status.STALE, controller.resolve("fixture", reviewed.id, reviewed.digest).status)
            }
            if (!manageAfterActivation) switchManagedVersion()
            // This is an already adopted Session, before its explicit consent.
            // The global choice must not mutate its package or grant consent.
            val handshakeDeadline = SystemClock.elapsedRealtime() + 15000
            while (session.info.phase != ExtensionSession.Phase.CONSENT) {
                assertTrue("Client Session ended before consent: phase=${session.info.phase}, end=${session.info.end}, error=${session.info.error}",
                    SystemClock.elapsedRealtime() < handshakeDeadline)
                session = controller.stepSession(5)
            }
            session = controller.decideSession(session.info.consentToken, ExtensionSession.Decision.ALLOW)
            while (session.info.phase != ExtensionSession.Phase.ACTIVE) {
                assertTrue("Client Session did not become active", SystemClock.elapsedRealtime() < deadline)
                session = controller.stepSession(5)
            }
            val activation = runCatching { controller.activateClient(reviewed.id, reviewed.digest, "fixture", emptyList()) }
            assertTrue("already adopted Session lost its approved package snapshot: ${activation.exceptionOrNull()}", activation.isSuccess)
            var runtime = activation.getOrThrow()
            assertEquals(ExtensionRuntime.Phase.CREATED, runtime.info.phase)
            while (runtime.info.phase == ExtensionRuntime.Phase.CREATED || runtime.info.phase == ExtensionRuntime.Phase.STARTING) {
                assertTrue("Client Runtime did not become active", SystemClock.elapsedRealtime() < deadline)
                runtime = controller.stepRuntime()
                assertTrue("Client Runtime failed: ${runtime.info}",
                    runtime.info.phase == ExtensionRuntime.Phase.STARTING || runtime.info.phase == ExtensionRuntime.Phase.ACTIVE)
                SystemClock.sleep(2)
            }
            assertEquals(ExtensionRuntime.Phase.ACTIVE, runtime.info.phase)
            if (manageAfterActivation) switchManagedVersion()
            assertEquals(reviewed.digest, runtime.packageDigest)
            assertEquals(OsConstants.EAGAIN, lock(packageDir, installedSlot))
            assertEquals(0, lock(packageDir, nextSlot))
            if (developmentFirst) {
                val revision = checkNotNull(PluginReleaseCatalog.open(packageDir)).use { it.selection().revision }
                val rejected = runCatching { controller.previewVersionRemoval(UUID.randomUUID().toString(), installedSlot, revision) }.exceptionOrNull()
                assertTrue("live Runtime did not prevent removal", rejected is PluginReleaseRemoval.Rejected && rejected.reason == PluginReleaseRemoval.Reason.OCCUPIED)
            }
            val choice = checkNotNull(PluginReleaseCatalog.open(packageDir)).use { it.selection() }
            changer.disableInstalledVersion(upgraded.id, choice.revision, true)
            assertEquals(ExtensionRegistry.Status.DENIED, changer.approve(upgraded.id, upgraded.digest))
            val outgoing = controller.requestRuntimeExecutor("fixture.echo",
                "{\"text\":\"native\",\"count\":23}".toByteArray())
            assertEquals(ExtensionSession.RPCStatus.OK, outgoing.status)
            val outgoingHandle = checkNotNull(outgoing.handle)
            var nativeRequestHandled = false
            var outgoingConfirmed = false
            for (iteration in 0 until 1000) {
                SystemClock.sleep(1)
                runtime = controller.stepRuntime()
                assertEquals("Client Runtime stopped during the bidirectional RPC exchange: ${runtime.info}",
                    ExtensionRuntime.Phase.ACTIVE, runtime.info.phase)
                val (requestStatus, request) = controller.takeRuntimeRequest()
                assertTrue(requestStatus == ExtensionSession.RPCStatus.OK || requestStatus == ExtensionSession.RPCStatus.WAIT)
                if (request != null) {
                    assertEquals("fixture.ask", request.method)
                    assertEquals("{\"text\":\"bridge\",\"count\":17}", request.input.toString(Charsets.UTF_8))
                    assertEquals(ExtensionSession.RPCStatus.OK, controller.respondRuntimeRequest(request.ticket,
                        "{\"kind\":\"success\",\"value\":{\"text\":\"bridge\",\"count\":17}}".toByteArray()))
                    nativeRequestHandled = true
                }
                if (!outgoingConfirmed) {
                    val (resultStatus, result) = controller.runtimeExecutorRequestResult(outgoingHandle)
                    assertEquals(ExtensionSession.RPCStatus.OK, resultStatus)
                    if (result?.confirmed == true) {
                        assertEquals(ExtensionSession.RPCState.SUCCEEDED, result.state)
                        assertEquals("{\"kind\":\"success\",\"value\":{\"text\":\"native\",\"count\":23}}",
                            result.outcome.toString(Charsets.UTF_8))
                        assertEquals(ExtensionSession.RPCStatus.OK, controller.forgetRuntimeExecutorRequest(outgoingHandle))
                        outgoingConfirmed = true
                    }
                }
                if (nativeRequestHandled && outgoingConfirmed) break
            }
            assertTrue("Native Host did not receive an Executor request through the adopted Runtime", nativeRequestHandled)
            assertTrue("Native Host Executor RPC did not reach a confirmed outcome", outgoingConfirmed)
            // Local respond() queues bytes. Keep pumping until the independent
            // Executor has actually consumed that reply and written its proof.
            val proofDeadline = SystemClock.elapsedRealtime() + 5000
            while (!remoteCompletionProof()) {
                assertTrue("remote completion proof missing", SystemClock.elapsedRealtime() < proofDeadline)
                runtime = controller.stepRuntime()
                assertEquals(ExtensionRuntime.Phase.ACTIVE, runtime.info.phase)
                SystemClock.sleep(1)
            }
            controller.requestStopRuntime()
            while (runtime.info.phase in setOf(ExtensionRuntime.Phase.CREATED, ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.ACTIVE)) {
                assertTrue("Runtime stop timed out", SystemClock.elapsedRealtime() < deadline)
                runtime = controller.stepRuntime()
            }
            controller.releaseFinished()
            assertEquals("completed Runtime retained its version lease", 0, lock(packageDir, installedSlot))
            if (developmentFirst) {
                val revision = checkNotNull(PluginReleaseCatalog.open(packageDir)).use { it.selection().revision }
                val view = controller.previewVersionRemoval(UUID.randomUUID().toString(), installedSlot, revision)
                assertTrue(controller.confirmVersionRemoval(view.id, true).complete)
                assertFalse(File(packageDir, installedSlot).exists())
                assertEquals(ExtensionRegistry.Status.STALE, controller.resolve("fixture", reviewed.id, reviewed.digest).status)
            }
        } finally {
            changer.close(); controller.close()
            assertTrue(knownHosts.delete() || !knownHosts.exists())
            PluginInstallTestFiles.clean(packageDir); development?.let(PluginInstallTestFiles::clean)
        }
    }

}
