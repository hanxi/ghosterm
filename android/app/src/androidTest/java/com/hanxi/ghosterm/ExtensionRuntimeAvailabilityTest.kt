package com.hanxi.ghosterm

import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.hanxi.ghosterm.bridge.ExtensionChannel
import com.hanxi.ghosterm.bridge.ExtensionChannelException
import com.hanxi.ghosterm.bridge.ExtensionSession
import com.hanxi.ghosterm.bridge.ExtensionRuntime
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import java.security.MessageDigest

@RunWith(AndroidJUnit4::class)
class ExtensionRuntimeAvailabilityTest {
    @Test fun buildCapabilityAndDisabledOwnership() {
        assertEquals(if (BuildConfig.PLUGIN_JS_ENABLED) 0 else 7, ExtensionRuntime.supportStatus())
        if (BuildConfig.PLUGIN_JS_ENABLED) return
        val f = ParcelFileDescriptor.createSocketPair(); val a = f[0].detachFd(); val b = f[1].detachFd()
        val config = ExtensionSession.Config("org.ghosterm.preview", "0.1.0", "d".repeat(64), "e".repeat(64), "0.1.0",
            required = listOf("ui.form"), available = listOf("ui.form"), peerKind = ExtensionSession.PeerKind.LOCAL, peerIdentity = "disabled-runtime", methods = emptyList())
        val h = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(a, a), ExtensionSession.Role.HOST, config)
        val e = ExtensionSession.adopt(ExtensionChannel.adoptDescriptors(b, b), ExtensionSession.Role.EXECUTOR, config)
        try {
            val source = "export const value=1".toByteArray(); val digest = MessageDigest.getInstance("SHA-256").digest(source).joinToString("") { "%02x".format(it.toInt() and 255) }
            try { ExtensionRuntime.adopt(e, ExtensionRuntime.Config(listOf(ExtensionRuntime.Module("plugin:/entry.js", source, digest)), "plugin:/entry.js", ExtensionSession.Role.EXECUTOR)); fail("unsupported runtime admitted") }
            catch (error: ExtensionChannelException) { assertEquals(7, error.status) }
            assertEquals(ExtensionSession.Phase.WAIT_HELLO, e.info().phase)
        } finally { e.close(); h.close() }
    }
}
