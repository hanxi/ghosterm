package com.hanxi.ghosterm

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject

class PluginTrustStore(context: Context) {
    private val preferences = context.getSharedPreferences("plugin-trust-v1", Context.MODE_PRIVATE)
    fun records(): List<PluginTrustRecord> = runCatching {
        val array = JSONArray(preferences.getString("records", "[]"))
        (0 until array.length()).map { value -> decode(array.getJSONObject(value)) }.filter { it.isValid }
    }.getOrDefault(emptyList())
    fun put(record: PluginTrustRecord) {
        val values = records().filterNot { it.id == record.id }.plus(record)
        preferences.edit().putString("records", JSONArray(values.map(::encode)).toString()).apply()
    }
    fun remove(id: String) {
        preferences.edit().putString("records", JSONArray(records().filterNot { it.id == id }.map(::encode)).toString()).apply()
    }
    private fun encode(value: PluginTrustRecord) = JSONObject().apply {
        put("id", value.id); put("repository", value.repository); put("releaseTag", value.releaseTag); put("pluginId", value.pluginId); put("version", value.version); put("role", value.role.name); put("publisherFingerprint", value.publisherFingerprint); put("archiveSha256", value.archiveSha256); put("releaseJsonSha256", value.releaseJsonSha256); put("installedAt", value.installedAtEpochMs); put("pinned", value.pinned); value.lastCheckedAtEpochMs?.let { put("lastCheckedAt", it) }
    }
    private fun decode(value: JSONObject) = PluginTrustRecord(value.getString("id"), value.getString("repository"), value.getString("releaseTag"), value.getString("pluginId"), value.getString("version"), PluginInstallPlan.Role.valueOf(value.getString("role")), value.getString("publisherFingerprint"), value.getString("archiveSha256"), value.getString("releaseJsonSha256"), value.getLong("installedAt"), value.getBoolean("pinned"), if (value.has("lastCheckedAt")) value.getLong("lastCheckedAt") else null)
}
