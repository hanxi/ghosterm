package com.hanxi.ghosterm

data class PluginCatalogEntry(val source: PluginGitHubSource, val displayName: String, val defaultRole: String, val publisherFingerprint: String, val sortOrder: Int, val automaticUpdates: Boolean)
object PluginCatalog {
    val builtIn = listOf(
        PluginCatalogEntry(PluginGitHubSource.parse("hanxi/ghosterm:preview").getOrThrow(), "官方 Preview", "both", "", 10, true),
        PluginCatalogEntry(PluginGitHubSource.parse("hanxi/ghosterm:table").getOrThrow(), "官方 Table", "client", "", 20, true)
    )
}
