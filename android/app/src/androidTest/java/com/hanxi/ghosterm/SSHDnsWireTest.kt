package com.hanxi.ghosterm

import android.app.Application
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.os.CancellationSignal
import android.net.DnsResolver
import android.system.ErrnoException
import androidx.lifecycle.ViewModelStore
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.*
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import java.io.Closeable
import java.io.File
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.SocketTimeoutException
import java.util.UUID
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

/** Real DnsResolver -> netd -> UDP fixture. Only a random qname token is redirected;
 * device DNS settings, unrelated requests, VPN and private DNS remain untouched. */
class SSHDnsWireTest {
    private val instrumentation=InstrumentationRegistry.getInstrumentation()
    private val root=File(instrumentation.targetContext.cacheDir,"dns-wire-${UUID.randomUUID()}")
    private fun shell(command:String)=instrumentation.uiAutomation.executeShellCommand(command).use {
        ParcelFileDescriptor.AutoCloseInputStream(it).bufferedReader().readText().trim()
    }
    private fun await(seconds:Long=10,block:()->Boolean) {
        val deadline=SystemClock.elapsedRealtime()+seconds*1000
        while(SystemClock.elapsedRealtime()<deadline) { if(block())return;SystemClock.sleep(10) }
        fail("DNS wire fixture condition timed out")
    }
    private fun owners():Int {
        val type=Class.forName("com.hanxi.ghosterm.bridge.NativeSession")
        return type.getDeclaredMethod("ownerCount").invoke(type.getField("INSTANCE").get(null)) as Int
    }
    @Before fun setup() { root.mkdirs();await { owners()==0 } }
    @After fun cleanup() { await { owners()==0 };root.deleteRecursively() }
    private enum class Mode { POSITIVE, NXDOMAIN, EMPTY, SERVFAIL, HOLD }
    /** Restrict only the AOSP reachability probes for this test UID. DNS and
     * loopback SSH remain routable; no DNS/VPN settings or other UIDs change. */
    private suspend fun <T> withoutPublicProbeRoutes(block:suspend ()->T):T {
        assertEquals("1",shell("getprop ro.kernel.qemu"))
        assertTrue(shell("su 0 id").contains("uid=0"))
        val uid=android.os.Process.myUid()
        val priority=4000+java.security.SecureRandom().nextInt(3000)
        val rules=listOf("-4" to "8.8.8.8/32","-6" to "2000::/128")
        val installed=mutableListOf<Pair<String,String>>()
        var failure:Throwable?=null
        try {
            for((family,address) in rules) {
                assertFalse("Probe priority already occupied",shell("su 0 ip $family rule show").lineSequence().any { it.startsWith("$priority:") })
                val rule="pref $priority uidrange $uid-$uid to $address unreachable"
                installed+=family to rule
                shell("su 0 ip $family rule add $rule")
                assertTrue("Probe route did not install",shell("su 0 ip $family rule show").lineSequence().any { it.startsWith("$priority:") && it.contains("uidrange $uid-$uid") })
            }
            return block()
        } catch(error:Throwable) { failure=error;throw error }
        finally {
            var cleanup:Throwable?=null
            for((family,rule) in installed.asReversed())try {
                shell("su 0 ip $family rule del $rule")
                assertFalse("Probe route leaked",shell("su 0 ip $family rule show").lineSequence().any { it.startsWith("$priority:") && it.contains("uidrange $uid-$uid") })
            } catch(error:Throwable) { if(cleanup==null)cleanup=error else cleanup.addSuppressed(error) }
            cleanup?.let { if(failure!=null)failure.addSuppressed(it) else throw it }
        }
    }
    private suspend fun legacyQuery(host:String):List<InetAddress> = suspendCancellableCoroutine { waiting ->
        val signal=CancellationSignal();waiting.invokeOnCancellation { signal.cancel() }
        DnsResolver.getInstance().query(null,host,DnsResolver.FLAG_EMPTY,{ it.run() },signal,
            object:DnsResolver.Callback<List<InetAddress>> {
                override fun onAnswer(answer:List<InetAddress>,rcode:Int) { if(waiting.isActive)waiting.resume(answer) }
                override fun onError(error:DnsResolver.DnsException) { if(waiting.isActive)waiting.resumeWithException(error) }
            })
    }
    @Test fun domainResolutionDoesNotDependOnPublicProbeRoutes()=runBlocking {
        val fixture=sshFixture()
        Dns(Mode.POSITIVE).use { dns -> withoutPublicProbeRoutes {
            Socket().use { it.connect(InetSocketAddress("127.0.0.1",fixture.getInt("port")),2000) }
            val legacy=runCatching { withTimeout(5000) { legacyQuery(dns.host) } }.exceptionOrNull()
            assertTrue("Legacy query must reproduce its preflight failure: $legacy",legacy is DnsResolver.DnsException)
            assertEquals(64,((legacy as DnsResolver.DnsException).cause as ErrnoException).errno)
            assertEquals("Preflight failed before sending DNS",0,dns.packets.get())
            val result=runCatching { withTimeout(10000) { AndroidSSHResolver.resolve(dns.host) } }
            assertTrue("Domain resolution must not depend on public probe routes: ${result.exceptionOrNull()}",result.isSuccess)
            assertTrue(result.getOrThrow().any { it.isLoopbackAddress })
            assertTrue("Actual system DNS query must reach the fixture",dns.packets.get()>0)
            verifySshIdentityAndOutput(dns.host,result.getOrThrow(),fixture)
        } }
    }
    private inner class Dns(private var initial:Mode):Closeable {
        val token="gtdns"+UUID.randomUUID().toString().replace("-","").take(16)
        val host="$token.ghosterm.test"
        @Volatile var mode=initial
        val questions=CopyOnWriteArrayList<String>()
        val packets=AtomicInteger()
        val lateAnswers=AtomicInteger()
        private val lock=Any()
        @Volatile var error:Throwable?=null
        private val held=CopyOnWriteArrayList<DatagramPacket>()
        private val socket=DatagramSocket(0,InetAddress.getByName("127.0.0.1")).apply { soTimeout=200 }
        private val prefix=requireNotNull(InstrumentationRegistry.getArguments().getString("networkRulePrefix")).also {
            require(it.matches(Regex("ghosterm-network-[a-f0-9]{16}")))
        }
        private val tag="$prefix-dns-$token"
        private val rule="OUTPUT -p udp --dport 53 -m string --algo bm --string $token -m comment --comment $tag -j DNAT --to-destination 127.0.0.1:${socket.localPort}"
        private var installed=false
        private val worker:Thread
        init {
            try {
                assertEquals("1",shell("getprop ro.kernel.qemu"));assertTrue(shell("su 0 id").contains("uid=0"))
                shell("su 0 iptables -t nat -I $rule");installed=true
                assertTrue("DNS rule did not install",shell("su 0 iptables -t nat -S OUTPUT").contains(tag))
                worker=thread(name="ghosterm-dns-wire",isDaemon=true) {
                    while(!socket.isClosed) {
                        try {
                            val packet=DatagramPacket(ByteArray(4096),4096);socket.receive(packet)
                            val owned=DatagramPacket(packet.data.copyOf(packet.length),packet.length,packet.address,packet.port)
                            val (name,_,_)=question(owned.data);check(name.startsWith(token))
                            questions+=name
                            synchronized(lock) {
                                val selected=mode
                                if(selected==Mode.HOLD)held+=owned
                                packets.incrementAndGet()
                                if(selected!=Mode.HOLD)reply(owned,selected)
                            }
                        } catch(_:SocketTimeoutException) {} catch(failure:Throwable) { if(!socket.isClosed)error=failure }
                    }
                }
            } catch(failure:Throwable) { socket.close();if(installed)shell("su 0 iptables -t nat -D $rule");throw failure }
        }
        private fun u16(bytes:ByteArray,at:Int)=((bytes[at].toInt() and 255) shl 8) or (bytes[at+1].toInt() and 255)
        private fun question(bytes:ByteArray):Triple<String,Int,Int> {
            check(bytes.size>=12 && u16(bytes,4)==1);var at=12;val labels=ArrayList<String>()
            while(true) {
                check(at<bytes.size);val length=bytes[at++].toInt() and 255
                if(length==0)break
                check(length<=63 && at+length<=bytes.size);labels+=String(bytes,at,length,Charsets.US_ASCII);at+=length
            }
            check(at+4<=bytes.size && u16(bytes,at+2)==1)
            return Triple(labels.joinToString("."),u16(bytes,at),at+4)
        }
        private fun reply(packet:DatagramPacket,selected:Mode,late:Boolean=false) {
            val (_,type,end)=question(packet.data);val positive=selected==Mode.POSITIVE && type==1
            val rcode=when(selected) { Mode.NXDOMAIN->3;Mode.SERVFAIL->2;else->0 }
            val result=packet.data.copyOf(end+(if(positive)16 else 0))
            result[2]=0x85.toByte();result[3]=(0x80 or rcode).toByte()
            result[6]=0;result[7]=if(positive)1 else 0
            for(i in 8..11)result[i]=0
            if(positive) {
                val answer=byteArrayOf(0xc0.toByte(),12,0,1,0,1,0,0,0,0,0,4,127,0,0,1)
                answer.copyInto(result,end)
            }
            socket.send(DatagramPacket(result,result.size,packet.address,packet.port))
            if(late)lateAnswers.incrementAndGet()
        }
        fun release() = synchronized(lock) { mode=Mode.POSITIVE;held.toList().forEach { reply(it,Mode.POSITIVE,true);held.remove(it) } }
        override fun close() {
            try {
                if(installed) { shell("su 0 iptables -t nat -D $rule");installed=false }
                assertFalse("DNS rule leaked",shell("su 0 iptables -t nat -S OUTPUT").contains(tag))
            } finally { socket.close();worker.join(2000) }
            assertNull("DNS responder failed",error)
        }
    }
    private fun verifySshIdentityAndOutput(host:String,addresses:List<InetAddress>,fixture:org.json.JSONObject) {
        val known=File(root,"known_hosts")
        val target=ResolvedSSHConnection(host,fixture.getInt("port"),fixture.getString("user"),known.path,
            addresses.map { ResolvedSSHAddress(it.address,(it as? java.net.Inet6Address)?.scopeId ?: 0) },8000)
        RemoteSession(target).use { session ->
            var auth=session.authentication()
            await { auth=session.authentication();assertNotEquals(auth.description,SSHAuthPhase.FAILED,auth.phase);auth.phase==SSHAuthPhase.HOSTKEY && auth.request>0 }
            assertEquals(fixture.getString("fingerprint"),auth.fingerprint);session.decide(auth,true)
            await { auth=session.authentication();assertNotEquals(auth.description,SSHAuthPhase.FAILED,auth.phase);auth.phase==SSHAuthPhase.CREDENTIALS && auth.request>0 }
            val pem=fixture.getString("pem").toByteArray();session.privateKey(auth,pem);assertTrue(pem.all { it==0.toByte() })
            await { val state=session.state();assertFalse(state.description,state.terminal);state.connected }
            session.resize(70,20,8,16);session.commit("printf '\\033[2J\\033[HDNS_WIRE_OK\\r\\n'\r")
            await { session.frame(0)?.let { frame -> (0 until frame.rows).any { frame.line(it).trim()=="DNS_WIRE_OK" } }==true }
            assertTrue(known.readText().contains("[$host]:${fixture.getInt("port")}"));assertFalse(known.readText().contains("127.0.0.1"))
        }
    }
    @Test fun realDnsAnswersReachSshAndPreserveUnicodeHostIdentity()=runBlocking {
        val fixture=sshFixture()
        Dns(Mode.POSITIVE).use { dns ->
            val host="${dns.token}.例子.测试"
            val resolved=runCatching { withTimeout(15000) { AndroidSSHResolver.resolve(host) } }
            assertTrue("real DNS must resolve: ${resolved.exceptionOrNull()}",resolved.isSuccess)
            val addresses=resolved.getOrThrow()
            assertTrue(addresses.any { it.isLoopbackAddress });assertTrue(dns.questions.isNotEmpty())
            assertTrue(dns.questions.all { it=="${dns.token}.xn--fsqu00a.xn--0zwm56d" })
            verifySshIdentityAndOutput(host,addresses,fixture)
        }
    }
    @Test fun actualNxdomainEmptyAndServfailRemainDistinct()=runBlocking {
        for((mode,kind) in listOf(Mode.NXDOMAIN to SSHResolveFailure.NAME_NOT_FOUND,Mode.EMPTY to SSHResolveFailure.NO_ADDRESS,Mode.SERVFAIL to SSHResolveFailure.DNS_FAILURE)) {
            Dns(mode).use { dns ->
                val failure=runCatching { withTimeout(20000) { AndroidSSHResolver.resolve(dns.host) } }.exceptionOrNull()
                assertTrue("$mode returned $failure",failure is SSHResolveException)
                assertEquals(mode.toString(),kind,(failure as SSHResolveException).failure);assertTrue(dns.packets.get()>0)
                assertEquals(0,owners());assertTrue(root.list()!!.isEmpty())
            }
        }
    }
    private fun model(dns:Dns,budget:Long=8000,test:(SessionsViewModel)->Unit) {
        val application=object:Application() { override fun getFilesDir()=root }
        val store=ViewModelStore();lateinit var model:SessionsViewModel
        instrumentation.runOnMainSync { model=SessionsViewModel(application,connectionBudgetMillis=budget,monitorSessions=false);store.put("dns",model) }
        try { test(model) } finally { instrumentation.runOnMainSync { store.clear() } }
    }
    @Test fun actualDnsCancellationAndLateAnswerCannotCreateAnOwner() {
        Dns(Mode.HOLD).use { dns -> model(dns) { model ->
            instrumentation.runOnMainSync { model.connect(PreparedSSH("dns",dns.host,"user",22,emptyList())) }
            await { dns.packets.get()>0 };assertEquals(0,owners())
            instrumentation.runOnMainSync { model.cancelConnection(model.connecting!!.id) }
            dns.release();assertTrue("late DNS answer must actually be sent",dns.lateAnswers.get()>0);SystemClock.sleep(200)
            instrumentation.runOnMainSync { assertNull(model.connecting);assertTrue(model.sessions.isEmpty()) }
            assertEquals(0,owners());assertFalse(File(root,"home/.ssh/known_hosts").exists())
        } }
    }
    @Test fun productDeadlineIncludesActualDnsAndLateAnswerDoesNotChangeFailure() {
        Dns(Mode.HOLD).use { dns -> model(dns,1500) { model ->
            instrumentation.runOnMainSync { model.connect(PreparedSSH("dns",dns.host,"user",22,emptyList())) }
            await { dns.packets.get()>0 };assertEquals(0,owners())
            await { var failed=false;instrumentation.runOnMainSync { failed=model.connecting?.failed==true };failed }
            dns.release();assertTrue("late DNS answer must actually be sent",dns.lateAnswers.get()>0);SystemClock.sleep(200)
            instrumentation.runOnMainSync { assertEquals("SSH 连接超时",model.connecting!!.message);assertTrue(model.sessions.isEmpty()) }
            assertEquals(0,owners());assertFalse(File(root,"home/.ssh/known_hosts").exists())
        } }
    }
}
