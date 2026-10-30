package com.hanxi.ghosterm

import android.os.SystemClock
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import com.hanxi.ghosterm.terminal.TerminalFrame
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.Socket
import java.util.UUID

@RunWith(AndroidJUnit4::class)
class RemoteTerminalTest {
    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        val port = requireNotNull(args.getString("fixturePort")) { "Run scripts/test-android-ssh.sh (real sshd required)" }.toInt()
        Socket("127.0.0.1", port).use { socket ->
            socket.soTimeout = 5000
            socket.getOutputStream().write((args.getString("fixtureToken") + "\n").toByteArray())
            JSONObject(socket.getInputStream().bufferedReader().readLine())
        }
    }
    private val host = "android-fixture.ghosterm.test"
    private fun connection(file: File, timeout: Int = 10000) = ResolvedSSHConnection(host, fixture.getInt("port"),
        fixture.getString("user"), file.absolutePath, byteArrayOf(127,0,0,1), timeoutMillis = timeout)
    private fun record(marker: String = "", other: Boolean = false) =
        marker + "[$host]:${fixture.getInt("port")} " + fixture.getString(if (other) "otherPublic" else "public")
    private fun inDirectory(test: (File) -> Unit) {
        val dir = File(InstrumentationRegistry.getInstrumentation().targetContext.cacheDir, "ssh-${UUID.randomUUID()}").apply { mkdirs() }
        try { test(dir) } finally { dir.deleteRecursively() }
    }
    private fun await(timeout: Long = 10000, test: () -> Boolean) {
        val end = SystemClock.uptimeMillis() + timeout
        do { if (test()) return; SystemClock.sleep(10) } while (SystemClock.uptimeMillis() < end)
        fail("等待真实 SSH 状态超时")
    }
    private fun phase(session: RemoteSession, desired: SSHAuthPhase): SSHAuthState {
        var latest = session.authentication()
        await {
            latest = session.authentication()
            if (desired != SSHAuthPhase.FAILED) assertNotEquals(latest.description, SSHAuthPhase.FAILED, latest.phase)
            latest.phase == desired && (desired !in listOf(SSHAuthPhase.HOSTKEY, SSHAuthPhase.CREDENTIALS) || latest.request > 0)
        }
        return latest
    }
    private fun authenticate(session: RemoteSession): SSHAuthState {
        val prompt = phase(session, SSHAuthPhase.CREDENTIALS)
        assertTrue(prompt.methods and 1 != 0)
        val key = fixture.getString("pem").toByteArray()
        session.privateKey(prompt, key)
        assertTrue("caller secret buffer not cleared", key.all { it == 0.toByte() })
        await { val state = session.state(); assertFalse(state.description, state.terminal); state.connected }
        return prompt
    }
    private fun frame(session: RemoteSession, test: (TerminalFrame) -> Boolean): TerminalFrame {
        var current: TerminalFrame? = null
        await {
            session.frame(current?.revision ?: 0)?.let { current = it }
            current?.let(test) == true
        }
        return current!!
    }
    private fun ownerCount(): Int {
        val type = Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
        return type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null)) as Int
    }

    @Test fun unknownHostTrustKeyAuthenticationAndRemotePtyReachVt() = inDirectory { dir ->
        val known = File(dir,"known_hosts")
        RemoteSession(connection(known)).use { session ->
            val trust = phase(session, SSHAuthPhase.HOSTKEY)
            assertEquals(SSHHostState.UNKNOWN, trust.host)
            assertEquals("ssh-ed25519", trust.keyType)
            assertEquals(fixture.getString("fingerprint"), trust.fingerprint)
            assertEquals(0, trust.methods)
            assertFalse(known.exists())
            session.decide(trust, true)
            authenticate(session)
            assertTrue(known.readText(), known.readText().contains("[$host]:${fixture.getInt("port")}"))
            assertFalse(known.readText().contains("127.0.0.1"))
            session.resize(61,19,8,17)
            frame(session) { it.cols == 61 && it.rows == 19 }
            session.commit("stty -echo; PS1=''; printf '\\033[2J\\033[H'; stty size; printf 'SSH_中é\\r\\n'\r")
            val output = frame(session) { it.line(1) == "SSH_中é" }
            assertEquals("19 61", output.line(0))
            session.commit("exit 7\r")
            await { session.state().terminal }
            assertNull("SSH backend does not expose exit-status availability", session.state().exitCode)
        }
    }
    @Test fun rejectionStopsBeforeCredentialsAndDoesNotSaveTrust() = inDirectory { dir ->
        val known = File(dir,"known_hosts")
        RemoteSession(connection(known)).use { session ->
            val prompt = phase(session,SSHAuthPhase.HOSTKEY)
            session.decide(prompt,false)
            val failed = phase(session,SSHAuthPhase.FAILED)
            assertEquals(5,failed.error); assertEquals(0,failed.methods)
            assertFalse(known.exists())
            assertThrows(IllegalStateException::class.java) { session.decide(prompt,true) }
        }
    }
    @Test fun revokedMismatchAndUnsupportedStayDistinct() = inDirectory { dir ->
        listOf(Triple("@revoked ", false, SSHHostState.REVOKED), Triple("",true,SSHHostState.MISMATCH),
            Triple("@cert-authority ",false,SSHHostState.UNCHECKED)).forEach { (marker,other,state) ->
            val known = File(dir,"known_hosts").apply { writeText(record(marker,other)) }
            val before = known.readBytes()
            RemoteSession(connection(known)).use { session ->
                val failed = phase(session,SSHAuthPhase.FAILED)
                assertEquals(state,failed.host)
                assertEquals(if (marker == "@cert-authority ") 7 else 5,failed.error)
                assertEquals(0,failed.request); assertEquals(0,failed.methods)
                assertArrayEquals(before,known.readBytes())
                if (state == SSHHostState.REVOKED) assertTrue(failed.description.contains("已撤销"))
            }
        }
    }
    @Test fun savingTrustFailureStopsBeforeCredentials() = inDirectory { dir ->
        val known = File(dir,"missing/known_hosts")
        RemoteSession(connection(known)).use { session ->
            session.decide(phase(session,SSHAuthPhase.HOSTKEY),true)
            val failed = phase(session,SSHAuthPhase.FAILED)
            assertEquals(3,failed.error); assertEquals(SSHHostState.UNKNOWN,failed.host)
            assertEquals(0,failed.methods); assertFalse(known.exists())
            assertEquals("无法保存主机信任记录，连接已停止",failed.description)
        }
    }
    @Test fun wrongKeyRetriesWithNewRequestAndClearsSecrets() = inDirectory { dir ->
        val known = File(dir,"known_hosts").apply { writeText(record()) }
        RemoteSession(connection(known)).use { session ->
            val first = phase(session,SSHAuthPhase.CREDENTIALS)
            assertEquals(SSHHostState.KNOWN,first.host)
            val wrong = fixture.getString("wrongPem").toByteArray()
            session.privateKey(first,wrong)
            assertTrue(wrong.all { it == 0.toByte() })
            await { session.authentication().let { it.phase == SSHAuthPhase.CREDENTIALS && it.request > 0 && it.request != first.request } }
            val retry = session.authentication()
            assertEquals(4,retry.error)
            val stale = fixture.getString("pem").toByteArray()
            assertThrows(IllegalStateException::class.java) { session.privateKey(first,stale) }
            assertTrue(stale.all { it == 0.toByte() })
            authenticate(session)
        }
    }
    @Test fun promptsCannotCrossSessionsAndUnsupportedPasswordsAreCleared() = inDirectory { dir ->
        val known = File(dir,"known_hosts").apply { writeText(record()) }
        RemoteSession(connection(known)).use { a -> RemoteSession(connection(known)).use { b ->
            val pa = phase(a,SSHAuthPhase.CREDENTIALS); val pb = phase(b,SSHAuthPhase.CREDENTIALS)
            val secret = fixture.getString("pem").toByteArray()
            assertThrows(IllegalStateException::class.java) { b.privateKey(pa,secret) }
            assertTrue(secret.all { it == 0.toByte() })
            val password = "unused password".toByteArray()
            assertThrows(IllegalStateException::class.java) { b.password(pb,password) }
            assertTrue(password.all { it == 0.toByte() })
            assertEquals(pb.request,b.authentication().request)
            authenticate(a); authenticate(b)
        } }
    }
    @Test fun deadlineContinuesWhileWaitingForTrust() = inDirectory { dir ->
        RemoteSession(connection(File(dir,"known_hosts"),2000)).use { session ->
            val pending = phase(session,SSHAuthPhase.HOSTKEY)
            val failed = phase(session,SSHAuthPhase.FAILED)
            assertTrue(failed.deadline); assertEquals(0,failed.methods)
            assertThrows(IllegalStateException::class.java) { session.decide(pending,true) }
        }
    }
    @Test fun deadlineContinuesWhileWaitingForCredentials() = inDirectory { dir ->
        val known = File(dir,"known_hosts").apply { writeText(record()) }
        RemoteSession(connection(known,2000)).use { session ->
            val pending = phase(session,SSHAuthPhase.CREDENTIALS)
            val failed = phase(session,SSHAuthPhase.FAILED)
            assertTrue(failed.deadline)
            val late = fixture.getString("pem").toByteArray()
            assertThrows(IllegalStateException::class.java) { session.privateKey(pending,late) }
            assertTrue(late.all { it == 0.toByte() })
        }
    }
    @Test fun malformedNativeInputsFailWithoutOpeningAnOwner() = inDirectory { dir ->
        val before = ownerCount()
        val known = File(dir,"known_hosts")
        listOf(byteArrayOf(), byteArrayOf(1,2,3), ByteArray(17)).forEach { address ->
            assertThrows(IllegalStateException::class.java) {
                RemoteSession(ResolvedSSHConnection(host,fixture.getInt("port"),fixture.getString("user"),known.absolutePath,address))
            }
        }
        assertThrows(IllegalStateException::class.java) {
            RemoteSession(ResolvedSSHConnection("bad\u0000host",22,"user",known.absolutePath,byteArrayOf(127,0,0,1)))
        }
        assertEquals(before,ownerCount()); assertFalse(known.exists())
        known.writeText(record())
        RemoteSession(connection(known)).use { session ->
            val prompt = phase(session,SSHAuthPhase.CREDENTIALS)
            val invalid = byteArrayOf(65,0,66,67)
            assertThrows(IllegalStateException::class.java) { session.privateKey(prompt,invalid) }
            assertTrue(invalid.all { it == 0.toByte() })
            assertEquals(prompt.request,session.authentication().request)
            authenticate(session)
        }
    }
    @Test fun cancelInvalidatesHandleAndReleasesNativeOwner() = inDirectory { dir ->
        val before = ownerCount()
        val session = RemoteSession(connection(File(dir,"known_hosts")))
        try {
            val pending = phase(session,SSHAuthPhase.HOSTKEY)
            val calibrationStart = SystemClock.uptimeMillis()
            repeat(20) { SystemClock.sleep(10) }
            val calibration = SystemClock.uptimeMillis() - calibrationStart
            val started = SystemClock.uptimeMillis()
            session.close()
            assertTrue("close waited for network",SystemClock.uptimeMillis()-started<maxOf(250L, (calibration * 1.5).toLong()+50))
            assertThrows(IllegalStateException::class.java) { session.decide(pending,true) }
            assertThrows(IllegalStateException::class.java) { session.authentication() }
            await(maxOf(3000L, calibration*3+500)) { ownerCount() <= before }
            assertFalse(File(dir,"known_hosts").exists())
        } finally { session.close() }
    }
}
