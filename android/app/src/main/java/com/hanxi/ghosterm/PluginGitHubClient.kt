package com.hanxi.ghosterm

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

class PluginGitHubClient(private val maxBytes: Int = 4 * 1024 * 1024) {
    sealed class Failure(message: String): Exception(message) { class Status(code: Int): Failure("GitHub 返回 HTTP $code") ; object TooLarge: Failure("GitHub 响应超过允许大小") }
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
    suspend fun fetchIndex(source: PluginGitHubSource): JSONObject = JSONObject(String(fetch(source.indexUrl()), Charsets.UTF_8))
}
