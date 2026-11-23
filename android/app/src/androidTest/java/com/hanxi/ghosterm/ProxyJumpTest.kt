package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class ProxyJumpTest {
    private fun await(condition: () -> Boolean) {
        val end = SystemClock.elapsedRealtime() + 20000
        do { if (condition()) return; SystemClock.sleep(10) } while (SystemClock.elapsedRealtime() < end)
        fail("ProxyJump fixture timeout")
    }
    private fun owners(name: String): Int {
        val type = Class.forName("com.hanxi.ghosterm.bridge.$name")
        return type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null)) as Int
    }
    @Test fun configurationPreservesRouteAndRejectsMalformedOrUnsupportedHops() {
        fun prepare(text: String) = ConfigBridge.prepareConnection(text.toByteArray(), "dest")
        val route = prepare("Host dest\n HostName final.invalid\n User final\n ProxyJump via@gateway:2222,second\n ServerAliveInterval 35\n ServerAliveCountMax 0\nHost gateway\n HostName first.invalid\n IdentityFile key-one\n ServerAliveInterval 12\n ServerAliveCountMax 5\nHost second\n User two\n ProxyJump ignored\n")
        assertEquals("final.invalid", route.host)
        assertEquals(listOf("first.invalid", "second"), route.jumps.map { it.host })
        assertEquals(listOf("via", "two"), route.jumps.map { it.user })
        assertEquals(2222, route.jumps.first().port)
        assertEquals(listOf("key-one"), route.jumps.first().identityFiles)
        assertEquals(listOf(12, 0), route.jumps.map { it.serverAliveInterval })
        assertEquals(listOf(5, 3), route.jumps.map { it.serverAliveCountMax })
        assertEquals(35, route.serverAliveInterval); assertEquals(0, route.serverAliveCountMax)
        val nested = prepare("Host dest\n ProxyJump first,second\nHost first\n ProxyJump outer\n")
        assertEquals(listOf("outer", "first", "second"), nested.jumps.map { it.alias })
        assertTrue(prepare("Host dest\n ProxyJump none\n").jumps.isEmpty())
        for (bad in listOf("", "first,", ",first", "first,,second", "dest", "a,b,c,d,e,f,g,h", "x:0", "x:65536", "ssh://x/path", "[broken]")) {
            try { prepare("Host dest\n ProxyJump $bad\n"); fail("accepted invalid route: $bad") } catch (_: IllegalStateException) { }
        }
        try { prepare("Host dest\n ProxyJump jump\nHost jump\n ProxyCommand false\n"); fail("unsupported jump admitted") } catch (_: IllegalStateException) { }
    }
    @Test fun missingJumpUsernamePromptsBeforeDnsWithoutChangingTheRequest() {
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        fun main(action: () -> Unit) = instrumentation.runOnMainSync(action)
        val directory = File(instrumentation.targetContext.cacheDir, "jump-user-" + UUID.randomUUID()).apply { mkdirs() }
        val application = object: android.app.Application() {
            init { attachBaseContext(instrumentation.targetContext) }
            override fun getFilesDir() = directory
        }
        val called = java.util.concurrent.atomic.AtomicBoolean(false)
        val store = androidx.lifecycle.ViewModelStore()
        val original = PreparedSSH("dest", "final.invalid", "final-user", 22, emptyList(), listOf(PreparedSSH("jump", "first.invalid", "", 22, emptyList())))
        lateinit var model: SessionsViewModel
        try {
            main {
                model = SessionsViewModel(application, SSHResolver { called.set(true); error("owned DNS probe") }, monitorSessions = false)
                store.put("username", model); model.connect(original)
                val attempt = model.connecting!!
                assertFalse("Missing jump username must remain an interactive prompt", attempt.passiveProgress)
                assertEquals("jump", attempt.authenticationTarget.alias)
                assertFalse(called.get())
                model.setConnectionUsername(attempt.id, "jump-user")
                assertEquals("final-user", attempt.username)
                assertEquals("jump-user", attempt.target.jumps.single().user)
                assertEquals("", original.jumps.single().user)
            }
            await { called.get() }
        } finally { main { store.clear() }; directory.deleteRecursively() }
    }
    @Test fun productResolvesOnlyFirstHopAndSeparatesAuthentication() {
        val fixture = sshFixture()
        val ports = fixture.getJSONArray("ports").let { a -> (0 until a.length()).map(a::getInt) }
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        fun main(action: () -> Unit) = instrumentation.runOnMainSync(action)
        val folder = File(instrumentation.targetContext.cacheDir, "jump-product-" + UUID.randomUUID()).apply { mkdirs() }
        val app = object: android.app.Application() {
            init { attachBaseContext(instrumentation.targetContext) }
            override fun getFilesDir() = folder
        }
        val jumps = ports.dropLast(1).mapIndexed { index, port -> PreparedSSH("hop$index", if (index == 0) "first-hop.invalid" else "127.0.0.1", fixture.getString("user"), port, listOf("identity-$index")) }
        val target = PreparedSSH("dest", "127.0.0.1", fixture.getString("user"), ports.last(), listOf("identity-final"), jumps)
        val lookedUp = java.util.concurrent.CopyOnWriteArrayList<String>()
        val store = androidx.lifecycle.ViewModelStore()
        val testReset = fixture.has("reset")
        lateinit var model: SessionsViewModel
        main {
            model = SessionsViewModel(app, SSHResolver { host -> lookedUp += host; listOf(java.net.InetAddress.getByAddress(byteArrayOf(127,0,0,1))) }, 20000, testReset)
            store.put("route", model); model.connect(target)
        }
        val seen = mutableSetOf<Long>(); val hosts = mutableListOf<Int>(); val credentials = mutableListOf<Int>()
        try {
            await {
                var ready = false
                main {
                    val attempt = model.connecting
                    assertFalse(attempt?.message, attempt?.failed == true)
                    attempt?.auth?.let { auth ->
                        if (auth.request > 0 && seen.add(auth.request)) {
                            assertEquals(ports[auth.hop], attempt.authenticationTarget.port)
                            assertEquals(if (auth.hop == ports.lastIndex) listOf("identity-final") else listOf("identity-${auth.hop}"), attempt.authenticationIdentityFiles)
                            if (auth.phase == SSHAuthPhase.HOSTKEY) { assertTrue("previous hop identity attempts leaked", attempt.triedIdentityIds.isEmpty()); hosts += auth.hop; model.decide(attempt.id, auth, true) }
                            if (auth.phase == SSHAuthPhase.CREDENTIALS) {
                                credentials += auth.hop
                                attempt.triedIdentityIds.add("hop-${auth.hop}")
                                attempt.connection!!.privateKey(auth, fixture.getString("pem").toByteArray())
                            }
                        }
                    }
                    ready = model.sessions.singleOrNull()?.state?.connected == true
                }
                ready
            }
            assertEquals(listOf("first-hop.invalid"), lookedUp)
            assertEquals(ports.indices.toList(), hosts)
            assertEquals(ports.indices.toList(), credentials)
            if (testReset) {
                assertTrue("Reset test requires plugin runtime discovery", BuildConfig.PLUGIN_RUNTIME_ENABLED)
                lateinit var candidate: FrontendCandidate
                val session = model.sessions.single()
                val offer = "EREREREREREREREREREREaqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqAAsvd29yay9mcm9udA=="
                session.connection.resize(60, 12, 10, 20)
                session.connection.visible(true)
                session.connection.commit("printf '\\r\\nJUMP_%s\\r\\n' READY\r")
                await {
                    session.connection.frame(-1)?.let { frame ->
                        (0 until frame.rows).any { frame.line(it).contains("JUMP_READY") }
                    } == true
                }
                session.connection.commit("printf '\\033]777;ghosterm-discovery;1;$offer\\007'\r")
                await {
                    var found = false
                    main {
                        model.frontendCandidates.singleOrNull { it.sourceID == session.id }?.let {
                            candidate = it
                            found = true
                        }
                    }
                    found
                }
                resetSSHFixtureConnection()
                await {
                    var stopped = false
                    main { stopped = session.state.terminal && model.frontendCandidates.isEmpty() }
                    stopped
                }
                main {
                    assertSame(session, model.sessions.single())
                    assertFalse("Candidate must clear before expiry", candidate.expired)
                    assertTrue(session.state.error, session.state.error?.startsWith("终端读取失败") == true)
                }
            }
        } finally { main { store.clear() }; await { owners("NativeSession") == 0 }; folder.deleteRecursively() }
    }
    @Test fun innerHostRejectionAndCancellationNeverCreateATerminal() {
        val fixture = sshFixture()
        val ports = fixture.getJSONArray("ports").let { a -> (0 until a.length()).map(a::getInt) }
        val folder = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "jump-stop-" + UUID.randomUUID()).apply { mkdirs() }
        val user = fixture.getString("user")
        try {
            for (cancel in listOf(false, true)) {
                val known = File(folder, if (cancel) "cancel" else "reject")
                val request = ResolvedSSHConnection("127.0.0.1", ports.last(), user, known.path,
                    listOf(ResolvedSSHAddress(byteArrayOf(127,0,0,1))), 10000,
                    ports.dropLast(1).mapIndexed { i, port -> PreparedSSH("hop$i", "127.0.0.1", user, port, emptyList()) })
                val session = RemoteSession(request)
                val seen = mutableSetOf<Long>(); var stopped = false
                try {
                    await {
                        val auth = session.authentication()
                        if (auth.phase == SSHAuthPhase.HOSTKEY && auth.hop == ports.lastIndex) {
                            assertFalse(session.state().connected)
                            if (cancel) session.close() else session.decide(auth, false)
                            stopped = true
                        } else if (auth.request > 0 && seen.add(auth.request)) {
                            if (auth.phase == SSHAuthPhase.HOSTKEY) session.decide(auth, true)
                            if (auth.phase == SSHAuthPhase.CREDENTIALS) session.privateKey(auth, fixture.getString("pem").toByteArray())
                        }
                        stopped
                    }
                    if (!cancel) await {
                        val auth = session.authentication()
                        assertFalse(session.state().connected)
                        auth.phase == SSHAuthPhase.FAILED && auth.error == 5
                    }
                } finally { session.close() }
                await { owners("NativeSession") == 0 }
            }
        } finally { folder.deleteRecursively() }
    }
    @Test fun realRouteTerminalFilesAndTmux(): Unit = runBlocking {
        val fixture = sshFixture()
        val ports = fixture.getJSONArray("ports").let { array -> (0 until array.length()).map(array::getInt) }
        val folder = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "jump-" + UUID.randomUUID()).apply { mkdirs() }
        val known = File(folder, "known_hosts")
        val config = ports.mapIndexed { index, port ->
            "Host ${if (index == ports.lastIndex) "dest" else "hop$index"}\n HostName 127.0.0.1\n User ${fixture.getString("user")}\n Port $port\n" +
                if (index == ports.lastIndex) " ProxyJump " + (0 until index).joinToString(",") { "hop$it" } + "\n" else ""
        }.joinToString("")
        val target = ConfigBridge.prepareConnection(config.toByteArray(), "dest")
        fun request() = ResolvedSSHConnection(target.host, target.port, target.user, known.path,
            listOf(ResolvedSSHAddress(byteArrayOf(127,0,0,1))), 20000, target.jumps)
        fun authenticate(session: AuthenticatedSSH, ready: () -> Boolean): List<Int> {
            val supplied = mutableListOf<Int>(); val seen = mutableSetOf<Long>()
            await {
                val auth = session.authentication()
                assertNotEquals(auth.description, SSHAuthPhase.FAILED, auth.phase)
                assertNotEquals(auth.description, SSHAuthPhase.CANCELLED, auth.phase)
                if (auth.request > 0 && seen.add(auth.request)) {
                    if (auth.phase == SSHAuthPhase.HOSTKEY) session.decide(auth, true)
                    else if (auth.phase == SSHAuthPhase.CREDENTIALS) {
                        supplied += auth.hop
                        val key = fixture.getString("pem").toByteArray()
                        session.privateKey(auth, key)
                        assertTrue(key.all { it == 0.toByte() })
                    }
                }
                ready()
            }
            assertEquals(ports.indices.toList(), supplied)
            return supplied
        }
        fun command(session: RemoteSession, marker: String) {
            session.commit("printf '${marker}_%s\\n' DONE\r")
            await { session.frame(0)?.let { f -> (0 until f.rows).any { f.line(it).contains("${marker}_DONE") } } == true }
        }
        try {
            RemoteSession(request()).use { terminal ->
                authenticate(terminal) { terminal.state().connected }
                command(terminal, "JUMP_TERMINAL")
            }
            val files = RemoteFileSession(request())
            try {
                authenticate(files) { files.state().ready }
                val payload = files.browse().entries.single { it.name == "payload" }
                assertEquals("owned Android route fixture", files.previewText(payload.path))
            } finally { files.closeAndAwait() }
            val manager = RemoteTmuxSession(request(), socketPath = fixture.getString("socket"))
            try {
                authenticate(manager) { manager.state().ready }
                val discovered = manager.discover()
                assertEquals(discovered.error, 0, discovered.status)
                assertTrue(discovered.candidates.any { it.usable })
                assertEquals(0, manager.selectExecutable(fixture.getString("tmux")).status)
                assertEquals(RemoteTmuxResult.CONFIRMED, manager.create("jump-test").result)
                var selected = manager.list().entries.single()
                assertEquals(RemoteTmuxResult.CONFIRMED, manager.rename(selected, "jump-renamed").result)
                selected = manager.list().entries.single()
                assertEquals("jump-renamed", selected.name)
                val attachment = manager.target(selected)
                val attachConnection = attachment.connection()
                assertEquals("attach must retain the management route", target.jumps, attachConnection.jumps)
                RemoteSession(attachConnection, attachment).use { terminal ->
                    authenticate(terminal) { terminal.state().connected }
                    command(terminal, "JUMP_TMUX")
                }
                selected = manager.list().entries.single()
                assertEquals(RemoteTmuxResult.CONFIRMED, manager.terminate(selected).result)
                assertTrue(manager.list().entries.isEmpty())
            } finally { manager.closeAndAwait() }
            await { owners("NativeSession") == 0 && owners("NativeTmux") == 0 && owners("NativeFiles") == 0 }
        } finally { folder.deleteRecursively() }
    }
}
