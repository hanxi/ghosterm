package com.hanxi.ghosterm

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

class PluginGitHubClient(private val maxBytes: Int = 4 * 1024 * 1024) {
    sealed class Failure(message: String): Exception(message) { class Status(code: Int): Failure("GitHub 返回 HTTP $code"); object TooLarge: Failure("GitHub 响应超过允许大小"); object InvalidIndex: Failure("GitHub 插件索引无效") }
    suspend fun fetch(url: String): ByteArray = withContext(Dispatchers.IO) {
        val connection = URL(url).openConnection() as HttpURLConnection
        try {
            connection.requestMethod = "GET"; connection.setRequestProperty("Accept", "application/json")
            if (connection.responseCode !in 200..299) throw Failure.Status(connection.responseCode)
            val output = ByteArray(maxBytes); var total = 0
            connection.inputStream.use { input ->
                while (true) { val count = input.read(output, total, output.size - total); if (count < 0) break; total += count; if (total == output.size) { if (input.read() >= 0) throw Failure.TooLarge; break } }
            }
            output.copyOf(total)
        } finally { connection.disconnect() }
    }
    suspend fun fetchIndex(source: PluginGitHubSource): PluginIndex {
        val index = parseIndex(fetch(source.indexUrl()))
        if (!index.validate(source)) throw Failure.InvalidIndex
        return index
    }
    private fun parseIndex(bytes: ByteArray): PluginIndex {
        val root = JSONObject(String(bytes, Charsets.UTF_8))
        val plugins = root.getJSONArray("plugins").let { array -> (0 until array.length()).map { item ->
            val value = array.getJSONObject(item)
            PluginIndexEntry(value.getString("id"), value.getString("name"), value.getString("version"), value.getString("role"), value.getString("archive"), value.getString("releaseJSON"), value.getString("releaseSignature"), value.getLong("archiveBytes"), value.getString("archiveSHA256"), strings(value.optJSONArray("permissions")), strings(value.optJSONArray("capabilities")))
        } }
        return PluginIndex(root.getInt("schemaVersion"), root.getString("repository"), root.getString("releaseTag"), root.getString("publisher"), root.getString("publisherFingerprint"), root.getString("publisherPublicKey"), plugins, root.getString("signatureDomain"), root.getString("signature"))
    }
    private fun strings(array: JSONArray?): List<String> = array?.let { (0 until it.length()).map(it::getString) } ?: emptyList()
}
