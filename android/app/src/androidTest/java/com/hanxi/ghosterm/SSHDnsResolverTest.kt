package com.hanxi.ghosterm

import android.app.Application
import android.net.DnsResolver
import android.os.CancellationSignal
import android.os.SystemClock
import android.system.ErrnoException
import android.system.OsConstants
import androidx.lifecycle.ViewModelStore
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.*
import org.junit.*
import org.junit.Assert.*
import java.io.File
import java.net.InetAddress
import java.net.Inet6Address
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean

class SSHDnsResolverTest {
    private class Query(private val immediate:Boolean=false) : SSHDnsQuery {
        @Volatile var name:String?=null
        lateinit var signal:CancellationSignal
        lateinit var callback:SSHDnsCallback
        override fun query(host:String,cancellation:CancellationSignal,callback:SSHDnsCallback) {
            signal=cancellation;this.callback=callback;name=host
            if(immediate)callback.onAnswer(listOf(InetAddress.getByAddress(byteArrayOf(127,0,0,1))),0)
        }
    }
    private fun dnsError(code:Int,cause:Exception)=SSHDnsError(code,cause,IllegalStateException("controlled platform error",cause))
    private fun address(value:Int)=InetAddress.getByAddress(byteArrayOf(127,0,0,value.toByte()))
    private suspend fun answer(rcode:Int,items:List<InetAddress> = emptyList()):SSHResolveException = coroutineScope {
        val query=Query();val job=async(start=CoroutineStart.UNDISPATCHED) { runCatching { SystemSSHResolver(query).resolve("fixture.test") } }
        query.callback.onAnswer(items,rcode)
        val failure=job.await().exceptionOrNull();assertTrue(failure.toString(),failure is SSHResolveException)
        failure as SSHResolveException
    }
    @Test fun numericIpv4AndScopedIpv6NeverQueryDns()=runBlocking {
        val query=Query(true);val resolver=SystemSSHResolver(query)
        assertArrayEquals(address(1).address,resolver.resolve("127.0.0.1").single().address)
        val ipv6=resolver.resolve("::1%1").single() as Inet6Address
        assertEquals(1,ipv6.scopeId);assertTrue(ipv6.isLoopbackAddress);assertNull(query.name)
    }
    @Test fun internationalDomainIsAsciiForDnsWithoutChangingOriginalInput()=runBlocking {
        val query=Query();val host="例子.测试";val worker=async(start=CoroutineStart.UNDISPATCHED) { SystemSSHResolver(query).resolve(host) }
        assertEquals("xn--fsqu00a.xn--0zwm56d",query.name);query.callback.onAnswer(listOf(address(1)),0)
        assertEquals(listOf(address(1)),worker.await());assertEquals("例子.测试",host)
    }
    @Test fun unicodeDeviationCharactersDoNotResolveToADifferentAsciiDomain()=runBlocking {
        val query=Query();val worker=async(start=CoroutineStart.UNDISPATCHED) { SystemSSHResolver(query).resolve("faß.de") }
        assertEquals("xn--fa-hia.de",query.name);assertNotEquals("fass.de",query.name)
        query.callback.onAnswer(listOf(address(1)),0);assertEquals(listOf(address(1)),worker.await())
    }
    @Test fun asciiHostnameFormsRemainUnchanged()=runBlocking {
        val query=Query();val name="_srv.ab--edge.Example."
        val worker=async(start=CoroutineStart.UNDISPATCHED) { SystemSSHResolver(query).resolve(name) }
        assertEquals(name,query.name);query.callback.onAnswer(listOf(address(1)),0);assertEquals(listOf(address(1)),worker.await())
    }
    @Test fun invalidNamesDoNotStartPlatformQuery()=runBlocking {
        for(host in listOf(""," ","bad host","bad\u0000host","a..b","a".repeat(64)+".test","bad\uFFFD.test","a\u200Db.test")) {
            val query=Query(true);val failure=runCatching { SystemSSHResolver(query).resolve(host) }.exceptionOrNull()
            assertTrue("host=$host failure=$failure",failure is SSHResolveException)
            assertEquals(SSHResolveFailure.INVALID_HOST,(failure as SSHResolveException).failure);assertNull(query.name)
        }
    }
    @Test fun nxdomainEmptyAndServerFailureAreDistinctAndNeverReturnAnswers()=runBlocking {
        val missing=answer(3,listOf(address(1)));assertEquals(SSHResolveFailure.NAME_NOT_FOUND,missing.failure);assertEquals(3,missing.responseCode)
        val empty=answer(0);assertEquals(SSHResolveFailure.NO_ADDRESS,empty.failure);assertEquals(0,empty.responseCode)
        for(code in listOf(2,5,15)) {
            val error=answer(code,listOf(address(1)));assertEquals(SSHResolveFailure.DNS_FAILURE,error.failure);assertEquals(code,error.responseCode)
            assertNotEquals(missing.message,error.message);assertNotEquals(empty.message,error.message)
        }
    }
    @Test fun addressLimitRejectsInsteadOfSilentlyTruncating()=runBlocking {
        val error=answer(0,List(33) { address(1) });assertEquals(SSHResolveFailure.TOO_MANY_ADDRESSES,error.failure)
    }
    @Test fun returnedAddressesAreAnOrderedSnapshotAndKeepIpv6Scope()=runBlocking {
        val query=Query();val worker=async(start=CoroutineStart.UNDISPATCHED) { SystemSSHResolver(query).resolve("fixture.test") }
        val ipv6=Inet6Address.getByAddress(null,ByteArray(16).also { it[15]=1 },1)
        val original=mutableListOf(address(2),ipv6,address(1));query.callback.onAnswer(original,0);original.clear()
        val result=worker.await();assertEquals(listOf(address(2),ipv6,address(1)),result);assertEquals(1,(result[1] as Inet6Address).scopeId)
    }
    @Test fun cancellationReachesAndroidAndLateCallbacksCannotCompleteTheRequest()=runBlocking {
        val query=Query();val worker=async(start=CoroutineStart.UNDISPATCHED) { SystemSSHResolver(query).resolve("fixture.test") }
        worker.cancelAndJoin();assertTrue(query.signal.isCanceled)
        val late=runCatching {
            query.callback.onAnswer(listOf(address(1)),0)
            query.callback.onError(dnsError(DnsResolver.ERROR_SYSTEM,ErrnoException("dns",64)))
        }
        assertTrue("late callbacks must be ignored",late.isSuccess);assertTrue(worker.isCancelled)
    }
    @Test fun parseNetworkAndTimeoutErrorsRetainTheirCausesAndCategories()=runBlocking {
        val cases=listOf(
            DnsResolver.ERROR_PARSE to IllegalArgumentException("malformed response"),
            DnsResolver.ERROR_SYSTEM to ErrnoException("dns",64),
            DnsResolver.ERROR_SYSTEM to ErrnoException("dns",OsConstants.ETIMEDOUT),
            99 to IllegalStateException("unknown resolver error"))
        for((code,cause) in cases) {
            val query=Query();val worker=async(start=CoroutineStart.UNDISPATCHED) { runCatching { SystemSSHResolver(query).resolve("fixture.test") } }
            val original=dnsError(code,cause);query.callback.onError(original)
            val failure=worker.await().exceptionOrNull() as SSHResolveException
            assertSame(original.original,failure.cause);assertEquals(code,failure.resolverCode)
            assertEquals((cause as? ErrnoException)?.errno,failure.systemError)
            assertEquals(when { code==DnsResolver.ERROR_PARSE -> SSHResolveFailure.RESPONSE_INVALID
                cause is ErrnoException && cause.errno==64 -> SSHResolveFailure.NETWORK_UNAVAILABLE
                else -> SSHResolveFailure.DNS_FAILURE },failure.failure)
            if(cause is ErrnoException && cause.errno==OsConstants.ETIMEDOUT)assertTrue(failure.message!!.contains("超时"))
        }
    }
    @Test fun synchronousQueryFailureIsExplicitAndCancellationIsNotNetworkFailure()=runBlocking {
        val original=SecurityException("controlled query start failure")
        val failure=runCatching { SystemSSHResolver(SSHDnsQuery { _,_,_->throw original }).resolve("fixture.test") }.exceptionOrNull() as SSHResolveException
        assertSame(original,failure.cause);assertEquals(SSHResolveFailure.DNS_FAILURE,failure.failure)
        val cancelled=runCatching { SystemSSHResolver(SSHDnsQuery { _,_,_->throw CancellationException("controlled cancel") }).resolve("fixture.test") }.exceptionOrNull()
        assertTrue(cancelled.toString(),cancelled is CancellationException)
    }
    @Test fun dnsFailureIsVisibleInProductAndCreatesNoTerminal() {
        val instrumentation=InstrumentationRegistry.getInstrumentation();val dir=File(instrumentation.targetContext.cacheDir,"dns-model-${UUID.randomUUID()}").apply { mkdirs() }
        val application=object:Application() { override fun getFilesDir()=dir }
        val query=Query();val store=ViewModelStore();lateinit var model:SessionsViewModel
        try {
            instrumentation.runOnMainSync {
                model=SessionsViewModel(application,SystemSSHResolver(query),monitorSessions=false);store.put("dns",model)
                model.connect(PreparedSSH("dns","fixture.test","user",22,emptyList()))
            }
            val entered=AtomicBoolean(false)
            val deadline=SystemClock.elapsedRealtime()+5000
            while(query.name==null && SystemClock.elapsedRealtime()<deadline)SystemClock.sleep(10)
            assertEquals("fixture.test",query.name);query.callback.onAnswer(emptyList(),3)
            while(!entered.get() && SystemClock.elapsedRealtime()<deadline) {
                instrumentation.runOnMainSync { entered.set(model.connecting?.failed==true) };SystemClock.sleep(10)
            }
            assertTrue(entered.get())
            instrumentation.runOnMainSync {
                assertEquals("DNS 未找到此域名，请检查主机地址",model.connecting!!.message);assertTrue(model.sessions.isEmpty())
            }
            assertFalse(File(dir,"home/.ssh/known_hosts").exists())
        } finally { instrumentation.runOnMainSync { store.clear() };dir.deleteRecursively() }
    }
}
