package com.hanxi.ghosterm

import android.app.Application
import android.os.SystemClock
import androidx.lifecycle.ViewModelStore
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.hanxi.ghosterm.bridge.ConfigBridge
import com.hanxi.ghosterm.bridge.PreparedSSH
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.suspendCancellableCoroutine
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.InetAddress
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.Continuation
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine

@RunWith(AndroidJUnit4::class)
class SSHConnectionModelTest {
    private val instrumentation = InstrumentationRegistry.getInstrumentation()
    private fun main(action: () -> Unit) = instrumentation.runOnMainSync(action)
    private fun await(condition: () -> Boolean) {
        val end = SystemClock.uptimeMillis()+5000
        while (SystemClock.uptimeMillis()<end) { if(condition()) return; SystemClock.sleep(10) }
        fail("SSH controller condition timed out")
    }
    private fun withModel(resolver: SSHResolver,budget: Long = 60000,test: (SessionsViewModel) -> Unit) {
        val directory = File(instrumentation.targetContext.cacheDir,"ssh-model-${UUID.randomUUID()}").apply { mkdirs() }
        val application = object: Application() { override fun getFilesDir() = directory }
        val store = ViewModelStore()
        lateinit var model: SessionsViewModel
        main { model = SessionsViewModel(application,resolver,budget); store.put("test",model) }
        try { test(model) } finally { main { store.clear() }; directory.deleteRecursively() }
    }
    private fun target(user: String = "owner") = PreparedSSH("fixture","fixture.test",user,22,emptyList())
    @Test fun cancelledDnsCannotCreateAnOwnerFromALateResult() {
        var reply: Continuation<List<InetAddress>>? = null
        val entered = AtomicBoolean(false)
        withModel(SSHResolver { suspendCoroutine { reply = it; entered.set(true) } }) { model ->
            main { model.connect(target()) }
            await { entered.get() }
            main { model.cancelConnection(model.connecting!!.id) }
            reply!!.resume(listOf(InetAddress.getByAddress(byteArrayOf(127,0,0,1))))
            main { assertNull(model.connecting); assertTrue(model.sessions.isEmpty()) }
            // Give the resumed dispatcher an opportunity; cancellation must win before native creation.
            SystemClock.sleep(200)
            main { assertNull(model.connecting); assertTrue(model.sessions.isEmpty()) }
        }
    }
    @Test fun timeoutCancelsDnsAndShowsFailure() {
        val cancelled = AtomicBoolean(false)
        withModel(SSHResolver { suspendCancellableCoroutine { it.invokeOnCancellation { cancelled.set(true) } } },250) { model ->
            main { model.connect(target()) }
            await { cancelled.get() }
            await { var failed = false; main { failed = model.connecting?.failed == true }; failed }
            main { assertEquals("SSH 连接超时",model.connecting!!.message); assertTrue(model.sessions.isEmpty()) }
        }
    }
    @Test fun missingUsernameDoesNotResolveUntilValidInputAndDoesNotMutateTarget() {
        val called = AtomicBoolean(false)
        val original = target(user = "")
        withModel(SSHResolver { called.set(true); error("test resolver reached") }) { model ->
            main { model.connect(original); assertEquals("",model.connecting!!.username) }
            assertFalse(called.get())
            main { model.setConnectionUsername(model.connecting!!.id,"bad user"); assertNotNull(model.connecting!!.credentialError) }
            assertFalse(called.get())
            main { model.setConnectionUsername(model.connecting!!.id,"owner") }
            await { called.get() }
            assertEquals("",original.user)
        }
    }
    @Test fun numericResolverPreservesIpv4AndIpv6Bytes() = runBlocking {
        assertArrayEquals(byteArrayOf(127,0,0,1),AndroidSSHResolver.resolve("127.0.0.1").single().address)
        assertArrayEquals(ByteArray(16).also { it[15] = 1 },AndroidSSHResolver.resolve("::1").single().address)
    }
    @Test fun preparationRechecksIncludedConfigurationAndPreservesRawBytes() {
        val directory = File(instrumentation.targetContext.cacheDir,"ssh-target-${UUID.randomUUID()}").apply { mkdirs() }
        try {
            val include = File(directory,"included.conf").apply { writeText("Host target\n HostName first.test\n Port 2202\n IdentityFile /identity/reference\n") }
            val bytes = "Include ${include.path}\n".toByteArray(); val before = bytes.copyOf()
            val first = ConfigBridge.prepareConnection(bytes,"target")
            assertEquals("first.test",first.host); assertEquals(2202,first.port); assertEquals("",first.user)
            assertEquals(listOf("/identity/reference"),first.identityFiles)
            include.writeText("Host target\n HostName second.test\n User other\n Port 2203\n")
            val second = ConfigBridge.prepareConnection(bytes,"target")
            assertEquals("second.test",second.host); assertEquals("other",second.user); assertEquals(2203,second.port)
            include.appendText(" ProxyJump blocked\n")
            assertThrows(IllegalStateException::class.java) { ConfigBridge.prepareConnection(bytes,"target") }
            assertArrayEquals(before,bytes)
        } finally { directory.deleteRecursively() }
    }
}
