package com.hanxi.ghosterm

import android.net.DnsResolver
import android.os.CancellationSignal
import android.system.ErrnoException
import android.system.OsConstants
import kotlinx.coroutines.*
import org.junit.Test
import org.junit.Assert.*
import java.io.IOException
import java.net.InetAddress
import java.net.Inet6Address

class SSHDnsCompatibilityTest {
    private class Primary(val failure:Exception?=null):SSHDnsQuery {
        var calls=0
        lateinit var signal:CancellationSignal
        lateinit var callback:SSHDnsCallback
        override fun query(host:String,cancellation:CancellationSignal,callback:SSHDnsCallback) {
            calls++;signal=cancellation;this.callback=callback;failure?.let { throw it }
        }
    }
    private class Records:SSHDnsRecordQuery {
        data class Request(val host:String,val type:SSHDnsRecordType,val signal:CancellationSignal,val callback:SSHDnsCallback)
        val requests=mutableListOf<Request>()
        var failure:Exception?=null
        override fun query(host:String,type:SSHDnsRecordType,cancellation:CancellationSignal,callback:SSHDnsCallback) {
            requests+=Request(host,type,cancellation,callback);if(type==SSHDnsRecordType.A)failure?.let { throw it }
        }
    }
    private data class Operation(val primary:Primary,val records:Records,val result:Deferred<Result<List<InetAddress>>>)
    private fun CoroutineScope.begin(primary:Primary=Primary(),records:Records=Records()):Operation {
        val worker=async(start=CoroutineStart.UNDISPATCHED) { runCatching { SystemSSHResolver(CompatibleSSHDnsQuery(primary,records)).resolve("fixture.test") } }
        return Operation(primary,records,worker)
    }
    private fun error(errno:Int=64,code:Int=DnsResolver.ERROR_SYSTEM):SSHDnsError {
        val cause=ErrnoException("controlled DNS",errno)
        return SSHDnsError(code,cause,IOException("controlled DNS failure",cause))
    }
    private fun Operation.fallback(errno:Int=64) {
        primary.callback.onError(error(errno));assertEquals("Both record families must start",2,records.requests.size)
    }
    private fun address(n:Int)=InetAddress.getByAddress(byteArrayOf(127,0,0,n.toByte()))
    private fun ipv6(n:Int=1)=Inet6Address.getByAddress(null,ByteArray(16).also { it[0]=0xfe.toByte();it[1]=0x80.toByte();it[15]=n.toByte() },7)
    @Test fun ordinaryAnswersRetainPlatformOrderAndDoNotStartOtherQueries()=runBlocking {
        val op=begin();val expected=listOf(ipv6(),address(1));op.primary.callback.onAnswer(expected,0)
        assertEquals(expected,op.result.await().getOrThrow());assertTrue(op.records.requests.isEmpty())
    }
    @Test fun preflightFailuresQueryBothFamiliesAndPreserveIpv6Scope()=runBlocking {
        for(errno in listOf(64,OsConstants.ENETDOWN,OsConstants.ENETUNREACH)) {
            val op=begin();op.fallback(errno)
            assertEquals(listOf(SSHDnsRecordType.A,SSHDnsRecordType.AAAA),op.records.requests.map { it.type })
            assertTrue(op.records.requests.all { it.host=="fixture.test" })
            val v6=ipv6();op.records.requests[1].callback.onAnswer(listOf(v6),0)
            assertFalse("One answer must not discard the other family",op.result.isCompleted)
            op.records.requests[0].callback.onAnswer(listOf(address(1)),0)
            assertEquals(listOf(address(1),v6),op.result.await().getOrThrow());assertEquals(7,v6.scopeId)
        }
    }
    @Test fun ordinaryFailuresDoNotStartCompatibilityQueries()=runBlocking {
        for(failure in listOf(error(64,DnsResolver.ERROR_PARSE),error(OsConstants.ETIMEDOUT),error(OsConstants.EACCES))) {
            val op=begin();op.primary.callback.onError(failure)
            assertTrue("Ordinary failure must not start other queries",op.records.requests.isEmpty())
            val result=op.result.await().exceptionOrNull();assertTrue(result is SSHResolveException)
            assertSame(failure.original,result!!.cause);assertTrue(op.records.requests.isEmpty())
        }
        val op=begin();op.primary.callback.onAnswer(emptyList(),3)
        assertEquals(SSHResolveFailure.NAME_NOT_FOUND,(op.result.await().exceptionOrNull() as SSHResolveException).failure)
        assertTrue(op.records.requests.isEmpty())
    }
    @Test fun ipv6OnlyAndOneFamilyFailureStillReturnAvailableAddresses()=runBlocking {
        val op=begin();op.fallback();val v6=ipv6()
        op.records.requests[0].callback.onError(error(OsConstants.ETIMEDOUT));op.records.requests[1].callback.onAnswer(listOf(v6),0)
        assertEquals(listOf(v6),op.result.await().getOrThrow())
    }
    @Test fun serverErrorsNeverContributeAddressesOrInventNxdomain()=runBlocking {
        val op=begin();op.fallback();op.records.requests[0].callback.onAnswer(listOf(address(1)),2);op.records.requests[1].callback.onAnswer(emptyList(),0)
        val failed=op.result.await().exceptionOrNull();assertTrue("Failed response cannot supply addresses",failed is SSHResolveException)
        assertEquals(SSHResolveFailure.DNS_FAILURE,(failed as SSHResolveException).failure)
        for((rcode,expected) in listOf(3 to SSHResolveFailure.NAME_NOT_FOUND,0 to SSHResolveFailure.NO_ADDRESS)) {
            val next=begin();next.fallback();next.records.requests.forEach { it.callback.onAnswer(emptyList(),rcode) }
            assertEquals(expected,(next.result.await().exceptionOrNull() as SSHResolveException).failure)
        }
        val unknown=begin();unknown.fallback();val failure=error(OsConstants.EACCES)
        unknown.records.requests[0].callback.onError(failure);unknown.records.requests[1].callback.onAnswer(emptyList(),0)
        val observed=unknown.result.await().exceptionOrNull() as SSHResolveException
        assertEquals(SSHResolveFailure.DNS_FAILURE,observed.failure);assertSame(failure.original,observed.cause)
    }
    @Test fun systemDnsFailureDoesNotClaimAllConnectivityIsUnavailable()=runBlocking {
        val op=begin();op.fallback();val failure=error()
        op.records.requests.forEach { it.callback.onError(failure) }
        val result=op.result.await().exceptionOrNull();assertTrue(result is SSHResolveException)
        result as SSHResolveException
        assertEquals(SSHResolveFailure.NETWORK_UNAVAILABLE,result.failure)
        assertTrue(result.message.orEmpty().contains("系统 DNS"))
        assertFalse(result.message.orEmpty().contains("当前网络不可用"));assertSame(failure.original,result.cause)
    }
    @Test fun combinedLimitRejectsRatherThanTruncatingAnAddressFamily()=runBlocking {
        val op=begin();op.fallback()
        op.records.requests[0].callback.onAnswer((1..17).map(::address),0);op.records.requests[1].callback.onAnswer((1..16).map(::ipv6),0)
        val failure=op.result.await().exceptionOrNull();assertTrue(failure is SSHResolveException)
        assertEquals(SSHResolveFailure.TOO_MANY_ADDRESSES,(failure as SSHResolveException).failure)
    }
    @Test fun cancellationReachesThreeIndependentSignalsAndRejectsLateCallbacks()=runBlocking {
        val op=begin();op.fallback();val a=op.records.requests[0];val aaaa=op.records.requests[1]
        assertNotSame(op.primary.signal,a.signal);assertNotSame(a.signal,aaaa.signal)
        op.result.cancelAndJoin()
        assertTrue(op.primary.signal.isCanceled);assertTrue(a.signal.isCanceled);assertTrue(aaaa.signal.isCanceled)
        a.callback.onAnswer(listOf(address(1)),0);aaaa.callback.onAnswer(listOf(ipv6()),0)
        op.primary.callback.onAnswer(listOf(address(9)),0);assertTrue(op.result.isCancelled)
        val signal=CancellationSignal();signal.cancel();val primary=Primary();val records=Records()
        CompatibleSSHDnsQuery(primary,records).query("fixture.test",signal,object:SSHDnsCallback {
            override fun onAnswer(answer:List<InetAddress>,rcode:Int)=fail("Cancelled query answered")
            override fun onError(error:SSHDnsError)=fail("Cancelled query failed")
        })
        assertEquals(0,primary.calls);assertTrue(records.requests.isEmpty())
    }
    @Test fun stalePrimaryAndDuplicateFamilyCallbacksCannotReplaceTheSnapshot()=runBlocking {
        val op=begin();op.fallback();op.primary.callback.onAnswer(listOf(address(9)),0)
        assertFalse("Late primary cannot bypass family completion",op.result.isCompleted)
        val answer=mutableListOf(address(1));op.records.requests[0].callback.onAnswer(answer,0);answer[0]=address(8)
        op.records.requests[0].callback.onAnswer(listOf(address(9)),0);op.records.requests[1].callback.onAnswer(emptyList(),0)
        assertEquals(listOf(address(1)),op.result.await().getOrThrow())
    }
    @Test fun synchronousFailuresReleaseSignalsAndDoNotChangeCancellationSemantics()=runBlocking {
        val failure=IOException("startup failure");val primary=Primary(failure);val op=begin(primary)
        assertSame(failure,(op.result.await().exceptionOrNull() as SSHResolveException).cause);assertTrue(primary.signal.isCanceled)
        val canceled=begin(Primary(CancellationException("cancel startup")))
        assertTrue(canceled.result.await().exceptionOrNull() is CancellationException)
        val records=Records().apply { this.failure=failure };val partial=begin(records=records);partial.fallback()
        assertTrue(records.requests[0].signal.isCanceled)
        records.requests[1].callback.onAnswer(emptyList(),0)
        assertSame(failure,(partial.result.await().exceptionOrNull() as SSHResolveException).cause)
    }
}
