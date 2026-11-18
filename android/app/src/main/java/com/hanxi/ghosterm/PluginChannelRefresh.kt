package com.hanxi.ghosterm

import android.content.Context
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.URI
import java.util.Base64
import javax.net.ssl.HttpsURLConnection

internal class PluginChannelRefresh private constructor(
    val store: PluginChannelPolicyStore,
    private val cache: File,
    private val directory: URI,
) {
    suspend fun refresh(): PluginReleaseSignature.VerifiedChannelPolicy = withContext(Dispatchers.IO) {
        withTimeout(45_000) { download() }
    }

    private suspend fun download(): PluginReleaseSignature.VerifiedChannelPolicy {
        val context = kotlin.coroutines.coroutineContext
        fun fetch(name: String, limit: Int): ByteArray {
            val connection = directory.resolve(name).toURL().openConnection() as HttpsURLConnection
            connection.instanceFollowRedirects = false
            connection.useCaches = false
            connection.connectTimeout = 15_000
            connection.readTimeout = 15_000
            try {
                check(connection.responseCode == 200) { "插件渠道未返回所需的签名材料。" }
                check(connection.contentLengthLong <= limit) { "插件渠道返回的签名材料超过上限。" }
                return connection.inputStream.use { input ->
                    val result = ByteArrayOutputStream()
                    val buffer = ByteArray(8192)
                    while (true) {
                        context.ensureActive()
                        val count = input.read(buffer)
                        if (count < 0) break
                        check(result.size() <= limit - count) { "插件渠道返回的签名材料超过上限。" }
                        result.write(buffer, 0, count)
                    }
                    result.toByteArray()
                }
            } finally { connection.disconnect() }
        }

        val statement = fetch("channel-policy.json", 65536)
        val signature = fetch("channel-policy.sig", 64)
        val policy = fetch("policy.json", 131072)
        context.ensureActive()
        try { Os.mkdir(cache.path, 448) }
        catch (error: ErrnoException) { if (error.errno != OsConstants.EEXIST) throw error }
        val seconds = System.currentTimeMillis() / 1000
        check(seconds >= 0) { "系统时间无效，无法刷新插件渠道。" }
        return store.refresh(statement, signature, policy, seconds.toULong())
    }

    companion object {
        fun configured(context: Context): PluginChannelRefresh? {
            if (BuildConfig.PLUGIN_CHANNEL_NAME.isEmpty()) return null
            val endpoint = URI(BuildConfig.PLUGIN_CHANNEL_URL.trimEnd('/') + "/")
            val cache = File(context.filesDir, "plugin-channel-policy")
            val root = Base64.getDecoder().decode(BuildConfig.PLUGIN_CHANNEL_ROOT)
            return PluginChannelRefresh(PluginChannelPolicyStore(cache, BuildConfig.PLUGIN_CHANNEL_NAME, root), cache, endpoint)
        }
    }
}
