package com.hanxi.ghosterm

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeNotNull
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
            assertEquals(2uL, refresh("valid").refresh().generation)
            val redirect = runCatching { refresh("redirect").refresh() }.exceptionOrNull()
            assertTrue(redirect.toString(), redirect is IllegalStateException && redirect.message!!.contains("签名材料"))
            val oversized = runCatching { refresh("oversized").refresh() }.exceptionOrNull()
            assertTrue(oversized.toString(), oversized is IllegalStateException && oversized.message!!.contains("超过上限"))
            val mixed = runCatching { refresh("mixed").refresh() }.exceptionOrNull()
            assertEquals(PluginReleaseSignature.Failure.CHANNEL_STATEMENT, (mixed as? PluginReleaseSignature.Rejected)?.reason)
            val now = (System.currentTimeMillis() / 1000).toULong()
            assertEquals(2uL, store.useCurrent(now) { it.generation })
        } finally { PluginInstallTestFiles.clean(directory) }
    }
}
