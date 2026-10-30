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
import java.io.File
import java.net.InetAddress
import java.net.Socket
import java.util.UUID

/** Protocol barriers identify the exact SSH stage; normal PTY/PID uses RemoteTerminalTest. */
@RunWith(AndroidJUnit4::class)
class SSHChannelTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private val host = "channel-fixture.ghosterm.test"
    private val fixture: JSONObject by lazy {
        val args = InstrumentationRegistry.getArguments()
        Socket("127.0.0.1", requireNotNull(args.getString("fixturePort")).toInt()).use { socket ->
            socket.soTimeout=5000
            socket.getOutputStream().write((args.getString("fixtureToken")+"\n").toByteArray())
            JSONObject(socket.getInputStream().bufferedReader().readLine())
        }
    }
    private val peer get() = fixture.getJSONObject("channels")
    private fun control(op: String, user: String = "", stage: String = "channel", action: String = "hold"): JSONObject =
        Socket("127.0.0.1",peer.getInt("controlPort")).use { socket ->
            socket.soTimeout=5000
            val request=JSONObject().put("token",peer.getString("token")).put("op",op).put("user",user).put("stage",stage).put("action",action)
            socket.getOutputStream().write((request.toString()+"\n").toByteArray())
            JSONObject(socket.getInputStream().bufferedReader().readLine())
        }
    private fun events(user: String): List<String> {
        val values=control("state",user).getJSONArray("events")
        return (0 until values.length()).map(values::getString)
    }
    private fun owners(): Int {
        val type=Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
        return type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null)) as Int
    }
    private fun await(timeout: Long=10000, message: String="等待 SSH 通道状态超时", test: () -> Boolean) {
        val end=SystemClock.uptimeMillis()+timeout
        do { if(test())return;SystemClock.sleep(10) } while(SystemClock.uptimeMillis()<end)
        fail(message)
    }
    private fun calibration(): Long {
        val start=SystemClock.uptimeMillis()
        repeat(20) { SystemClock.sleep(10) }
        return SystemClock.uptimeMillis()-start
    }
    private fun directory(test: (File) -> Unit) {
        val dir=File(instrumentation.targetContext.cacheDir,"channel-${UUID.randomUUID()}").apply { mkdirs() }
        try { test(dir) } finally { await { owners()==0 };dir.deleteRecursively() }
    }
    private fun known(dir: File): File = File(dir,"known_hosts").apply {
        writeText("[$host]:${peer.getInt("port")} "+peer.getString("public")+"\n")
    }
    private fun connection(file: File,user: String,budget: Int=10000) = ResolvedSSHConnection(
        host,peer.getInt("port"),user,file.absolutePath,byteArrayOf(127,0,0,1),timeoutMillis=budget)
    private fun credentials(session: RemoteSession): SSHAuthState {
        await {
            val auth=session.authentication()
            assertNotEquals(auth.description,SSHAuthPhase.FAILED,auth.phase)
            auth.phase==SSHAuthPhase.CREDENTIALS && auth.request>0
        }
        return session.authentication()
    }
    private fun supply(session: RemoteSession,prompt: SSHAuthState) {
        val key=fixture.getString("pem").toByteArray()
        session.privateKey(prompt,key)
        assertTrue(key.all { it==0.toByte() })
    }
    private fun prefix(stage: String) = listOf("channel","pty","shell").takeWhile { it!=stage }+stage
    @Before fun previousOwnersRelease() { await { owners()==0 } }

    private fun cancelAt(stage: String) = directory { dir ->
        val user=control("new",stage=stage).getString("user")
        val bound=maxOf(1500L,(calibration()*1.5).toLong()+500)
        val session=RemoteSession(connection(known(dir),user))
        try {
            supply(session,credentials(session))
            await { events(user).contains(stage) }
            assertFalse(session.state().connected)
            session.close()
            await(bound,"取消 $stage 后 native owner 未及时释放") { owners()==0 }
            control("release",user)
            SystemClock.sleep(100)
            assertEquals("取消后仍发出后续 SSH 请求",prefix(stage),events(user))
        } finally { session.close();control("release",user) }
    }
    @Test fun cancelChannelOpenReleasesOwner() = cancelAt("channel")
    @Test fun cancelPtyRequestReleasesOwner() = cancelAt("pty")
    @Test fun cancelShellRequestReleasesOwner() = cancelAt("shell")

    private fun remainingBudget(stage: String) = directory { dir ->
        val user=control("new",stage=stage).getString("user")
        val known=known(dir)
        val bound=maxOf(4000L,3000+(calibration()*1.5).toLong()+500)
        val start=SystemClock.uptimeMillis()
        val session=RemoteSession(connection(known,user,3000))
        try {
            val prompt=credentials(session)
            assertTrue("夹具准备耗尽连接预算",SystemClock.uptimeMillis()-start<1000)
            SystemClock.sleep((1500-(SystemClock.uptimeMillis()-start)).coerceAtLeast(0))
            supply(session,prompt)
            await { events(user).contains(stage) }
            await((bound-(SystemClock.uptimeMillis()-start)).coerceAtLeast(1),"$stage 打开重置了连接总预算") { session.state().terminal }
            assertEquals("打开远端 PTY/shell 失败或超时",session.state().error)
            assertFalse(session.state().connected)
            assertEquals(prefix(stage),events(user))
        } finally { session.close();control("release",user) }
    }
    @Test fun channelOpenUsesRemainingConnectionBudget() = remainingBudget("channel")
    @Test fun ptyRequestUsesRemainingConnectionBudget() = remainingBudget("pty")
    @Test fun shellRequestUsesRemainingConnectionBudget() = remainingBudget("shell")

    private fun refused(stage: String, error: String="服务器拒绝远端 PTY/shell", action: String="deny") = directory { dir ->
        val user=control("new",stage=stage,action=action).getString("user")
        RemoteSession(connection(known(dir),user)).use { session ->
            supply(session,credentials(session))
            await { session.state().terminal }
            assertEquals(error,session.state().error)
            assertFalse(session.state().connected)
            assertEquals(prefix(stage),events(user))
        }
    }
    @Test fun channelRefusalIsIoAndSendsNoPty() = refused("channel", "打开远端 PTY/shell 失败或超时")
    @Test fun ptyRefusalDoesNotFallBackToShell() = refused("pty")
    @Test fun shellRefusalIsExplicit() = refused("shell")

    @Test fun channelDisconnectIsNotRefusal() = refused("channel", "打开远端 PTY/shell 失败或超时", "disconnect")
    @Test fun ptyDisconnectIsNotRefusal() = refused("pty", "打开远端 PTY/shell 失败或超时", "disconnect")
    @Test fun shellDisconnectIsNotRefusal() = refused("shell", "打开远端 PTY/shell 失败或超时", "disconnect")

    @Test fun productCancellationDropsLateShellAndNewRequestIsIndependent() = directory { dir ->
        val user=control("new",stage="shell").getString("user")
        val port=peer.getInt("port")
        val pem=fixture.getString("pem").toByteArray()
        val home=File(dir,"home/.ssh").apply { mkdirs() }
        known(home)
        val cancelBound=maxOf(1500L,(calibration()*1.5).toLong()+500)
        val store=ViewModelStore()
        val application=object:Application() { override fun getFilesDir()=dir }
        lateinit var model: SessionsViewModel
        instrumentation.runOnMainSync {
            model=SessionsViewModel(application,SSHResolver { listOf(InetAddress.getByAddress(byteArrayOf(127,0,0,1))) })
            store.put("test",model)
            model.connect(PreparedSSH("channel",host,user,port,emptyList()))
        }
        try {
            await {
                var ready=false
                instrumentation.runOnMainSync {
                    val attempt=model.connecting!!
                    assertFalse(attempt.message,attempt.failed)
                    ready=attempt.auth?.phase==SSHAuthPhase.CREDENTIALS
                }
                ready
            }
            instrumentation.runOnMainSync { model.connecting!!.let { it.connection!!.privateKey(it.auth!!,pem) } }
            await { events(user).contains("shell") }
            instrumentation.runOnMainSync { model.connecting!!.let { model.cancelConnection(it.id) } }
            await(cancelBound) { owners()==0 }
            control("release",user)
            SystemClock.sleep(100)
            instrumentation.runOnMainSync {
                assertNull(model.connecting); assertTrue(model.sessions.isEmpty())
                model.connect(PreparedSSH("new",host,user,port,emptyList()))
            }
            await {
                var ready=false
                instrumentation.runOnMainSync { ready=model.connecting?.auth?.phase==SSHAuthPhase.CREDENTIALS }
                ready
            }
            instrumentation.runOnMainSync { assertTrue(model.sessions.isEmpty());model.connecting!!.let { model.cancelConnection(it.id) } }
        } finally {
            pem.fill(0);control("release",user)
            instrumentation.runOnMainSync { store.clear() }
        }
    }
}
