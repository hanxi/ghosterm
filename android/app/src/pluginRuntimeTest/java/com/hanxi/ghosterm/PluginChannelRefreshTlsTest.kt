package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeNotNull
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import java.net.URI
import java.security.KeyStore
import java.security.cert.CertificateFactory
import java.util.Base64
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManagerFactory

@RunWith(AndroidJUnit4::class)
class PluginChannelRefreshTlsTest {
    @Test fun loopbackTlsRefreshEnforcesBoundsAndPreservesCommittedSnapshot() = runBlocking {
        val args = InstrumentationRegistry.getArguments()
        val origin = args.getString("channelTlsOrigin")
        assumeNotNull(origin)
        val decoder = Base64.getDecoder()
        fun argument(name: String) = decoder.decode(checkNotNull(args.getString(name)))
        val root = argument("channelTlsRoot")
        val certificate = CertificateFactory.getInstance("X.509")
            .generateCertificate(argument("channelTlsCertificate").inputStream())
        val trust = KeyStore.getInstance(KeyStore.getDefaultType()).apply {
            load(null)
            setCertificateEntry("fixture", certificate)
        }
        val trustManagers = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).apply {
            init(trust)
        }
        val tls = SSLContext.getInstance("TLS").apply { init(null, trustManagers.trustManagers, null) }
        val directory = PluginInstallTestFiles.root()
        try {
            val store = PluginChannelPolicyStore(directory, "fixture", root)
            fun refresh(path: String) = PluginChannelRefresh(store, directory, URI("$origin/$path/")) { uri ->
                (uri.toURL().openConnection() as HttpsURLConnection).apply { sslSocketFactory = tls.socketFactory }
            }
            val vector = JSONObject(InstrumentationRegistry.getInstrumentation().context.assets
                .open("plugin-release/vector.json").bufferedReader().use { it.readText() })
            val descriptor = vector.getString("descriptor").toByteArray()
            val signature = decoder.decode(vector.getString("signature"))
            fun now() = (System.currentTimeMillis() / 1000).toULong()
            val valid = refresh("valid").refresh()
            assertEquals(2uL, valid.generation)
            assertEquals("org.ghosterm.signature", valid.authenticate(descriptor, signature, now()).proof.pluginID)
            val redirect = runCatching { refresh("redirect").refresh() }.exceptionOrNull()
            assertTrue(redirect.toString(), redirect is IllegalStateException && redirect.message!!.contains("签名材料"))
            val oversized = runCatching { refresh("oversized").refresh() }.exceptionOrNull()
            assertTrue(oversized.toString(), oversized is IllegalStateException && oversized.message!!.contains("超过上限"))
            val mixed = runCatching { refresh("mixed").refresh() }.exceptionOrNull()
            assertEquals(PluginReleaseSignature.Failure.CHANNEL_STATEMENT, (mixed as? PluginReleaseSignature.Rejected)?.reason)
            assertEquals(2uL, store.useCurrent(now()) { it.generation })
            assertEquals(3uL, refresh("revoked").refresh().generation)
            val rollback = runCatching { refresh("valid").refresh() }.exceptionOrNull()
            assertEquals(PluginReleaseSignature.Failure.CHANNEL_HISTORY, (rollback as? PluginReleaseSignature.Rejected)?.reason)
            store.useCurrent(now()) { revoked ->
                assertEquals(3uL, revoked.generation)
                val denied = runCatching { revoked.authenticate(descriptor, signature, now()) }.exceptionOrNull()
                assertEquals(PluginReleaseSignature.Failure.REVOKED_KEY, (denied as? PluginReleaseSignature.Rejected)?.reason)
            }
        } finally { PluginInstallTestFiles.clean(directory) }
    }
}
