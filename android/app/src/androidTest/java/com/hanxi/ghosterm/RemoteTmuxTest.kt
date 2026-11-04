package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.terminal.TerminalFrame
import java.io.File
import java.util.UUID
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import org.junit.runner.RunWith

/** Real SSH -> Core tmux -> ordinary transport -> production VT. No fake list. */
@RunWith(AndroidJUnit4::class)
class RemoteTmuxTest {
    private val fixture by lazy { sshFixture() }
    private val tmux get()=fixture.getJSONObject("tmux")
    private lateinit var folder: File
    private lateinit var known: File
    private val managers=mutableListOf<RemoteTmuxSession>()
    private val terminals=mutableListOf<RemoteSession>()
    private val host="tmux-fixture.ghosterm.test"
    private fun socket()=tmux.getString("socketPrefix")+"-"+UUID.randomUUID().toString().take(8)
    private fun request(user: String=fixture.getString("user"))=ResolvedSSHConnection(host,fixture.getInt("port"),
        user,known.path,byteArrayOf(127,0,0,1),timeoutMillis=15000)
    private fun owners(type: String): Int {
        val cls=Class.forName("com.hanxi.ghosterm.bridge."+type)
        return cls.getDeclaredMethod("ownerCount").invoke(cls.getField("INSTANCE").get(null)) as Int
    }
    private fun await(condition: ()->Boolean) {
        val end=SystemClock.elapsedRealtime()+15000
        do { if(condition())return;SystemClock.sleep(10) } while(SystemClock.elapsedRealtime()<end)
        fail("真实 tmux 条件超时")
    }
    @Before fun setup() {
        await { owners("NativeTmux")==0 && owners("NativeSession")==0 }
        folder=File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir,"tmux-"+UUID.randomUUID()).apply { mkdirs() }
        known=File(folder,"known_hosts").apply { writeText("["+host+"]:"+fixture.getInt("port")+" "+fixture.getString("public")) }
    }
    @After fun cleanup(): Unit=runBlocking {
        terminals.forEach { it.close() }
        managers.forEach { it.closeAndAwait() }
        await { owners("NativeTmux")==0 && owners("NativeSession")==0 }
        folder.deleteRecursively()
        Unit
    }
    private fun authenticate(session: AuthenticatedSSH, key: String = "pem", ready: ()->Boolean) {
        var prompt: SSHAuthState?=null
        await {
            val value=session.authentication()
            assertNotEquals(value.description,SSHAuthPhase.FAILED,value.phase)
            prompt=value
            value.phase==SSHAuthPhase.CREDENTIALS && value.request>0
        }
        val bytes=tmux.getString(key).toByteArray()
        session.privateKey(checkNotNull(prompt),bytes)
        assertTrue("tmux credential caller buffer cleared",bytes.all { it==0.toByte() })
        await {
            val auth=session.authentication()
            assertNotEquals(auth.description,SSHAuthPhase.FAILED,auth.phase)
            ready()
        }
    }
    private suspend fun manager(socket: String,budget: Int=5000): RemoteTmuxSession {
        val session=RemoteTmuxSession(request(),budget,socket).also { managers+=it }
        authenticate(session) { session.state().ready }
        assertEquals(0,session.selectExecutable(tmux.getString("path")).status)
        return session
    }
    private fun attach(manager: RemoteTmuxSession,entry: RemoteTmuxEntry): RemoteSession {
        val session=RemoteSession(request(),manager.target(entry)).also { terminals+=it }
        authenticate(session) { val state=session.state();assertFalse(state.description,state.terminal);state.connected }
        return session
    }
    private fun frame(session: RemoteSession,condition: (TerminalFrame)->Boolean): TerminalFrame {
        var latest: TerminalFrame?=null
        try { await {
            val state=session.state();assertFalse(state.description,state.terminal)
            session.frame(latest?.revision ?: 0)?.let { latest=it }
            latest?.let(condition)==true
        } } catch(failure: AssertionError) {
            throw AssertionError("tmux frame: "+latest?.let { f -> (0 until f.rows).joinToString(" / ",transform=f::line) },failure)
        }
        return checkNotNull(latest)
    }
    @Test fun discoverAndAbsenceAreRealAndDoNotStartAServer(): Unit=runBlocking {
        val connection=RemoteTmuxSession(request(),socketPath=socket()).also { managers+=it }
        authenticate(connection) { connection.state().ready }
        val discovery=connection.discover()
        assertEquals(discovery.error,0,discovery.status)
        assertTrue(discovery.candidates.any { it.path==tmux.getString("path") && it.usable && it.version.startsWith("tmux ") })
        assertEquals(0,connection.selectExecutable(tmux.getString("path")).status)
        val result=connection.list()
        assertEquals(result.error,0,result.status);assertEquals(RemoteTmuxServer.ABSENT,result.server);assertTrue(result.entries.isEmpty())
        assertEquals(RemoteTmuxServer.ABSENT,connection.list().server)
    }
    @Test fun remoteLoginFindsTmuxOutsideExecPathAndUsesTheAbsoluteCandidate(): Unit=runBlocking {
        for ((key,path) in listOf("loginPem" to tmux.getString("path")) +
            if (tmux.has("homebrewPem")) listOf("homebrewPem" to "/opt/homebrew/bin/tmux") else emptyList()) {
            val connection=RemoteTmuxSession(request(),socketPath=socket()).also { managers+=it }
            authenticate(connection,key) { connection.state().ready }
            val discovery=connection.discover()
            assertEquals(discovery.error,0,discovery.status)
            assertTrue("Remote login candidate missing: $path",discovery.candidates.any { it.path==path && it.usable })
            assertEquals(0,connection.selectExecutable(path).status)
            assertEquals(RemoteTmuxServer.ABSENT,connection.list().server)
            assertEquals(RemoteTmuxResult.CONFIRMED,connection.create("login-path").result)
            val created=connection.list()
            assertEquals("$key: ${created.error}",0,created.status)
            assertEquals("$key must keep its created session",1,created.entries.size)
            val entry=created.entries.single()
            assertEquals("login-path",entry.name)
            assertEquals(RemoteTmuxResult.CONFIRMED,connection.terminate(entry).result)
            connection.closeAndAwait()
        }
    }
    @Test fun createRenameAndTerminateUseActualIdentitiesAndInvalidateOldSelections(): Unit=runBlocking {
        val connection=manager(socket())
        val name="安卓-"+UUID.randomUUID().toString().take(8)
        assertEquals(RemoteTmuxResult.CONFIRMED,connection.create(name).result)
        val first=connection.list().entries.single()
        assertEquals(name,first.name);assertEquals(0,first.destroyUnattached);assertEquals(0,first.exitUnattached)
        assertEquals(RemoteTmuxResult.CONFIRMED,connection.rename(first,name+"-改名").result)
        try { connection.target(first);fail("old selection must be invalidated") } catch(expected: IllegalStateException) { assertTrue(expected.message!!.contains("过期")) }
        val renamed=connection.list().entries.single()
        assertEquals(name+"-改名",renamed.name);assertEquals(first.id,renamed.id);assertEquals(first.serverPid,renamed.serverPid)
        assertEquals(RemoteTmuxResult.CONFIRMED,connection.terminate(renamed).result)
        val empty=connection.list();assertEquals(0,empty.status);assertTrue(empty.entries.isEmpty())
    }
    @Test fun unsupportedAndUnknownVersionsStayDistinctAndCanRecover(): Unit=runBlocking {
        val connection=manager(socket())
        assertEquals(7,connection.selectExecutable(tmux.getString("unsupported")).status)
        assertTrue(connection.list().status!=0)
        assertEquals(6,connection.selectExecutable(tmux.getString("unknown")).status)
        val missing=connection.selectExecutable(tmux.getString("path")+"-missing")
        assertNotEquals(0,missing.status)
        assertTrue(connection.list().entries.isEmpty())
        assertEquals(0,connection.selectExecutable(tmux.getString("path")).status)
        assertEquals(RemoteTmuxServer.ABSENT,connection.list().server)
    }
    @Test fun externalRenameRejectsTheOldIdentityWithoutTerminatingItsTask(): Unit=runBlocking {
        val address=socket();val first=manager(address)
        assertEquals(RemoteTmuxResult.CONFIRMED,first.create("before").result)
        val stale=first.list().entries.single()
        val other=manager(address)
        assertEquals(RemoteTmuxResult.CONFIRMED,other.rename(other.list().entries.single(),"after").result)
        val rejected=first.terminate(stale)
        assertEquals(RemoteTmuxResult.REJECTED,rejected.result);assertEquals(2,rejected.status)
        assertEquals("after",other.list().entries.single().name)
    }
    @Test fun closedClientAndManagementConnectionPreserveTheInnerTaskPid(): Unit=runBlocking {
        val address=socket();val first=manager(address)
        assertEquals(RemoteTmuxResult.CONFIRMED,first.create("keep-task").result)
        val terminal=attach(first,first.list().entries.single())
        terminal.commit("stty -echo; PS1=''; KEEP_TMUX=kept; printf '\\033[2J\\033[HTMUX_PID:%s:%s\\r\\n' \"\u0024\u0024\" \"\u0024KEEP_TMUX\"\r")
        val initial=frame(terminal) { it.line(0).matches(Regex("TMUX_PID:[0-9]+:kept")) }.line(0)
        first.closeAndAwait()
        terminal.commit("printf '\\033[2J\\033[HTMUX_PID:%s:%s\\r\\n' \"\u0024\u0024\" \"\u0024KEEP_TMUX\"\r")
        assertEquals(initial,frame(terminal) { it.line(0)==initial }.line(0))
        terminal.close()
        await { owners("NativeSession")==0 }
        val refreshed=manager(address)
        val same=refreshed.list().entries.single()
        assertEquals("keep-task",same.name)
        val resumed=attach(refreshed,same)
        resumed.commit("printf '\\033[2J\\033[HTMUX_PID:%s:%s\\r\\n' \"\u0024\u0024\" \"\u0024KEEP_TMUX\"\r")
        assertEquals(initial,frame(resumed) { it.line(0)==initial }.line(0))
    }
    @Test fun mutationRefusalIsDifferentFromAnUnacknowledgedSuccessfulCreate(): Unit=runBlocking {
        val address=socket();val connection=manager(address,budget=1500)
        assertEquals(0,connection.selectExecutable(tmux.getString("rejected")).status)
        val rejected=connection.create("refused")
        assertEquals(RemoteTmuxResult.REJECTED,rejected.result);assertNotEquals(0,rejected.status)
        assertEquals(0,connection.selectExecutable(tmux.getString("uncertain")).status)
        val unknown=connection.create("may-exist")
        assertEquals("sent create without acknowledgement must stay unknown",RemoteTmuxResult.UNKNOWN,unknown.result)
        assertNotEquals(0,unknown.status)
        val inspect=manager(address)
        assertEquals("may-exist",inspect.list().entries.single().name)
    }
    @Test fun cancellationWaitsForNativeOwnerRelease(): Unit=runBlocking {
        val connection=manager(socket())
        val job=launch(Dispatchers.IO) { connection.selectExecutable(tmux.getString("slow")) }
        await { connection.state().operationRunning }
        val calibrationStart=SystemClock.elapsedRealtime()
        repeat(10) { SystemClock.sleep(10) }
        val calibration=SystemClock.elapsedRealtime()-calibrationStart
        assertTrue("runner calibration",calibration<500)
        val started=SystemClock.elapsedRealtime()
        job.cancelAndJoin()
        assertTrue("cancel waits for bounded native release",SystemClock.elapsedRealtime()-started<maxOf(1500,calibration*3+500))
        assertEquals(0,owners("NativeTmux"));assertTrue(connection.state().finished)
    }
    @Test fun attachUsesTheAcknowledgedManagementAddressInsteadOfANewCandidate(): Unit=runBlocking {
        val connection=manager(socket())
        assertEquals(RemoteTmuxResult.CONFIRMED,connection.create("pinned").result)
        val target=connection.target(connection.list().entries.single())
        val changed=ResolvedSSHConnection(host,fixture.getInt("port"),fixture.getString("user"),known.path,
            byteArrayOf(192.toByte(),0,2,1),timeoutMillis=3000)
        val terminal=RemoteSession(changed,target).also { terminals+=it }
        authenticate(terminal) { val state=terminal.state();assertFalse(state.description,state.terminal);state.connected }
        terminal.commit("printf '\\033[2J\\033[HPINNED_ENDPOINT\\r\\n'\r")
        frame(terminal) { it.line(0)=="PINNED_ENDPOINT" }
    }
    @Test fun executionNamespaceIsValidatedBeforeAttachingADifferentKeysEnvironment(): Unit=runBlocking {
        val connection=manager(socket())
        assertEquals(RemoteTmuxResult.CONFIRMED,connection.create("correct-server").result)
        val terminal=RemoteSession(request(),connection.target(connection.list().entries.single())).also { terminals+=it }
        var prompt: SSHAuthState?=null
        await { terminal.authentication().also { prompt=it }.phase==SSHAuthPhase.CREDENTIALS }
        val key=tmux.getString("redirectPem").toByteArray()
        terminal.privateKey(checkNotNull(prompt),key)
        assertTrue(key.all { it==0.toByte() })
        await { terminal.state().let { it.connected || it.terminal } }
        assertFalse("must reject a different execution tmux namespace before PTY attach",terminal.state().connected)
        assertEquals("execution key authenticated before namespace validation",SSHAuthPhase.TAKEN,terminal.authentication().phase)
        assertTrue(terminal.state().terminal)
        assertEquals("correct-server",connection.list().entries.single().name)
    }
    @Test fun nativeRefreshEpochRejectsAnOldTargetEvenIfTheCoreIdentityStillExists(): Unit=runBlocking {
        val connection=manager(socket())
        assertEquals(RemoteTmuxResult.CONFIRMED,connection.create("fresh-only").result)
        val old=connection.list().entries.single()
        val epoch=old.javaClass.getDeclaredField("snapshot").apply { isAccessible=true }.getLong(old)
        val index=old.javaClass.getDeclaredField("index").apply { isAccessible=true }.getInt(old)
        connection.list()
        val handle=connection.javaClass.getDeclaredField("handle").apply { isAccessible=true }.getLong(connection)
        val type=Class.forName("com.hanxi.ghosterm.bridge.NativeTmux")
        val instance=type.getField("INSTANCE").get(null)
        val begin=type.getDeclaredMethod("begin",Long::class.javaPrimitiveType,Int::class.javaPrimitiveType,
            Long::class.javaPrimitiveType,Int::class.javaPrimitiveType,ByteArray::class.java)
        val result=type.getDeclaredMethod("result",Long::class.javaPrimitiveType,Long::class.javaPrimitiveType)
        val finish=type.getDeclaredMethod("finish",Long::class.javaPrimitiveType,Long::class.javaPrimitiveType)
        val request=begin.invoke(instance,handle,6,epoch,index,byteArrayOf()) as Long
        var packet: ByteArray?=null
        await { packet=result.invoke(instance,handle,request) as ByteArray?;packet!=null }
        try {
            val data=java.nio.ByteBuffer.wrap(checkNotNull(packet)).order(java.nio.ByteOrder.BIG_ENDIAN)
            assertNotEquals("native refresh epoch must reject an old target",0,data.getInt(8))
            assertEquals("unattempted stale selection is a rejection",1,data.getInt(12))
        } finally { finish.invoke(instance,handle,request) }
        assertEquals("fresh-only",connection.list().entries.single().name)
    }
    @Test fun equalNumericAuthenticationIdentityCannotBypassTheOwnersScope(): Unit=runBlocking {
        val first=RemoteTmuxSession(request(),socketPath=socket()).also { managers+=it }
        val second=RemoteTmuxSession(request(),socketPath=socket()).also { managers+=it }
        var foreign: SSHAuthState?=null;var current: SSHAuthState?=null
        await { first.authentication().also { foreign=it }.phase==SSHAuthPhase.CREDENTIALS }
        await { second.authentication().also { current=it }.phase==SSHAuthPhase.CREDENTIALS }
        val actual=checkNotNull(current)
        val id=actual.javaClass.getDeclaredField("owner").apply { isAccessible=true }.getLong(actual)
        val scope=actual.javaClass.getDeclaredField("scope").apply { isAccessible=true }.get(foreign)
        // Independent native registries can have equal numeric handles. Keep
        // the destination's actual request but lend another owner's scope.
        val constructor=SSHAuthState::class.java.declaredConstructors.single { it.parameterCount==10 }
        val collision=constructor.newInstance(id,actual.request,actual.phase,actual.error,actual.host,
            actual.deadline,actual.methods,actual.keyType,actual.fingerprint,scope) as SSHAuthState
        val secret=tmux.getString("pem").toByteArray()
        try { second.privateKey(collision,secret);fail("equal numeric handle must not bypass authentication scope") }
        catch(expected: IllegalStateException) { assertTrue(expected.message!!.contains("不属于")) }
        assertTrue(secret.all { it==0.toByte() })
    }
    @Test fun anotherOwnersCredentialPromptIsRejectedAndSecretIsCleared(): Unit=runBlocking {
        val first=RemoteTmuxSession(request(),socketPath=socket()).also { managers+=it }
        val second=RemoteTmuxSession(request(),socketPath=socket()).also { managers+=it }
        var prompt: SSHAuthState?=null
        await { first.authentication().also { prompt=it }.phase==SSHAuthPhase.CREDENTIALS }
        await { second.authentication().phase==SSHAuthPhase.CREDENTIALS }
        val secret=tmux.getString("pem").toByteArray()
        try { second.privateKey(checkNotNull(prompt),secret);fail("foreign authentication prompt accepted") }
        catch(expected: IllegalStateException) { assertTrue(expected.message!!.contains("不属于")) }
        assertTrue(secret.all { it==0.toByte() })
    }
}
