package com.hanxi.ghosterm

import java.net.URI

data class PluginGitHubSource(val owner: String, val repository: String, val plugin: String? = null, val tag: String? = null) {
    val repositoryPath get() = "$owner/$repository"
    fun indexUrl(): String = "https://github.com/$repositoryPath/releases/" + (tag?.let { "download/$it/plugin-index.json" } ?: "latest/download/plugin-index.json")
    fun assetUrl(name: String, releaseTag: String): String? = if (name.isBlank() || name.contains('/') || name.contains("..")) null else "https://github.com/$repositoryPath/releases/download/$releaseTag/$name"
    companion object {
        private val segment = Regex("[A-Za-z0-9_-]{1,128}")
        fun parse(input: String): Result<PluginGitHubSource> = runCatching {
            var value = input.trim()
            require(value.isNotEmpty())
            if (value.startsWith("https://") || value.startsWith("http://")) {
                val uri = URI(value); require(uri.scheme == "https" && uri.host == "github.com")
                value = uri.path.trim('/')
            }
            val colon = value.indexOf(':')
            val suffix = if (colon >= 0) value.substring(colon + 1) else null
            val path = if (colon >= 0) value.substring(0, colon) else value
            val parts = path.split('/')
            require(parts.size == 2)
            require(segment.matches(parts[0]))
            val repoTag = parts[1].split('@', limit = 2)
            require(segment.matches(repoTag[0]) && !repoTag[0].endsWith(".git"))
            var tag = repoTag.getOrNull(1)?.also { require(segment.matches(it)) }
            var plugin: String? = null
            suffix?.let {
                val pluginParts = it.split('@', limit = 2)
                require(segment.matches(pluginParts[0])); plugin = pluginParts[0]
                if (pluginParts.size == 2) { require(segment.matches(pluginParts[1])); tag = pluginParts[1] }
            }
            PluginGitHubSource(parts[0], repoTag[0], plugin, tag)
        }
    }
}

data class PluginIndexEntry(val id: String, val name: String, val version: String, val role: String, val archive: String, val releaseJSON: String, val releaseSignature: String, val archiveBytes: Long, val archiveSHA256: String, val permissions: List<String> = emptyList(), val capabilities: List<String> = emptyList())

data class PluginIndex(
    val schemaVersion: Int,
    val repository: String,
    val releaseTag: String,
    val publisher: String,
    val publisherFingerprint: String,
    val publisherPublicKey: String,
    val plugins: List<PluginIndexEntry>,
    val signatureDomain: String,
    val signature: String
) {
    fun validate(source: PluginGitHubSource): Boolean = schemaVersion == 1 && repository == source.repositoryPath &&
        (source.tag == null || source.tag == releaseTag) && publisher.isNotBlank() && publisherFingerprint.length == 64 &&
        publisherPublicKey.isNotBlank() && signatureDomain == "ghosterm.plugin-index.v1" && signature.isNotBlank() && plugins.isNotEmpty() &&
        plugins.all { it.id.isNotBlank() && it.version.isNotBlank() && it.role in setOf("client", "executor", "both") && it.archiveBytes > 0 && it.archiveSHA256.length == 64 }
}
