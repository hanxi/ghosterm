package com.hanxi.ghosterm

data class PluginTrustRecord(
    val id: String,
    val repository: String,
    val releaseTag: String,
    val pluginId: String,
    val version: String,
    val role: PluginInstallPlan.Role,
    val publisherFingerprint: String,
    val archiveSha256: String,
    val releaseJsonSha256: String,
    val installedAtEpochMs: Long,
    val pinned: Boolean,
    val lastCheckedAtEpochMs: Long?
) {
    val isValid get() = repository.isNotBlank() && releaseTag.isNotBlank() && pluginId.isNotBlank() && version.isNotBlank() && publisherFingerprint.length == 64 && archiveSha256.length == 64 && releaseJsonSha256.length == 64
}
