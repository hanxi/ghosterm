package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.Closeable
import java.io.File
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

/** Real TCP + sshd. A silent accepted socket is negotiation, not a TCP blackhole. */
@RunWith(AndroidJUnit4::class)
class SSHNetworkTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val host = "network-fixture.ghosterm.test"
    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", requireNotNull(args.getString("fixturePort")).toInt()).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray())
            JSONObject(socket.getInputStream().bufferedReader().readLine())
        }
    }
    private fun ip(value: String) = InetAddress.getByName(value)
    private fun candidate(value: String) = ResolvedSSHAddress(ip(value).address)
    private fun await(timeout: Long = 10000, condition: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + timeout
        do { if (condition()) return; SystemClock.sleep(10) } while (SystemClock.elapsedRealtime() < end)
        fail("Network condition timed out")
    }
    private fun owners(): Int {
        val type = Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
        return type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null)) as Int
    }
    @Before fun releasedOwners() { await { owners() == 0 } }
    private fun directory(test: (File) -> Unit) {
        val dir = File(instrumentation.targetContext.cacheDir, "network-${UUID.randomUUID()}").apply { mkdirs() }
        try { test(dir) } finally { await { owners() == 0 }; dir.deleteRecursively() }
    }
    private fun connection(dir: File, port: Int, addresses: List<ResolvedSSHAddress>, timeout: Int = 8000) =
        ResolvedSSHConnection(host, port, fixture.getString("user"), File(dir,"known_hosts").path, addresses, timeout)
    private fun phase(session: RemoteSession, target: SSHAuthPhase): SSHAuthState {
        var state = session.authentication()
        await {
            state = session.authentication()
            if (target != SSHAuthPhase.FAILED) assertNotEquals(state.description, SSHAuthPhase.FAILED, state.phase)
            state.phase == target && (target !in listOf(SSHAuthPhase.HOSTKEY, SSHAuthPhase.CREDENTIALS) || state.request > 0)
        }
        return state
    }
    private fun trust(session: RemoteSession) {
        val prompt = phase(session, SSHAuthPhase.HOSTKEY)
        assertEquals(fixture.getString("fingerprint"), prompt.fingerprint)
        session.decide(prompt, true)
    }
    private fun terminal(session: RemoteSession) {
        val prompt = phase(session, SSHAuthPhase.CREDENTIALS)
        val secret = fixture.getString("pem").toByteArray()
        session.privateKey(prompt, secret)
        assertTrue(secret.all { it == 0.toByte() })
        await { val state=session.state(); assertFalse(state.description,state.terminal); state.connected }
        session.resize(70,20,8,16)
        session.commit("stty -echo; printf '\\033[2J\\033[HNET_PID_%s\\r\\n' \"$$\"\r")
        await { session.frame(0)?.let { frame -> (0 until frame.rows).any { frame.line(it).matches(Regex("NET_PID_[0-9]+")) } } == true }
    }
    /** Bind one exact address. Count and retain accepted sockets to expose forbidden fallback. */
    private inner class Endpoint(address: String, port: Int = 0, private val silent: Boolean = false, private val brokenHandshake: Boolean = false) : Closeable {
        val accepted = AtomicInteger()
        private val sockets = CopyOnWriteArrayList<Socket>()
        private val server = ServerSocket().apply { reuseAddress=true; bind(InetSocketAddress(ip(address),port)) }
        val port: Int get() = server.localPort
        private val worker = thread(name="ssh-network-listener", isDaemon=true) {
            try {
                while (!server.isClosed) {
                    val client=server.accept(); sockets.add(client); accepted.incrementAndGet()
                    if (!silent) thread(name="ssh-network-relay", isDaemon=true) {
                        try {
                            if (brokenHandshake) {
                                client.soTimeout=5000
                                client.getInputStream().bufferedReader().readLine()
                                client.close(); return@thread
                            }
                            Socket("127.0.0.1",fixture.getInt("port")).use { remote ->
                                sockets.add(remote)
                                val back=thread(isDaemon=true) {
                                    try { remote.getInputStream().copyTo(client.getOutputStream()) }
                                    catch (_: Exception) {} finally { client.close() }
                                }
                                try { client.getInputStream().copyTo(remote.getOutputStream()) }
                                finally { remote.close(); client.close(); back.join(2000) }
                            }
                        } catch (_: Exception) { client.close() }
                    }
                }
            } catch (_: Exception) { /* Closing the owned listener ends accept. */ }
        }
        override fun close() { server.close(); sockets.forEach { it.close() }; worker.join(2000) }
    }
    private fun shell(command: String): String = instrumentation.uiAutomation.executeShellCommand(command).use {
        android.os.ParcelFileDescriptor.AutoCloseInputStream(it).bufferedReader().readText().trim()
    }
    /** Only emulator loopback and this test's reserved port; no device-wide routing change. */
    private fun dropped(port: Int, test: () -> Unit) {
        assertEquals("TCP loss fixture requires an emulator", "1", shell("getprop ro.kernel.qemu"))
        assertTrue("TCP loss fixture requires emulator root", shell("su 0 id").contains("uid=0"))
        val prefix=requireNotNull(InstrumentationRegistry.getArguments().getString("networkRulePrefix"))
        require(prefix.matches(Regex("ghosterm-network-[a-f0-9]{16}")))
        val tag="$prefix-${UUID.randomUUID()}"
        val rule="OUTPUT -p tcp -d 127.0.0.2 --dport $port -m comment --comment $tag -j DROP"
        try {
            assertEquals("", shell("su 0 iptables -I $rule"))
            assertTrue(shell("su 0 iptables -S OUTPUT").contains(tag))
            test()
        } finally {
            shell("su 0 iptables -D $rule")
            assertFalse("test rule leaked", shell("su 0 iptables -S OUTPUT").contains(tag))
        }
    }
    @Test fun refusedIpv4FallsBackAndPreservesHostIdentity() = directory { dir ->
        Endpoint("127.0.0.1").use { endpoint ->
            RemoteSession(connection(dir,endpoint.port,listOf(candidate("127.0.0.2"),candidate("127.0.0.1")))).use { session ->
                trust(session); terminal(session)
                assertEquals(1,endpoint.accepted.get())
                val known=File(dir,"known_hosts").readText()
                assertTrue(known.contains("[$host]:${endpoint.port}")); assertFalse(known.contains("127.0.0.1"))
            }
        }
    }
    @Test fun ipv4FailureReachesRealIpv6AndIpv6FailureReachesIpv4() = directory { dir ->
        listOf("::1" to "127.0.0.2", "127.0.0.1" to "::2").forEachIndexed { index, (good,bad) ->
            val path=File(dir,"case-$index").apply { mkdirs() }
            Endpoint(good).use { endpoint ->
                RemoteSession(connection(path,endpoint.port,listOf(candidate(bad),candidate(good)))).use { session ->
                    trust(session); terminal(session); assertEquals(1,endpoint.accepted.get())
                }
            }
        }
    }
    @Test fun candidateSnapshotDoesNotFollowCallerMutation() = directory { dir ->
        Endpoint("127.0.0.1").use { endpoint ->
            val bytes=ip("127.0.0.1").address
            val list=mutableListOf(ResolvedSSHAddress(bytes))
            val request=connection(dir,endpoint.port,list)
            bytes[3]=2; list.clear()
            val opened=try { RemoteSession(request) }
                catch (failure: Exception) { throw AssertionError("immutable request lost its candidate snapshot",failure) }
            opened.use { session -> trust(session); terminal(session) }
        }
    }
    @Test fun invalidCandidateSetsDoNotCreateOwners() = directory { dir ->
        val invalid=listOf(emptyList(),List(33) { candidate("127.0.0.1") },
            listOf(ResolvedSSHAddress(ByteArray(3))),listOf(ResolvedSSHAddress(ip("127.0.0.1").address,1)),
            listOf(ResolvedSSHAddress(ip("::1").address,-1)))
        invalid.forEach { addresses ->
            assertThrows(IllegalStateException::class.java) { RemoteSession(connection(dir,22,addresses)).close() }
            assertEquals(0,owners())
        }
        assertFalse(File(dir,"known_hosts").exists())
    }
    @Test fun tcpBlackholeAdvancesAndDuplicateAddressesAreSkipped() = directory { dir ->
        Endpoint("127.0.0.1").use { endpoint -> dropped(endpoint.port) {
            val start=SystemClock.elapsedRealtime()
            RemoteSession(connection(dir,endpoint.port,listOf(candidate("127.0.0.2"),candidate("127.0.0.2"),candidate("127.0.0.1")),3500)).use { session ->
                trust(session)
                assertTrue("must wait for the first TCP candidate",SystemClock.elapsedRealtime()-start>=1800)
                terminal(session); assertEquals(1,endpoint.accepted.get())
            }
        } }
    }
    @Test fun candidateSwitchDoesNotResetTotalDeadline() = directory { dir ->
        Endpoint("127.0.0.1",silent=true).use { endpoint -> dropped(endpoint.port) {
            val calibrationStart=SystemClock.elapsedRealtime()
            repeat(50) { SystemClock.sleep(10) }
            val allowance=maxOf(2000L, ((SystemClock.elapsedRealtime()-calibrationStart)*1.5).toLong()+750L)
            val start=SystemClock.elapsedRealtime()
            RemoteSession(connection(dir,endpoint.port,listOf(candidate("127.0.0.2"),candidate("127.0.0.1")),3000)).use { session ->
                phase(session,SSHAuthPhase.NEGOTIATING)
                val switched=SystemClock.elapsedRealtime()
                val failure=phase(session,SSHAuthPhase.FAILED)
                assertTrue(failure.deadline); assertEquals(3,failure.error)
                val ended=SystemClock.elapsedRealtime()
                assertTrue("total deadline ended too early",ended-start>=2800)
                assertTrue("candidate reset the remaining budget",ended-switched<allowance)
            }
        } }
    }
    @Test fun cancelledTcpDoesNotStartAnotherCandidateAndOwnerReleases() = directory { dir ->
        Endpoint("127.0.0.1").use { endpoint -> dropped(endpoint.port) {
            val session=RemoteSession(connection(dir,endpoint.port,listOf(candidate("127.0.0.2"),candidate("127.0.0.1"))))
            try {
                await { owners()==1 }; SystemClock.sleep(100)
                assertEquals(SSHAuthPhase.TCP,session.authentication().phase)
            } finally { session.close() }
            await { owners()==0 }
            assertEquals(0,endpoint.accepted.get()); assertFalse(File(dir,"known_hosts").exists())
        } }
    }
    @Test fun negotiationTimeoutDoesNotTryAnotherAddress() = directory { dir ->
        Endpoint("127.0.0.1",silent=true).use { first -> Endpoint("127.0.0.2",first.port).use { second ->
            RemoteSession(connection(dir,first.port,listOf(candidate("127.0.0.1"),candidate("127.0.0.2")),3000)).use { session ->
                phase(session,SSHAuthPhase.NEGOTIATING)
                val failed=phase(session,SSHAuthPhase.FAILED)
                assertTrue(failed.deadline); assertEquals(1,first.accepted.get()); assertEquals(0,second.accepted.get())
                assertFalse(File(dir,"known_hosts").exists())
            }
        } }
    }
    @Test fun negotiationIoFailureDoesNotTryAnotherAddress() = directory { dir ->
        Endpoint("127.0.0.1",brokenHandshake=true).use { first -> Endpoint("127.0.0.2",first.port).use { second ->
            RemoteSession(connection(dir,first.port,listOf(candidate("127.0.0.1"),candidate("127.0.0.2")))).use { session ->
                val failed=phase(session,SSHAuthPhase.FAILED)
                assertEquals(3,failed.error); assertFalse(failed.deadline)
                assertEquals(1,first.accepted.get()); assertEquals(0,second.accepted.get())
                assertFalse(File(dir,"known_hosts").exists())
            }
        } }
    }
    @Test fun trustRejectionAndKnownHostFailuresNeverTryAnotherAddress() = directory { dir ->
        Endpoint("127.0.0.1").use { first -> Endpoint("127.0.0.2",first.port).use { second ->
            val known=File(dir,"known_hosts")
            val cases=listOf("reject","revoked","mismatch","unsupported","save-error")
            cases.forEach { mode ->
                known.delete()
                when(mode) {
                    "revoked" -> known.writeText("@revoked [$host]:${first.port} "+fixture.getString("public"))
                    "mismatch" -> known.writeText("[$host]:${first.port} "+fixture.getString("otherPublic"))
                    "unsupported" -> known.writeText("@cert-authority [$host]:${first.port} "+fixture.getString("public"))
                }
                val path=if(mode=="save-error") File(dir,"missing") else dir
                RemoteSession(connection(path,first.port,listOf(candidate("127.0.0.1"),candidate("127.0.0.2")))).use { session ->
                    if(mode=="reject" || mode=="save-error") session.decide(phase(session,SSHAuthPhase.HOSTKEY),mode=="save-error")
                    val failed=phase(session,SSHAuthPhase.FAILED)
                    assertEquals(if(mode=="unsupported") 7 else if(mode=="save-error") 3 else 5,failed.error)
                    assertEquals(0,second.accepted.get())
                }
            }
            assertEquals(cases.size,first.accepted.get())
        } }
    }
    @Test fun hostPromptRetainsItsOwnerPastTcpGraceAndCancellationReleasesIt() = directory { dir ->
        Endpoint("127.0.0.1").use { first -> Endpoint("127.0.0.2",first.port).use { second ->
            val session=RemoteSession(connection(dir,first.port,listOf(candidate("127.0.0.1"),candidate("127.0.0.2"))))
            try {
                val prompt=phase(session,SSHAuthPhase.HOSTKEY)
                SystemClock.sleep(2200)
                assertEquals(prompt.request,session.authentication().request)
                assertEquals(SSHAuthPhase.HOSTKEY,session.authentication().phase)
                assertEquals(0,second.accepted.get())
            } finally { session.close() }
            await { owners()==0 }; assertEquals(0,second.accepted.get())
        } }
    }
    @Test fun productPassesAllResolvedAddressesToNativeOwner() = directory { dir ->
        Endpoint("127.0.0.1").use { endpoint ->
            val user=fixture.getString("user")
            val store=ViewModelStore()
            val application=object: Application() { override fun getFilesDir() = dir }
            lateinit var model: SessionsViewModel
            instrumentation.runOnMainSync {
                model=SessionsViewModel(application,SSHResolver { listOf(ip("127.0.0.2"),ip("127.0.0.1")) })
                store.put("test",model)
                model.connect(PreparedSSH("network",host,user,endpoint.port,emptyList()))
            }
            try {
                await {
                    var ready=false
                    instrumentation.runOnMainSync {
                        val attempt=model.connecting!!
                        assertFalse(attempt.message,attempt.failed)
                        ready=attempt.auth?.phase==SSHAuthPhase.HOSTKEY
                    }
                    ready
                }
                instrumentation.runOnMainSync { model.connecting!!.let { model.decide(it.id,it.auth!!,false) } }
                await { var failed=false; instrumentation.runOnMainSync { failed=model.connecting?.failed==true }; failed }
                assertEquals(1,endpoint.accepted.get())
                assertFalse(File(dir,"home/.ssh/known_hosts").exists())
            } finally { instrumentation.runOnMainSync { store.clear() } }
        }
    }
}
